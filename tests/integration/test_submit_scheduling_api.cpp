// M3.1 判题任务调度 HTTP 集成测试 + 真实并发判题测试。
//
// 使用隔离临时库、随机端口、受控执行器与独立判题目录，不触碰正式数据。
// 区分三类：
//   - 调度集成测试（可控执行器 + 同步门）：队列满载 503、未接收不落库、结果归属、
//     异常隔离、排队时间不计入运行耗时/不误判 TLE、繁忙时健康检查与题目查询可响应、
//     停止时排空已接收任务；
//   - 持久化并发测试（直接调用 SubmitService + 显式原提交时间）：同一用户同题目并发
//     计数正确、首次 AC 取最早原提交时间；
//   - 真实判题测试（LocalExecutor + 真实 g++/gcc）：C++17/C11 并发结果正确、子进程回收。
//
// 运行方式：ctest --test-dir build -R submit_scheduling_api --output-on-failure
// 或直接执行 build/oj_submit_scheduling_api_test。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "auth/jwt.h"
#include "db/database.h"
#include "db/schema.h"
#include "db/submissions.h"
#include "db/users.h"
#include "http/server.h"
#include "judge/executor.h"
#include "judge/manager.h"
#include "submit/submit.h"

namespace {

using nlohmann::json;
using oj::judge::JudgeManager;

int g_failures = 0;

void check(bool cond, const std::string &msg) {
  if (cond) {
    std::cout << "  [PASS] " << msg << "\n";
  } else {
    std::cout << "  [FAIL] " << msg << "\n";
    ++g_failures;
  }
}

const std::string kTestSecret = "it-sched-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";

oj::auth::JwtConfig make_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = kTestSecret;
  cfg.expires_seconds = 3600;
  return cfg;
}

// 受控执行器：可按条件阻塞编译（同步门），也可抛出异常，驱动调度边界测试。
class GatedExecutor : public oj::judge::IExecutor {
public:
  struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    int entered = 0;
    bool open = false;

    void enter() {
      std::unique_lock<std::mutex> lock(mutex);
      ++entered;
      cv.notify_all();
      cv.wait(lock, [this]() { return open; });
    }
    bool wait_entered(int count, std::chrono::milliseconds timeout) {
      std::unique_lock<std::mutex> lock(mutex);
      return cv.wait_for(lock, timeout, [&]() { return entered >= count; });
    }
    void release() {
      std::lock_guard<std::mutex> lock(mutex);
      open = true;
      cv.notify_all();
    }
  };

  Gate *gate = nullptr;
  bool block_compile = false;   // 编译前进入同步门
  std::string block_marker;     // 仅当源码包含该标记时才阻塞（空=不按标记）
  bool throw_on_compile = false;
  bool throw_on_run = false;
  std::string run_output = "2\n";
  int run_exit_code = 0;
  long long run_time_ms = 1;
  // true 时按源码中 "OUT:<值>" 标记决定该次判题的运行输出（以唯一产物路径为键），
  // 用于验证并发提交的临时目录/输出/结果互不混用。
  bool use_source_marker_output = false;

  oj::judge::ProcessResult compile(
      const oj::judge::CompileRequest &request) override {
    std::string source;
    if (gate != nullptr || use_source_marker_output) {
      std::ifstream in(request.source_path);
      std::stringstream buffer;
      buffer << in.rdbuf();
      source = buffer.str();
    }
    if (gate != nullptr) {
      bool should_block = block_compile;
      if (!block_marker.empty()) {
        should_block = source.find(block_marker) != std::string::npos;
      }
      if (should_block) {
        gate->enter();
      }
    }
    if (throw_on_compile) {
      throw std::runtime_error("fake compile crash");
    }
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    std::ofstream out(request.output_path, std::ios::binary);
    out << "fake-program";
    out.close();
    if (use_source_marker_output) {
      std::string value;
      const std::string marker = "OUT:";
      std::size_t pos = source.find(marker);
      if (pos != std::string::npos) {
        std::size_t begin = pos + marker.size();
        std::size_t end = source.find('\n', begin);
        value = source.substr(begin, end == std::string::npos
                                         ? std::string::npos
                                         : end - begin);
      }
      std::lock_guard<std::mutex> lock(outputs_mutex_);
      outputs_[request.output_path] = value;
    }
    return result;
  }

  oj::judge::ProcessResult run(const oj::judge::RunRequest &request,
                               const std::string &) override {
    if (throw_on_run) {
      throw std::runtime_error("fake run crash");
    }
    std::string output = run_output;
    if (use_source_marker_output) {
      std::lock_guard<std::mutex> lock(outputs_mutex_);
      auto it = outputs_.find(request.executable_path);
      if (it != outputs_.end()) {
        output = it->second;
      }
    }
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = run_exit_code;
    result.stdout_data = output;
    result.time_ms = run_time_ms;
    return result;
  }

