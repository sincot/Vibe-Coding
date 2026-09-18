#include "http/server.h"

#include <sys/socket.h>

#include <utility>

#include <nlohmann/json.hpp>

#include "auth/register.h"
#include "log.h"

namespace oj {

namespace {
using nlohmann::json;

// 统一 JSON 响应。错误约定（见 SPEC M1.1 实施说明 / README）：
//   成功 201；非法输入 400；昵称冲突 409；内部故障 500。
// 错误响应体统一为 {"error": "..."}，且不泄露数据库/SQL/哈希等内部细节。
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

} // namespace

HttpServer::HttpServer(std::string host, int port, Database &db)
    : host_(std::move(host)),
      port_(port),
      db_(db),
      register_service_(db_, account_gen_) {
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

} // namespace oj
