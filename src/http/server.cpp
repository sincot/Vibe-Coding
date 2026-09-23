#include "http/server.h"

#include <sys/socket.h>

#include <chrono>
#include <filesystem>
#include <utility>

#include <nlohmann/json.hpp>

#include "auth/password.h"
#include "auth/password_change.h"
#include "auth/validation.h"
#include "db/in_flight.h"
#include "judge/local_executor.h"
#include "log.h"
#include "problem/list_query.h"
#include "problem/testcase_validation.h"
#include "problem/validation.h"
#include "user/admin_user_validation.h"

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

// 判题等待队列满载：立即拒绝并返回稳定的业务错误标识与重试提示，不阻塞等待、
// 不无限积压，也不由前端自动重试。
void send_judge_queue_full(httplib::Response &res) {
  res.set_header("Retry-After", "1");
  json body;
  body["error"] = "判题队列已满，请稍后重试";
  body["code"] = "JUDGE_QUEUE_FULL";
  body["retryable"] = true;
  send_json(res, 503, body);
}

// 调度器已停止接收（服务正在停止）：同样以 503 明确表示暂时不可用。
void send_judge_unavailable(httplib::Response &res) {
  json body;
  body["error"] = "判题服务暂时不可用";
  body["code"] = "JUDGE_UNAVAILABLE";
  body["retryable"] = true;
  send_json(res, 503, body);
}

// 同一提交已有正在进行的重判：返回明确冲突标识，避免重复排队。
void send_rejudge_in_progress(httplib::Response &res) {
  json body;
  body["error"] = "该提交正在重判中";
  body["code"] = "REJUDGE_IN_PROGRESS";
  send_json(res, 409, body);
}

// 重判并发去重守卫：在作用域结束时从“进行中”集合移除该提交 ID。
class RejudgeGuard {
public:
  RejudgeGuard(std::mutex &mutex, std::unordered_set<std::int64_t> &set,
               std::int64_t id)
      : mutex_(mutex), set_(set), id_(id), active_(true) {}

  void release() { active_ = false; }

  ~RejudgeGuard() {
    if (!active_) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    set_.erase(id_);
  }

private:
  std::mutex &mutex_;
  std::unordered_set<std::int64_t> &set_;
  std::int64_t id_;
  bool active_;
};

// 除判题并发上限之外额外保留的 HTTP 处理线程数，用于在判题繁忙/队列满载时仍能
// 应答健康检查与题目查询等非提交请求。取值需覆盖教学规模下的常规并发查询。
constexpr int kHttpReserveThreads = 8;

// 健康检查：确认服务存活并正常响应 JSON。
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

