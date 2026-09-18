#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <httplib.h>

#include "auth/account.h"
#include "auth/register.h"

namespace oj {

class Database;

// HTTP 服务封装：注册路由、启动监听、优雅停止。
//
// 启动流程拆分为 bind 与 listen 两步：bind_to_port() 同步返回绑定结果，
// 用于在进入监听循环前发现「端口被占用 / 地址不可用」等错误并给出明确提示；
// listen_after_bind() 在后台线程中进入 accept 循环，stop() 关闭监听套接字，
// 使循环退出并回收线程资源。
class HttpServer {
public:
  HttpServer(std::string host, int port, Database &db);
  ~HttpServer();

  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;

  // 绑定并启动监听。成功返回 true；失败时返回 false 并通过 error 给出原因。
  bool start(std::string &error);

  // 请求停止并等待监听线程退出（可重复调用，幂等）。
  void stop();

  bool is_running() const;

private:
  void setup_routes();
  void handle_register(const httplib::Request &req, httplib::Response &res);

  std::string host_;
  int port_;
  Database &db_;
  auth::RandomAccountGenerator account_gen_;
  auth::RegisterService register_service_;
  httplib::Server svr_;
  std::thread listen_thread_;
  std::atomic<bool> running_{false};
};

} // namespace oj
