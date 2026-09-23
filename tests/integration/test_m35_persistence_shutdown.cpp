// M3.5 持久化与停止清理 HTTP 集成测试。
//
// 使用隔离临时库、随机端口、专用判题目录与独立测试服务进程，不触碰正式服务与
// 正式提交记录。以受控执行器 + 同步门 + 故障注入组织边界测试，再用少量真实进程
// 验证；不通过耗尽整机资源或高并发制造故障。
//
// 覆盖：
//   - 完整 AC/WA/CE/SYSERR 结果保存后关闭重开数据库仍可正确读取（源码、逐点详情、
//     编译信息、耗时/内存、未采集指标的 null 表示）；
//   - 事务中途失败整体回滚（提交记录与做题状态一起回滚）；
//   - 数据库锁竞争下有界等待：短期竞争成功、长期竞争明确失败且不永久挂起；
//   - 同一任务正常完成与取消竞争时只产生一次最终处理与正确计数；
//   - 客户端在提交被接收后断开，任务仍完成并保存，已保存结果不被删除；
//   - 空闲 / 编译中 / 排队 / 结果保存阶段停止服务均按约定收尾；
//   - 停止过程中发起新提交不会误入已停止的执行器；
//   - 重复（含并发）正常停止通知不重复回收、不死锁；
//   - 停止后无遗留子进程、僵尸进程与判题临时目录；
//   - 正常失败路径反复执行后文件描述符、线程、内存与判题目录无持续异常增长；
//   - 正常停止并重启后已保存提交与用户题目状态一致，服务还能继续判题。
//
// 运行方式：ctest --test-dir build -R m35_persistence_shutdown --output-on-failure
// 或直接执行 build/oj_m35_persistence_shutdown_test。

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <signal.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
using oj::judge::SubmissionTask;
namespace fs = std::filesystem;

int g_failures = 0;

void check(bool cond, const std::string &msg) {
  if (cond) {
    std::cout << "  [PASS] " << msg << "\n";
  } else {
    std::cout << "  [FAIL] " << msg << "\n";
    ++g_failures;
  }
}

const std::string kTestSecret = "it-m35-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";
constexpr auto kShort = std::chrono::milliseconds(5000);

oj::auth::JwtConfig make_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = kTestSecret;
  cfg.expires_seconds = 3600;
  return cfg;
}

// 可注入执行器：可控编译/运行结果，并可在编译前进入同步门（用于制造「编译中停止」
// 与「已接收后断开」的确定性窗口）。不启动真实进程。
class ScriptedExecutor : public oj::judge::IExecutor {
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

  // 编译阶段行为
  bool compile_launch_error = false;
  bool compile_sandbox_error = false;
  int compile_exit_code = 0;
  std::string compile_output;
  bool compile_output_truncated = false;
  long long compile_time_ms = 1;

  // 运行阶段行为
  bool run_launch_error = false;
  bool run_timeout = false;
  bool run_memory_exceeded = false;
  bool run_output_truncated = false;
  std::string run_output;
  std::string run_stderr;
  int run_exit_code = 0;
  long long run_time_ms = 1;
  long long run_memory_kb = 0;

  oj::judge::ProcessResult compile(
      const oj::judge::CompileRequest &request) override {
    if (gate != nullptr) {
      gate->enter();
    }
    oj::judge::ProcessResult result;
    if (compile_launch_error) {
      result.launch_error = true;
      result.sandbox_error = compile_sandbox_error;
      result.launch_error_message = "fake: 编译环境不可用";
      return result;
    }
    result.launched = true;
    result.exited = true;
    result.exit_code = compile_exit_code;
    result.stdout_data = compile_output;
    result.stdout_truncated = compile_output_truncated;
    result.time_ms = compile_time_ms;
    if (compile_exit_code == 0) {
      std::ofstream out(request.output_path, std::ios::binary);
      out << "fake-program";
    }
    return result;
  }

  oj::judge::ProcessResult run(const oj::judge::RunRequest &,
                               const std::string &) override {
    oj::judge::ProcessResult result;
    if (run_launch_error) {
      result.launch_error = true;
      result.launch_error_message = "fake: 无法启动";
      return result;
    }
    result.launched = true;
    if (run_timeout) {
      result.timed_out = true;
      result.termination = oj::judge::TerminationReason::TimedOut;
      result.term_signal = SIGKILL;
      result.exited = false;
    } else if (run_memory_exceeded) {
      result.memory_exceeded = true;
      result.termination = oj::judge::TerminationReason::MemoryExceeded;
      result.exited = false;
    } else {
      result.exited = true;
      result.exit_code = run_exit_code;
      result.termination = run_exit_code == 0
                               ? oj::judge::TerminationReason::Completed
                               : oj::judge::TerminationReason::NonZeroExit;
    }
    result.stdout_data = run_output;
    result.stderr_data = run_stderr;
    result.stdout_truncated = run_output_truncated;
    result.time_ms = run_time_ms;
    result.memory_kb = run_memory_kb;
    return result;
  }
};

