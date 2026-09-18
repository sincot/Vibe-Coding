#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <httplib.h>

#include "auth/account.h"
#include "auth/authorize.h"
#include "auth/context.h"
#include "auth/jwt.h"
#include "auth/login.h"
#include "auth/password_change.h"
#include "auth/rate_limit.h"
#include "auth/register.h"
#include "db/users.h"

namespace oj {

class Database;

// HTTP 服务封装：注册路由、启动监听、优雅停止。
//
// 启动流程拆分为 bind 与 listen 两步：bind_to_port() 同步返回绑定结果，
// 用于在进入监听循环前发现「端口被占用 / 地址不可用」等错误并给出明确提示；
// listen_after_bind() 在后台线程中进入 accept 循环，stop() 关闭监听套接字，
// 使循环退出并回收线程资源。
//
// enable_test_routes 仅用于集成测试：为 true 时额外注册测试专用路由（如
// /api/test/admin-only），用于在正式管理员业务接口（M2）落地前验证管理员权限
// 与首次改密限制的组合行为。正式服务始终以 false 启动，不暴露测试入口。
class HttpServer {
public:
  HttpServer(std::string host, int port, Database &db, auth::JwtConfig jwt_config,
             bool enable_test_routes = false);
  ~HttpServer();

  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;

  // 绑定并启动监听。成功返回 true；失败时返回 false 并通过 error 给出原因。
  bool start(std::string &error);

  // 请求停止并等待监听线程退出（可重复调用，幂等）。
  void stop();

  bool is_running() const;

  // 暴露限速器，便于集成测试验证限速行为（返回引用，测试方可注入时钟）。
  auth::RateLimiter &rate_limiter() { return rate_limiter_; }

private:
  void setup_routes();
  void handle_register(const httplib::Request &req, httplib::Response &res);
  void handle_login(const httplib::Request &req, httplib::Response &res);
  void handle_me(const httplib::Request &req, httplib::Response &res);
  void handle_change_password(const httplib::Request &req,
                              httplib::Response &res);
  void handle_test_admin_only(const httplib::Request &req,
                              httplib::Response &res);

  std::string host_;
  int port_;
  Database &db_;
  auth::JwtService jwt_;
  UserStore user_store_;
  auth::RateLimiter rate_limiter_;
  auth::RandomAccountGenerator account_gen_;
  auth::RegisterService register_service_;
  auth::LoginService login_service_;
  auth::ChangePasswordService change_password_service_;
  bool enable_test_routes_;
  httplib::Server svr_;
  std::thread listen_thread_;
  std::atomic<bool> running_{false};
};

} // namespace oj
