#include "http/server.h"

#include <sys/socket.h>

#include <utility>

#include <nlohmann/json.hpp>

#include "log.h"

namespace oj {

namespace {
using nlohmann::json;

// 最小健康检查：确认服务存活并正常响应 JSON。
void handle_health(const httplib::Request &, httplib::Response &res) {
  json body;
  body["status"] = "ok";
  res.set_content(body.dump(), "application/json");
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

HttpServer::HttpServer(std::string host, int port)
    : host_(std::move(host)), port_(port) {
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
}

} // namespace oj