class TempDir {
public:
  explicit TempDir(const std::string &label) {
    fs::path base = fs::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
      auto candidate = base / (label + "_" + std::to_string(::getpid()) + "_" +
                               std::to_string(i));
      std::error_code ec;
      fs::create_directories(candidate, ec);
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
      fs::remove_all(path_, ec);
    }
  }

  std::string db_path() const { return (path_ / "oj.db").string(); }
  std::string sub(const std::string &name) const {
    return (path_ / name).string();
  }
  const fs::path &path() const { return path_; }

private:
  fs::path path_;
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

// 隔离临时库 + 随机端口真实 HTTP + 可选注入执行器。
class Env {
public:
  Env(const std::string &label, const std::string &db_path,
      oj::judge::IExecutor *executor = nullptr,
      JudgeManager::Options manager_options = JudgeManager::Options())
      : dir_(label) {
    std::string err;
    const std::string path = db_path.empty() ? dir_.db_path() : db_path;
    db_ = oj::Database::open(path, err);
    if (!db_) {
      return;
    }
    if (!oj::initialize_schema(*db_, kAdminPassword, err)) {
      return;
    }
    oj::judge::JudgeOptions judge_options;
    judge_options.workspace_root = dir_.sub("judge");
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
  std::string workspace_root() const { return dir_.sub("judge"); }

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

// ---------------------------------------------------------------------------
// 进程 / 资源观测
// ---------------------------------------------------------------------------

int count_open_fds() {
  DIR *dir = ::opendir("/proc/self/fd");
  if (dir == nullptr) {
    return -1;
  }
  int count = 0;
  for (;;) {
    dirent *entry = ::readdir(dir);
    if (entry == nullptr) {
      break;
    }
    if (entry->d_name[0] != '.') {
      ++count;
    }
  }
  ::closedir(dir);
  return count;
}

long read_proc_status_kb(const char *key) {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(key, 0) == 0) {
      std::string digits;
      for (char c : line) {
        if (c >= '0' && c <= '9') {
          digits.push_back(c);
        }
      }
      if (!digits.empty()) {
        return std::stol(digits);
      }
    }
  }
  return -1;
}

int count_workspace_dirs(const std::string &root) {
  std::error_code ec;
  if (!fs::exists(root, ec)) {
    return 0;
  }
  int count = 0;
  for (const auto &entry : fs::directory_iterator(root, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("oj_judge_", 0) == 0) {
      ++count;
    }
  }
  return count;
}

bool no_leftover_children() {
  int status = 0;
  return ::waitpid(-1, &status, WNOHANG) <= 0;
}

// 外部 SQLite 连接，用于在测试内制造真实的多连接写锁竞争。
class ExternalLock {
public:
  bool open(const std::string &path) {
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE, nullptr) !=
        SQLITE_OK) {
      return false;
    }
    sqlite3_exec(db_, "PRAGMA busy_timeout=0;", nullptr, nullptr, nullptr);
    return true;
  }
  bool begin() {
    return sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) ==
           SQLITE_OK;
  }
  bool commit() {
    return sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) ==
           SQLITE_OK;
  }
  ~ExternalLock() {
    if (db_ != nullptr) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      sqlite3_close(db_);
    }
  }

private:
  sqlite3 *db_ = nullptr;
};

// ---------------------------------------------------------------------------
// A. 完整结果持久化 + 关闭重开可读
// ---------------------------------------------------------------------------