private:
  std::mutex outputs_mutex_;
  std::map<std::string, std::string> outputs_;
};

class TempDir {
public:
  explicit TempDir(const std::string &label) {
    std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
      auto candidate = base / (label + "_" + std::to_string(::getpid()) + "_" +
                               std::to_string(i));
      std::error_code ec;
      std::filesystem::create_directories(candidate, ec);
      if (!ec) {
        path_ = candidate;
        return;
      }
    }
    path_.clear();
  }

  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }

  std::string db_path() const { return (path_ / "oj.db").string(); }
  std::string sub(const std::string &name) const {
    return (path_ / name).string();
  }

private:
  std::filesystem::path path_;
};

int find_free_port() {
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  addr.sin_port = 0;
  ::bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(sock, reinterpret_cast<sockaddr *>(&addr), &len);
  int port = ntohs(addr.sin_port);
  ::close(sock);
  return port;
}

httplib::Client make_client(int port) {
  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(10, 0);
  cli.set_read_timeout(60, 0);
  cli.set_write_timeout(60, 0);
  return cli;
}

// 隔离临时库 + 随机端口真实 HTTP + 可选注入执行器 + 可配置调度器选项。
class Env {
public:
  Env(const std::string &label, oj::judge::IExecutor *executor = nullptr,
      oj::judge::JudgeOptions judge_options = {},
      JudgeManager::Options manager_options = JudgeManager::Options())
      : dir_(label) {
    std::string err;
    db_ = oj::Database::open(dir_.db_path(), err);
    if (!db_) {
      return;
    }
    if (!oj::initialize_schema(*db_, kAdminPassword, err)) {
      return;
    }
    if (judge_options.workspace_root.empty()) {
      judge_options.workspace_root = dir_.sub("judge");
    }
    port_ = find_free_port();
    server_ = std::make_unique<oj::HttpServer>(
        "127.0.0.1", port_, *db_, make_config(),
        /*enable_test_routes=*/false, executor, judge_options,
        /*web_root=*/"", manager_options);
    if (!server_->start(err)) {
      return;
    }
    started_ = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  bool ok() const { return started_; }
  int port() const { return port_; }
  oj::Database &db() { return *db_; }
  oj::HttpServer *server() { return server_.get(); }

  void stop() {
    if (server_) {
      server_->stop();
    }
  }
  void close_db() {
    if (db_) {
      db_->close();
    }
  }

private:
  TempDir dir_;
  std::unique_ptr<oj::Database> db_;
  std::unique_ptr<oj::HttpServer> server_;
  int port_ = 0;
  bool started_ = false;
};

// ---------------------------------------------------------------------------
// 数据库辅助
// ---------------------------------------------------------------------------

std::int64_t count_rows(oj::Database &db, const std::string &table) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT COUNT(*) FROM " + table, stmt, err)) {
    return -1;
  }
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t count_status_rows(oj::Database &db, std::int64_t uid,
                               std::int64_t pid) {
  std::string err;
  oj::Statement stmt;
  db.prepare("SELECT COUNT(*) FROM user_problem_status WHERE user_id = ? AND "
             "problem_id = ?",
             stmt, err);
  stmt.bind(1, static_cast<sqlite3_int64>(uid));
  stmt.bind(2, static_cast<sqlite3_int64>(pid));
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t user_id(oj::Database &db, const std::string &account) {
  std::string err;
  oj::Statement stmt;
  db.prepare("SELECT id FROM users WHERE account = ?", stmt, err);
  stmt.bind(1, account);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t insert_problem(oj::Database &db, const std::string &title,
                            int visible = 1, int time_limit_ms = 2000) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("INSERT INTO problems (title, description, difficulty, tags, "
                  "visible, time_limit_ms) VALUES (?, '题面', 'easy', '测试', "
                  "?, ?) RETURNING id",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, title);
  stmt.bind(2, visible);
  stmt.bind(3, time_limit_ms);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

bool insert_testcase(oj::Database &db, std::int64_t problem_id, int ord,
                     const std::string &input, const std::string &output) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("INSERT INTO testcases (problem_id, ord, input, output, "
                  "is_sample) VALUES (?, ?, ?, ?, 0)",
                  stmt, err)) {
    return false;
  }
  stmt.bind(1, static_cast<sqlite3_int64>(problem_id));
  stmt.bind(2, ord);
  stmt.bind(3, input);
  stmt.bind(4, output);
  return stmt.step() == SQLITE_DONE;
}

