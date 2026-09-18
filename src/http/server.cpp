#include "http/server.h"

#include <sys/socket.h>

#include <utility>

#include <nlohmann/json.hpp>

#include "auth/password_change.h"
#include "auth/validation.h"
#include "log.h"

namespace oj {

namespace {
using nlohmann::json;

// 统一 JSON 响应。错误约定（见 README）：
//   注册成功 201；登录成功 200；非法输入 400；登录失败/无效认证/旧密码错误 401；
//   昵称冲突 409；权限不足/必须先改密 403；限速 429；内部故障 500。
// 错误响应体统一为 {"error": "..."}，且不泄露数据库/SQL/哈希/密钥/token 等内部细节。
void send_json(httplib::Response &res, int status, const json &body) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

void send_error(httplib::Response &res, int status, const std::string &message) {
  json body;
  body["error"] = message;
  send_json(res, status, body);
}

// 必须先改密的错误响应：除 {"error": "..."} 外附加稳定错误码，供前端识别并
// 跳转到改密流程。字段名与取值作为 API 约定固定，不随文案变化。
void send_password_change_required(httplib::Response &res) {
  json body;
  body["error"] = "请先修改密码";
  body["code"] = "PASSWORD_CHANGE_REQUIRED";
  send_json(res, 403, body);
}

// 最小健康检查：确认服务存活并正常响应 JSON。
void handle_health(const httplib::Request &, httplib::Response &res) {
  json body;
  body["status"] = "ok";
  send_json(res, 200, body);
}

// 自定义套接字选项：仅启用 SO_REUSEADDR，显式关闭 cpp-httplib 默认的
// SO_REUSEPORT。SO_REUSEPORT 允许多个进程同时监听同一端口，会导致「端口被
// 占用」无法被 bind_to_port() 检测；SO_REUSEADDR 仅允许复用 TIME_WAIT 状态
// 的端口，对处于 LISTEN 状态的端口 bind 会返回 EADDRINUSE，从而正确报错，
// 同时不影响停止后在同一端口快速重启。
void configure_socket(socket_t sock) {
  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<void *>(&yes),
             sizeof(yes));
}

// 解析登录请求体，提取 account 与 password（均为非空字符串）。
// 返回 false 时已设置响应，调用方直接返回。
bool parse_login_body(const std::string &body, std::string &account,
                      std::string &password, httplib::Response &res) {
  json parsed;
  try {
    parsed = json::parse(body);
  } catch (const std::exception &) {
    send_error(res, 400, "请求体不是合法的 JSON");
    return false;
  }
  if (!parsed.is_object()) {
    send_error(res, 400, "请求体必须是 JSON 对象");
    return false;
  }
  if (!parsed.contains("account") || !parsed["account"].is_string()) {
    send_error(res, 400, "缺少字段或类型错误：account 须为字符串");
    return false;
  }
  if (!parsed.contains("password") || !parsed["password"].is_string()) {
    send_error(res, 400, "缺少字段或类型错误：password 须为字符串");
    return false;
  }
  account = parsed["account"].get<std::string>();
  password = parsed["password"].get<std::string>();
  if (account.empty() || password.empty()) {
    send_error(res, 400, "account 与 password 不能为空");
    return false;
  }
  return true;
}

// 解析改密请求体，提取 old_password 与 new_password（均为非空字符串）。
// 只读取这两个字段；用户身份由已验证的当前用户上下文决定，忽略客户端传入的
// 任何 id / account / role 等字段，防止越权修改他人密码或提权。
bool parse_password_change_body(const std::string &body, std::string &old_password,
                                std::string &new_password,
                                httplib::Response &res) {
  json parsed;
  try {
    parsed = json::parse(body);
  } catch (const std::exception &) {
    send_error(res, 400, "请求体不是合法的 JSON");
    return false;
  }
  if (!parsed.is_object()) {
    send_error(res, 400, "请求体必须是 JSON 对象");
    return false;
  }
  if (!parsed.contains("old_password") || !parsed["old_password"].is_string()) {
    send_error(res, 400, "缺少字段或类型错误：old_password 须为字符串");
    return false;
  }
  if (!parsed.contains("new_password") || !parsed["new_password"].is_string()) {
    send_error(res, 400, "缺少字段或类型错误：new_password 须为字符串");
    return false;
  }
  old_password = parsed["old_password"].get<std::string>();
  new_password = parsed["new_password"].get<std::string>();
  if (old_password.empty() || new_password.empty()) {
    send_error(res, 400, "old_password 与 new_password 不能为空");
    return false;
  }
  return true;
}

// 对已验证登录的用户执行管理员权限检查；不满足时设置响应并返回 false。
// 组合了「具备 admin 角色」与「已完成必要改密」两项要求（登录已由调用方保证）。
bool enforce_admin(const auth::AuthUser &user, httplib::Response &res) {
  switch (auth::check_admin(user)) {
    case auth::AdminCheck::Ok:
      return true;
    case auth::AdminCheck::NotAdmin:
      send_error(res, 403, "权限不足");
      return false;
    case auth::AdminCheck::PasswordChangeRequired:
      send_password_change_required(res);
      return false;
  }
  return false;
}

// 构造不含敏感字段的用户信息 JSON（绝不包含 password_hash）。
json public_user_json(const auth::AuthUser &user) {
  json j;
  j["id"] = user.id;
  j["account"] = user.account;
  j["nickname"] = user.nickname;
  j["role"] = user.role;
  j["reset_pwd_flag"] = user.reset_pwd_flag;
  return j;
}

json public_user_json(const UserRecord &user) {
  json j;
  j["id"] = user.id;
  j["account"] = user.account;
  j["nickname"] = user.nickname;
  j["role"] = user.role;
  j["reset_pwd_flag"] = user.reset_pwd_flag;
  return j;
}

} // namespace