void test_full_result_persisted_reopen() {
  std::cout << "完整结果持久化：AC/WA/CE/SYSERR 关闭重开数据库仍可正确读取\n";
  TempDir dir("m35_persist");
  const std::string dbpath = dir.db_path();
  std::int64_t uid = 0;
  std::int64_t pid = 0;
  std::int64_t wa_id = 0;
  std::int64_t ce_id = 0;
  std::int64_t sys_id = 0;
  const std::string source = "// source-marker-keep\nint main(){return 0;}\n";
  std::string first_ac_at;

  {
    ScriptedExecutor ex;
    ex.run_output = "1\n";
    ex.run_stderr = "advance-notice";
    ex.run_time_ms = 7;
    ex.run_memory_kb = 1234;
    ex.compile_output = "warn-line";
    ex.compile_output_truncated = true;
    Env env("m35_persist_a", dbpath, &ex);
    check(env.ok(), "服务启动成功");
    if (!env.ok()) {
      return;
    }
    httplib::Client cli = make_client(env.port());
    User u = make_user(cli, env.db(), "m35_user_a", "M35Pw123");
    uid = u.id;
    pid = insert_problem(env.db(), "M35 持久化题");
    insert_testcase(env.db(), pid, 0, "PASSTOKEN\n", "1\n");
    insert_testcase(env.db(), pid, 1, "FAILTOKEN\n", "2\n");
    const std::string pid_text = std::to_string(pid);

    // WA：第 1 点 AC，第 2 点 WA。
    auto wa = submit(cli, u.token, pid_text, "cpp17", source);
    check(wa && wa->status == 200, "WA 提交返回 200");
    if (wa) {
      json body = json::parse(wa->body);
      wa_id = body.value("id", 0LL);
      check(body.value("status", "") == "WA", "汇总状态为 WA");
      check(body.value("runtime_ms", 0LL) == 14,
            "runtime_ms 为各点耗时之和（7+7）");
      check(body.value("memory_kb", 0LL) == 1234, "memory_kb 为峰值 RSS");
      check(body.value("compile_output_truncated", false) == true,
            "编译诊断截断标识被保留");
      check(body.contains("results") && body["results"].size() == 2,
            "逐点结果两条");
      if (body.contains("results") && body["results"].size() == 2) {
        const json &pass = body["results"][0];
        const json &fail = body["results"][1];
        check(pass.value("status", "") == "AC" && !pass.contains("input") &&
                  !pass.contains("expected_output"),
              "AC 点不泄露隐藏输入/答案");
        check(fail.value("status", "") == "WA" &&
                  fail.value("input", "") == "FAILTOKEN\n" &&
                  fail.value("expected_output", "") == "2\n" &&
                  fail.value("actual_output", "") == "1\n",
              "WA 点保留输入/期望/实际输出");
        check(fail.contains("reason"), "WA 点保留结构化终止原因");
      }
    }

    // CE：编译失败，未运行任何测试点。
    ex.compile_exit_code = 2;
    ex.compile_output = "main.cpp:1: error: forced";
    ex.compile_output_truncated = false;
    auto ce = submit(cli, u.token, pid_text, "c11", "bad{}");
    check(ce && ce->status == 200 && json::parse(ce->body).value("status", "") == "CE",
          "CE 提交返回 200 且状态为 CE");
    if (ce) {
      json body = json::parse(ce->body);
      ce_id = body.value("id", 0LL);
      check(body["results"].empty(), "CE 无逐点结果");
      check(body.value("memory_kb", json(0LL)).is_null(),
            "CE 未采集内存表示为 null 而非 0");
      check(body.value("compile_output", "").find("forced") != std::string::npos,
            "CE 保留编译诊断");
    }

    // SYSERR：编译环境故障（非用户错误）。
    ex.compile_exit_code = 0;
    ex.compile_launch_error = true;
    ex.compile_sandbox_error = true;
    auto sys = submit(cli, u.token, pid_text, "cpp17", source);
    check(sys && sys->status == 200 &&
              json::parse(sys->body).value("status", "") == "SYSERR",
          "内部故障提交状态为 SYSERR（非 HTTP 500）");
    if (sys) {
      sys_id = json::parse(sys->body).value("id", 0LL);
    }

    env.stop();
    env.close_db();
  }

  // 关闭重开：所有已保存内容仍可正确读取。
  {
    std::string err;
    auto db = oj::Database::open(dbpath, err);
    check(db != nullptr, "重开数据库成功");
    if (!db) {
      return;
    }
    oj::SubmissionStore store(*db);
    bool found = false;
    oj::SubmissionRecord wa;
    check(store.find_by_id(wa_id, found, wa, err) && found, "WA 记录可读取");
    check(wa.source_code == source, "WA 源码完整保留");
    check(wa.status == "WA" && wa.runtime_ms == 14 && wa.memory_kb == 1234,
          "WA 状态/耗时/内存完整保留");
    check(wa.compile_msg.find("warn-line") != std::string::npos,
          "WA 编译信息完整保留");
    json per_case = json::parse(wa.per_case);
    check(per_case.size() == 2 && per_case[1].value("input", "") == "FAILTOKEN\n" &&
              per_case[1].contains("actual_output"),
          "WA 逐点详情（输入/期望/实际）完整保留");
    check(per_case[1].value("reason", "").empty() == false,
          "WA 结构化原因完整保留");

    oj::SubmissionRecord ce;
    found = false;
    check(store.find_by_id(ce_id, found, ce, err) && found, "CE 记录可读取");
    check(ce.status == "CE" && ce.source_code == "bad{}",
          "CE 状态与源码完整保留");

    oj::SubmissionRecord sys;
    found = false;
    check(store.find_by_id(sys_id, found, sys, err) && found, "SYSERR 记录可读取");
    check(sys.status == "SYSERR", "SYSERR 状态完整保留");

    // 状态：提交次数 3、从未 AC、无首次 AC 时间。
    oj::UserProblemStatusStore statuses(*db);
    bool has = false;
    oj::UserProblemStatusRecord status;
    check(statuses.find(uid, pid, has, status, err) && has,
          "用户题目状态记录存在");
    check(!status.accepted && status.submit_count == 3 &&
              !status.has_first_ac_at,
          "状态：3 次提交、未 AC、无首次 AC 时间");
    db->close();
  }
}

