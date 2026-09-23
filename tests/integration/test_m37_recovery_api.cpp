// M3.7 崩溃恢复与在途任务持久化集成测试。
//
// 使用 /tmp 下的隔离临时库、随机端口真实 HTTP 服务与可注入执行器，不触碰正式数据。
// 覆盖：
//   - 接收边界：任务在判题前持久化在途记录；结算前不计入 submissions / submit_count；
//   - 容量预留：队列满载时第 3 个请求 503 且不产生在途记录；
//   - 原子结算：同一在途任务重复结算被拒绝，仅一条提交、计数一次；
//   - 启动恢复：遗留 pending 任务重开库后重新入队判题并结算，保留原提交时间与首次 AC；
//   - 恢复幂等：已结算任务再次恢复不重复计数/AC；
//   - 恢复容量：队列暂满时等待而非丢弃，所有遗留任务最终结算；
//   - 无法判题（题目不存在）或主动放弃时标记 interrupted 并保留信息，不再恢复；
//   - 删题保护：存在未结算在途任务时 409；仅 interrupted 残留不阻塞且随删题清理；
//   - 旧库重开：既有终态提交不会被识别为待恢复任务；
//   - Rejudge 不读写在途表、不新增普通提交、不增加次数。
//
// 运行方式：ctest --test-dir build -R m37_recovery_api --output-on-failure
// 或直接执行 build/oj_m37_recovery_api_test。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "auth/jwt.h"
#include "db/database.h"
#include "db/in_flight.h"
#include "db/problem_admin.h"
#include "db/schema.h"
#include "db/submissions.h"
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

const std::string kTestSecret = "it-m37-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";
const std::string kAdminNewPassword = "AdminNewPass1";
constexpr auto kShort = std::chrono::milliseconds(5000);

oj::auth::JwtConfig make_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = kTestSecret;
  cfg.expires_seconds = 3600;
  return cfg;
}

// 可控执行器：编译成功即写出产物文件，运行返回可配置输出。不启动真实进程。
class FakeExecutor : public oj::judge::IExecutor {
public:
  std::string run_output = "2\n";
  int run_exit_code = 0;

  oj::judge::ProcessResult compile(
      const oj::judge::CompileRequest &request) override {
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    std::ofstream out(request.output_path, std::ios::binary);
    out << "fake-program";
    return result;
  }

  oj::judge::ProcessResult run(const oj::judge::RunRequest &,
                               const std::string &) override {
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = run_exit_code;
    result.stdout_data = run_output;
    result.time_ms = 1;
    return result;
  }
};

// 门控执行器：编译前进入同步门，直到 release()。用于制造确定的「判题中」窗口。
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
  std::string run_output = "2\n";

  oj::judge::ProcessResult compile(
      const oj::judge::CompileRequest &request) override {
    if (gate != nullptr) {
      gate->enter();
    }
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    std::ofstream out(request.output_path, std::ios::binary);
    out << "gated-program";
    return result;
  }

  oj::judge::ProcessResult run(const oj::judge::RunRequest &,
                               const std::string &) override {
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    result.stdout_data = run_output;
    result.time_ms = 1;
    return result;
  }
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