HttpServer::HttpServer(std::string host, int port, Database &db,
                       auth::JwtConfig jwt_config, bool enable_test_routes)
    : host_(std::move(host)),
      port_(port),
      db_(db),
      jwt_(std::move(jwt_config.secret), jwt_config.expires_seconds),
      user_store_(db),
      rate_limiter_(auth::RateLimiter::Config{}),
      account_gen_(),
      register_service_(db, account_gen_),
      login_service_(db, jwt_),
      change_password_service_(db),
      enable_test_routes_(enable_test_routes) {
  svr_.set_socket_options(configure_socket);
}

HttpServer::~HttpServer() {
  stop();
}

bool HttpServer::start(std::string &error) {
  setup_routes();

  if (!svr_.bind_to_port(host_.c_str(), port_)) {
    error = "无法绑定 " + host_ + ":" + std::to_string(port_) +
            "（地址不可用或端口已被占用）";
    return false;
  }

  listen_thread_ = std::thread([this]() { svr_.listen_after_bind(); });
  running_ = true;
  return true;
}

void HttpServer::stop() {
  if (running_.exchange(false)) {
    svr_.stop();
    if (listen_thread_.joinable()) {
      listen_thread_.join();
    }
  }
}

bool HttpServer::is_running() const {
  return running_.load();
}

void HttpServer::setup_routes() {
  svr_.Get("/api/health", handle_health);
  svr_.Post("/api/register", [this](const httplib::Request &req,
                                    httplib::Response &res) {
    handle_register(req, res);
  });
  svr_.Post("/api/login", [this](const httplib::Request &req,
                                 httplib::Response &res) {
    handle_login(req, res);
  });
  svr_.Get("/api/me", [this](const httplib::Request &req,
                              httplib::Response &res) {
    handle_me(req, res);
  });
  svr_.Post("/api/me/password", [this](const httplib::Request &req,
                                       httplib::Response &res) {
    handle_change_password(req, res);
  });

  // 测试专用路由：仅 enable_test_routes_ 为 true（集成测试）时注册，
  // 用于在正式管理员业务接口落地前验证管理员权限与首次改密限制的组合行为。
  // 正式服务以 false 启动，不会暴露此入口。
  if (enable_test_routes_) {
    svr_.Get("/api/test/admin-only", [this](const httplib::Request &req,
                                            httplib::Response &res) {
      handle_test_admin_only(req, res);
    });
  }
}

void HttpServer::handle_register(const httplib::Request &req,
                                 httplib::Response &res) {
  json body;
  try {
    body = json::parse(req.body);
  } catch (const std::exception &) {
    send_error(res, 400, "请求体不是合法的 JSON");
    return;
  }
  if (!body.is_object()) {
    send_error(res, 400, "请求体必须是 JSON 对象");
    return;
  }

  // 仅读取 nickname 与 password；role / account 等字段一律忽略（由后端控制，
  // 客户端无法借此提升权限或指定账号）。
  if (!body.contains("nickname") || !body["nickname"].is_string()) {
    send_error(res, 400, "缺少字段或类型错误：nickname 须为字符串");
    return;
  }
  if (!body.contains("password") || !body["password"].is_string()) {
    send_error(res, 400, "缺少字段或类型错误：password 须为字符串");
    return;
  }

  std::string nickname = body["nickname"].get<std::string>();
  std::string password = body["password"].get<std::string>();

  auto outcome = register_service_.register_user(nickname, password);
  switch (outcome.kind) {
    case auth::RegisterOutcome::Kind::Success: {
      json resp;
      resp["account"] = outcome.user.account;
      resp["nickname"] = outcome.user.nickname;
      resp["role"] = outcome.user.role;
      resp["id"] = outcome.user.id;
      send_json(res, 201, resp);
      return;
    }
    case auth::RegisterOutcome::Kind::InvalidNickname:
    case auth::RegisterOutcome::Kind::InvalidPassword:
      send_error(res, 400, outcome.error);
      return;
    case auth::RegisterOutcome::Kind::NicknameTaken:
      send_error(res, 409, outcome.error);
      return;
    case auth::RegisterOutcome::Kind::InternalError:
      send_error(res, 500, "内部错误");
      return;
  }
}