// ---------------------------------------------------------------------------
// B. 事务中途失败整体回滚
// ---------------------------------------------------------------------------

void test_transaction_rollback() {
  std::cout << "事务中途失败：提交记录与做题状态一起回滚\n";
  ScriptedExecutor ex;
  ex.run_output = "2\n";
  Env env("m35_rollback", "", &ex);
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m35_rb", "M35Rb123");
  std::int64_t pid = insert_problem(env.db(), "M35 回滚题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");

  const std::int64_t subs_before = count_rows(env.db(), "submissions");
  const std::int64_t status_before = count_rows(env.db(), "user_problem_status");

  std::string err;
  check(env.db().exec("CREATE TRIGGER m35_fail_ups BEFORE INSERT ON "
                      "user_problem_status BEGIN SELECT RAISE(FAIL, 'forced'); "
                      "END;",
                      err),
        "创建强制失败触发器");
  auto res = submit(cli, u.token, std::to_string(pid), "cpp17", "x");
  check(res && res->status == 500, "状态写入失败返回 500");
  check(count_rows(env.db(), "submissions") == subs_before,
        "提交记录随事务回滚");
  check(count_rows(env.db(), "user_problem_status") == status_before,
        "做题状态随事务回滚");

  check(env.db().exec("DROP TRIGGER m35_fail_ups;", err), "移除触发器");
  auto ok = submit(cli, u.token, std::to_string(pid), "cpp17", "x");
  check(ok && ok->status == 200 &&
            json::parse(ok->body).value("status", "") == "AC",
        "故障消除后提交成功并计数");
}

// ---------------------------------------------------------------------------
// C. 数据库锁竞争：有界等待，成功或明确失败，不永久挂起
// ---------------------------------------------------------------------------

void test_db_lock_contention_bounded() {
  std::cout << "数据库写锁竞争：短期等待成功、长期竞争明确失败且不挂起\n";
  ScriptedExecutor ex;
  ex.run_output = "2\n";
  TempDir dir("m35_lock");
  Env env("m35_lock_env", dir.db_path(), &ex);
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m35_lock", "M35Lk123");
  std::int64_t pid = insert_problem(env.db(), "M35 锁题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  const std::string pid_text = std::to_string(pid);

  // 短期竞争：外部连接持写锁约 300ms 后释放，服务应在 busy_timeout 内成功。
  {
    ExternalLock lock;
    check(lock.open(dir.db_path()), "打开外部连接");
    check(lock.begin(), "外部连接取得写锁");
    std::thread releaser([&lock] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      lock.commit();
    });
    const auto start = std::chrono::steady_clock::now();
    auto res = submit(cli, u.token, pid_text, "cpp17", "x");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    releaser.join();
    check(res && res->status == 200 &&
              json::parse(res->body).value("status", "") == "AC",
          "短期锁竞争下写入成功");
    check(elapsed >= 250 && elapsed < 5000,
          "短期竞争按有界等待完成（未立即失败、未无限等待）");
  }

  // 长期竞争：外部连接持写锁超过 busy_timeout，服务应明确失败而非永久挂起。
  {
    ExternalLock lock;
    check(lock.open(dir.db_path()), "重新打开外部连接");
    check(lock.begin(), "外部连接再次取得写锁");
    const std::int64_t before = count_rows(env.db(), "submissions");
    const auto start = std::chrono::steady_clock::now();
    auto res = submit(cli, u.token, pid_text, "cpp17", "x");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    check(res && res->status == 500, "长期锁竞争明确返回 500（不假成功）");
    check(elapsed >= 4500 && elapsed < 12000,
          "长期竞争在 busy_timeout 附近有界失败，未永久挂起");
    check(count_rows(env.db(), "submissions") == before,
          "失败事务未产生提交记录");
    lock.commit();
  }
}