// 隔离临时库 + 随机端口真实 HTTP + 可选注入执行器 + 可配置调度器选项 + 可复用库路径。
class Env {
public:
  Env(const std::string &label, oj::judge::IExecutor *executor = nullptr,
      oj::judge::JudgeOptions judge_options = {},
      JudgeManager::Options manager_options = JudgeManager::Options(),
      const std::string &db_path = "")
      : dir_(label), db_path_override_(db_path) {
    std::string err;
    db_ = oj::Database::open(db_path_override_.empty() ? dir_.db_path()
                                                       : db_path_override_,
                             err);
    if (!db_) {
      return;
    }
    if (!oj::initialize_schema(*db_, kAdminPassword, err)) {
      return;
    }
    if (judge_options.workspace_root.empty()) {
      judge_options.workspace_root = dir_.db_path() + "_ws";
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
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
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
  std::string db_path_override_;
  std::unique_ptr<oj::Database> db_;
  std::unique_ptr<oj::HttpServer> server_;
  int port_ = 0;
  bool started_ = false;
};

// ---------------------------------------------------------------------------
// HTTP / DB 辅助
// ---------------------------------------------------------------------------

std::int64_t count_rows(oj::Database &db, const std::string &table) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT COUNT(*) FROM " + table, stmt, err)) {
    return -1;
  }
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t count_for(oj::Database &db, const std::string &table,
                       std::int64_t problem_id) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT COUNT(*) FROM " + table + " WHERE problem_id = ?",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, static_cast<sqlite3_int64>(problem_id));
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t user_id(oj::Database &db, const std::string &account) {
  std::string err;
  oj::Statement stmt;
  db.prepare("SELECT id FROM users WHERE account = ?", stmt, err);
  stmt.bind(1, account);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t submit_count_of(oj::Database &db, std::int64_t uid,
                             std::int64_t pid) {
  std::string err;
  oj::Statement stmt;
  db.prepare("SELECT submit_count FROM user_problem_status WHERE user_id = ? "
             "AND problem_id = ?",
             stmt, err);
  stmt.bind(1, static_cast<sqlite3_int64>(uid));
  stmt.bind(2, static_cast<sqlite3_int64>(pid));
  // 无状态记录等价于提交次数 0（尚未结算任何提交）。
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : 0;
}

std::int64_t insert_problem(oj::Database &db, const std::string &title,
                            int visible = 1) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("INSERT INTO problems (title, description, difficulty, tags, "
                  "visible, time_limit_ms) VALUES (?, '题面', 'easy', '测试', "
                  "?, 2000) RETURNING id",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, title);
  stmt.bind(2, visible);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

bool insert_testcase(oj::Database &db, std::int64_t problem_id,
                     const std::string &input, const std::string &output) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("INSERT INTO testcases (problem_id, ord, input, output, "
                  "is_sample) VALUES (?, 0, ?, ?, 0)",
                  stmt, err)) {
    return false;
  }
  stmt.bind(1, static_cast<sqlite3_int64>(problem_id));
  stmt.bind(2, input);
  stmt.bind(3, output);
  return stmt.step() == SQLITE_DONE;
}

// 直接写入一条 pending 在途任务，模拟「接收后、结算前崩溃」遗留的记录。
std::string insert_pending(oj::Database &db, std::int64_t uid, std::int64_t pid,
                           const std::string &submitted_at,
                           const std::string &source = "int main(){}") {
  oj::InFlightStore store(db);
  oj::InFlightTask task;
  task.user_id = uid;
  task.problem_id = pid;
  task.language = "cpp17";
  task.source_code = source;
  task.submitted_at = submitted_at;
  std::int64_t id = 0;
  std::string err;
  if (!store.insert(task, id, err)) {
    return "";
  }
  return task.task_id;
}

bool read_in_flight(oj::Database &db, const std::string &task_id,
                    oj::InFlightTask &out) {
  oj::InFlightStore store(db);
  bool found = false;
  std::string err;
  store.find_by_task_id(task_id, found, out, err);
  return found;
}

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

std::string admin_login(httplib::Client &cli) {
  int status = 0;
  std::string token = login(cli, "admin", kAdminPassword, status);
  if (token.empty()) {
    return "";
  }
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  auto res = cli.Post("/api/me/password", h,
                      "{\"old_password\":\"" + kAdminPassword +
                          "\",\"new_password\":\"" + kAdminNewPassword + "\"}",
                      "application/json");
  if (!res || res->status != 200) {
    return "";
  }
  return token;
}

httplib::Result submit(httplib::Client &cli, const std::string &token,
                       std::int64_t problem_id, const std::string &code) {
  json body;
  body["language"] = "cpp17";
  body["code"] = code;
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  const std::string path =
      "/api/problems/" + std::to_string(problem_id) + "/submit";
  return cli.Post(path.c_str(), h, body.dump(), "application/json");
}

std::string submit_status(httplib::Client &cli, const std::string &token,
                          std::int64_t problem_id, const std::string &code) {
  auto res = submit(cli, token, problem_id, code);
  if (!res || res->status != 200) {
    return "";
  }
  return json::parse(res->body).value("status", "");
}

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return predicate();
}

