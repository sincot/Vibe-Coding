#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

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
#include "db/testcase_admin.h"
#include "db/user_admin.h"
#include "db/users.h"
#include "judge/executor.h"
#include "judge/judge.h"
#include "judge/manager.h"
#include "submit/rejudge.h"
#include "submit/submit.h"

namespace oj {

class Database;

// 题目接口的访问者身份（游客 / 普通用户 / 管理员）。
// authenticated 表示携带了可验证的 token；is_admin 表示通过管理员检查
// （已登录 + 已完成首次改密 + 当前数据库角色为 admin）。user_id 来自已验证的
// 当前用户上下文，绝不来自客户端参数。
struct ProblemViewer {
  bool authenticated = false;
  bool is_admin = false;
  std::int64_t user_id = 0;
};

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
             std::string web_root = "",
             judge::JudgeManager::Options manager_options = {});
  ~HttpServer();

  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;

  // 绑定并启动监听。成功返回 true；失败时返回 false 并通过 error 给出原因。
  bool start(std::string &error);

  // 请求停止并等待监听线程退出（可重复调用，幂等）。随后停止判题调度器：不再接收
  // 新任务，并执行完所有已接收任务后回收 worker，保证在关闭数据库前没有 worker
  // 仍在使用数据库。
  void stop();

  bool is_running() const;

  // 暴露判题调度器，便于集成测试观察 worker / 队列状态（不转移所有权）。
  judge::JudgeManager *judge_manager() { return judge_manager_.get(); }

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
  void handle_admin_list_testcases(const httplib::Request &req,
                                   httplib::Response &res);
  void handle_admin_create_testcase(const httplib::Request &req,
                                    httplib::Response &res);
  void handle_admin_update_testcase(const httplib::Request &req,
                                    httplib::Response &res);
  void handle_admin_delete_testcase(const httplib::Request &req,
                                    httplib::Response &res);
  void handle_admin_list_users(const httplib::Request &req,
                               httplib::Response &res);
  void handle_admin_update_user(const httplib::Request &req,
                                httplib::Response &res);
  void handle_admin_rejudge(const httplib::Request &req,
                            httplib::Response &res);
  void handle_test_admin_only(const httplib::Request &req,
                              httplib::Response &res);

  // 管理员业务入口的登录检查：解析 Bearer token 并验证身份，成功时填充 user；
  // 之后再执行 enforce_admin（已登录 + 已完成首次改密 + admin 角色）。任一失败
  // 时已写入响应并返回 false，调用方直接返回。所有管理员接口统一复用本方法。
  bool require_admin(const httplib::Request &req, httplib::Response &res,
                     auth::AuthUser &user);

  // 解析可选的访问者身份：未携带 Authorization 头时视为游客；携带时复用已有
  // 身份验证。认证失败已写入响应并返回 false；成功时填充 viewer。
  bool resolve_viewer(const httplib::Request &req, httplib::Response &res,
                      ProblemViewer &viewer);

  // 将已持久化的提交记录与判题结果构造为统一 JSON 响应（提交 / 重判复用）。
  nlohmann::json submission_result_json(const SubmissionRecord &record,
                                        const judge::JudgeResult &judge);

  std::string host_;
  int port_;
  Database &db_;
  auth::JwtService jwt_;
  UserStore user_store_;
  ProblemStore problem_store_;
  ProblemAdminStore problem_admin_store_;
  TestcaseAdminStore testcase_admin_store_;
  UserAdminStore user_admin_store_;
  auth::RateLimiter rate_limiter_;
  auth::RandomAccountGenerator account_gen_;
  auth::RegisterService register_service_;
  auth::LoginService login_service_;
  auth::ChangePasswordService change_password_service_;
  // 判题执行器与提交服务：owned_executor_ 仅在未注入执行器时创建。
  std::unique_ptr<judge::IExecutor> owned_executor_;
  judge::IExecutor *judge_executor_ = nullptr;
  std::unique_ptr<submit::SubmitService> submit_service_;
  std::unique_ptr<submit::RejudgeService> rejudge_service_;
  // 判题任务调度器：持有 worker 线程池与有界等待队列，在 submit_service_ /
  // rejudge_service_ 之后构造、之前析构，确保调度器停止时服务仍然有效。
  std::unique_ptr<judge::JudgeManager> judge_manager_;
  // 重判并发去重：同一提交 ID 同时只能有一个待执行或正在执行的重判。
  std::mutex rejudge_mutex_;
  std::unordered_set<std::int64_t> rejudge_in_flight_;
  // cpp-httplib 请求处理线程数：>= 调度器并发上限 + 保留量，避免同步等待判题占满
  // 全部 HTTP 处理能力（健康检查与题目查询始终有可用线程）。
  int http_thread_count_ = 0;
  bool enable_test_routes_;
  std::string web_root_;
  httplib::Server svr_;
  std::thread listen_thread_;
  std::atomic<bool> running_{false};
  // 停止收尾日志只输出一次：stop() 可被显式调用与析构重复调用，避免重复刷屏。
  std::atomic<bool> stop_finished_logged_{false};
};

} // namespace oj