// ---------------------------------------------------------------------------
// D. 正常完成与取消竞争：只产生一次最终处理与正确计数
// ---------------------------------------------------------------------------

void test_cancel_completion_race_single_final() {
  std::cout << "正常完成与取消竞争：每个任务只最终处理一次、计数正确\n";
  constexpr int kIterations = 3;
  for (int i = 0; i < kIterations; ++i) {
    ScriptedExecutor ex;
    ScriptedExecutor::Gate gate;
    ex.gate = &gate;
    ex.run_output = "2\n";
    Env env("m35_race_" + std::to_string(i), "", &ex,
            JudgeManager::Options(/*capacity=*/8, /*workers=*/1));
    if (!env.ok()) {
      check(false, "服务启动成功");
      return;
    }
    httplib::Client cli = make_client(env.port());
    User u = make_user(cli, env.db(), "m35_race_" + std::to_string(i), "M35Rc123");
    std::int64_t pid = insert_problem(env.db(), "M35 竞争题");
    insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");

    std::string status;
    std::thread submitter([&] {
      httplib::Client thread_cli = make_client(env.port());
      auto res = submit(thread_cli, u.token, std::to_string(pid), "cpp17", "x");
      if (res && res->status == 200) {
        status = json::parse(res->body).value("status", "");
      }
    });
    const bool started = gate.wait_entered(1, kShort);
    check(started, "任务已开始执行（编译门内）");
    if (!started) {
      gate.release();
      submitter.join();
      env.stop();
      continue;
    }

    // 同时触发取消与放行，制造完成/取消竞争。
    std::thread canceller(
        [&] { env.server()->judge_manager()->cancel_all(); });
    std::thread releaser([&] { gate.release(); });
    canceller.join();
    releaser.join();
    submitter.join();

    check(status == "AC" || status == "SYSERR",
          "竞争结果为 AC 或 服务取消 SYSERR（明确终态）");
    check(count_rows(env.db(), "submissions") == 1,
          "同一任务只保存一次（无重复写入）");
    check(count_status_rows(env.db(), u.id, pid) == 1,
          "只有一条状态记录");

    oj::UserProblemStatusStore statuses(env.db());
    bool has = false;
    oj::UserProblemStatusRecord rec;
    std::string err;
    statuses.find(u.id, pid, has, rec, err);
    check(has && rec.submit_count == 1, "提交次数精确为 1（无重复计数）");

    env.stop();
  }
}

// 判题管理器层：并发取消与完成竞争下 handler 恰好被调用一次。
void test_manager_handler_invoked_once_under_race() {
  std::cout << "调度器竞争：取消与完成同时发生时 handler 恰好执行一次\n";
  constexpr int kIterations = 40;
  bool all_once = true;
  for (int i = 0; i < kIterations; ++i) {
    std::atomic<int> calls{0};
    ScriptedExecutor::Gate gate;
    JudgeManager manager(
        [&](const SubmissionTask &) {
          gate.enter();
          calls.fetch_add(1);
          oj::submit::SubmitService::Outcome outcome;
          outcome.kind = oj::submit::SubmitService::Kind::Ok;
          return outcome;
        },
        JudgeManager::Options(8, 1));

    auto result = manager.submit(SubmissionTask{});
    if (result.status != JudgeManager::EnqueueStatus::Accepted) {
      all_once = false;
      manager.shutdown();
      continue;
    }
    const bool entered = gate.wait_entered(1, kShort);
    std::thread canceller([&] { manager.cancel_all(); });
    std::thread releaser([&] { gate.release(); });
    canceller.join();
    releaser.join();
    result.future.get();
    if (calls.load() != 1 || manager.accepted_count() != 1 ||
        manager.completed_count() != 1) {
      all_once = false;
    }
    manager.shutdown();
    if (!entered) {
      all_once = false;
    }
  }
  check(all_once, "40 轮竞争下每个任务只最终处理一次、计数一致");
}

// ---------------------------------------------------------------------------
// E. 客户端断开不丢弃已接收任务
// ---------------------------------------------------------------------------

bool send_raw_request_keep_open(int port, const std::string &token,
                                std::int64_t problem_id,
                                const std::string &code, int &sock_out) {
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(sock);
    return false;
  }
  json body;
  body["language"] = "cpp17";
  body["code"] = code;
  const std::string payload = body.dump();
  std::ostringstream req;
  req << "POST /api/problems/" << problem_id << "/submit HTTP/1.1\r\n"
      << "Host: 127.0.0.1\r\n"
      << "Authorization: Bearer " << token << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Content-Length: " << payload.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << payload;
  const std::string data = req.str();
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t n = ::send(sock, data.data() + offset, data.size() - offset,
                             MSG_NOSIGNAL);
    if (n <= 0) {
      ::close(sock);
      return false;
    }
    offset += static_cast<std::size_t>(n);
  }
  sock_out = sock;
  return true;
}