bool read_status(oj::Database &db, std::int64_t uid, std::int64_t pid,
                 bool &found, oj::UserProblemStatusRecord &out) {
  oj::UserProblemStatusStore store(db);
  std::string err;
  return store.find(uid, pid, found, out, err);
}

// ---------------------------------------------------------------------------
// HTTP 辅助
// ---------------------------------------------------------------------------

std::string register_user(httplib::Client &cli, const std::string &nickname,
                          const std::string &password) {
  auto res = cli.Post("/api/register",
                      "{\"nickname\":\"" + nickname + "\",\"password\":\"" +
                          password + "\"}",
                      "application/json");
  if (!res || res->status != 201) {
    return "";
  }
  return json::parse(res->body).value("account", "");
}

std::string login(httplib::Client &cli, const std::string &account,
                  const std::string &password, int &status) {
  auto res = cli.Post("/api/login",
                      "{\"account\":\"" + account + "\",\"password\":\"" +
                          password + "\"}",
                      "application/json");
  status = res ? res->status : -1;
  if (!res || res->body.empty()) {
    return "";
  }
  return json::parse(res->body).value("token", "");
}

httplib::Result submit(httplib::Client &cli, const std::string &token,
                       const std::string &problem_id,
                       const std::string &language, const std::string &code) {
  json body;
  body["language"] = language;
  body["code"] = code;
  httplib::Headers headers{{"Authorization", "Bearer " + token}};
  const std::string path = "/api/problems/" + problem_id + "/submit";
  return cli.Post(path.c_str(), headers, body.dump(), "application/json");
}

struct User {
  std::string account;
  std::string token;
  std::int64_t id = 0;
};

User make_user(httplib::Client &cli, oj::Database &db, const std::string &nick,
               const std::string &pw) {
  User user;
  user.account = register_user(cli, nick, pw);
  int status = 0;
  user.token = login(cli, user.account, pw, status);
  user.id = user_id(db, user.account);
  return user;
}

// 等待条件成立（有界轮询，短间隔），用于观察调度器状态；失败返回 false。
template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

constexpr auto kShort = std::chrono::milliseconds(3000);

// ---------------------------------------------------------------------------
// T1：队列满载立即 503，未接收请求不落库
// ---------------------------------------------------------------------------