// 解析题目 ID：仅接受正的十进制整数（可含前导零），否则视为非法客户端输入。
bool parse_problem_id(const std::string &text, std::int64_t &out) {
  if (text.empty() || text.size() > 19) {
    return false;
  }
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  try {
    std::size_t pos = 0;
    long long value = std::stoll(text, &pos);
    if (pos != text.size() || value <= 0) {
      return false;
    }
    out = static_cast<std::int64_t>(value);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

// 列表条目 JSON：只含列表展示所需字段，不含题面与任何测试用例。
// viewer_authenticated 为 true 时附带本人对该题的 AC 状态（solved）；
// 游客不附带该字段，避免把「本人状态」伪装给未登录访问者。
json problem_summary_json(const ProblemSummary &problem,
                          bool viewer_authenticated) {
  json j;
  j["id"] = problem.id;
  j["title"] = problem.title;
  j["difficulty"] = problem.difficulty;
  j["tags"] = problem.tags;
  j["visible"] = problem.visible;
  j["pass_count"] = problem.pass_count;
  if (viewer_authenticated) {
    j["solved"] = problem.solved;
  }
  return j;
}

// 详情 JSON：只含元数据与公开样例，绝不包含隐藏用例。
json problem_detail_json(const ProblemRecord &problem,
                         const std::vector<SampleCase> &samples) {
  json j;
  j["id"] = problem.id;
  j["title"] = problem.title;
  j["description"] = problem.description;
  j["difficulty"] = problem.difficulty;
  j["tags"] = problem.tags;
  j["time_limit_ms"] = problem.time_limit_ms;
  j["memory_limit_kb"] = problem.memory_limit_kb;
  j["visible"] = problem.visible;
  json sample_array = json::array();
  for (const SampleCase &sample : samples) {
    json item;
    item["input"] = sample.input;
    item["output"] = sample.output;
    sample_array.push_back(std::move(item));
  }
  j["samples"] = std::move(sample_array);
  return j;
}

// 管理员用例 JSON：包含编辑所需字段（用例 ID、题目归属、ord、输入、期望输出）以及
// is_sample 标记，供后台区分公开样例与隐藏用例。仅用于受管理员权限保护的管理接口，
// 绝不用于公开题目列表/详情。
json admin_testcase_json(const TestcaseRecord &tc, std::int64_t problem_id) {
  json j;
  j["id"] = tc.id;
  j["problem_id"] = problem_id;
  j["ord"] = tc.ord;
  j["input"] = tc.input;
  j["output"] = tc.output;
  j["is_sample"] = tc.is_sample;
  return j;
}

// 管理员用户列表条目 JSON：仅含管理所需字段，绝不包含 password_hash。
// 结构体 UserSummary 本身也不含哈希，双重保证不泄露敏感字段。
json admin_user_summary_json(const UserSummary &user) {
  json j;
  j["id"] = user.id;
  j["account"] = user.account;
  j["nickname"] = user.nickname;
  j["role"] = user.role;
  j["reset_pwd_flag"] = user.reset_pwd_flag;
  j["created_at"] = user.created_at;
  return j;
}

} // namespace

HttpServer::HttpServer(std::string host, int port, Database &db,
                       auth::JwtConfig jwt_config, bool enable_test_routes,
                       judge::IExecutor *judge_executor,
                       judge::JudgeOptions judge_options, std::string web_root,
                       judge::JudgeManager::Options manager_options)
    : host_(std::move(host)),
      port_(port),
      db_(db),
      jwt_(std::move(jwt_config.secret), jwt_config.expires_seconds),
      user_store_(db),
      problem_store_(db),
      problem_admin_store_(db),
      testcase_admin_store_(db),
      user_admin_store_(db),
      rate_limiter_(auth::RateLimiter::Config{}),
      account_gen_(),
      register_service_(db, account_gen_),
      login_service_(db, jwt_),
      change_password_service_(db),
      enable_test_routes_(enable_test_routes),
      web_root_(std::move(web_root)) {
  // 判题执行器：测试可注入可控实现；正式运行使用默认 LocalExecutor（仅开发环境
  // 验证，完整沙箱在 M3 提供）。
  if (judge_executor != nullptr) {
    judge_executor_ = judge_executor;
  } else {
    owned_executor_ = std::make_unique<judge::LocalExecutor>();
    judge_executor_ = owned_executor_.get();
  }
  submit_service_ = std::make_unique<submit::SubmitService>(
      db, *judge_executor_, judge_options);
  rejudge_service_ = std::make_unique<submit::RejudgeService>(
      db, *judge_executor_, std::move(judge_options));

  // 判题调度器：handler 复用 SubmitService / RejudgeService，仅负责调度。
  judge_manager_ = std::make_unique<judge::JudgeManager>(
      [this](const judge::SubmissionTask &task) -> submit::SubmitService::Outcome {
        if (task.rejudge_submission_id != 0) {
          return rejudge_service_->rejudge(task);
        }
        return submit_service_->submit(task.user_id, task.problem_id,
                                       task.language, task.source_code,
                                       task.viewer_is_admin, task.submitted_at,
                                       task.cancel.get(),
                                       task.in_flight_task_id);
      },
      manager_options);

  // 启动恢复服务：实例标识用于任务认领归属，便于排查多实例/崩溃后的占用。
  recovery_service_ = std::make_unique<submit::RecoveryService>(
      db, *judge_manager_, generate_task_id(),
      submit::RecoveryService::Options());

  // 协调 HTTP 处理线程与判题并发：最坏情况下被同步等待占用的 HTTP 线程数不超过
  // 「正在执行的判题任务 + 等待队列容量」，再额外保留 kHttpReserveThreads 个线程
  // 用于健康检查与题目查询等请求，避免把阻塞从判题器整体转移到 HTTP 层。
  http_thread_count_ = judge_manager_->worker_count() +
                       static_cast<int>(judge_manager_->queue_capacity()) +
                       kHttpReserveThreads;

  // 限制请求体上限，防止超大请求打爆内存；超长源码另在提交接口按字节上限校验。
  svr_.set_payload_max_length(submit::kMaxRequestBodyBytes);
  svr_.set_socket_options(configure_socket);
}

HttpServer::~HttpServer() {
  stop();
}

bool HttpServer::start(std::string &error) {
  setup_routes();

  // 使用显式大小的请求处理线程池，保证同步等待判题的请求不会占满全部 HTTP 能力。
  const int http_threads = http_thread_count_ > 0 ? http_thread_count_ : 1;
  svr_.new_task_queue = [http_threads]() -> httplib::TaskQueue * {
    return new httplib::ThreadPool(static_cast<std::size_t>(http_threads));
  };

  if (!svr_.bind_to_port(host_.c_str(), port_)) {
    error = "无法绑定 " + host_ + ":" + std::to_string(port_) +
            "（地址不可用或端口已被占用）";
    return false;
  }

  listen_thread_ = std::thread([this]() { svr_.listen_after_bind(); });
  running_ = true;
  return true;
}

std::size_t HttpServer::recover_pending_tasks() {
  if (!recovery_service_) {
    return 0;
  }
  return recovery_service_->run();
}

void HttpServer::stop() {
  // 先请求中止启动恢复投递（若正在恢复），避免停止流程与恢复互相等待。
  if (recovery_service_) {
    recovery_service_->abort();
  }
  if (running_.exchange(false)) {
    // 停止顺序（避免 HTTP 线程与判题器互相等待）：
    //   1) 先通知判题调度器取消：正在执行的判题任务尽快终止子进程，等待队列中的任务
    //      在取出后短路、不再启动新进程；cancel_all 只置位、不阻塞。
    //   2) 再 svr_.stop() 停止监听并等待 HTTP 处理线程结束。此时同步等待判题结果的
    //      请求会随各任务交付结果而返回，不会与停止流程互相等待。
    //   （若先 svr_.stop()，cpp-httplib 会等待仍在同步等待判题结果的 HTTP 处理线程，
    //    而判题器又在等待停止流程，造成不必要的长时间等待。）
    log(LogLevel::Info, "HTTP 服务开始停止：先取消判题调度，再停止监听");
    if (judge_manager_) {
      judge_manager_->cancel_all();
    }
    svr_.stop();
    if (listen_thread_.joinable()) {
      listen_thread_.join();
    }
  }
  // HTTP 处理线程（含正在同步等待判题结果的请求）此时已全部结束；再停止判题
  // 调度器并回收 worker，保证随后关闭数据库时没有 worker 仍在使用数据库。已接收
  // 任务的结果（含取消产生的 SYSERR）都在 join 之前持久化，数据库保持可用。
  // 停止等待预算：取消是协作式的（执行器约每 20ms 轮询），正常情况下应在数秒内
  // 完成；单次判题仍有全局 60s 硬上限兜底，不会无限等待。超过预算仅记录告警，不
  // 强制杀死线程（避免破坏数据库/文件系统一致性），也不宣称已保存全部结果。
  if (judge_manager_) {
    const auto start = std::chrono::steady_clock::now();
    judge_manager_->shutdown();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    if (!stop_finished_logged_.exchange(true)) {
      constexpr long long kStopWaitBudgetMs = 30000;
      if (elapsed_ms > kStopWaitBudgetMs) {
        log(LogLevel::Warn,
            "判题调度器停止耗时 " + std::to_string(elapsed_ms) +
                " ms，超出预算 " + std::to_string(kStopWaitBudgetMs) +
                " ms（已接收 " +
                std::to_string(judge_manager_->accepted_count()) + "，已完成 " +
                std::to_string(judge_manager_->completed_count()) + "）");
      } else {
        log(LogLevel::Info, "判题调度器已停止并回收 worker（耗时 " +
                                std::to_string(elapsed_ms) + " ms，已接收 " +
                                std::to_string(judge_manager_->accepted_count()) +
                                "，已完成 " +
                                std::to_string(judge_manager_->completed_count()) +
                                "）");
      }
    }
  }
  // 幂等：可重复调用。
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

  // 题目接口为公开接口：未携带 token 时按游客处理，携带时复用已有身份验证。
  svr_.Get("/api/problems", [this](const httplib::Request &req,
                                   httplib::Response &res) {
    handle_problem_list(req, res);
  });
  // 详情路径用正则匹配任意非空路径段，便于对非法 ID 返回明确的 400（而非 404）。
  // [^/]+ 不匹配斜杠，因此不会吞掉后续 /api/problems/{id}/submit 等子路径。
  svr_.Get(R"(/api/problems/([^/]+))", [this](const httplib::Request &req,
                                              httplib::Response &res) {
    handle_problem_detail(req, res);
  });
  // 提交接口（M1.6，需登录）。与详情路由方法不同，互不干扰；ID 段同样用
  // [^/]+ 以对非法 ID 返回明确的 400。
  svr_.Post(R"(/api/problems/([^/]+)/submit)",
            [this](const httplib::Request &req, httplib::Response &res) {
              handle_submit(req, res);
            });

  // 管理员题目管理接口（M2.1）：建题 / 改题 / 删题。三者统一走
  // require_admin（登录 + 已完成首次改密 + 当前数据库角色为 admin）。
  // 隐藏用例的增删改属 M2.2，不在这些接口范围内。
  svr_.Post("/api/admin/problems", [this](const httplib::Request &req,
                                          httplib::Response &res) {
    handle_admin_create_problem(req, res);
  });
  svr_.Put(R"(/api/admin/problems/([^/]+))",
           [this](const httplib::Request &req, httplib::Response &res) {
             handle_admin_update_problem(req, res);
           });
  svr_.Delete(R"(/api/admin/problems/([^/]+))",
              [this](const httplib::Request &req, httplib::Response &res) {
                handle_admin_delete_problem(req, res);
              });

  // 管理员测试用例接口（M2.2）：读/增/改/删某题的隐藏测试用例。均统一走
  // require_admin；公开样例由 M2.1 的题目接口维护，这里只操作 is_sample=0。
  // 具体子路径使用完整匹配的正则，不会与上面的 /api/admin/problems/{id} 混淆。
  svr_.Get(R"(/api/admin/problems/([^/]+)/testcases)",
           [this](const httplib::Request &req, httplib::Response &res) {
             handle_admin_list_testcases(req, res);
           });
  svr_.Post(R"(/api/admin/problems/([^/]+)/testcases)",
            [this](const httplib::Request &req, httplib::Response &res) {
              handle_admin_create_testcase(req, res);
            });
  svr_.Put(R"(/api/admin/problems/([^/]+)/testcases/([^/]+))",
           [this](const httplib::Request &req, httplib::Response &res) {
             handle_admin_update_testcase(req, res);
           });
  svr_.Delete(R"(/api/admin/problems/([^/]+)/testcases/([^/]+))",
              [this](const httplib::Request &req, httplib::Response &res) {
                handle_admin_delete_testcase(req, res);
              });

  // 管理员用户接口（M2.4）：用户列表 / 重置密码 / 改角色。统一走 require_admin
  // （登录 + 已完成首次改密 + 当前数据库角色为 admin），角色按数据库最新值判定。
  svr_.Get("/api/admin/users", [this](const httplib::Request &req,
                                      httplib::Response &res) {
    handle_admin_list_users(req, res);
  });
  svr_.Put("/api/admin/users", [this](const httplib::Request &req,
                                      httplib::Response &res) {
    handle_admin_update_user(req, res);
  });

  // 管理员重判接口（M3.6）：使用原提交源码/语言与当前题目配置重新判题，
  // 更新原记录并联动重算用户题目状态；不新增提交记录、不增加次数。
  svr_.Post(R"(/api/admin/submissions/([^/]+)/rejudge)",
            [this](const httplib::Request &req, httplib::Response &res) {
              handle_admin_rejudge(req, res);
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

  // 静态资源最后挂载：先注册 /api 路由，确保 API 与健康检查始终优先可用。
  mount_static();
}

void HttpServer::mount_static() {
  if (web_root_.empty()) {
    return;
  }

  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path root(web_root_);

  // 防御性校验：拒绝把项目根目录 / 系统根目录作为静态根，避免整个工程被下载。
  // 只允许挂载一个明确命名的、独立的目录（默认 web/）。
  const fs::path normalized = root.lexically_normal();
  if (normalized == "." || normalized == "/" || normalized.empty() ||
      !fs::is_directory(normalized, ec)) {
    log(LogLevel::Warn, "静态资源目录无效，已跳过静态托管: " + web_root_);
    return;
  }

  if (!svr_.set_mount_point("/", normalized.string())) {
    log(LogLevel::Warn, "静态资源挂载失败: " + web_root_);
    return;
  }
  log(LogLevel::Info, "静态资源已挂载: " + web_root_ + " -> /");
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

bool HttpServer::resolve_viewer(const httplib::Request &req,
                                httplib::Response &res,
                                ProblemViewer &viewer) {
  viewer = ProblemViewer{};
  const std::string authorization = req.get_header_value("Authorization");
  if (authorization.empty()) {
    return true; // 游客
  }

  std::string token;
  if (!auth::extract_bearer_token(authorization, token)) {
    send_error(res, 401, "未提供有效的认证信息");
    return false;
  }

  auth::AuthUser user;
  std::string err;
  switch (auth::authenticate_request(jwt_, user_store_, token, user, err)) {
    case auth::AuthResult::Ok:
      break;
    case auth::AuthResult::Unauthorized:
      // 验证失败的 token 绝不当作管理员（或普通用户）身份，沿用 401 约定。
      send_error(res, 401, "认证失败");
      return false;
    case auth::AuthResult::InternalError:
      send_error(res, 500, "内部错误");
      return false;
  }

  viewer.authenticated = true;
  viewer.user_id = user.id;
  // 只有通过 M1.3 管理员检查（已登录 + 已完成首次改密 + admin 角色）才可查看隐藏题。
  viewer.is_admin = auth::check_admin(user) == auth::AdminCheck::Ok;
  return true;
}

bool HttpServer::require_admin(const httplib::Request &req,
                               httplib::Response &res, auth::AuthUser &user) {
  std::string token;
  if (!auth::extract_bearer_token(req.get_header_value("Authorization"), token)) {
    send_error(res, 401, "未提供有效的认证信息");
    return false;
  }

  std::string err;
  switch (auth::authenticate_request(jwt_, user_store_, token, user, err)) {
    case auth::AuthResult::Ok:
      break;
    case auth::AuthResult::Unauthorized:
      send_error(res, 401, "认证失败");
      return false;
    case auth::AuthResult::InternalError:
      send_error(res, 500, "内部错误");
      return false;
  }

  // 角色与首次改密标记均取自数据库最新值（authenticate_request 已回查），
  // 不信任客户端提交的角色字段，也不依赖 JWT 中可能过时的角色。
  return enforce_admin(user, res);
}

void HttpServer::handle_problem_list(const httplib::Request &req,
                                     httplib::Response &res) {
  ProblemViewer viewer;
  if (!resolve_viewer(req, res, viewer)) {
    return;
  }

  // 解析并校验查询参数。空参数表示不限；非法参数按项目约定返回 400。
  problem::RawListParams raw;
  raw.q = req.has_param("q") ? req.get_param_value("q") : "";
  raw.difficulty = req.has_param("difficulty")
                       ? req.get_param_value("difficulty")
                       : "";
  raw.tag = req.has_param("tag") ? req.get_param_value("tag") : "";
  raw.page = req.has_param("page") ? req.get_param_value("page") : "";
  raw.visible =
      req.has_param("visible") ? req.get_param_value("visible") : "";

  problem::ListFilter filter;
  std::string param_error;
  if (!problem::parse_list_query(raw, filter, param_error)) {
    send_error(res, 400, param_error);
    return;
  }

  ProblemListQuery query;
  query.keyword = filter.keyword;
  query.difficulty = filter.difficulty;
  query.tag = filter.tag;
  query.page = filter.page;
  query.page_size = filter.page_size;
  // 本人状态只取自已验证的当前用户上下文；游客传 0（不匹配任何用户记录）。
  query.viewer_user_id = viewer.authenticated ? viewer.user_id : 0;

  // 可见性：非管理员始终只能看可见题目，即使显式传入 visible=0 也不得扩大范围；
  // 管理员可按 visible 参数筛选「全部 / 公开 / 隐藏」。
  if (!viewer.is_admin) {
    query.visibility = ProblemVisibility::VisibleOnly;
  } else {
    switch (filter.visible) {
      case problem::VisibleFilter::OnlyVisible:
        query.visibility = ProblemVisibility::VisibleOnly;
        break;
      case problem::VisibleFilter::OnlyHidden:
        query.visibility = ProblemVisibility::HiddenOnly;
        break;
      case problem::VisibleFilter::All:
        query.visibility = ProblemVisibility::All;
        break;
    }
  }

  ProblemListResult result;
  std::string err;
  if (!problem_store_.query(query, result, err)) {
    log(LogLevel::Error, "题目列表查询失败: " + err);
    send_error(res, 500, "内部错误");
    return;
  }

  json list = json::array();
  for (const ProblemSummary &problem : result.items) {
    list.push_back(problem_summary_json(problem, viewer.authenticated));
  }
  const long long total_pages =
      result.total == 0
          ? 0
          : (result.total + filter.page_size - 1) / filter.page_size;
  json body;
  body["problems"] = std::move(list);
  body["page"] = filter.page;
  body["page_size"] = filter.page_size;
  body["total"] = result.total;
  body["total_pages"] = total_pages;
  send_json(res, 200, body);
}

void HttpServer::handle_problem_detail(const httplib::Request &req,
                                       httplib::Response &res) {
  std::int64_t id = 0;
  if (req.matches.size() < 2 || !parse_problem_id(req.matches[1].str(), id)) {
    send_error(res, 400, "非法题目 ID");
    return;
  }

  ProblemViewer viewer;
  if (!resolve_viewer(req, res, viewer)) {
    return;
  }

  bool found = false;
  ProblemRecord problem;
  std::string err;
  if (!problem_store_.find_by_id(id, found, problem, err)) {
    log(LogLevel::Error, "题目详情查询失败: " + err);
    send_error(res, 500, "内部错误");
    return;
  }
  // 不存在的题目与当前用户无权查看的隐藏题目统一返回 404，避免通过状态码差异
  // 探测隐藏题目是否存在，也避免通过直接请求 ID 绕过可见性限制。
  if (!found || (!problem.visible && !viewer.is_admin)) {
    send_error(res, 404, "题目不存在");
    return;
  }

  std::vector<SampleCase> samples;
  if (!problem_store_.list_samples(id, samples, err)) {
    log(LogLevel::Error, "题目样例查询失败: " + err);
    send_error(res, 500, "内部错误");
    return;
  }

  send_json(res, 200, problem_detail_json(problem, samples));
}

void HttpServer::handle_submit(const httplib::Request &req,
                               httplib::Response &res) {
  // 1. 校验题目 ID（非法 ID 属客户端输入错误）。
  std::int64_t problem_id = 0;
  if (req.matches.size() < 2 ||
      !parse_problem_id(req.matches[1].str(), problem_id)) {
    send_error(res, 400, "非法题目 ID");
    return;
  }

  // 2. 登录校验：提交必须来自已验证的当前用户，用户归属不由客户端指定。
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

  // 3. 首次强制改密限制：尚未完成必要改密的用户不能提交。
  if (auth::requires_password_change(user)) {
    send_password_change_required(res);
    return;
  }

  // 4. 请求体与参数校验。只读取 language 与 code；客户端传入的 user_id / id /
  //    status / testcases 等字段一律忽略，无法改变提交归属或判题结果。
  json parsed;
  try {
    parsed = json::parse(req.body);
  } catch (const std::exception &) {
    send_error(res, 400, "请求体不是合法的 JSON");
    return;
  }
  if (!parsed.is_object()) {
    send_error(res, 400, "请求体必须是 JSON 对象");
    return;
  }
  if (!parsed.contains("language") || !parsed["language"].is_string()) {
    send_error(res, 400, "缺少字段或类型错误：language 须为字符串");
    return;
  }
  if (!parsed.contains("code") || !parsed["code"].is_string()) {
    send_error(res, 400, "缺少字段或类型错误：code 须为字符串");
    return;
  }

  std::string canonical_language;
  if (!submit::parse_submission_language(parsed["language"].get<std::string>(),
                                         canonical_language)) {
    send_error(res, 400, "不支持的语言：仅支持 cpp17（C++17）与 c11（C11）");
    return;
  }
  const std::string source_code = parsed["code"].get<std::string>();
  std::string code_error;
  if (!submit::validate_source_code(source_code, code_error)) {
    send_error(res, 400, code_error);
    return;
  }

  // 5. 可见性：管理员（已登录 + 已完成首次改密 + admin 角色）可向隐藏题提交，
  //    其余用户按 M1.4 规则仅可向可见题提交。此检查在写库前完成，未通过时绝不
  //    产生可执行的在途任务。
  const bool is_admin = auth::check_admin(user) == auth::AdminCheck::Ok;

  bool problem_found = false;
  ProblemRecord problem;
  if (!problem_store_.find_by_id(problem_id, problem_found, problem, err)) {
    log(LogLevel::Error, "提交：题目查询失败: " + err);
    send_error(res, 500, "内部错误");
    return;
  }
  if (!problem_found || (!problem.visible && !is_admin)) {
    send_error(res, 404, "题目不存在");
    return;
  }

  // 6. 接收边界，顺序为「容量预留 → 写库 → 入队」，保证一致：
  //    - 容量检查失败不写库、不产生任务；
  //    - 写库失败释放预留，明确返回错误，不声称已接收；
  //    - 写库成功后才入队，崩溃时任务仍在库中可恢复。
  const std::string submitted_at = submit::utc_timestamp_now();
  judge::JudgeManager::Reservation reservation = judge_manager_->reserve();
  if (reservation.status == judge::JudgeManager::EnqueueStatus::QueueFull) {
    send_judge_queue_full(res);
    return;
  }
  if (reservation.status == judge::JudgeManager::EnqueueStatus::Stopped) {
    send_judge_unavailable(res);
    return;
  }

  std::string in_flight_task_id;
  std::string persist_error;
  submit::SubmitService::PersistKind persisted = submit_service_->persist_in_flight(
      user.id, problem_id, canonical_language, source_code, submitted_at,
      in_flight_task_id, persist_error);
  if (persisted != submit::SubmitService::PersistKind::Ok) {
    judge_manager_->release(reservation);
    if (persisted == submit::SubmitService::PersistKind::ProblemNotFound) {
      send_error(res, 404, "题目不存在");
      return;
    }
    log(LogLevel::Error, "提交：持久化在途任务失败（用户 " +
                             std::to_string(user.id) + "，题目 " +
                             std::to_string(problem_id) + "）： " +
                             persist_error);
    send_error(res, 500, "内部错误");
    return;
  }

  // 7. 构造自带完整数据的调度任务并提交预留槽位。原始提交时间在接收时采集，
  //    排队等待不计入用户程序耗时；任务携带在途标识，结算时删除对应记录。
  judge::SubmissionTask task;
  task.user_id = user.id;
  task.problem_id = problem_id;
  task.language = canonical_language;
  task.source_code = source_code;
  task.viewer_is_admin = is_admin;
  task.submitted_at = submitted_at;
  task.in_flight_task_id = in_flight_task_id;

  judge::JudgeManager::SubmitResult enqueued =
      judge_manager_->commit(std::move(task), reservation);
  if (enqueued.status != judge::JudgeManager::EnqueueStatus::Accepted) {
    // 预留有效时 commit 必成功；此处仅作防御，若失败则放弃在途记录避免误恢复。
    log(LogLevel::Error, "提交：入队失败（在途任务 " + in_flight_task_id + "）");
    submit_service_->discard_in_flight(in_flight_task_id);
    send_judge_unavailable(res);
    return;
  }

  // 8. 同步等待该提交的判题与持久化结果（结果通道与本次请求一一对应）。
  submit::SubmitService::Outcome outcome = enqueued.future.get();
  switch (outcome.kind) {
    case submit::SubmitService::Kind::ProblemNotFound:
      send_error(res, 404, "题目不存在");
      return;
    case submit::SubmitService::Kind::AlreadySettled:
      log(LogLevel::Error, "提交：任务 " + in_flight_task_id +
                               " 已被结算，拒绝重复结算");
      send_error(res, 500, "内部错误");
      return;
    case submit::SubmitService::Kind::InternalError:
      log(LogLevel::Error, "提交失败（用户 " + std::to_string(user.id) +
                               "，题目 " + std::to_string(problem_id) +
                               "）： " + outcome.error);
      send_error(res, 500, "内部错误");
      return;
    case submit::SubmitService::Kind::Ok:
      break;
  }

  const SubmissionRecord &saved = outcome.submission;
  // 运行日志只记录必要元信息，不写完整源码、token 或隐藏用例内容。
  log(LogLevel::Info, "提交 #" + std::to_string(saved.id) + "（用户 " +
                          std::to_string(saved.user_id) + "，题目 " +
                          std::to_string(saved.problem_id) + "，语言 " +
                          saved.language + "）：" + saved.status);

  json body = submission_result_json(saved, outcome.judge);
  if (body.is_null()) {
    // submission_result_json 已记录日志并设置 500 响应。
    return;
  }
  send_json(res, 200, body);
}

json HttpServer::submission_result_json(
    const SubmissionRecord &record, const judge::JudgeResult &judge) {
  json body;
  body["id"] = record.id;
  body["problem_id"] = record.problem_id;
  body["language"] = record.language;
  body["status"] = record.status;
  body["passed"] = judge.passed;
  body["total"] = judge.total;
  body["runtime_ms"] = record.runtime_ms;
  // 采集到峰值 RSS 时返回数值；未采集到（如编译失败或采样失败）返回 null，
  // 明确区分「未采集」与真实的 0。单位 kB。
  body["memory_kb"] =
      record.memory_kb > 0 ? json(record.memory_kb) : json(nullptr);
  body["compile_time_ms"] = judge.compile_time_ms;
  body["compile_ok"] = judge.compile_ok;
  body["compile_output"] = record.compile_msg;
  body["compile_output_truncated"] = judge.compile_output_truncated;
  body["message"] = judge.message;
  body["created_at"] = record.created_at;
  try {
    body["results"] = json::parse(record.per_case);
  } catch (const std::exception &) {
    // 正常路径不会发生；作为内部故障处理，不返回半成品结果。
    log(LogLevel::Error, "提交记录 #" + std::to_string(record.id) +
                             " 的逐点结果 JSON 解析失败");
    return json();
  }
  return body;
}

void HttpServer::handle_admin_create_problem(const httplib::Request &req,
                                             httplib::Response &res) {
  auth::AuthUser admin;
  if (!require_admin(req, res, admin)) {
    return;
  }

  json body;
  try {
    body = json::parse(req.body);
  } catch (const std::exception &) {
    send_error(res, 400, "请求体不是合法的 JSON");
    return;
  }

  // 只读取题目可写字段；id/created_at/updated_at/seed_key 等由服务端管理，
  // 客户端传入一律忽略。
  problem::ProblemData data;
  std::string validation_error;
  if (!problem::parse_create_problem(body, data, validation_error)) {
    send_error(res, 400, validation_error);
    return;
  }

  std::int64_t new_id = 0;
  std::string err;
  if (!problem_admin_store_.create(data, new_id, err)) {
    log(LogLevel::Error, "创建题目失败: " + err);
    send_error(res, 500, "内部错误");
    return;
  }

  log(LogLevel::Info, "管理员 " + std::to_string(admin.id) + " 创建题目 #" +
                          std::to_string(new_id));
  json resp;
  resp["id"] = new_id;
  send_json(res, 201, resp);
}

void HttpServer::handle_admin_update_problem(const httplib::Request &req,
                                             httplib::Response &res) {
  auth::AuthUser admin;
  if (!require_admin(req, res, admin)) {
    return;
  }

  std::int64_t id = 0;
  if (req.matches.size() < 2 || !parse_problem_id(req.matches[1].str(), id)) {
    send_error(res, 400, "非法题目 ID");
    return;
  }

  json body;
  try {
    body = json::parse(req.body);
  } catch (const std::exception &) {
    send_error(res, 400, "请求体不是合法的 JSON");
    return;
  }

  // 部分更新语义：仅更新请求体中出现的字段，未出现的字段保持原值，
  // 避免遗漏字段被意外清空。
  problem::ProblemPatch patch;
  std::string validation_error;
  if (!problem::parse_update_problem(body, patch, validation_error)) {
    send_error(res, 400, validation_error);
    return;
  }

  std::string err;
  switch (problem_admin_store_.update(id, patch, err)) {
    case ProblemAdminStore::UpdateStatus::Updated: {
      json resp;
      resp["id"] = id;
      resp["status"] = "ok";
      send_json(res, 200, resp);
      return;
    }
    case ProblemAdminStore::UpdateStatus::NotFound:
      send_error(res, 404, "题目不存在");
      return;
    case ProblemAdminStore::UpdateStatus::Error:
      log(LogLevel::Error, "修改题目失败: " + err);
      send_error(res, 500, "内部错误");
      return;
  }
}

void HttpServer::handle_admin_delete_problem(const httplib::Request &req,
                                             httplib::Response &res) {
  auth::AuthUser admin;
  if (!require_admin(req, res, admin)) {
    return;
  }

  std::int64_t id = 0;
  if (req.matches.size() < 2 || !parse_problem_id(req.matches[1].str(), id)) {
    send_error(res, 400, "非法题目 ID");
    return;
  }

  std::string err;
  switch (problem_admin_store_.remove(id, err)) {
    case ProblemAdminStore::DeleteStatus::Deleted: {
      log(LogLevel::Info, "管理员 " + std::to_string(admin.id) + " 删除题目 #" +
                              std::to_string(id));
      json resp;
      resp["status"] = "ok";
      send_json(res, 200, resp);
      return;
    }
    case ProblemAdminStore::DeleteStatus::NotFound:
      send_error(res, 404, "题目不存在");
      return;
    case ProblemAdminStore::DeleteStatus::HasSubmissions:
      send_error(res, 409, "题目已有提交记录或正在判题的任务，无法删除");
      return;
    case ProblemAdminStore::DeleteStatus::Error:
      log(LogLevel::Error, "删除题目失败: " + err);
      send_error(res, 500, "内部错误");
      return;
  }
}

void HttpServer::handle_admin_list_testcases(const httplib::Request &req,
                                             httplib::Response &res) {
  auth::AuthUser admin;
  if (!require_admin(req, res, admin)) {
    return;
  }

  std::int64_t problem_id = 0;
  if (req.matches.size() < 2 ||
      !parse_problem_id(req.matches[1].str(), problem_id)) {
    send_error(res, 400, "非法题目 ID");
    return;
  }

  bool found = false;
  ProblemRecord problem;
  std::string err;
  if (!problem_store_.find_by_id(problem_id, found, problem, err)) {
    log(LogLevel::Error, "管理员用例读取：题目查询失败: " + err);
    send_error(res, 500, "内部错误");
    return;
  }
  if (!found) {
    send_error(res, 404, "题目不存在");
    return;
  }

  std::vector<TestcaseRecord> records;
  if (!problem_store_.list_testcases(problem_id, records, err)) {
    log(LogLevel::Error, "管理员用例读取失败: " + err);
    send_error(res, 500, "内部错误");
    return;
  }

  const std::size_t total = records.size();
  json list = json::array();
  for (const TestcaseRecord &tc : records) {
    list.push_back(admin_testcase_json(tc, problem_id));
  }
  json body;
  body["problem_id"] = problem_id;
  body["testcases"] = std::move(list);
  body["total"] = total;
  send_json(res, 200, body);
}

void HttpServer::handle_admin_create_testcase(const httplib::Request &req,
                                              httplib::Response &res) {
  auth::AuthUser admin;
  if (!require_admin(req, res, admin)) {
    return;
  }

  std::int64_t problem_id = 0;
  if (req.matches.size() < 2 ||
      !parse_problem_id(req.matches[1].str(), problem_id)) {
    send_error(res, 400, "非法题目 ID");
    return;
  }

  json body;
  try {
    body = json::parse(req.body);
  } catch (const std::exception &) {
    send_error(res, 400, "请求体不是合法的 JSON");
    return;
  }

  // 只读取 input/output/ord；id/problem_id/is_sample 等由服务端管理，客户端传入
  // 一律忽略，用例归属只由 URL 中的题目 ID 决定。
  problem::TestcaseData data;
  std::string validation_error;
  if (!problem::parse_create_testcase(body, data, validation_error)) {
    send_error(res, 400, validation_error);
    return;
  }

  std::int64_t new_id = 0;
  int ord = 0;
  std::string err;
  switch (testcase_admin_store_.create(problem_id, data, new_id, ord, err)) {
    case TestcaseAdminStore::CreateStatus::Created: {
      log(LogLevel::Info, "管理员 " + std::to_string(admin.id) +
                              " 为题目 #" + std::to_string(problem_id) +
                              " 新增用例 #" + std::to_string(new_id));
      json resp;
      resp["id"] = new_id;
      resp["problem_id"] = problem_id;
      resp["ord"] = ord;
      send_json(res, 201, resp);
      return;
    }
    case TestcaseAdminStore::CreateStatus::ProblemNotFound:
      send_error(res, 404, "题目不存在");
      return;
    case TestcaseAdminStore::CreateStatus::OrdExhausted:
      send_error(res, 409, "自动分配 ord 已达上限，请显式指定 ord");
      return;
    case TestcaseAdminStore::CreateStatus::Error:
      log(LogLevel::Error, "新增用例失败: " + err);
      send_error(res, 500, "内部错误");
      return;
  }
}

void HttpServer::handle_admin_update_testcase(const httplib::Request &req,
                                              httplib::Response &res) {
  auth::AuthUser admin;
  if (!require_admin(req, res, admin)) {
    return;
  }

  std::int64_t problem_id = 0;
  std::int64_t testcase_id = 0;
  if (req.matches.size() < 3 ||
      !parse_problem_id(req.matches[1].str(), problem_id)) {
    send_error(res, 400, "非法题目 ID");
    return;
  }
  if (!parse_problem_id(req.matches[2].str(), testcase_id)) {
    send_error(res, 400, "非法用例 ID");
    return;
  }

  json body;
  try {
    body = json::parse(req.body);
  } catch (const std::exception &) {
    send_error(res, 400, "请求体不是合法的 JSON");
    return;
  }

  // 部分更新语义：仅更新请求体中出现的字段，未出现的字段保持原值；显式空串可清空。
  problem::TestcasePatch patch;
  std::string validation_error;
  if (!problem::parse_update_testcase(body, patch, validation_error)) {
    send_error(res, 400, validation_error);
    return;
  }

  int ord = 0;
  std::string err;
  switch (testcase_admin_store_.update(problem_id, testcase_id, patch, ord,
                                       err)) {
    case TestcaseAdminStore::UpdateStatus::Updated: {
      json resp;
      resp["id"] = testcase_id;
      resp["problem_id"] = problem_id;
      resp["ord"] = ord;
      resp["status"] = "ok";
      send_json(res, 200, resp);
      return;
    }
    case TestcaseAdminStore::UpdateStatus::NotFound:
      send_error(res, 404, "用例不存在");
      return;
    case TestcaseAdminStore::UpdateStatus::Error:
      log(LogLevel::Error, "修改用例失败: " + err);
      send_error(res, 500, "内部错误");
      return;
  }
}

void HttpServer::handle_admin_delete_testcase(const httplib::Request &req,
                                              httplib::Response &res) {
  auth::AuthUser admin;
  if (!require_admin(req, res, admin)) {
    return;
  }

  std::int64_t problem_id = 0;
  std::int64_t testcase_id = 0;
  if (req.matches.size() < 3 ||
      !parse_problem_id(req.matches[1].str(), problem_id)) {
    send_error(res, 400, "非法题目 ID");
    return;
  }
  if (!parse_problem_id(req.matches[2].str(), testcase_id)) {
    send_error(res, 400, "非法用例 ID");
    return;
  }

  std::string err;
  switch (testcase_admin_store_.remove(problem_id, testcase_id, err)) {
    case TestcaseAdminStore::DeleteStatus::Deleted: {
      log(LogLevel::Info, "管理员 " + std::to_string(admin.id) +
                              " 删除题目 #" + std::to_string(problem_id) +
                              " 的用例 #" + std::to_string(testcase_id));
      json resp;
      resp["status"] = "ok";
      send_json(res, 200, resp);
      return;
    }
    case TestcaseAdminStore::DeleteStatus::NotFound:
      send_error(res, 404, "用例不存在");
      return;
    case TestcaseAdminStore::DeleteStatus::Error:
      log(LogLevel::Error, "删除用例失败: " + err);
      send_error(res, 500, "内部错误");
      return;
  }
}

void HttpServer::handle_admin_list_users(const httplib::Request &req,
                                         httplib::Response &res) {
  auth::AuthUser admin;
  if (!require_admin(req, res, admin)) {
    return;
  }

  // 分页沿用 GET /api/problems 的既有约定（page 正整数、每页固定 20）。
  const std::string page_text =
      req.has_param("page") ? req.get_param_value("page") : "";
  int page = 1;
  std::string param_error;
  if (!useradmin::parse_page(page_text, page, param_error)) {
    send_error(res, 400, param_error);
    return;
  }

  std::vector<UserSummary> users;
  long long total = 0;
  std::string err;
  if (!user_admin_store_.list_users(page, useradmin::kUserPageSize, users, total,
                                    err)) {
    log(LogLevel::Error, "管理员用户列表查询失败: " + err);
    send_error(res, 500, "内部错误");
    return;
  }

  json list = json::array();
  for (const UserSummary &user : users) {
    list.push_back(admin_user_summary_json(user));
  }
  const long long total_pages =
      total == 0 ? 0
                 : (total + useradmin::kUserPageSize - 1) /
                       useradmin::kUserPageSize;
  json body;
  body["users"] = std::move(list);
  body["page"] = page;
  body["page_size"] = useradmin::kUserPageSize;
  body["total"] = total;
  body["total_pages"] = total_pages;
  send_json(res, 200, body);
}

void HttpServer::handle_admin_update_user(const httplib::Request &req,
                                          httplib::Response &res) {
  auth::AuthUser admin;
  if (!require_admin(req, res, admin)) {
    return;
  }

  json body;
  try {
    body = json::parse(req.body);
  } catch (const std::exception &) {
    send_error(res, 400, "请求体不是合法的 JSON");
    return;
  }

  // 只读取 action / user_id / new_password / role；account、nickname、
  // reset_pwd_flag、password_hash 等字段一律忽略，不同操作只改对应字段。
  useradmin::Request action;
  std::string validation_error;
  if (!useradmin::parse_request(body, action, validation_error)) {
    send_error(res, 400, validation_error);
    return;
  }

  if (action.action == useradmin::Action::ResetPassword) {
    // 新密码哈希在数据库事务外完成（argon2id 较耗时，不应长时间持有写锁）。
    std::string new_hash;
    std::string hash_error;
    if (!auth::hash_password(action.new_password, new_hash, hash_error)) {
      log(LogLevel::Error, "管理员重置密码：哈希失败: " + hash_error);
      send_error(res, 500, "内部错误");
      return;
    }

    std::string err;
    switch (user_admin_store_.reset_password(action.user_id, new_hash, err)) {
      case UserAdminStore::ResetStatus::Updated: {
        // 日志只记录操作者、目标与结果，不记录密码/哈希。
        log(LogLevel::Info, "管理员 " + std::to_string(admin.id) +
                                " 重置用户 " + std::to_string(action.user_id) +
                                " 的密码：成功");
        json resp;
        resp["user_id"] = action.user_id;
        resp["status"] = "ok";
        send_json(res, 200, resp);
        return;
      }
      case UserAdminStore::ResetStatus::NotFound:
        send_error(res, 404, "用户不存在");
        return;
      case UserAdminStore::ResetStatus::Error:
        log(LogLevel::Error, "管理员重置密码失败: " + err);
        send_error(res, 500, "内部错误");
        return;
    }
  }

  // ChangeRole
  std::string err;
  switch (user_admin_store_.change_role(action.user_id, action.role, err)) {
    case UserAdminStore::RoleStatus::Updated: {
      log(LogLevel::Info, "管理员 " + std::to_string(admin.id) + " 将用户 " +
                              std::to_string(action.user_id) + " 角色改为 " +
                              action.role + "：成功");
      json resp;
      resp["user_id"] = action.user_id;
      resp["role"] = action.role;
      resp["status"] = "ok";
      send_json(res, 200, resp);
      return;
    }
    case UserAdminStore::RoleStatus::NotFound:
      send_error(res, 404, "用户不存在");
      return;
    case UserAdminStore::RoleStatus::LastAdmin:
      send_error(res, 409, "不能取消最后一个管理员的权限");
      return;
    case UserAdminStore::RoleStatus::Error:
      log(LogLevel::Error, "管理员修改角色失败: " + err);
      send_error(res, 500, "内部错误");
      return;
  }
}

void HttpServer::handle_admin_rejudge(const httplib::Request &req,
                                      httplib::Response &res) {
  auth::AuthUser admin;
  if (!require_admin(req, res, admin)) {
    return;
  }

  // 1. 校验提交 ID。
  std::int64_t submission_id = 0;
  if (req.matches.size() < 2 ||
      !parse_problem_id(req.matches[1].str(), submission_id)) {
    send_error(res, 400, "非法提交 ID");
    return;
  }

  // 2. 读取原提交记录，用于构造调度任务（源码/语言/题目归属均来自数据库）。
  SubmissionStore submission_store(db_);
  bool found = false;
  SubmissionRecord record;
  std::string err;
  if (!submission_store.find_by_id(submission_id, found, record, err)) {
    log(LogLevel::Error, "管理员重判：读取提交失败 #" +
                              std::to_string(submission_id) + ": " + err);
    send_error(res, 500, "内部错误");
    return;
  }
  if (!found) {
    send_error(res, 404, "提交记录不存在");
    return;
  }

  // 3. 并发去重：同一提交 ID 同时只能有一个待执行或正在执行的重判。
  std::unique_ptr<RejudgeGuard> guard;
  {
    std::lock_guard<std::mutex> lock(rejudge_mutex_);
    if (rejudge_in_flight_.count(submission_id) != 0) {
      send_rejudge_in_progress(res);
      return;
    }
    rejudge_in_flight_.insert(submission_id);
    guard = std::make_unique<RejudgeGuard>(rejudge_mutex_, rejudge_in_flight_,
                                           submission_id);
  }

  // 4. 构造自带完整数据的调度任务，交给 JudgeManager（与普通提交共用队列/worker）。
  judge::SubmissionTask task;
  task.user_id = record.user_id;
  task.problem_id = record.problem_id;
  task.language = record.language;
  task.source_code = record.source_code;
  task.viewer_is_admin = true; // 管理员重判不受题目可见性限制
  task.submitted_at = submit::utc_timestamp_now();
  task.rejudge_submission_id = submission_id;

  judge::JudgeManager::SubmitResult enqueued = judge_manager_->submit(std::move(task));
  if (enqueued.status == judge::JudgeManager::EnqueueStatus::QueueFull) {
    send_judge_queue_full(res);
    return;
  }
  if (enqueued.status == judge::JudgeManager::EnqueueStatus::Stopped) {
    send_judge_unavailable(res);
    return;
  }

  // 5. 同步等待判题与持久化结果。
  submit::SubmitService::Outcome outcome;
  try {
    outcome = enqueued.future.get();
  } catch (const std::exception &e) {
    log(LogLevel::Error, "管理员重判：结果通道异常 #" +
                              std::to_string(submission_id) + ": " + e.what());
    send_error(res, 500, "内部错误");
    return;
  }

  switch (outcome.kind) {
    case submit::SubmitService::Kind::ProblemNotFound:
      send_error(res, 404, outcome.error.empty() ? "提交记录不存在" : outcome.error);
      return;
    case submit::SubmitService::Kind::InternalError:
      log(LogLevel::Error, "管理员 " + std::to_string(admin.id) +
                               " 重判提交 #" + std::to_string(submission_id) +
                               " 失败：" + outcome.error);
      send_error(res, 500, outcome.error.empty() ? "内部错误" : outcome.error);
      return;
    case submit::SubmitService::Kind::Ok:
      break;
  }

  // 6. 返回更新后的提交结果。
  const SubmissionRecord &saved = outcome.submission;
  log(LogLevel::Info, "管理员 " + std::to_string(admin.id) + " 重判提交 #" +
                          std::to_string(saved.id) + "（用户 " +
                          std::to_string(saved.user_id) + "，题目 " +
                          std::to_string(saved.problem_id) + "，语言 " +
                          saved.language + "）：" + saved.status);

  json body = submission_result_json(saved, outcome.judge);
  if (body.is_null()) {
    send_error(res, 500, "内部错误");
    return;
  }
  send_json(res, 200, body);
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