// ---------------------------------------------------------------------------
// T-001/T-002：接收即持久化、容量拒绝不落库
// ---------------------------------------------------------------------------

void test_receive_persists_before_settle() {
  std::cout << "接收边界：判题前持久化在途记录，结算前不计入统计\n";
  GatedExecutor executor;
  GatedExecutor::Gate gate;
  executor.gate = &gate;
  Env env("m37_recv", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "recv_user", "RecvPw123");
  std::int64_t pid = insert_problem(env.db(), "接收边界题");
  check(pid > 0 && insert_testcase(env.db(), pid, "1 1\n", "2\n"),
        "题目与用例就绪");

  int submit_http = 0;
  std::thread submitter([&]() {
    httplib::Client c = make_client(env.port());
    auto res = submit(c, user.token, pid, "int main(){}");
    submit_http = res ? res->status : -1;
  });
  check(gate.wait_entered(1, kShort), "任务进入判题阶段");
  check(count_rows(env.db(), "in_flight_tasks") == 1, "判题中在途记录为 1");
  check(count_rows(env.db(), "submissions") == 0, "判题中无最终提交记录");
  check(submit_count_of(env.db(), user.id, pid) == 0, "判题中提交次数为 0");

  gate.release();
  submitter.join();
  check(submit_http == 200, "提交返回 200");
  check(count_rows(env.db(), "in_flight_tasks") == 0, "结算后在途记录已删除");
  check(count_rows(env.db(), "submissions") == 1, "结算后仅一条提交记录");
  check(submit_count_of(env.db(), user.id, pid) == 1, "结算后提交次数为 1");
  env.close_db();
}

void test_queue_full_rejects_without_in_flight() {
  std::cout << "容量拒绝：队列满载第 3 个请求 503 且不产生在途记录\n";
  GatedExecutor executor;
  GatedExecutor::Gate gate;
  executor.gate = &gate;
  Env env("m37_full", &executor, {},
          JudgeManager::Options(/*capacity=*/1, /*workers=*/1));
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "full_user", "FullPw123");
  std::int64_t pid = insert_problem(env.db(), "满载题");
  insert_testcase(env.db(), pid, "1 1\n", "2\n");
  const std::string code = "int main(){}";

  std::thread a([&]() {
    httplib::Client c = make_client(env.port());
    submit(c, user.token, pid, code);
  });
  check(gate.wait_entered(1, kShort), "任务 A 开始执行");
  std::thread b([&]() {
    httplib::Client c = make_client(env.port());
    submit(c, user.token, pid, code);
  });
  check(wait_until([&]() { return env.server()->judge_manager()->queued_count() == 1; },
                   kShort),
        "任务 B 进入等待队列");

  httplib::Client third = make_client(env.port());
  auto rejected = submit(third, user.token, pid, code);
  check(rejected && rejected->status == 503, "任务 C 满载返回 503");
  if (rejected) {
    check(json::parse(rejected->body).value("code", "") == "JUDGE_QUEUE_FULL",
          "返回稳定错误码 JUDGE_QUEUE_FULL");
  }
  check(count_rows(env.db(), "in_flight_tasks") == 2,
        "被拒请求不产生在途记录（仅 A/B 两条）");
  check(count_rows(env.db(), "submissions") == 0, "被拒请求不产生提交记录");

  gate.release();
  a.join();
  b.join();
  check(count_rows(env.db(), "in_flight_tasks") == 0, "A/B 结算后在途清空");
  check(count_rows(env.db(), "submissions") == 2, "A/B 各持久化一次");
  check(submit_count_of(env.db(), user.id, pid) == 2, "计数为 2，未计入被拒请求");
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-005：同一在途任务只结算一次
// ---------------------------------------------------------------------------