void HttpServer::handle_login(const httplib::Request &req,
                              httplib::Response &res) {
  std::string account;
  std::string password;
  if (!parse_login_body(req.body, account, password, res)) {
    return;
  }

  // 限速维度：客户端来源 IP（req.remote_addr）。不信任任何客户端提供的转发头
  // （如 X-Forwarded-For），避免通过伪造来源头绕过限速。
  const std::string &client_ip = req.remote_addr;

  auto limit = rate_limiter_.allow(client_ip);
  if (limit.status == auth::RateLimiter::Status::Blocked) {
    res.set_header("Retry-After", std::to_string(limit.retry_after_seconds));
    send_error(res, 429, "登录尝试过于频繁，请稍后再试");
    return;
  }

  auto outcome = login_service_.login(account, password);
  switch (outcome.kind) {
    case auth::LoginOutcome::Kind::Success: {
      rate_limiter_.clear(client_ip);
      json resp;
      resp["token"] = outcome.token;
      resp["token_type"] = "Bearer";
      resp["expires_in"] = outcome.expires_in_seconds;
      resp["user"] = public_user_json(outcome.user);
      send_json(res, 200, resp);
      return;
    }
    case auth::LoginOutcome::Kind::InvalidCredentials:
      send_error(res, 401, "账号或密码错误");
      return;
    case auth::LoginOutcome::Kind::InternalError:
      // 内部故障（数据库等）不按用户密码错误计数，避免故障期间误触限速。
      rate_limiter_.clear(client_ip);
      send_error(res, 500, "内部错误");
      return;
  }
}

void HttpServer::handle_me(const httplib::Request &req, httplib::Response &res) {
  std::string token;
  if (!auth::extract_bearer_token(req.get_header_value("Authorization"), token)) {
    send_error(res, 401, "未提供有效的认证信息");
    return;
  }

  auth::AuthUser user;
  std::string err;
  switch (auth::authenticate_request(jwt_, user_store_, token, user, err)) {
    case auth::AuthResult::Ok:
      send_json(res, 200, public_user_json(user));
      return;
    case auth::AuthResult::Unauthorized:
      send_error(res, 401, "认证失败");
      return;
    case auth::AuthResult::InternalError:
      send_error(res, 500, "内部错误");
      return;
  }
}

void HttpServer::handle_change_password(const httplib::Request &req,
                                        httplib::Response &res) {
  // 先完成登录校验，目标用户来自已验证的当前用户上下文（token 验证 + 数据库
  // 回查），客户端传入的 id / account 等字段一律被忽略。
  std::string token;
  if (!auth::extract_bearer_token(req.get_header_value("Authorization"), token)) {
    send_error(res, 401, "未提供有效的认证信息");
    return;
  }

  auth::AuthUser user;
  std::string err;
  switch (auth::authenticate_request(jwt_, user_store_, token, user, err)) {
    case auth::AuthResult::Ok:
      break;
    case auth::AuthResult::Unauthorized:
      send_error(res, 401, "认证失败");
      return;
    case auth::AuthResult::InternalError:
      send_error(res, 500, "内部错误");
      return;
  }

  std::string old_password;
  std::string new_password;
  if (!parse_password_change_body(req.body, old_password, new_password, res)) {
    return;
  }

  // 复用注册密码规则校验新密码，并要求新密码与旧密码不同；不做任何裁剪/截断。
  std::string validation_err;
  if (!auth::validate_password_change(old_password, new_password,
                                      validation_err)) {
    send_error(res, 400, validation_err);
    return;
  }

  auto result =
      change_password_service_.change_password(user.id, old_password,
                                               new_password);
  switch (result.outcome) {
    case auth::ChangePasswordService::Outcome::Success: {
      json resp;
      resp["status"] = "ok";
      send_json(res, 200, resp);
      return;
    }
    case auth::ChangePasswordService::Outcome::InvalidOldPassword:
      send_error(res, 401, "旧密码错误");
      return;
    case auth::ChangePasswordService::Outcome::UserNotFound:
      send_error(res, 401, "认证失败");
      return;
    case auth::ChangePasswordService::Outcome::InternalError:
      send_error(res, 500, "内部错误");
      return;
  }
}

void HttpServer::handle_test_admin_only(const httplib::Request &req,
                                        httplib::Response &res) {
  std::string token;
  if (!auth::extract_bearer_token(req.get_header_value("Authorization"), token)) {
    send_error(res, 401, "未提供有效的认证信息");
    return;
  }

  auth::AuthUser user;
  std::string err;
  switch (auth::authenticate_request(jwt_, user_store_, token, user, err)) {
    case auth::AuthResult::Ok:
      break;
    case auth::AuthResult::Unauthorized:
      send_error(res, 401, "认证失败");
      return;
    case auth::AuthResult::InternalError:
      send_error(res, 500, "内部错误");
      return;
  }

  if (!enforce_admin(user, res)) {
    return;
  }

  json resp;
  resp["status"] = "ok";
  send_json(res, 200, resp);
}

} // namespace oj