void test_queue_full_rejects_without_persisting() {
  std::cout << "队列满载：立即 503、稳定错误标识与重试提示，未接收不落库\n";
  GatedExecutor executor;
  GatedExecutor::Gate gate;
  executor.gate = &gate;
  executor.block_compile = true;
  executor.run_output = "2\n";
  Env env("sched_full", &executor, {},
          JudgeManager::Options(/*capacity=*/1, /*workers=*/1));
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "full_user", "FullPw123");
  std::int64_t pid = insert_problem(env.db(), "队列满载题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  const std::string pid_text = std::to_string(pid);

  check(count_rows(env.db(), "submissions") == 0, "初始无提交记录");

  // 任务 A 占住唯一 worker。
  std::string status_a;
  std::thread worker_thread([&]() {
    httplib::Client thread_cli = make_client(env.port());
    auto res = submit(thread_cli, user.token, pid_text, "cpp17", "int main(){}");
    if (res && res->status == 200) {
      status_a = json::parse(res->body).value("status", "");
    }
  });
  check(gate.wait_entered(1, kShort), "任务 A 开始执行");

  // 任务 B 进入唯一等待槽位。
  std::string status_b;
  std::thread queue_thread([&]() {
    httplib::Client thread_cli = make_client(env.port());
    auto res = submit(thread_cli, user.token, pid_text, "cpp17", "int main(){}");
    if (res && res->status == 200) {
      status_b = json::parse(res->body).value("status", "");
    }
  });
  check(wait_until(
            [&]() { return env.server()->judge_manager()->queued_count() == 1; },
            kShort),
        "任务 B 进入等待队列");

  // 任务 C 到达时队列已满：立即 503，附带稳定 code 与 Retry-After。
  httplib::Client third_cli = make_client(env.port());
  auto rejected = submit(third_cli, user.token, pid_text, "cpp17", "int main(){}");
  check(rejected && rejected->status == 503, "队列满载返回 503");
  if (rejected) {
    json body = json::parse(rejected->body);
    check(body.value("code", "") == "JUDGE_QUEUE_FULL",
          "返回稳定业务标识 JUDGE_QUEUE_FULL");
    check(rejected->get_header_value("Retry-After") == "1",
          "返回 Retry-After 重试提示");
  }
  check(count_rows(env.db(), "submissions") == 0,
        "被拒请求不创建提交记录（尚未接收）");

  gate.release();
  worker_thread.join();
  queue_thread.join();
  check(status_a == "AC" && status_b == "AC", "已接收任务均完成并得到 AC");

  check(count_rows(env.db(), "submissions") == 2,
        "已接收任务各持久化一次（共 2 条）");
  oj::UserProblemStatusRecord st;
  bool found = false;
  read_status(env.db(), user.id, pid, found, st);
  check(found && st.submit_count == 2, "被拒请求不增加提交次数（计数为 2）");
  check(count_status_rows(env.db(), user.id, pid) == 1, "只有一条状态记录");
}

// ---------------------------------------------------------------------------
// T2：判题繁忙/队列满载时健康检查与题目查询仍可响应
// ---------------------------------------------------------------------------

void test_health_and_queries_while_busy() {
  std::cout << "判题繁忙且队列满载时，健康检查与题目查询仍可响应\n";
  GatedExecutor executor;
  GatedExecutor::Gate gate;
  executor.gate = &gate;
  executor.block_compile = true;
  executor.run_output = "2\n";
  Env env("sched_busy", &executor, {},
          JudgeManager::Options(/*capacity=*/1, /*workers=*/1));
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "busy_user", "BusyPw123");
  std::int64_t pid = insert_problem(env.db(), "繁忙题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  const std::string pid_text = std::to_string(pid);

  std::atomic<bool> done_a{false};
  std::atomic<bool> done_b{false};
  std::thread worker_thread([&]() {
    httplib::Client thread_cli = make_client(env.port());
    submit(thread_cli, user.token, pid_text, "cpp17", "int main(){}");
    done_a = true;
  });
  check(gate.wait_entered(1, kShort), "任务 A 开始执行");
  std::thread queue_thread([&]() {
    httplib::Client thread_cli = make_client(env.port());
    submit(thread_cli, user.token, pid_text, "cpp17", "int main(){}");
    done_b = true;
  });
  check(wait_until(
            [&]() { return env.server()->judge_manager()->queued_count() == 1; },
            kShort),
        "任务 B 进入等待队列（判题繁忙）");

  // 另起客户端：健康检查与题目查询必须仍能在规定时间内响应。
  httplib::Client probe = make_client(env.port());
  probe.set_read_timeout(2, 0);
  auto health = probe.Get("/api/health");
  check(health && health->status == 200 &&
            json::parse(health->body).value("status", "") == "ok",
        "判题繁忙时健康检查仍响应 200");
  auto problems = probe.Get("/api/problems");
  check(problems && problems->status == 200,
        "判题繁忙时题目列表查询仍响应 200");

  gate.release();
  worker_thread.join();
  queue_thread.join();
  check(done_a && done_b, "后台任务在放行后正常完成");
}

