#pragma once

#include <atomic>
#include <memory>
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
#include "db/problem_admin.h"
#include "db/problems.h"
#include "db/users.h"
#include "judge/executor.h"
#include "judge/judge.h"
#include "submit/submit.h"

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
//
// judge_executor 供测试注入可控执行器（如模拟内部判题故障、跳过真实编译）。
// 为 nullptr 时使用默认的 LocalExecutor（仅开发环境验证）。judge_options 可覆盖
// 判题工作目录等配置。正式服务无需传入二者。
class HttpServer {
public:
  // web_root 为前端静态资源目录：非空且存在时以只读方式挂载到 URL 根路径
  // 「/」（见 mount_static）。测试默认传空串，不启用静态托管，避免误暴露工作目录。
  HttpServer(std::string host, int port, Database &db, auth::JwtConfig jwt_config,
             bool enable_test_routes = false,
             judge::IExecutor *judge_executor = nullptr,
             judge::JudgeOptions judge_options = {},
             std::string web_root = "");
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
  // 挂载前端静态资源。仅当 web_root_ 非空、是存在的目录且不是危险路径（项目根 /
  // 系统根）时，才把该目录只读挂载到 URL 根路径「/」。挂载目录之外的文件（数据库、
  // 源码、配置、判题临时目录）不会被暴露；cpp-httplib 的路径校验同时阻止「..」
  // 越界访问。挂载失败只记录日志，不影响 /api 路由与健康检查。
  void mount_static();
  void handle_register(const httplib::Request &req, httplib::Response &res);
  void handle_login(const httplib::Request &req, httplib::Response &res);
  void handle_me(const httplib::Request &req, httplib::Response &res);
  void handle_change_password(const httplib::Request &req,
                              httplib::Response &res);
  void handle_problem_list(const httplib::Request &req,
                           httplib::Response &res);
  void handle_problem_detail(const httplib::Request &req,
                             httplib::Response &res);
  void handle_submit(const httplib::Request &req, httplib::Response &res);
  void handle_admin_create_problem(const httplib::Request &req,
                                   httplib::Response &res);
  void handle_admin_update_problem(const httplib::Request &req,
                                   httplib::Response &res);
  void handle_admin_delete_problem(const httplib::Request &req,
                                   httplib::Response &res);
  void handle_test_admin_only(const httplib::Request &req,
                              httplib::Response &res);

  // 管理员业务入口的登录检查：解析 Bearer token 并验证身份，成功时填充 user；
  // 之后再执行 enforce_admin（已登录 + 已完成首次改密 + admin 角色）。任一失败
  // 时已写入响应并返回 false，调用方直接返回。所有管理员接口统一复用本方法。
  bool require_admin(const httplib::Request &req, httplib::Response &res,
                     auth::AuthUser &user);

  // 解析可选的访问者身份：未携带 Authorization 头时视为游客；携带时复用已有
  // 身份验证。认证失败已写入响应并返回 false；成功时 is_admin 表示是否通过
  // 管理员检查（已登录 + 已完成首次改密 + admin 角色），用于题目可见性判断。
  bool resolve_viewer(const httplib::Request &req, httplib::Response &res,
                      bool &is_admin);

  std::string host_;
  int port_;
  Database &db_;
  auth::JwtService jwt_;
  UserStore user_store_;
  ProblemStore problem_store_;
  ProblemAdminStore problem_admin_store_;
  auth::RateLimiter rate_limiter_;
  auth::RandomAccountGenerator account_gen_;
  auth::RegisterService register_service_;
  auth::LoginService login_service_;
  auth::ChangePasswordService change_password_service_;
  // 判题执行器与提交服务：owned_executor_ 仅在未注入执行器时创建。
  std::unique_ptr<judge::IExecutor> owned_executor_;
  judge::IExecutor *judge_executor_ = nullptr;
  std::unique_ptr<submit::SubmitService> submit_service_;
  bool enable_test_routes_;
  std::string web_root_;
  httplib::Server svr_;
  std::thread listen_thread_;
  std::atomic<bool> running_{false};
};

} // namespace oj
