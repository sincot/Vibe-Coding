#include "http/server.h"

#include <sys/socket.h>

#include <utility>

#include <nlohmann/json.hpp>

#include "auth/register.h"
#include "log.h"

namespace oj {

namespace {
using nlohmann::json;

// 统一 JSON 响应。错误约定（见 README）：
//   注册成功 201；登录成功 200；非法输入 400；登录失败/无效认证 401；
//   昵称冲突 409；限速 429；内部故障 500。
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
                       auth::JwtConfig jwt_config)
    : host_(std::move(host)),
      port_(port),
      db_(db),
      jwt_(std::move(jwt_config.secret), jwt_config.expires_seconds),
      user_store_(db),
      rate_limiter_(auth::RateLimiter::Config{}),
      account_gen_(),
      register_service_(db, account_gen_),
      login_service_(db, jwt_) {
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

} // namespace oj