void test_client_disconnect_keeps_result() {
  std::cout << "客户端断开：已接收任务继续执行并保存，结果不被删除\n";
  ScriptedExecutor ex;
  ScriptedExecutor::Gate gate;
  ex.gate = &gate;
  ex.run_output = "2\n";
  Env env("m35_disconnect", "", &ex, JudgeManager::Options(8, 1));
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m35_disc", "M35Dc123");
  std::int64_t pid = insert_problem(env.db(), "M35 断连题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");

  int sock = -1;
  check(send_raw_request_keep_open(env.port(), u.token, pid, "int main(){}",
                                   sock),
        "发送原始提交请求");
  check(gate.wait_entered(1, kShort), "服务已接收并在执行该任务");
  ::close(sock); // 客户端在结果返回前断开

  gate.release();
  check(wait_until([&] { return count_rows(env.db(), "submissions") == 1; },
                   kShort),
        "客户端断开后任务仍完成并保存");
  check(count_rows(env.db(), "submissions") == 1,
        "已保存结果未因断开被删除");
  check(count_status_rows(env.db(), u.id, pid) == 1, "状态记录正确");

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// F. 停止各阶段与新提交拒绝
// ---------------------------------------------------------------------------

void test_idle_stop_idempotent() {
  std::cout << "空闲重复停止：并发重复通知不重复回收、不死锁\n";
  ScriptedExecutor ex;
  Env env("m35_idle", "", &ex, JudgeManager::Options(8, 2));
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }

  std::atomic<int> returned{0};
  std::vector<std::thread> stoppers;
  for (int i = 0; i < 4; ++i) {
    stoppers.emplace_back([&] {
      env.stop();
      returned.fetch_add(1);
    });
  }
  for (auto &t : stoppers) {
    t.join();
  }
  env.stop(); // 再次调用
  check(returned.load() == 4, "并发重复停止均安全返回");
  check(count_rows(env.db(), "submissions") == 0, "空闲停止不产生提交记录");

  // 停止后新提交不再进入执行器。
  httplib::Client cli = make_client(env.port());
  auto res = submit(cli, "token", "1", "cpp17", "x");
  check(!res || res->status != 200, "停止后新提交被拒绝（连接失败或 503）");
  check(count_rows(env.db(), "submissions") == 0, "被拒提交不落库");
}

void test_stop_during_compile_and_reject_new() {
  std::cout << "编译中停止 + 停止中发起新提交：不误入执行器、结果落库\n";
  ScriptedExecutor ex;
  ScriptedExecutor::Gate gate;
  ex.gate = &gate;
  ex.run_output = "2\n";
  Env env("m35_stop_compile", "", &ex, JudgeManager::Options(8, 1));
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m35_stopc", "M35Sc123");
  std::int64_t pid = insert_problem(env.db(), "M35 停止题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  const std::string pid_text = std::to_string(pid);

  std::string status;
  std::thread submitter([&] {
    httplib::Client thread_cli = make_client(env.port());
    auto res = submit(thread_cli, u.token, pid_text, "cpp17", "x");
    if (res && res->status == 200) {
      status = json::parse(res->body).value("status", "");
    }
  });
  check(gate.wait_entered(1, kShort), "任务 A 在编译阶段");

  // 开始停止：先通知取消（确定性进入 stopped 状态），再发起新提交。
  env.server()->judge_manager()->cancel_all();
  auto rejected = submit(cli, u.token, pid_text, "cpp17", "int main(){}");
  check(rejected && rejected->status == 503, "停止中提交返回 503");
  if (rejected) {
    check(rejected->body.find("JUDGE_UNAVAILABLE") != std::string::npos,
          "返回稳定错误标识 JUDGE_UNAVAILABLE");
  }

  gate.release();
  std::thread stopper([&] { env.stop(); });
  stopper.join();
  submitter.join();

  check(status == "SYSERR", "编译中的任务以服务取消 SYSERR 明确结束");
  check(count_rows(env.db(), "submissions") == 1,
        "仅已接收任务落库（被拒请求不落库）");
  check(count_status_rows(env.db(), u.id, pid) == 1, "状态记录一条");
  check(no_leftover_children(), "停止后无遗留子进程");
  check(count_workspace_dirs(env.workspace_root()) == 0, "停止后无遗留判题目录");
  env.close_db();
}

void test_stop_during_result_saving_batch() {
  std::cout << "结果保存阶段停止：所有已接收（200）提交均已落库一次\n";
  ScriptedExecutor ex;
  ex.run_output = "2\n";
  Env env("m35_batch", "", &ex, JudgeManager::Options(32, 4));
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m35_batch", "M35Bt123");
  std::int64_t pid = insert_problem(env.db(), "M35 批量题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  const std::string pid_text = std::to_string(pid);

  constexpr int kThreads = 8;
  std::atomic<int> accepted_200{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      httplib::Client thread_cli = make_client(env.port());
      auto res = submit(thread_cli, u.token, pid_text, "cpp17",
                        "int main(){}");
      if (res && res->status == 200) {
        accepted_200.fetch_add(1);
      }
    });
  }
  // 在批量提交进行中触发停止。
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    env.stop();
  });
  for (auto &t : threads) {
    t.join();
  }
  stopper.join();

  const std::int64_t rows = count_rows(env.db(), "submissions");
  check(rows == accepted_200.load(),
        "每个获得 200 的已接收任务恰好落库一次（无丢失、无重复）");
  check(rows >= 1, "批量停止过程中至少有任务完成");
  check(no_leftover_children(), "停止后无遗留子进程");
  env.close_db();
}