void test_same_in_flight_settled_once() {
  std::cout << "原子结算：同一在途任务重复结算被拒绝且不产生第二条记录\n";
  FakeExecutor executor;
  Env env("m37_settle", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "settle_user", "SettlePw1");
  std::int64_t pid = insert_problem(env.db(), "结算题");
  insert_testcase(env.db(), pid, "1 1\n", "2\n");
  const std::string task_id =
      insert_pending(env.db(), user.id, pid, "2021-05-06 07:08:09");
  check(!task_id.empty(), "已写入在途记录");

  oj::submit::SubmitService service(env.db(), executor);
  auto first = service.submit(user.id, pid, "cpp17", "int main(){}", false,
                              "2021-05-06 07:08:09", nullptr, task_id);
  check(first.kind == oj::submit::SubmitService::Kind::Ok, "第一次结算成功");
  check(count_rows(env.db(), "submissions") == 1, "第一次结算产生 1 条提交");

  auto second = service.submit(user.id, pid, "cpp17", "int main(){}", false,
                               "2021-05-06 07:08:09", nullptr, task_id);
  check(second.kind == oj::submit::SubmitService::Kind::AlreadySettled,
        "第二次结算被拒绝（AlreadySettled）");
  check(count_rows(env.db(), "submissions") == 1, "未产生第二条提交记录");
  check(submit_count_of(env.db(), user.id, pid) == 1, "提交次数仍为 1");
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-006/T-007/T-008/T-009：启动恢复
// ---------------------------------------------------------------------------

void test_recovery_replays_and_preserves_time() {
  std::cout << "启动恢复：遗留任务重开库后重新判题，保留原提交时间与首次 AC\n";
  TempDir dir("m37_rec");
  const std::string original_time = "2020-01-02 03:04:05";
  std::int64_t uid = 0;
  std::int64_t pid = 0;
  std::string task_id;
  {
    FakeExecutor executor;
    Env env("m37_rec1", &executor, {}, JudgeManager::Options(), dir.db_path());
    check(env.ok(), "首次启动成功");
    httplib::Client cli = make_client(env.port());
    User user = make_user(cli, env.db(), "rec1_user", "RecPw123");
    uid = user.id;
    pid = insert_problem(env.db(), "恢复题");
    insert_testcase(env.db(), pid, "1 1\n", "2\n");
    task_id = insert_pending(env.db(), uid, pid, original_time);
    check(!task_id.empty(), "遗留 pending 在途记录已写入");
    check(count_rows(env.db(), "in_flight_tasks") == 1,
          "崩溃前遗留记录仍在");
    env.stop();
    env.close_db();
  }

  {
    FakeExecutor executor;
    Env env("m37_rec2", &executor, {}, JudgeManager::Options(), dir.db_path());
    check(env.ok(), "第二次启动成功");
    const std::size_t recovered = env.server()->recover_pending_tasks();
    check(recovered == 1, "恢复重新入队 1 个任务");
    check(wait_until([&]() { return count_rows(env.db(), "submissions") == 1; },
                     kShort),
          "恢复任务完成并持久化");
    check(count_rows(env.db(), "in_flight_tasks") == 0, "结算后在途记录删除");

    oj::SubmissionStore store(env.db());
    bool found = false;
    oj::SubmissionRecord record;
    std::string err;
    store.find_by_id(1, found, record, err);
    // 提交 ID 未必为 1（admin 不产生提交），用仅有一条记录的方式读取。
    if (!found) {
      std::string e2;
      oj::Statement stmt;
      env.db().prepare("SELECT id FROM submissions LIMIT 1", stmt, e2);
      if (stmt.step() == SQLITE_ROW) {
        store.find_by_id(stmt.column_int64(0), found, record, err);
      }
    }
    check(found, "可读取恢复产生的提交记录");
    if (found) {
      check(record.status == "AC", "恢复结果按实际判题为 AC");
      check(record.created_at == original_time,
            "恢复提交保留原提交时间，未用重启时间覆盖");
      check(record.source_code == "int main(){}", "恢复使用已保存的源码");
    }
    check(submit_count_of(env.db(), uid, pid) == 1, "恢复后计数为 1");

    // 首次 AC 时间取原提交时间。
    std::string e3;
    oj::Statement stmt;
    env.db().prepare("SELECT first_ac_at, status FROM user_problem_status WHERE "
                     "user_id = ? AND problem_id = ?",
                     stmt, e3);
    stmt.bind(1, static_cast<sqlite3_int64>(uid));
    stmt.bind(2, static_cast<sqlite3_int64>(pid));
    bool has = stmt.step() == SQLITE_ROW;
    check(has, "存在用户题目状态记录");
    if (has) {
      check(stmt.column_text(0) == original_time,
            "首次 AC 时间为原提交时间");
      check(stmt.column_text(1) == "accepted", "状态为 accepted");
    }

    // 幂等：再次恢复不重复计数。
    const std::size_t again = env.server()->recover_pending_tasks();
    check(again == 0, "已结算任务不再被恢复");
    check(count_rows(env.db(), "submissions") == 1, "提交记录数仍为 1");
    check(submit_count_of(env.db(), uid, pid) == 1, "提交次数仍为 1");
    env.close_db();
  }
}

void test_recovery_waits_for_capacity_without_drop() {
  std::cout << "恢复容量：队列暂满时等待而非丢弃，遗留任务最终全部结算\n";
  GatedExecutor executor;
  GatedExecutor::Gate gate;
  executor.gate = &gate;
  Env env("m37_reccap", &executor, {},
          JudgeManager::Options(/*capacity=*/1, /*workers=*/1));
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  User user = make_user(cli, env.db(), "cap_user", "CapPw123");
  std::int64_t pid = insert_problem(env.db(), "恢复容量题");
  insert_testcase(env.db(), pid, "1 1\n", "2\n");

  // 3 条遗留任务：capacity=1、workers=1 时恢复必须等待容量而不能丢弃。
  for (int i = 0; i < 3; ++i) {
    check(!insert_pending(env.db(), user.id, pid, "2022-01-0" +
                                                      std::to_string(i + 1) +
                                                      " 00:00:00")
               .empty(),
          "写入遗留任务 " + std::to_string(i + 1));
  }

  std::atomic<std::size_t> recovered{0};
  std::thread recovery([&]() {
    recovered.store(env.server()->recover_pending_tasks());
  });
  check(gate.wait_entered(1, kShort), "第一条恢复任务开始执行");
  check(count_rows(env.db(), "submissions") == 0, "等待期间尚未结算");
  gate.release();
  recovery.join();

  check(recovered.load() == 3, "三条遗留任务全部重新入队（无丢弃）");
  check(wait_until([&]() { return count_rows(env.db(), "submissions") == 3; },
                   kShort),
        "三条遗留任务全部结算");
  check(count_rows(env.db(), "in_flight_tasks") == 0, "恢复后在途清空");
  check(submit_count_of(env.db(), user.id, pid) == 3, "计数为 3");
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-010：无法判题 / 主动放弃 -> interrupted
// ---------------------------------------------------------------------------

void test_missing_problem_marks_interrupted() {
  std::cout << "无法判题：题目不存在时在途任务标记为中断并保留信息\n";
  FakeExecutor executor;
  Env env("m37_intr", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  User user = make_user(cli, env.db(), "intr_user", "IntrPw123");
  std::int64_t pid = insert_problem(env.db(), "中断题");
  const std::string task_id =
      insert_pending(env.db(), user.id, pid, "2023-03-03 03:03:03");
  check(!task_id.empty(), "在途记录已写入");

  // 以不存在的题目 ID 结算：应标记中断而非写入提交。
  oj::submit::SubmitService service(env.db(), executor);
  auto outcome = service.submit(user.id, /*nonexistent=*/999999, "cpp17",
                                "int main(){}", false, "2023-03-03 03:03:03",
                                nullptr, task_id);
  check(outcome.kind == oj::submit::SubmitService::Kind::ProblemNotFound,
        "结算返回题目不存在");

  oj::InFlightTask loaded;
  check(read_in_flight(env.db(), task_id, loaded), "在途记录仍保留");
  check(loaded.state == "interrupted", "状态为 interrupted");
  check(!loaded.reason.empty(), "记录了中断原因");
  check(count_rows(env.db(), "submissions") == 0, "不产生最终提交记录");
  check(env.server()->recover_pending_tasks() == 0, "中断任务不再被恢复");
  env.close_db();
}

void test_discard_in_flight_marks_interrupted() {
  std::cout << "主动放弃：discard 将未入队在途任务标记为中断\n";
  FakeExecutor executor;
  Env env("m37_disc", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  User user = make_user(cli, env.db(), "disc_user", "DiscPw1");
  std::int64_t pid = insert_problem(env.db(), "放弃题");
  const std::string task_id =
      insert_pending(env.db(), user.id, pid, "2023-04-04 04:04:04");
  check(!task_id.empty(), "在途记录已写入");

  oj::submit::SubmitService service(env.db(), executor);
  service.discard_in_flight(task_id);

  oj::InFlightTask loaded;
  check(read_in_flight(env.db(), task_id, loaded), "在途记录仍保留");
  check(loaded.state == "interrupted", "状态为 interrupted");
  check(env.server()->recover_pending_tasks() == 0, "被放弃任务不再恢复");
  check(count_rows(env.db(), "submissions") == 0, "不产生提交记录");
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-017/T-018：删题保护与 interrupted 清理
// ---------------------------------------------------------------------------

void test_delete_blocked_by_in_flight() {
  std::cout << "删题保护：存在未结算在途任务时拒绝删除（409）\n";
  FakeExecutor executor;
  Env env("m37_del", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");
  User user = make_user(cli, env.db(), "del_user", "DelPw123");

  auto create = cli.Post("/api/admin/problems",
                         httplib::Headers{{"Authorization", "Bearer " + admin}},
                         R"({"title":"删题保护题","difficulty":"easy"})",
                         "application/json");
  check(create && create->status == 201, "创建题目成功");
  std::int64_t pid = create ? json::parse(create->body).value("id", -1LL) : -1;
  check(pid > 0, "题目 ID 有效");

  const std::string task_id =
      insert_pending(env.db(), user.id, pid, "2024-01-01 00:00:00");
  check(!task_id.empty(), "写入未结算在途记录");

  auto del = cli.Delete(("/api/admin/problems/" + std::to_string(pid)).c_str(),
                        httplib::Headers{{"Authorization", "Bearer " + admin}});
  check(del && del->status == 409, "有在途任务时删除返回 409");
  check(count_rows(env.db(), "problems") == 1, "题目未被删除");
  check(count_rows(env.db(), "in_flight_tasks") == 1, "在途记录仍保留");

  // 清理在途记录后删除应成功。
  oj::InFlightStore store(env.db());
  bool removed = false;
  std::string err;
  store.remove_by_task_id(task_id, removed, err);
  check(removed, "清理在途记录");
  auto del2 = cli.Delete(("/api/admin/problems/" + std::to_string(pid)).c_str(),
                         httplib::Headers{{"Authorization", "Bearer " + admin}});
  check(del2 && del2->status == 200, "无在途任务后删除成功");
  check(count_rows(env.db(), "problems") == 0, "题目已删除");
  env.close_db();
}

void test_delete_clears_interrupted_residue() {
  std::cout << "删题清理：仅 interrupted 残留不阻塞删除并随题清理\n";
  FakeExecutor executor;
  Env env("m37_delintr", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  User user = make_user(cli, env.db(), "delintr", "DelIntr1");

  auto create = cli.Post("/api/admin/problems",
                         httplib::Headers{{"Authorization", "Bearer " + admin}},
                         R"({"title":"中断残留题","difficulty":"easy"})",
                         "application/json");
  std::int64_t pid = create ? json::parse(create->body).value("id", -1LL) : -1;
  check(pid > 0, "题目 ID 有效");

  const std::string task_id =
      insert_pending(env.db(), user.id, pid, "2024-02-02 00:00:00");
  oj::InFlightStore store(env.db());
  std::string err;
  check(store.mark_interrupted(task_id, "无法判题", err), "标记中断");
  check(count_rows(env.db(), "in_flight_tasks") == 1, "存在中断残留记录");

  auto del = cli.Delete(("/api/admin/problems/" + std::to_string(pid)).c_str(),
                        httplib::Headers{{"Authorization", "Bearer " + admin}});
  check(del && del->status == 200, "中断残留不阻塞删除");
  check(count_rows(env.db(), "problems") == 0, "题目已删除");
  check(count_rows(env.db(), "in_flight_tasks") == 0, "中断残留已随题清理");
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-019：旧库重开，既有终态不被误识别
// ---------------------------------------------------------------------------

void test_existing_terminal_not_recovered() {
  std::cout << "迁移/重开：既有终态提交不会被识别为待恢复任务\n";
  TempDir dir("m37_mig");
  std::int64_t uid = 0;
  std::int64_t pid = 0;
  {
    FakeExecutor executor;
    Env env("m37_mig1", &executor, {}, JudgeManager::Options(), dir.db_path());
    check(env.ok(), "首次启动成功");
    httplib::Client cli = make_client(env.port());
    User user = make_user(cli, env.db(), "mig_user", "MigPw123");
    uid = user.id;
    pid = insert_problem(env.db(), "迁移题");
    insert_testcase(env.db(), pid, "1 1\n", "2\n");
    check(submit_status(cli, user.token, pid, "int main(){}") == "AC",
          "首次提交 AC");
    check(count_rows(env.db(), "submissions") == 1, "已保存 1 条终态提交");
    check(count_rows(env.db(), "in_flight_tasks") == 0, "无在途记录");
    env.stop();
    env.close_db();
  }
  {
    FakeExecutor executor;
    Env env("m37_mig2", &executor, {}, JudgeManager::Options(), dir.db_path());
    check(env.ok(), "重开成功");
    check(env.server()->recover_pending_tasks() == 0, "无可恢复任务");
    check(count_rows(env.db(), "submissions") == 1, "终态提交仍为 1 条");
    check(submit_count_of(env.db(), uid, pid) == 1, "计数不变");
    env.close_db();
  }
}

// ---------------------------------------------------------------------------
// T-020：Rejudge 不读写在途表、不增次数
// ---------------------------------------------------------------------------

void test_rejudge_no_in_flight_and_no_count_change() {
  std::cout << "Rejudge：不产生在途记录、不新增普通提交、不增加次数\n";
  FakeExecutor executor;
  executor.run_output = "2\n";
  Env env("m37_rejudge", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  User user = make_user(cli, env.db(), "rej_user", "RejPw123");

  auto create = cli.Post("/api/admin/problems",
                         httplib::Headers{{"Authorization", "Bearer " + admin}},
                         R"({"title":"重判题","difficulty":"easy"})",
                         "application/json");
  std::int64_t pid = create ? json::parse(create->body).value("id", -1LL) : -1;
  check(pid > 0, "创建题目成功");
  insert_testcase(env.db(), pid, "1 1\n", "2\n");

  check(submit_status(cli, user.token, pid, "int main(){}") == "AC",
        "提交 AC");
  std::int64_t sid = -1;
  {
    std::string err;
    oj::Statement stmt;
    env.db().prepare("SELECT id FROM submissions LIMIT 1", stmt, err);
    if (stmt.step() == SQLITE_ROW) {
      sid = stmt.column_int64(0);
    }
  }
  check(sid > 0, "取得提交 ID");

  auto rej = cli.Post(
      ("/api/admin/submissions/" + std::to_string(sid) + "/rejudge").c_str(),
      httplib::Headers{{"Authorization", "Bearer " + admin}}, "{}",
      "application/json");
  check(rej && rej->status == 200, "重判成功");
  check(count_rows(env.db(), "in_flight_tasks") == 0, "重判不产生在途记录");
  check(count_rows(env.db(), "submissions") == 1, "重判不新增普通提交");
  check(submit_count_of(env.db(), user.id, pid) == 1, "重判不增加提交次数");
  env.close_db();
}

} // namespace

int main() {
  test_receive_persists_before_settle();
  test_queue_full_rejects_without_in_flight();
  test_same_in_flight_settled_once();
  test_recovery_replays_and_preserves_time();
  test_recovery_waits_for_capacity_without_drop();
  test_missing_problem_marks_interrupted();
  test_discard_in_flight_marks_interrupted();
  test_delete_blocked_by_in_flight();
  test_delete_clears_interrupted_residue();
  test_existing_terminal_not_recovered();
  test_rejudge_no_in_flight_and_no_count_change();

  std::cout << "\n==== M3.7 恢复集成测试"
            << (g_failures == 0 ? "全部通过" : "存在失败") << "（失败 "
            << g_failures << " 项）====\n";
  return g_failures == 0 ? 0 : 1;
}