// ---------------------------------------------------------------------------
// T3：执行器抛异常转换为内部判题错误 SYSERR，worker 继续处理
// ---------------------------------------------------------------------------

void test_executor_exception_becomes_syserr_and_recovers() {
  std::cout << "执行器抛异常：转为 SYSERR 记录，worker 继续处理后续任务\n";
  GatedExecutor executor;
  executor.throw_on_compile = true;
  Env env("sched_throw", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "throw_user", "ThrowPw1");
  std::int64_t pid = insert_problem(env.db(), "异常题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  const std::string pid_text = std::to_string(pid);

  auto bad = submit(cli, user.token, pid_text, "cpp17", "int main(){}");
  check(bad && bad->status == 200, "异常任务仍以 200 返回判题结果");
  std::int64_t bad_id = 0;
  if (bad) {
    json body = json::parse(bad->body);
    bad_id = body.value("id", 0LL);
    check(body.value("status", "") == "SYSERR", "异常转换为 SYSERR");
  }

  // worker 未退出：后续任务正常判题。
  executor.throw_on_compile = false;
  executor.run_output = "2\n";
  auto good = submit(cli, user.token, pid_text, "cpp17", "int main(){}");
  check(good && good->status == 200 &&
            json::parse(good->body).value("status", "") == "AC",
        "worker 继续处理后续任务并判为 AC");

  oj::SubmissionStore store(env.db());
  bool found = false;
  oj::SubmissionRecord record;
  std::string err;
  store.find_by_id(bad_id, found, record, err);
  check(found && record.status == "SYSERR", "SYSERR 已持久化");
}

// ---------------------------------------------------------------------------
// T4：同一用户同题目并发计数与首次 AC 最早时间
// ---------------------------------------------------------------------------

void test_first_ac_uses_earliest_original_time() {
  std::cout << "并发同题：计数正确，首次 AC 取最早原提交时间（乱序完成）\n";
  GatedExecutor executor;
  GatedExecutor::Gate gate;
  executor.gate = &gate;
  executor.block_marker = "BLOCK"; // 仅阻塞含该标记的源码
  executor.run_output = "2\n";
  Env env("sched_first_ac", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "firstac_user", "FirstAc1");
  std::int64_t pid = insert_problem(env.db(), "首次AC题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");

  oj::submit::SubmitService service(env.db(), executor, {});
  oj::submit::SubmitService::Outcome early;
  oj::submit::SubmitService::Outcome late;

  // 较早原提交时间：先发起但被同步门挡住。
  std::thread early_thread([&]() {
    early = service.submit(user.id, pid, "cpp17", "//BLOCK\n", false,
                           "2026-01-01 00:00:01");
  });
  check(gate.wait_entered(1, kShort), "较早任务开始但被阻塞");

  // 较晚原提交时间：立即完成并先落库。
  late = service.submit(user.id, pid, "cpp17", "//fast\n", false,
                        "2026-01-01 00:00:02");
  check(late.kind == oj::submit::SubmitService::Kind::Ok &&
            late.submission.status == "AC",
        "较晚任务先完成并 AC");

  gate.release();
  early_thread.join();
  check(early.kind == oj::submit::SubmitService::Kind::Ok &&
            early.submission.status == "AC",
        "较早任务随后完成并 AC");

  bool found = false;
  oj::UserProblemStatusRecord st;
  read_status(env.db(), user.id, pid, found, st);
  check(found && st.accepted, "状态为 accepted");
  check(found && st.submit_count == 2, "并发提交计数为 2（未丢失）");
  check(found && st.first_ac_at == "2026-01-01 00:00:01",
        "首次 AC 时间为最早原提交时间（未因完成顺序倒置而错）");
  check(count_status_rows(env.db(), user.id, pid) == 1, "只有一条状态记录");

  oj::SubmissionStore store(env.db());
  oj::SubmissionRecord record;
  std::string err;
  store.find_by_id(early.submission.id, found, record, err);
  check(found && record.created_at == "2026-01-01 00:00:01",
        "较早提交的 created_at 保留其原提交时间");
}

// ---------------------------------------------------------------------------
// T5：排队时间不计入程序运行耗时，也不误判 TLE
// ---------------------------------------------------------------------------

void test_queue_wait_not_counted_as_runtime() {
  std::cout << "排队时间不计入运行耗时、不误判 TLE\n";
  GatedExecutor executor;
  GatedExecutor::Gate gate;
  executor.gate = &gate;
  executor.block_compile = true;
  executor.run_output = "2\n";
  executor.run_time_ms = 5;
  Env env("sched_wait", &executor, {},
          JudgeManager::Options(/*capacity=*/4, /*workers=*/1));
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "wait_user", "WaitPw12");
  // 时限 50ms，远小于排队等待时间；程序本身只“运行” 5ms。
  std::int64_t pid = insert_problem(env.db(), "排队时间题", 1, 50);
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  const std::string pid_text = std::to_string(pid);

  std::string status_a;
  std::string status_b;
  long long runtime_b = -1;
  std::thread a([&]() {
    httplib::Client thread_cli = make_client(env.port());
    auto res = submit(thread_cli, user.token, pid_text, "cpp17", "int main(){}");
    if (res && res->status == 200) {
      status_a = json::parse(res->body).value("status", "");
    }
  });
  check(gate.wait_entered(1, kShort), "任务 A 开始执行");

  std::thread b([&]() {
    httplib::Client thread_cli = make_client(env.port());
    auto res = submit(thread_cli, user.token, pid_text, "cpp17", "int main(){}");
    if (res && res->status == 200) {
      json body = json::parse(res->body);
      status_b = body.value("status", "");
      runtime_b = body.value("runtime_ms", -1LL);
    }
  });
  check(wait_until(
            [&]() { return env.server()->judge_manager()->queued_count() == 1; },
            kShort),
        "任务 B 在队列中等待");

  // 让 B 的排队时间明显超过题目 50ms 时限。
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  gate.release();
  a.join();
  b.join();

  check(status_a == "AC" && status_b == "AC", "两个任务均为 AC（未误判 TLE）");
  check(runtime_b == 5, "运行耗时仅为程序执行时间（5ms），不含排队等待");
}