// ---------------------------------------------------------------------------
// G. 失败路径反复执行：资源无持续增长、临时目录清理
// ---------------------------------------------------------------------------

void test_repeated_failures_no_resource_growth() {
  std::cout << "失败路径反复执行：判题目录清理、fd/线程/内存无持续增长\n";
  ScriptedExecutor ex;
  ex.run_output = "OK\n";
  TempDir dir("m35_res");
  Env env("m35_res_env", dir.db_path(), &ex, JudgeManager::Options(8, 2));
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m35_res", "M35Rs123");
  std::int64_t pid = insert_problem(env.db(), "M35 资源题");
  insert_testcase(env.db(), pid, 0, "", "OK\n");
  const std::string pid_text = std::to_string(pid);
  const std::string workspace_root = dir.sub("judge");

  // 预热一次，避免首次懒加载计入增长。
  {
    ex.compile_exit_code = 0;
    ex.compile_launch_error = false;
    submit(cli, u.token, pid_text, "cpp17", "warmup");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const int fds_before = count_open_fds();
  const long threads_before = read_proc_status_kb("Threads:");
  const long rss_before = read_proc_status_kb("VmRSS:");

  struct Mode {
    const char *name;
    const char *expected;
  };
  const Mode modes[] = {
      {"AC", "AC"},       {"WA", "WA"},     {"RE", "RE"},
      {"TLE", "TLE"},     {"MLE", "MLE"},   {"OUTPUT", "RE"},
      {"CE", "CE"},       {"SYSERR", "SYSERR"},
  };

  bool all_ok = true;
  const int kRounds = 3;
  for (int round = 0; round < kRounds; ++round) {
    for (const Mode &mode : modes) {
      const std::string m = mode.name;
      ex.compile_launch_error = false;
      ex.compile_sandbox_error = false;
      ex.compile_exit_code = 0;
      ex.compile_output.clear();
      ex.run_launch_error = false;
      ex.run_timeout = false;
      ex.run_memory_exceeded = false;
      ex.run_output_truncated = false;
      ex.run_output = "OK\n";
      ex.run_stderr.clear();
      ex.run_exit_code = 0;
      ex.run_memory_kb = 100;

      if (m == "WA") {
        ex.run_output = "BAD\n";
      } else if (m == "RE") {
        ex.run_exit_code = 1;
        ex.run_stderr = "crash";
      } else if (m == "TLE") {
        ex.run_timeout = true;
      } else if (m == "MLE") {
        ex.run_memory_exceeded = true;
      } else if (m == "OUTPUT") {
        ex.run_output_truncated = true;
      } else if (m == "CE") {
        ex.compile_exit_code = 1;
        ex.compile_output = "compile error";
      } else if (m == "SYSERR") {
        ex.compile_launch_error = true;
        ex.compile_sandbox_error = true;
      }

      auto res = submit(cli, u.token, pid_text, "cpp17", "//" + m);
      std::string got;
      if (res && res->status == 200) {
        got = json::parse(res->body).value("status", "");
      }
      if (got != mode.expected) {
        all_ok = false;
        std::cout << "    [INFO] 模式 " << m << " 期望 " << mode.expected
                  << " 实得 " << got << "\n";
      }
    }
  }
  check(all_ok, "AC/WA/RE/TLE/MLE/输出超限/CE/SYSERR 分类均正确");

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const int fds_after = count_open_fds();
  const long threads_after = read_proc_status_kb("Threads:");
  const long rss_after = read_proc_status_kb("VmRSS:");

  check(count_workspace_dirs(workspace_root) == 0,
        "失败路径反复执行后判题临时目录均已清理");
  // 允许少量波动（HTTP 连接池/线程栈等），但不应持续异常增长。
  check(fds_after >= 0 && fds_before >= 0 && fds_after - fds_before <= 12,
        "文件描述符无明显增长（前 " + std::to_string(fds_before) + " 后 " +
            std::to_string(fds_after) + "）");
  check(threads_after > 0 && threads_after - threads_before <= 8,
        "线程数无明显增长（前 " + std::to_string(threads_before) + " 后 " +
            std::to_string(threads_after) + "）");
  check(rss_after > 0 && rss_after - rss_before <= 128 * 1024,
        "内存无明显增长（前 " + std::to_string(rss_before / 1024) + " MiB 后 " +
            std::to_string(rss_after / 1024) + " MiB）");

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// H. 停止后重启：数据一致且可继续判题
// ---------------------------------------------------------------------------

void test_restart_after_stop_consistent() {
  std::cout << "停止后重启：已保存提交与状态一致，服务可继续判题\n";
  TempDir dir("m35_restart");
  const std::string dbpath = dir.db_path();
  std::int64_t uid = 0;
  std::int64_t pid = 0;
  std::int64_t first_id = 0;

  {
    ScriptedExecutor ex;
    ex.run_output = "2\n";
    Env env("m35_restart_a", dbpath, &ex);
    check(env.ok(), "首次启动成功");
    if (!env.ok()) {
      return;
    }
    httplib::Client cli = make_client(env.port());
    User u = make_user(cli, env.db(), "m35_restart", "M35Rt123");
    uid = u.id;
    pid = insert_problem(env.db(), "M35 重启题");
    insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
    auto res = submit(cli, u.token, std::to_string(pid), "cpp17", "first");
    check(res && res->status == 200 &&
              json::parse(res->body).value("status", "") == "AC",
          "首次提交 AC");
    if (res) {
      first_id = json::parse(res->body).value("id", 0LL);
    }
    env.stop();
    env.close_db();
  }

  {
    ScriptedExecutor ex;
    ex.run_output = "2\n";
    Env env("m35_restart_b", dbpath, &ex);
    check(env.ok(), "重启成功");
    if (!env.ok()) {
      return;
    }
    oj::SubmissionStore store(env.db());
    bool found = false;
    oj::SubmissionRecord rec;
    std::string err;
    check(store.find_by_id(first_id, found, rec, err) && found &&
              rec.status == "AC" && rec.source_code == "first",
          "重启后提交记录与源码一致");
    oj::UserProblemStatusStore statuses(env.db());
    bool has = false;
    oj::UserProblemStatusRecord status;
    statuses.find(uid, pid, has, status, err);
    check(has && status.accepted && status.submit_count == 1 &&
              status.has_first_ac_at,
          "重启后 AC 状态与首次 AC 时间保留");

    httplib::Client cli = make_client(env.port());
    const std::string account = [&] {
      std::string acct;
      oj::Statement stmt;
      env.db().prepare("SELECT account FROM users WHERE id = ?", stmt, err);
      stmt.bind(1, static_cast<sqlite3_int64>(uid));
      if (stmt.step() == SQLITE_ROW) {
        acct = stmt.column_text(0);
      }
      return acct;
    }();
    int login_status = 0;
    const std::string token = login(cli, account, "M35Rt123", login_status);
    check(login_status == 200 && !token.empty(), "重启后仍可登录");
    auto res = submit(cli, token, std::to_string(pid), "cpp17", "second");
    check(res && res->status == 200 &&
              json::parse(res->body).value("status", "") == "AC",
          "重启后可继续判题");
    statuses.find(uid, pid, has, status, err);
    check(has && status.submit_count == 2, "重启后提交次数继续累加");

    env.stop();
    env.close_db();
  }
}

} // namespace

int main() {
  std::cout << "==== M3.5 持久化与停止清理集成测试 ====\n";
  test_full_result_persisted_reopen();
  test_transaction_rollback();
  test_db_lock_contention_bounded();
  test_cancel_completion_race_single_final();
  test_manager_handler_invoked_once_under_race();
  test_client_disconnect_keeps_result();
  test_idle_stop_idempotent();
  test_stop_during_compile_and_reject_new();
  test_stop_during_result_saving_batch();
  test_repeated_failures_no_resource_growth();
  test_restart_after_stop_consistent();

  if (g_failures == 0) {
    std::cout << "\n==== M3.5 集成测试全部通过 ====\n";
    return 0;
  }
  std::cout << "\n==== M3.5 集成测试存在失败：" << g_failures << " 项 ====\n";
  return 1;
}