// ---------------------------------------------------------------------------
// T6：真实 C++17/C11 并发判题，结果正确、子进程回收互不干扰
// ---------------------------------------------------------------------------

void test_real_compile_concurrent() {
  std::cout << "真实 C++17/C11 并发判题：结果正确、子进程回收互不干扰\n";
  TempDir workspace("sched_real_ws");
  oj::judge::JudgeOptions options;
  options.workspace_root = workspace.sub("judge");
  Env env("sched_real", nullptr, options, JudgeManager::Options(64, 4));
  check(env.ok(), "真实服务启动成功");
  const int port = env.port();
  httplib::Client cli = make_client(port);

  User user = make_user(cli, env.db(), "real_user", "RealPw123");
  std::int64_t pid = insert_problem(env.db(), "真实并发题");
  insert_testcase(env.db(), pid, 0, "1 2\n", "3\n");
  insert_testcase(env.db(), pid, 1, "100 -50\n", "50\n");
  const std::string pid_text = std::to_string(pid);

  const char *kCppSum =
      "#include <iostream>\n"
      "int main(){ long long a,b; if(!(std::cin>>a>>b)) return 0; "
      "std::cout<<(a+b)<<\"\\n\"; return 0; }\n";
  const char *kC11Sum =
      "#include <stdio.h>\n"
      "int main(void){ long long a,b; "
      "if(scanf(\"%lld %lld\",&a,&b)!=2) return 0; "
      "printf(\"%lld\\n\", a+b); return 0; }\n";
  const char *kWrong = "#include <cstdio>\n"
                       "int main(){ long long a,b; "
                       "if(scanf(\"%lld %lld\",&a,&b)!=2) return 0; "
                       "printf(\"0\\n\"); return 0; }\n";

  struct Case {
    std::string language;
    std::string code;
    std::string expected;
  };
  std::vector<Case> cases;
  for (int i = 0; i < 4; ++i) {
    cases.push_back({"cpp17", kCppSum, "AC"});
    cases.push_back({"c11", kC11Sum, "AC"});
    cases.push_back({"cpp17", kWrong, "WA"});
  }

  std::vector<std::string> results(cases.size());
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    threads.emplace_back([&, i]() {
      httplib::Client thread_cli = make_client(port);
      auto res = submit(thread_cli, user.token, pid_text, cases[i].language,
                        cases[i].code);
      if (res && res->status == 200) {
        results[i] = json::parse(res->body).value("status", "");
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  bool all_expected = true;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    if (results[i] != cases[i].expected) {
      all_expected = false;
      std::cout << "    [INFO] 用例 " << i << " 期望 " << cases[i].expected
                << " 实得 " << results[i] << "\n";
    }
  }
  check(all_expected, "并发真实判题结果各自正确（AC/WA 无混用）");

  // 所有判题子进程都应已由各自任务回收，不留僵尸、不互相抢退出状态。
  int status = 0;
  pid_t leftover = ::waitpid(-1, &status, WNOHANG);
  check(leftover <= 0, "无遗留子进程（未被回收）");

  oj::UserProblemStatusRecord st;
  bool found = false;
  read_status(env.db(), user.id, pid, found, st);
  check(found && st.submit_count == static_cast<int>(cases.size()),
        "并发真实提交计数正确");
}

// ---------------------------------------------------------------------------
// T7：停止时排空已接收任务、数据库顺序安全
// ---------------------------------------------------------------------------

void test_graceful_stop_drains_accepted_tasks() {
  std::cout << "优雅停止：已接收任务全部完成后再回收 worker\n";
  GatedExecutor executor;
  GatedExecutor::Gate gate;
  executor.gate = &gate;
  executor.block_compile = true;
  executor.run_output = "2\n";
  Env env("sched_stop", &executor, {},
          JudgeManager::Options(/*capacity=*/8, /*workers=*/1));
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "stop_user", "StopPw12");
  std::int64_t pid = insert_problem(env.db(), "停止题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  const std::string pid_text = std::to_string(pid);

  std::atomic<int> completed{0};
  std::thread worker_thread([&]() {
    httplib::Client thread_cli = make_client(env.port());
    if (submit(thread_cli, user.token, pid_text, "cpp17", "int main(){}")) {
      completed.fetch_add(1);
    }
  });
  check(gate.wait_entered(1, kShort), "任务 A 开始执行");
  std::thread queue_thread([&]() {
    httplib::Client thread_cli = make_client(env.port());
    if (submit(thread_cli, user.token, pid_text, "cpp17", "int main(){}")) {
      completed.fetch_add(1);
    }
  });
  check(wait_until(
            [&]() { return env.server()->judge_manager()->queued_count() == 1; },
            kShort),
        "任务 B 在队列中等待");

  // 触发停止：应等待已接收任务执行完再返回。
  std::atomic<bool> stop_done{false};
  std::thread stop_thread([&]() {
    env.stop();
    stop_done = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  check(!stop_done.load(), "停止不会在已接收任务完成前返回");

  gate.release();
  stop_thread.join();
  worker_thread.join();
  queue_thread.join();

  check(stop_done.load(), "停止完成，无永久等待");
  check(completed.load() == 2, "已接收任务在停止过程中全部完成");
  check(count_rows(env.db(), "submissions") == 2,
        "停止过程中已完成任务均已持久化");

  // 停止后才关闭数据库，顺序安全。
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-015：多用户 × 多题目 × 不同源码并发，判题结果、临时目录与入库互不混用
// ---------------------------------------------------------------------------

void test_multi_user_multi_problem_no_mix() {
  std::cout << "多用户多题目不同源码并发：临时目录/输出/结果/归属互不混用\n";
  GatedExecutor executor;
  executor.use_source_marker_output = true;
  Env env("sched_multimix", &executor, {}, JudgeManager::Options(64, 4));
  check(env.ok(), "服务启动成功");

  constexpr int kUsers = 3;
  constexpr int kProblems = 3;

  httplib::Client setup_cli = make_client(env.port());
  std::vector<User> users;
  for (int u = 0; u < kUsers; ++u) {
    User user = make_user(setup_cli, env.db(),
                          "mix_u" + std::to_string(u), "MixPw" + std::to_string(u));
    users.push_back(user);
  }

  // 每题期望输出互不相同，源码标记各自题目期望值；任一混用都会导致 WA。
  std::vector<std::int64_t> problems;
  for (int p = 0; p < kProblems; ++p) {
    const std::string expected = "E" + std::to_string(p);
    std::int64_t pid = insert_problem(env.db(), "mix_p" + std::to_string(p));
    insert_testcase(env.db(), pid, 0, "input\n", expected + "\n");
    problems.push_back(pid);
  }

  struct Submission {
    int user;
    int problem;
    bool expect_wa;
  };
  std::vector<Submission> plan;
  for (int u = 0; u < kUsers; ++u) {
    for (int p = 0; p < kProblems; ++p) {
      // 每个用户对 p==1 的提交故意输出错误，验证 WA 也归属正确。
      plan.push_back({u, p, u == 0 && p == 1});
    }
  }

  std::vector<std::string> statuses(plan.size());
  std::vector<std::int64_t> ids(plan.size(), 0);
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < plan.size(); ++i) {
    threads.emplace_back([&, i]() {
      httplib::Client thread_cli = make_client(env.port());
      const std::string marker =
          plan[i].expect_wa ? "BAD" : "E" + std::to_string(plan[i].problem);
      const std::string source = "// OUT:" + marker + "\n";
      auto res = submit(thread_cli, users[plan[i].user].token,
                        std::to_string(problems[plan[i].problem]), "cpp17", source);
      if (res && res->status == 200) {
        json body = json::parse(res->body);
        statuses[i] = body.value("status", "");
        ids[i] = body.value("id", 0LL);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  bool all_expected = true;
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const std::string expected = plan[i].expect_wa ? "WA" : "AC";
    if (statuses[i] != expected) {
      all_expected = false;
      std::cout << "    [INFO] user " << plan[i].user << " problem "
                << plan[i].problem << " 期望 " << expected << " 实得 "
                << statuses[i] << "\n";
    }
  }
  check(all_expected, "每个并发提交都得到各自源码/题目的判题结果（无混用）");

  // 逐条核对入库归属、题目、状态与源码，确认没有写串。
  oj::SubmissionStore store(env.db());
  bool ownership_ok = true;
  for (std::size_t i = 0; i < plan.size(); ++i) {
    bool found = false;
    oj::SubmissionRecord record;
    std::string err;
    if (!store.find_by_id(ids[i], found, record, err) || !found ||
        record.user_id != users[plan[i].user].id ||
        record.problem_id != problems[plan[i].problem] ||
        record.status != (plan[i].expect_wa ? "WA" : "AC") ||
        record.source_code.find("OUT:") == std::string::npos) {
      ownership_ok = false;
      std::cout << "    [INFO] 第 " << i << " 条入库记录与提交不匹配\n";
    }
  }
  check(ownership_ok, "每条入库记录的归属/题目/状态/源码均正确");
  check(count_rows(env.db(), "submissions") == static_cast<std::int64_t>(plan.size()),
        "并发提交各持久化一次（共 9 条）");

  bool status_rows_ok = true;
  for (int u = 0; u < kUsers; ++u) {
    for (int p = 0; p < kProblems; ++p) {
      if (count_status_rows(env.db(), users[u].id, problems[p]) != 1) {
        status_rows_ok = false;
      }
    }
  }
  check(status_rows_ok, "每个用户×题目只有一条状态记录");
}

} // namespace

int main() {
  test_queue_full_rejects_without_persisting();
  test_health_and_queries_while_busy();
  test_executor_exception_becomes_syserr_and_recovers();
  test_first_ac_uses_earliest_original_time();
  test_queue_wait_not_counted_as_runtime();
  test_real_compile_concurrent();
  test_graceful_stop_drains_accepted_tasks();
  test_multi_user_multi_problem_no_mix();

  std::cout << "\n==== 调度集成测试" << (g_failures == 0 ? "全部通过" : "存在失败")
            << "（失败 " << g_failures << " 项）====\n";
  return g_failures == 0 ? 0 : 1;
}
