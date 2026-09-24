// M5.2 数据访问与权限回归补充集成测试。
//
// 目标：在既有测试（admin_problems_api / admin_testcases_api / admin_users_api /
// rejudge_api / submit_api / problems_api / problem_list_query / m44_history_api /
// m45_leaderboard_api 等）已覆盖的基础上，补齐「身份 → 操作 → 数据对象 → 允许/拒绝」
// 访问矩阵中的系统性缺口与跨接口一致性问题。使用隔离临时库 + 随机端口 + 专用测试
// 密钥 + 可控执行器（FakeExecutor / GatedExecutor），不触碰正式数据库、不进行破坏性
// 压力测试、不启动 M5.3 的沙箱压力场景。
//
// 覆盖（与既有测试的分工见 tests/M5.2-test-report.md）：
//   T-001 全部 10 个管理员入口的四类身份矩阵（游客 401 / 普通用户 403 /
//         未完成首改管理员 403+PASSWORD_CHANGE_REQUIRED / 有权限管理员成功），
//         补齐 PUT /api/admin/problems/{id} 未改密管理员与 PUT /api/admin/users 游客缺口。
//   T-002 提交归属：他人直接按 ID 取详情 404 且响应不泄露源码/编译/WA 详情；
//         user_id/account/role/mine 参数不能扩大范围；未改密管理员视为非管理员；
//         改密后管理员可读他人记录。
//   T-003 题目可见性：隐藏题列表/详情/提交 404，visible 参数与角色伪造不能绕过；
//         总数一致；标签选项不泄露仅隐藏题目使用的标签。
//   T-004 可见性变化后的历史保留：本人历史/详情/状态保留，题面与新提交按规则拦截，
//         隐藏用例不通过历史接口泄露。
//   T-005 用例归属与字段控制：A 题路径 + B 题用例 ID 不误操作；problem_id/is_sample/id
//         客户端字段被忽略；隐藏用例不能变为公开样例。
//   T-006 非法/不存在对象：缺失/负数/零/越界/溢出/非法格式 ID；非法 JSON；超长请求体；
//         错误响应不泄露 SQL/路径；失败操作不产生部分写入。
//   T-007 写入字段控制与权限撤销：提交/建题/重判的 user_id/role/status/created_at
//         等服务端字段不可由客户端控制；角色撤销后旧 token 立即失去管理能力。
//   T-008 删除题目：有提交 409；无提交在同一路径清理关联数据且不误删其它题目/学生历史；
//         存在未结算在途任务 409。
//   T-009 用例增删改一致性：ord 顺序即判题顺序、公开样例不受影响、历史结果不被改写、
//         新提交使用修改后的用例。
//   T-010 Rejudge：只能由合格管理员发起、目标归属/源码不被请求篡改、原记录更新且不增加
//         次数、AC/首次 AC/通过人数/排行榜统计随重判一致、失败不产生部分更新。
//   T-011 受控并发：Rejudge 与新提交并发、删题与提交结算并发，断言关联约束与统计结果。

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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "auth/jwt.h"
#include "db/database.h"
#include "db/schema.h"
#include "http/server.h"
#include "judge/executor.h"

namespace {

using nlohmann::json;

int g_failures = 0;

void check(bool cond, const std::string &msg) {
  if (cond) {
    std::cout << "  [PASS] " << msg << "\n";
  } else {
    std::cout << "  [FAIL] " << msg << "\n";
    ++g_failures;
  }
}

const std::string kTestSecret = "it-m52-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";
const std::string kAdminNewPassword = "AdminNewPass1";

oj::auth::JwtConfig make_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = kTestSecret;
  cfg.expires_seconds = 3600;
  return cfg;
}

// 可控执行器：编译恒成功，运行输出固定；用于在不起真实编译/沙箱的前提下验证
// HTTP 数据访问与统计一致性（不属于 M5.3 沙箱压力场景）。
class FakeExecutor : public oj::judge::IExecutor {
public:
  std::string run_output = "2\n";

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
    result.exit_code = 0;
    result.stdout_data = run_output;
    result.time_ms = 1;
    return result;
  }
};

// 同步门执行器：gate 非空时阻塞编译阶段，用于受控并发场景。
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
    out << "fake-program";
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

// 隔离临时库 + 随机端口真实 HTTP + 可选注入执行器。
class Env {
public:
  Env(const std::string &label, oj::judge::IExecutor *executor = nullptr,
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
    oj::judge::JudgeOptions judge_options;
    judge_options.workspace_root = dir_.sub("judge");
    port_ = find_free_port();
    server_ = std::make_unique<oj::HttpServer>(
        "127.0.0.1", port_, *db_, make_config(),
        /*enable_test_routes=*/false, executor, judge_options,
        /*web_root=*/"");
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
  std::string db_path_override_;
  std::unique_ptr<oj::Database> db_;
  std::unique_ptr<oj::HttpServer> server_;
  int port_ = 0;
  bool started_ = false;
};

// ---------------------------------------------------------------------------
// 数据库辅助
// ---------------------------------------------------------------------------

long long scalar(oj::Database &db, const std::string &sql) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare(sql, stmt, err)) {
    return -1;
  }
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

long long scalar1(oj::Database &db, const std::string &sql,
                  long long a) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare(sql, stmt, err)) {
    return -1;
  }
  if (!stmt.bind(1, static_cast<sqlite3_int64>(a))) {
    return -1;
  }
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

long long scalar2(oj::Database &db, const std::string &sql, long long a,
                  long long b) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare(sql, stmt, err)) {
    return -1;
  }
  if (!stmt.bind(1, static_cast<sqlite3_int64>(a)) ||
      !stmt.bind(2, static_cast<sqlite3_int64>(b))) {
    return -1;
  }
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::string text1(oj::Database &db, const std::string &sql, long long a) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare(sql, stmt, err)) {
    return "";
  }
  if (!stmt.bind(1, static_cast<sqlite3_int64>(a))) {
    return "";
  }
  return stmt.step() == SQLITE_ROW ? stmt.column_text(0) : "";
}

std::int64_t user_id(oj::Database &db, const std::string &account) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT id FROM users WHERE account = ?", stmt, err)) {
    return -1;
  }
  stmt.bind(1, account);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t insert_problem(oj::Database &db, const std::string &title,
                            int visible = 1, const std::string &tags = "测试") {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("INSERT INTO problems (title, description, difficulty, tags, "
                  "visible) VALUES (?, '题面', 'easy', ?, ?) RETURNING id",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, title);
  stmt.bind(2, tags);
  stmt.bind(3, visible);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t insert_testcase(oj::Database &db, std::int64_t problem_id, int ord,
                             const std::string &input,
                             const std::string &output, bool is_sample) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("INSERT INTO testcases (problem_id, ord, input, output, "
                  "is_sample) VALUES (?, ?, ?, ?, ?) RETURNING id",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, static_cast<sqlite3_int64>(problem_id));
  stmt.bind(2, ord);
  stmt.bind(3, input);
  stmt.bind(4, output);
  stmt.bind(5, is_sample ? 1 : 0);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t insert_submission(oj::Database &db, std::int64_t uid,
                               std::int64_t pid, const std::string &language,
                               const std::string &source,
                               const std::string &status,
                               const std::string &created_at) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare(
          "INSERT INTO submissions (user_id, problem_id, language, "
          "source_code, status, per_case, compile_msg, runtime_ms, memory_kb, "
          "created_at) VALUES (?, ?, ?, ?, ?, '[]', '', 0, 0, ?) RETURNING id",
          stmt, err)) {
    return -1;
  }
  stmt.bind(1, static_cast<sqlite3_int64>(uid));
  stmt.bind(2, static_cast<sqlite3_int64>(pid));
  stmt.bind(3, language);
  stmt.bind(4, source);
  stmt.bind(5, status);
  stmt.bind(6, created_at);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

bool insert_in_flight(oj::Database &db, std::int64_t uid, std::int64_t pid,
                      const std::string &task_id, const std::string &state) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("INSERT INTO in_flight_tasks (task_id, user_id, problem_id, "
                  "language, source_code, submitted_at, state) VALUES (?, ?, ?, "
                  "'cpp17', 'x', '2020-01-01T00:00:00Z', ?)",
                  stmt, err)) {
    return false;
  }
  stmt.bind(1, task_id);
  stmt.bind(2, static_cast<sqlite3_int64>(uid));
  stmt.bind(3, static_cast<sqlite3_int64>(pid));
  stmt.bind(4, state);
  return stmt.step() == SQLITE_DONE;
}

std::int64_t count_submissions(oj::Database &db, std::int64_t uid,
                               std::int64_t pid) {
  return scalar2(db,
                 "SELECT COUNT(*) FROM submissions WHERE user_id = ? AND "
                 "problem_id = ?",
                 uid, pid);
}

std::int64_t count_testcases(oj::Database &db, std::int64_t pid) {
  return scalar1(db, "SELECT COUNT(*) FROM testcases WHERE problem_id = ?", pid);
}

std::int64_t count_status(oj::Database &db, std::int64_t pid) {
  return scalar1(db,
                 "SELECT COUNT(*) FROM user_problem_status WHERE problem_id = ?",
                 pid);
}

std::int64_t count_in_flight(oj::Database &db, std::int64_t pid) {
  return scalar1(db,
                 "SELECT COUNT(*) FROM in_flight_tasks WHERE problem_id = ?",
                 pid);
}

std::int64_t testcase_id(oj::Database &db, std::int64_t pid, bool is_sample) {
  return scalar2(db,
                 "SELECT id FROM testcases WHERE problem_id = ? AND is_sample "
                 "= ? ORDER BY id ASC LIMIT 1",
                 pid, is_sample ? 1 : 0);
}

std::string testcase_input(oj::Database &db, std::int64_t tid) {
  return text1(db, "SELECT input FROM testcases WHERE id = ?", tid);
}

// ---------------------------------------------------------------------------
// HTTP 辅助
// ---------------------------------------------------------------------------

json parse_json(const httplib::Result &res) {
  if (!res || res->body.empty()) {
    return json::object();
  }
  try {
    return json::parse(res->body);
  } catch (const std::exception &) {
    return json::object();
  }
}

httplib::Result api_get(httplib::Client &cli, const std::string &path,
                        const std::string &token) {
  if (token.empty()) {
    return cli.Get(path.c_str());
  }
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Get(path.c_str(), h);
}

httplib::Result api_post(httplib::Client &cli, const std::string &path,
                         const std::string &token, const std::string &body) {
  if (token.empty()) {
    return cli.Post(path.c_str(), body, "application/json");
  }
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Post(path.c_str(), h, body, "application/json");
}

httplib::Result api_put(httplib::Client &cli, const std::string &path,
                        const std::string &token, const std::string &body) {
  if (token.empty()) {
    return cli.Put(path.c_str(), body, "application/json");
  }
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Put(path.c_str(), h, body, "application/json");
}

httplib::Result api_delete(httplib::Client &cli, const std::string &path,
                           const std::string &token) {
  if (token.empty()) {
    return cli.Delete(path.c_str());
  }
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Delete(path.c_str(), h);
}

std::string register_user(httplib::Client &cli, const std::string &nickname,
                          const std::string &password) {
  json body{{"nickname", nickname}, {"password", password}};
  auto res = api_post(cli, "/api/register", "", body.dump());
  return res && res->status == 201 ? parse_json(res).value("account", "") : "";
}

std::string login(httplib::Client &cli, const std::string &account,
                  const std::string &password, int *status = nullptr) {
  json body{{"account", account}, {"password", password}};
  auto res = api_post(cli, "/api/login", "", body.dump());
  if (status != nullptr) {
    *status = res ? res->status : -1;
  }
  return res && res->status == 200 ? parse_json(res).value("token", "") : "";
}

httplib::Result change_password(httplib::Client &cli, const std::string &token,
                                const std::string &old_password,
                                const std::string &new_password) {
  json body{{"old_password", old_password}, {"new_password", new_password}};
  return api_post(cli, "/api/me/password", token, body.dump());
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
  user.token = login(cli, user.account, pw, &status);
  user.id = user_id(db, user.account);
  return user;
}

// 登录预置 admin（此时 reset_pwd_flag=1）。返回原始 token，可用于未改密身份断言。
std::string admin_fresh_token(httplib::Client &cli) {
  int status = 0;
  return login(cli, "admin", kAdminPassword, &status);
}

httplib::Result submit(httplib::Client &cli, const std::string &token,
                       std::int64_t problem_id, const std::string &language,
                       const std::string &code) {
  json body{{"language", language}, {"code", code}};
  return api_post(cli, "/api/problems/" + std::to_string(problem_id) + "/submit",
                  token, body.dump());
}

httplib::Result rejudge(httplib::Client &cli, const std::string &token,
                        std::int64_t submission_id) {
  return api_post(cli,
                  "/api/admin/submissions/" + std::to_string(submission_id) +
                      "/rejudge",
                  token, "{}");
}

long long pass_count(httplib::Client &cli, const std::string &admin_token,
                     std::int64_t problem_id) {
  auto res = api_get(cli, "/api/problems?visible=all&page=1&page_size=100",
                     admin_token);
  if (!res || res->status != 200) {
    return -1;
  }
  json body = parse_json(res);
  for (const auto &p : body["problems"]) {
    if (p.value("id", 0LL) == problem_id) {
      return p.value("pass_count", -1LL);
    }
  }
  return -1;
}

long long leaderboard_field(httplib::Client &cli, std::int64_t user_id,
                            const std::string &field) {
  auto res = api_get(cli, "/api/leaderboard?page=1", "");
  if (!res || res->status != 200) {
    return -1;
  }
  json body = parse_json(res);
  for (const auto &entry : body["leaderboard"]) {
    if (entry.value("user_id", 0LL) == user_id) {
      const auto &v = entry[field];
      if (v.is_null()) {
        return -2; // null 标记
      }
      return v.get<long long>();
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// T-001 管理员入口权限矩阵
// ---------------------------------------------------------------------------

void test_admin_endpoint_permission_matrix() {
  std::cout << "T-001 全部管理员入口的四类身份权限矩阵\n";
  FakeExecutor fake;
  Env env("m52_matrix", &fake);
  check(env.ok(), "T-001 服务启动成功");
  httplib::Client cli = make_client(env.port());

  User normal = make_user(cli, env.db(), "matrix_user", "MatrixPw1");
  check(!normal.token.empty(), "T-001 普通用户注册登录就绪");

  // 直接用数据库构造真实存在的目标对象，避免用管理员 token 参与准备。
  std::int64_t pid = insert_problem(env.db(), "矩阵题", 1);
  std::int64_t tid = insert_testcase(env.db(), pid, 0, "in\n", "2\n", false);
  std::int64_t tid_del =
      insert_testcase(env.db(), pid, 1, "del\n", "2\n", false);
  std::int64_t pid_del = insert_problem(env.db(), "待删矩阵题", 1);
  std::int64_t sid = insert_submission(env.db(), normal.id, pid, "cpp17",
                                       "src", "AC", "2020-01-01T00:00:00Z");
  check(pid > 0 && tid > 0 && tid_del > 0 && pid_del > 0 && sid > 0,
        "T-001 目标对象构造成功");

  std::string fresh = admin_fresh_token(cli);
  check(!fresh.empty(), "T-001 未改密管理员取得 token");

  struct EP {
    std::string method;
    std::string path;
    std::string body;
    int ok_status;
    std::string name;
  };
  std::vector<EP> eps = {
      {"POST", "/api/admin/problems",
       R"({"title":"矩阵新建题","difficulty":"easy"})", 201, "建题"},
      {"PUT", "/api/admin/problems/" + std::to_string(pid),
       R"({"visible":true})", 200, "改题"},
      {"DELETE", "/api/admin/problems/" + std::to_string(pid_del), "", 200,
       "删题"},
      {"GET", "/api/admin/problems/" + std::to_string(pid) + "/testcases", "",
       200, "读用例"},
      {"POST", "/api/admin/problems/" + std::to_string(pid) + "/testcases",
       R"({"input":"a\n","output":"b\n"})", 201, "增用例"},
      {"PUT",
       "/api/admin/problems/" + std::to_string(pid) + "/testcases/" +
           std::to_string(tid),
       R"({"input":"c\n"})", 200, "改用例"},
      {"DELETE",
       "/api/admin/problems/" + std::to_string(pid) + "/testcases/" +
           std::to_string(tid_del),
       "", 200, "删用例"},
      {"GET", "/api/admin/users", "", 200, "用户列表"},
      {"PUT", "/api/admin/users",
       "{\"action\":\"change_role\",\"user_id\":" + std::to_string(normal.id) +
           ",\"role\":\"user\"}",
       200, "用户操作"},
      {"POST",
       "/api/admin/submissions/" + std::to_string(sid) + "/rejudge", "{}", 200,
       "重判"},
  };

  auto call = [&](const EP &ep, const std::string &token) {
    if (ep.method == "GET") {
      return api_get(cli, ep.path, token);
    }
    if (ep.method == "POST") {
      return api_post(cli, ep.path, token, ep.body);
    }
    if (ep.method == "PUT") {
      return api_put(cli, ep.path, token, ep.body);
    }
    return api_delete(cli, ep.path, token);
  };

  // 游客：401；普通用户：403；未改密管理员：403 + PASSWORD_CHANGE_REQUIRED。
  for (const EP &ep : eps) {
    auto g = call(ep, "");
    check(g && g->status == 401, "T-001 游客 " + ep.name + " -> 401");

    auto n = call(ep, normal.token);
    check(n && n->status == 403, "T-001 普通用户 " + ep.name + " -> 403");

    auto f = call(ep, fresh);
    check(f && f->status == 403,
          "T-001 未改密管理员 " + ep.name + " -> 403");
    check(f && parse_json(f).value("code", "") == "PASSWORD_CHANGE_REQUIRED",
          "T-001 未改密管理员 " + ep.name + " 返回 PASSWORD_CHANGE_REQUIRED");

    // S-003：伪造/无效 Bearer 不得被当成任何身份，统一 401。
    auto fd = call(ep, "forged.invalid.token");
    check(fd && fd->status == 401,
          "S-003 伪造 token " + ep.name + " -> 401");
  }

  // 完成首次改密后同一 token 立即获得管理能力（权限按数据库最新状态判定）。
  auto changed = change_password(cli, fresh, kAdminPassword, kAdminNewPassword);
  check(changed && changed->status == 200, "T-001 管理员完成首次改密");
  auto me = api_get(cli, "/api/me", fresh);
  check(me && me->status == 200 &&
            parse_json(me).value("reset_pwd_flag", 1) == 0,
        "T-001 改密后 reset_pwd_flag=0");

  for (const EP &ep : eps) {
    auto r = call(ep, fresh);
    check(r && r->status == ep.ok_status,
          "T-001 有权限管理员 " + ep.name + " -> " +
              std::to_string(ep.ok_status));
  }

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-002 提交归属与隐私
// ---------------------------------------------------------------------------

void test_submission_ownership_privacy() {
  std::cout << "T-002 提交归属：跨用户详情 404 且不泄露，参数不能扩大范围\n";
  FakeExecutor fake;
  fake.run_output = "USER_ACTUAL_A\n";
  Env env("m52_owner", &fake);
  check(env.ok(), "T-002 服务启动成功");
  httplib::Client cli = make_client(env.port());

  User alice = make_user(cli, env.db(), "owner_alice", "AlicePw1");
  User bob = make_user(cli, env.db(), "owner_bob", "BobPw1");
  std::int64_t pid = insert_problem(env.db(), "归属题", 1);
  insert_testcase(env.db(), pid, 0, "SECRET_INPUT_A\n", "MAGIC_EXPECTED_A\n",
                  false);

  const std::string alice_source = "// SRC_A_MARKER_9f3\nint main(){}";
  auto a = submit(cli, alice.token, pid, "cpp17", alice_source);
  check(a && a->status == 200, "T-002 Alice 提交成功");
  std::int64_t sid = parse_json(a).value("id", 0LL);
  check(sid > 0 && parse_json(a).value("status", "") == "WA", "T-002 WA 提交");

  // Bob 直接按 ID 访问 Alice 的提交：404，且响应不包含 Alice 的敏感数据。
  auto b = api_get(cli, "/api/submissions/" + std::to_string(sid), bob.token);
  check(b && b->status == 404, "T-002 他人访问提交详情 -> 404");
  std::string braw = b ? b->body : "";
  check(braw.find("SRC_A_MARKER_9f3") == std::string::npos,
        "T-002 404 响应不含他人源码");
  check(braw.find("MAGIC_EXPECTED_A") == std::string::npos &&
            braw.find("SECRET_INPUT_A") == std::string::npos,
        "T-002 404 响应不含隐藏用例内容");

  // 历史列表：Bob 看不到 Alice 的记录；user_id/account/role/mine 参数不能扩大范围。
  const std::vector<std::string> widen = {
      "?mine&user_id=" + std::to_string(alice.id),
      "?mine&account=" + alice.account,
      "?mine&role=admin",
      "?mine&user_id=" + std::to_string(alice.id) + "&account=" + alice.account +
          "&role=admin",
  };
  for (const std::string &q : widen) {
    auto res = api_get(cli, "/api/submissions" + q, bob.token);
    check(res && res->status == 200, "T-002 Bob 历史带参数返回 200（" + q + "）");
    json body = parse_json(res);
    check(body.value("total", -1LL) == 0 && body["submissions"].empty(),
          "T-002 参数不能扩大历史范围（" + q + "）");
  }

  // 本人可读；未改密管理员视为非管理员（404）；改密后管理员可读。
  auto own = api_get(cli, "/api/submissions/" + std::to_string(sid), alice.token);
  check(own && own->status == 200 &&
            own->body.find("SRC_A_MARKER_9f3") != std::string::npos,
        "T-002 本人可读自己的提交源码");

  std::string fresh = admin_fresh_token(cli);
  auto pre = api_get(cli, "/api/submissions/" + std::to_string(sid), fresh);
  check(pre && pre->status == 404, "T-002 未改密管理员按普通用户处理 -> 404");

  auto changed = change_password(cli, fresh, kAdminPassword, kAdminNewPassword);
  check(changed && changed->status == 200, "T-002 管理员完成首次改密");
  auto admin_read =
      api_get(cli, "/api/submissions/" + std::to_string(sid), fresh);
  check(admin_read && admin_read->status == 200 &&
            admin_read->body.find("SRC_A_MARKER_9f3") != std::string::npos,
        "T-002 合格管理员可读他人提交源码");

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-003 题目可见性与元数据泄露
// ---------------------------------------------------------------------------

void test_problem_visibility_and_tags() {
  std::cout << "T-003 隐藏题目可见性与标签选项不泄露\n";
  FakeExecutor fake;
  Env env("m52_vis", &fake);
  check(env.ok(), "T-003 服务启动成功");
  httplib::Client cli = make_client(env.port());

  User normal = make_user(cli, env.db(), "vis_user", "VisPw1");
  std::int64_t pub = insert_problem(env.db(), "公开可见题", 1, "publictag");
  std::int64_t hid = insert_problem(env.db(), "隐藏不可见题", 0, "hiddentag");
  check(pub > 0 && hid > 0, "T-003 公开/隐藏题目构造成功");

  // 游客与普通用户列表：不含隐藏题，total 与可见数量一致。
  for (const std::string &token : {std::string(""), normal.token}) {
    auto res = api_get(cli, "/api/problems?page=1&page_size=100", token);
    check(res && res->status == 200, "T-003 列表返回 200");
    json body = parse_json(res);
    bool found_hidden = false;
    for (const auto &p : body["problems"]) {
      if (p.value("id", 0LL) == hid) {
        found_hidden = true;
      }
    }
    check(!found_hidden, "T-003 列表不含隐藏题");
    check(body.value("total", -1LL) == 1, "T-003 total 与可见数量一致");
  }

  // 直接指定隐藏题 ID：404；即使显式传 visible=0 也不能查看。
  auto d1 = api_get(cli, "/api/problems/" + std::to_string(hid), "");
  check(d1 && d1->status == 404, "T-003 游客直取隐藏题 -> 404");
  auto d2 =
      api_get(cli, "/api/problems/" + std::to_string(hid), normal.token);
  check(d2 && d2->status == 404, "T-003 普通用户直取隐藏题 -> 404");
  auto d3 = api_get(cli,
                    "/api/problems?page=1&visible=0&role=admin&user_id=1",
                    normal.token);
  check(d3 && d3->status == 200, "T-003 普通用户带 visible=0 列表 200");
  json d3body = parse_json(d3);
  check(d3body.value("total", -1LL) == 1, "T-003 visible=0 后 total 仍为 1");
  for (const auto &p : d3body["problems"]) {
    check(p.value("id", 0LL) != hid, "T-003 visible=0 不能暴露隐藏题");
  }

  // 标签选项：普通用户/游客不含仅隐藏题目使用的标签。
  for (const std::string &token : {std::string(""), normal.token}) {
    auto tags = api_get(cli, "/api/problem-tags", token);
    check(tags && tags->status == 200, "T-003 标签选项返回 200");
    std::string raw = tags ? tags->body : "";
    check(raw.find("publictag") != std::string::npos,
          "T-003 标签选项含公开题目标签");
    check(raw.find("hiddentag") == std::string::npos,
          "T-003 标签选项不含仅隐藏题目使用的标签");
  }

  // 提交隐藏题：404 且不产生任何提交/在途记录。
  auto s = submit(cli, normal.token, hid, "cpp17", "x");
  check(s && s->status == 404, "T-003 向隐藏题提交 -> 404");
  check(count_submissions(env.db(), normal.id, hid) == 0,
        "T-003 隐藏题提交未持久化");
  check(count_in_flight(env.db(), hid) == 0, "T-003 隐藏题提交未产生在途任务");

  // 管理员可见隐藏题及其标签。
  std::string fresh = admin_fresh_token(cli);
  change_password(cli, fresh, kAdminPassword, kAdminNewPassword);
  auto ad = api_get(cli, "/api/problems/" + std::to_string(hid), fresh);
  check(ad && ad->status == 200, "T-003 管理员可见隐藏题详情");
  auto atags = api_get(cli, "/api/problem-tags", fresh);
  check(atags && atags->body.find("hiddentag") != std::string::npos,
        "T-003 管理员标签选项含隐藏题目标签");

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-004 可见性变化后的历史保留
// ---------------------------------------------------------------------------

void test_visibility_change_history_retained() {
  std::cout << "T-004 题目转隐藏后本人历史/状态保留、题面与新提交拦截\n";
  FakeExecutor fake;
  Env env("m52_hist", &fake);
  check(env.ok(), "T-004 服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "hist_user", "HistPw1");
  std::int64_t pid = insert_problem(env.db(), "将由公开转隐藏", 1);
  insert_testcase(env.db(), pid, 0, "HIST_INPUT\n", "2\n", false);

  auto s = submit(cli, user.token, pid, "cpp17", "// hist source");
  check(s && s->status == 200, "T-004 提交成功");
  std::int64_t sid = parse_json(s).value("id", 0LL);
  check(parse_json(s).value("status", "") == "AC", "T-004 首次 AC");

  std::string fresh = admin_fresh_token(cli);
  change_password(cli, fresh, kAdminPassword, kAdminNewPassword);
  auto hide = api_put(cli, "/api/admin/problems/" + std::to_string(pid), fresh,
                      R"({"visible":false})");
  check(hide && hide->status == 200, "T-004 管理员将题目转为隐藏");

  // 题面不再可见，新提交被拒。
  auto det = api_get(cli, "/api/problems/" + std::to_string(pid), user.token);
  check(det && det->status == 404, "T-004 转隐藏后题面 404");
  auto re = submit(cli, user.token, pid, "cpp17", "// again");
  check(re && re->status == 404, "T-004 转隐藏后新提交 404");
  check(count_submissions(env.db(), user.id, pid) == 1,
        "T-004 新提交未持久化，历史提交保留");
  check(scalar2(env.db(),
                "SELECT submit_count FROM user_problem_status WHERE user_id = ? "
                "AND problem_id = ?",
                user.id, pid) == 1,
        "T-004 提交次数保持不变");

  // 本人历史、详情、状态仍可读，且不含隐藏用例内容。
  auto hist = api_get(cli,
                      "/api/submissions?mine&problem_id=" + std::to_string(pid),
                      user.token);
  check(hist && hist->status == 200 &&
            parse_json(hist).value("total", -1LL) == 1,
        "T-004 本人历史仍含该题记录");
  auto detail =
      api_get(cli, "/api/submissions/" + std::to_string(sid), user.token);
  check(detail && detail->status == 200, "T-004 本人提交详情仍可读");
  check(detail && detail->body.find("HIST_INPUT") == std::string::npos,
        "T-004 历史接口不下发隐藏用例内容");
  auto st = api_get(cli,
                    "/api/status?problem_id=" + std::to_string(pid),
                    user.token);
  check(st && st->status == 200, "T-004 本人状态接口可读");
  bool accepted = false;
  int submit_count = -1;
  json stbody = parse_json(st);
  for (const auto &e : stbody["statuses"]) {
    if (e.value("problem_id", 0LL) == pid) {
      accepted = e.value("status", "") == "accepted";
      submit_count = e.value("submit_count", -1);
    }
  }
  check(accepted && submit_count == 1, "T-004 状态仍为 accepted 且次数为 1");

  // 重新公开后详情恢复。
  auto show = api_put(cli, "/api/admin/problems/" + std::to_string(pid), fresh,
                      R"({"visible":true})");
  check(show && show->status == 200, "T-004 重新公开");
  auto det2 = api_get(cli, "/api/problems/" + std::to_string(pid), user.token);
  check(det2 && det2->status == 200, "T-004 重新公开后详情可见");

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-005 用例归属与字段控制
// ---------------------------------------------------------------------------

void test_testcase_ownership_and_field_control() {
  std::cout << "T-005 用例归属、客户端字段与公开样例边界\n";
  FakeExecutor fake;
  Env env("m52_tcown", &fake);
  check(env.ok(), "T-005 服务启动成功");
  httplib::Client cli = make_client(env.port());

  std::string fresh = admin_fresh_token(cli);
  change_password(cli, fresh, kAdminPassword, kAdminNewPassword);

  std::int64_t a = insert_problem(env.db(), "A题", 1, "tagA");
  std::int64_t b = insert_problem(env.db(), "B题", 1, "tagB");
  std::int64_t b_tc = insert_testcase(env.db(), b, 0, "B_ORIG\n", "B_OUT\n", false);
  insert_testcase(env.db(), a, 0, "A_SAMPLE\n", "A_SAMPLE_OUT\n", true);

  // 建用例时携带服务端字段：归属仍由 URL 题目决定，固定为隐藏用例。
  json create_body{{"input", "A_NEW\n"},
                   {"output", "A_NEW_OUT\n"},
                   {"problem_id", b},
                   {"id", 999999},
                   {"is_sample", true}};
  auto cr = api_post(cli,
                     "/api/admin/problems/" + std::to_string(a) + "/testcases",
                     fresh, create_body.dump());
  check(cr && cr->status == 201, "T-005 新建用例成功");
  std::int64_t a_new_tc = parse_json(cr).value("id", 0LL);
  check(scalar2(env.db(),
                "SELECT COUNT(*) FROM testcases WHERE problem_id = ? AND id = ? "
                "AND is_sample = 0",
                a, a_new_tc) == 1,
        "T-005 客户端 problem_id/is_sample/id 被忽略，归属 A 且为隐藏用例");
  check(scalar(env.db(), "SELECT COUNT(*) FROM testcases WHERE id = 999999") ==
            0,
        "T-005 客户端指定 id 未被采用");

  // S-002：管理员用例读取是唯一返回完整用例的入口，须含隐藏 input 与 is_sample=false。
  auto admin_tc = api_get(cli,
                          "/api/admin/problems/" + std::to_string(a) +
                              "/testcases",
                          fresh);
  check(admin_tc && admin_tc->status == 200, "S-002 管理员用例读取 200");
  json tc_body = parse_json(admin_tc);
  bool found_new_tc = false;
  bool new_tc_hidden = false;
  for (const auto &tc : tc_body["testcases"]) {
    if (tc.value("id", 0LL) == a_new_tc) {
      found_new_tc = true;
      new_tc_hidden = (tc.value("is_sample", true) == false);
    }
  }
  check(found_new_tc &&
            tc_body.dump().find("A_NEW") != std::string::npos,
        "S-002 管理员读取返回隐藏用例完整内容");
  check(new_tc_hidden, "S-002 管理员读取标注 is_sample=false");

  // A 题路径 + B 题用例 ID：改/删均 404，且不误伤 B 题。
  auto upd = api_put(cli,
                     "/api/admin/problems/" + std::to_string(a) +
                         "/testcases/" + std::to_string(b_tc),
                     fresh, R"({"input":"HACK\n"})");
  check(upd && upd->status == 404, "T-005 跨题改用例 -> 404");
  auto del = api_delete(cli,
                        "/api/admin/problems/" + std::to_string(a) +
                            "/testcases/" + std::to_string(b_tc),
                        fresh);
  check(del && del->status == 404, "T-005 跨题删用例 -> 404");
  check(testcase_input(env.db(), b_tc) == "B_ORIG\n",
        "T-005 B 题用例内容未被误改");
  check(scalar(env.db(), "SELECT COUNT(*) FROM testcases WHERE id = " +
                             std::to_string(b_tc)) == 1,
        "T-005 B 题用例未被误删");

  // 隐藏用例不能通过 is_sample 变为公开样例。
  auto flip = api_put(cli,
                      "/api/admin/problems/" + std::to_string(a) +
                          "/testcases/" + std::to_string(a_new_tc),
                      fresh, R"({"input":"A_MORE\n","is_sample":true})");
  check(flip && flip->status == 200, "T-005 带 is_sample 的改用例成功");
  check(scalar1(env.db(),
                "SELECT COUNT(*) FROM testcases WHERE id = ? AND is_sample = 0",
                a_new_tc) == 1,
        "T-005 隐藏用例仍为隐藏，未被提升为公开样例");
  // 仅传 is_sample：无可用字段 -> 400，且无写入。
  auto only = api_put(cli,
                      "/api/admin/problems/" + std::to_string(a) +
                          "/testcases/" + std::to_string(a_new_tc),
                      fresh, R"({"is_sample":true})");
  check(only && only->status == 400, "T-005 仅传 is_sample -> 400");

  // 公开样例通过用例接口操作 -> 404。
  std::int64_t a_sample = testcase_id(env.db(), a, true);
  auto sample_upd = api_put(cli,
                            "/api/admin/problems/" + std::to_string(a) +
                                "/testcases/" + std::to_string(a_sample),
                            fresh, R"({"input":"X\n"})");
  check(sample_upd && sample_upd->status == 404, "T-005 公开样例不可通过用例接口改写");
  check(testcase_input(env.db(), a_sample) == "A_SAMPLE\n",
        "T-005 公开样例内容保持不变");

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-006 非法/不存在对象与错误边界
// ---------------------------------------------------------------------------

void test_illegal_ids_and_error_boundaries() {
  std::cout << "T-006 非法/越界 ID、非法 JSON、超长请求体与错误不泄露\n";
  FakeExecutor fake;
  Env env("m52_illegal", &fake);
  check(env.ok(), "T-006 服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "illegal_user", "IllegalPw1");
  std::string fresh = admin_fresh_token(cli);
  change_password(cli, fresh, kAdminPassword, kAdminNewPassword);
  std::int64_t pid = insert_problem(env.db(), "非法测试题", 1);
  std::int64_t tid = insert_testcase(env.db(), pid, 0, "in\n", "2\n", false);

  const std::string overflow = "99999999999999999999"; // 20 位
  const std::vector<std::string> bad_ids = {overflow, "0", "-1", "abc", "1.5",
                                            "0x1", "+1", " 1"};

  for (const std::string &id : bad_ids) {
    auto pr = api_get(cli, "/api/problems/" + id, "");
    check(pr && pr->status == 400, "T-006 题目 ID '" + id + "' -> 400");
  }

  // 提交 ID 的越界/非法（既有测试未覆盖 overflow）。
  for (const std::string &id : std::vector<std::string>{overflow, "0", "-1",
                                                        "abc"}) {
    auto sr = api_get(cli, "/api/submissions/" + id, user.token);
    check(sr && sr->status == 400, "T-006 提交 ID '" + id + "' -> 400");
  }

  // 用例 ID 的越界/非法。
  for (const std::string &id : std::vector<std::string>{overflow, "0", "-1",
                                                        "abc"}) {
    auto ur = api_put(cli,
                      "/api/admin/problems/" + std::to_string(pid) +
                          "/testcases/" + id,
                      fresh, R"({"input":"x\n"})");
    check(ur && ur->status == 400, "T-006 用例 ID '" + id + "' -> 400");
  }

  // 不存在（合法格式）的记录：404，且不泄露内部信息。
  auto miss = api_get(cli, "/api/submissions/999999999", user.token);
  check(miss && miss->status == 404, "T-006 不存在提交 -> 404");
  std::string mraw = miss ? miss->body : "";
  check(mraw.find("SELECT") == std::string::npos &&
            mraw.find("sqlite") == std::string::npos &&
            mraw.find("/home/") == std::string::npos,
        "T-006 404 响应不泄露 SQL/路径");

  // 非法 JSON 与错误字段类型（管理员建题）：400，且不产生记录。
  std::int64_t before = scalar(env.db(), "SELECT COUNT(*) FROM problems");
  auto badjson = api_post(cli, "/api/admin/problems", fresh, "{not json");
  check(badjson && badjson->status == 400, "T-006 非法 JSON -> 400");
  auto badtype = api_post(cli, "/api/admin/problems", fresh,
                          R"({"title":123,"difficulty":"easy"})");
  check(badtype && badtype->status == 400, "T-006 错误字段类型 -> 400");
  check(scalar(env.db(), "SELECT COUNT(*) FROM problems") == before,
        "T-006 失败请求未产生部分写入");

  // 超长请求体（>1 MiB）：413，且未创建题目。
  std::string huge = R"({"title":")" + std::string(1024 * 1024 + 64, 'a') +
                     R"(","difficulty":"easy"})";
  auto too_large = api_post(cli, "/api/admin/problems", fresh, huge);
  check(too_large && too_large->status == 413, "T-006 超长请求体 -> 413");
  check(scalar(env.db(), "SELECT COUNT(*) FROM problems") == before,
        "T-006 超长请求未创建题目");

  // 用例不存在：404，且原用例不变。
  auto tc_miss = api_put(cli,
                         "/api/admin/problems/" + std::to_string(pid) +
                             "/testcases/999999999",
                         fresh, R"({"input":"x\n"})");
  check(tc_miss && tc_miss->status == 404, "T-006 不存在用例 -> 404");
  check(testcase_input(env.db(), tid) == "in\n", "T-006 原用例未被波及");

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-007 写入字段控制与权限撤销
// ---------------------------------------------------------------------------

void test_write_field_control_and_revocation() {
  std::cout << "T-007 服务端字段不可伪装、角色撤销后旧 token 立即失权\n";
  FakeExecutor fake;
  Env env("m52_fields", &fake);
  check(env.ok(), "T-007 服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "field_user", "FieldPw1");
  std::string fresh = admin_fresh_token(cli);
  change_password(cli, fresh, kAdminPassword, kAdminNewPassword);

  std::int64_t pid = insert_problem(env.db(), "字段题", 1);
  insert_testcase(env.db(), pid, 0, "in\n", "2\n", false);

  // 提交携带 user_id/status/created_at/role：归属仍为当前用户、时间为服务端。
  json submit_body{{"language", "cpp17"},
                   {"code", "// field source"},
                   {"user_id", 999999},
                   {"status", "AC"},
                   {"created_at", "1999-01-01T00:00:00Z"},
                   {"role", "admin"}};
  auto s = api_post(cli, "/api/problems/" + std::to_string(pid) + "/submit",
                    user.token, submit_body.dump());
  check(s && s->status == 200, "T-007 提交成功");
  std::int64_t sid = parse_json(s).value("id", 0LL);
  std::int64_t owner = scalar1(env.db(), "SELECT user_id FROM submissions WHERE "
                                          "id = ?",
                               sid);
  check(owner == user.id, "T-007 提交归属为调用者，user_id 伪装被忽略");
  std::string created = text1(env.db(), "SELECT created_at FROM submissions "
                                        "WHERE id = ?",
                              sid);
  check(created.rfind("1999", 0) != 0 && created.size() >= 10,
        "T-007 created_at 由服务端写入，未被客户端覆盖");

  // 建题携带 id/seed_key/created_at：服务端字段被忽略。
  json create_body{{"title", "字段新题"},
                   {"difficulty", "easy"},
                   {"id", 424242},
                   {"seed_key", "hack-seed"},
                   {"created_at", "1999-01-01T00:00:00Z"},
                   {"updated_at", "1999-01-01T00:00:00Z"}};
  auto cp = api_post(cli, "/api/admin/problems", fresh, create_body.dump());
  check(cp && cp->status == 201, "T-007 建题成功");
  std::int64_t new_pid = parse_json(cp).value("id", 0LL);
  check(new_pid > 0 && new_pid != 424242, "T-007 题目 ID 由服务端分配");
  check(text1(env.db(), "SELECT seed_key FROM problems WHERE id = ?", new_pid)
            .empty(),
        "T-007 seed_key 未被客户端设置");
  check(text1(env.db(), "SELECT created_at FROM problems WHERE id = ?", new_pid)
            .rfind("1999", 0) != 0,
        "T-007 题目时间由服务端写入");

  // Rejudge 请求体携带 code/user_id/status：不影响原提交归属与源码。
  json rej_body{{"code", "// INJECTED"},
                {"language", "c11"},
                {"user_id", 999999},
                {"status", "AC"},
                {"created_at", "1999-01-01T00:00:00Z"}};
  auto rj = api_post(cli,
                     "/api/admin/submissions/" + std::to_string(sid) +
                         "/rejudge",
                     fresh, rej_body.dump());
  check(rj && rj->status == 200, "T-007 重判成功");
  check(scalar1(env.db(), "SELECT user_id FROM submissions WHERE id = ?", sid) ==
            user.id,
        "T-007 重判后归属不变");
  check(text1(env.db(), "SELECT source_code FROM submissions WHERE id = ?", sid)
            .find("INJECTED") == std::string::npos,
        "T-007 重判未采用请求体源码");
  check(text1(env.db(), "SELECT language FROM submissions WHERE id = ?", sid) ==
            "cpp17",
        "T-007 重判未采用请求体语言");

  // 角色撤销：提升后旧 token 立即具备管理能力，降级后立即失去。
  auto promote = api_put(cli, "/api/admin/users", fresh,
                         "{\"action\":\"change_role\",\"user_id\":" +
                             std::to_string(user.id) + ",\"role\":\"admin\"}");
  check(promote && promote->status == 200, "T-007 提升用户为管理员");
  auto as_admin = api_get(cli, "/api/admin/users", user.token);
  check(as_admin && as_admin->status == 200,
        "T-007 旧 token 立即获得管理能力（按数据库最新角色）");

  auto demote = api_put(cli, "/api/admin/users", fresh,
                        "{\"action\":\"change_role\",\"user_id\":" +
                            std::to_string(user.id) + ",\"role\":\"user\"}");
  check(demote && demote->status == 200, "T-007 降级用户为普通用户");
  auto after = api_get(cli, "/api/admin/users", user.token);
  check(after && after->status == 403, "T-007 旧 token 立即失去管理能力 -> 403");

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-008 删除题目的关联数据一致性
// ---------------------------------------------------------------------------

void test_delete_problem_consistency() {
  std::cout << "T-008 删题：有提交拒绝、无提交清理关联、在途保护、不误删他人\n";
  FakeExecutor fake;
  Env env("m52_delete", &fake);
  check(env.ok(), "T-008 服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "del_user", "DelPw1");
  std::string fresh = admin_fresh_token(cli);
  change_password(cli, fresh, kAdminPassword, kAdminNewPassword);

  // P1：有提交，应拒绝删除且数据完整。
  std::int64_t p1 = insert_problem(env.db(), "有提交题", 1);
  std::int64_t p1_tc = insert_testcase(env.db(), p1, 0, "in\n", "2\n", false);
  std::int64_t p1_sid = insert_submission(env.db(), user.id, p1, "cpp17", "src",
                                          "AC", "2020-01-01T00:00:00Z");
  auto d1 = api_delete(cli, "/api/admin/problems/" + std::to_string(p1), fresh);
  check(d1 && d1->status == 409, "T-008 有提交题目删除 -> 409");
  check(scalar1(env.db(), "SELECT COUNT(*) FROM problems WHERE id = ?", p1) == 1,
        "T-008 题目保留");
  check(count_testcases(env.db(), p1) == 1 && count_submissions(env.db(),
                                                                user.id, p1) == 1,
        "T-008 用例与提交保留");

  // P2：无提交，但有用例与状态行，应被完整清理。
  std::int64_t p2 = insert_problem(env.db(), "无提交题", 1);
  insert_testcase(env.db(), p2, 0, "in\n", "2\n", false);
  std::string err;
  env.db().exec("INSERT INTO user_problem_status (user_id, problem_id, status, "
                "submit_count) VALUES (" +
                    std::to_string(user.id) + "," + std::to_string(p2) +
                    ",'none',0)",
                err);
  auto d2 = api_delete(cli, "/api/admin/problems/" + std::to_string(p2), fresh);
  check(d2 && d2->status == 200, "T-008 无提交题目删除 -> 200");
  check(scalar1(env.db(), "SELECT COUNT(*) FROM problems WHERE id = ?", p2) == 0 &&
            count_testcases(env.db(), p2) == 0 && count_status(env.db(), p2) == 0,
        "T-008 关联用例与状态一并清理，无孤立数据");

  // P3：存在未结算在途任务，应拒绝；清理后允许并移除残留。
  std::int64_t p3 = insert_problem(env.db(), "在途题", 1);
  check(insert_in_flight(env.db(), user.id, p3, "m52_inflight_pending",
                         "pending"),
        "T-008 写入在途任务");
  auto d3 = api_delete(cli, "/api/admin/problems/" + std::to_string(p3), fresh);
  check(d3 && d3->status == 409, "T-008 有在途任务删除 -> 409");
  check(count_in_flight(env.db(), p3) == 1, "T-008 在途任务保留");
  env.db().exec("UPDATE in_flight_tasks SET state='interrupted' WHERE "
                "problem_id=" +
                    std::to_string(p3),
                err);
  auto d3b = api_delete(cli, "/api/admin/problems/" + std::to_string(p3), fresh);
  check(d3b && d3b->status == 200, "T-008 中断在途不阻止删除 -> 200");
  check(count_in_flight(env.db(), p3) == 0, "T-008 残留在途记录被清理");

  // S-001：恢复扫描已将任务认领为 claimed，以及重置回 pending 时，删题同样必须被阻止；
  // 覆盖「删除 vs 崩溃恢复」的关联约束边界（确定性构造，不依赖运行期并发）。
  std::int64_t p4 = insert_problem(env.db(), "已认领在途题", 1);
  check(insert_in_flight(env.db(), user.id, p4, "m52_inflight_claimed",
                         "pending"),
        "S-001 写入在途任务");
  env.db().exec("UPDATE in_flight_tasks SET state='claimed' WHERE task_id="
                "'m52_inflight_claimed'",
                err);
  auto d4a = api_delete(cli, "/api/admin/problems/" + std::to_string(p4), fresh);
  check(d4a && d4a->status == 409, "S-001 被认领 claimed 时删题 -> 409");
  env.db().exec("UPDATE in_flight_tasks SET state='pending' WHERE task_id="
                "'m52_inflight_claimed'",
                err);
  auto d4b = api_delete(cli, "/api/admin/problems/" + std::to_string(p4), fresh);
  check(d4b && d4b->status == 409, "S-001 重置回 pending 时删题 -> 409");
  check(count_in_flight(env.db(), p4) == 1, "S-001 在途任务保留未被误删");
  env.db().exec("DELETE FROM in_flight_tasks WHERE task_id='m52_inflight_"
                "claimed'",
                err);
  auto d4c = api_delete(cli, "/api/admin/problems/" + std::to_string(p4), fresh);
  check(d4c && d4c->status == 200 && count_in_flight(env.db(), p4) == 0,
        "S-001 在途清除后可删且无残留");

  // 未受影响的 P1 及其提交/用例仍在。
  check(scalar1(env.db(), "SELECT COUNT(*) FROM problems WHERE id = ?", p1) == 1 &&
            count_testcases(env.db(), p1) == 1 &&
            scalar1(env.db(), "SELECT COUNT(*) FROM submissions WHERE id = ?",
                    p1_sid) == 1,
        "T-008 未误删其它题目及其学生提交");

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-009 用例增删改与判题一致性
// ---------------------------------------------------------------------------

void test_testcase_modify_judge_consistency() {
  std::cout << "T-009 用例顺序/样例边界/历史结果不被改写\n";
  FakeExecutor fake;
  fake.run_output = "WRONG\n";
  Env env("m52_tcmod", &fake);
  check(env.ok(), "T-009 服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "tc_user", "TcPw1");
  std::string fresh = admin_fresh_token(cli);
  change_password(cli, fresh, kAdminPassword, kAdminNewPassword);

  std::int64_t pid = insert_problem(env.db(), "用例顺序题", 1);
  // 有意乱序插入，验证按 (ord,id) 排序即判题顺序。
  std::int64_t tc_one =
      insert_testcase(env.db(), pid, 1, "ORD_ONE\n", "NOPE1\n", false);
  std::int64_t tc_zero =
      insert_testcase(env.db(), pid, 0, "ORD_ZERO\n", "NOPE0\n", false);
  insert_testcase(env.db(), pid, 2, "SAMPLE\n", "SAMPLE_OUT\n", true);

  auto s1 = submit(cli, user.token, pid, "cpp17", "// first");
  check(s1 && s1->status == 200 && parse_json(s1).value("status", "") == "WA",
        "T-009 首次提交为 WA");
  std::int64_t sid1 = parse_json(s1).value("id", 0LL);
  json results1 = parse_json(s1)["results"];
  // 该题含 2 个隐藏用例与 1 个公开样例，按 (ord,id) 升序全部参与判题。
  check(results1.size() == 3, "T-009 逐点结果覆盖全部用例");
  check(results1[0].value("input", "") == "ORD_ZERO\n" &&
            results1[1].value("input", "") == "ORD_ONE\n" &&
            results1[2].value("input", "") == "SAMPLE\n",
        "T-009 判题顺序按 (ord,id) 升序");

  // 修改 ord=0 用例的期望输出；公开样例不受影响。
  auto upd = api_put(cli,
                     "/api/admin/problems/" + std::to_string(pid) +
                         "/testcases/" + std::to_string(tc_zero),
                     fresh, R"({"output":"WRONG\n"})");
  check(upd && upd->status == 200, "T-009 修改用例成功");

  auto detail1 =
      api_get(cli, "/api/submissions/" + std::to_string(sid1), user.token);
  check(detail1 && detail1->status == 200, "T-009 历史详情可读");
  check(detail1 && detail1->body.find("NOPE0") != std::string::npos,
        "T-009 历史结果的期望输出未被静默改写");

  // 新提交使用修改后的用例：ord=0 点匹配，总体仍 WA（ord=1 不匹配）。
  auto s2 = submit(cli, user.token, pid, "cpp17", "// second");
  check(s2 && s2->status == 200 && parse_json(s2).value("status", "") == "WA",
        "T-009 新提交仍 WA");
  json results2 = parse_json(s2)["results"];
  check(results2[0].value("status", "") == "AC" &&
            !results2[0].contains("expected_output"),
        "T-009 新提交使用修改后的用例（改后点 AC 且不泄露答案）");

  // 删除 ord=1 隐藏用例后，剩余 1 隐藏点 + 1 公开样例；历史记录仍为 3 点。
  auto del = api_delete(cli,
                        "/api/admin/problems/" + std::to_string(pid) +
                            "/testcases/" + std::to_string(tc_one),
                        fresh);
  check(del && del->status == 200, "T-009 删除用例成功");
  auto s3 = submit(cli, user.token, pid, "cpp17", "// third");
  check(s3 && s3->status == 200 &&
            parse_json(s3)["results"].size() == 2,
        "T-009 新提交仅使用剩余用例");
  auto detail1b =
      api_get(cli, "/api/submissions/" + std::to_string(sid1), user.token);
  check(detail1b && parse_json(detail1b)["results"].size() == 3,
        "T-009 历史逐点结果不因用例删除而改变");

  // 公开样例仍可通过题目详情读取。
  auto pub = api_get(cli, "/api/problems/" + std::to_string(pid), fresh);
  check(pub && pub->body.find("SAMPLE_OUT") != std::string::npos,
        "T-009 公开样例不受隐藏用例增删改影响");

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-010 Rejudge 与排行榜/通过人数一致性
// ---------------------------------------------------------------------------

void test_rejudge_statistics_consistency() {
  std::cout << "T-010 Rejudge 更新原记录、不增次数并联动统计\n";
  FakeExecutor fake;
  fake.run_output = "2\n";
  Env env("m52_rejudge", &fake);
  check(env.ok(), "T-010 服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "rej_user", "RejPw1");
  std::string fresh = admin_fresh_token(cli);
  change_password(cli, fresh, kAdminPassword, kAdminNewPassword);

  std::int64_t pid = insert_problem(env.db(), "重判统计题", 1);
  std::int64_t tc =
      insert_testcase(env.db(), pid, 0, "in\n", "2\n", false);

  auto s = submit(cli, user.token, pid, "cpp17", "// will be rejudged");
  check(s && parse_json(s).value("status", "") == "AC", "T-010 提交 AC");
  std::int64_t sid = parse_json(s).value("id", 0LL);
  check(pass_count(cli, fresh, pid) == 1, "T-010 通过人数为 1");
  check(leaderboard_field(cli, user.id, "ac_count") == 1,
        "T-010 排行榜 AC 数为 1");
  check(leaderboard_field(cli, user.id, "submit_count") == 1,
        "T-010 排行榜提交次数为 1");

  // 修改用例使其不再通过，重判后状态/统计联动且不增加次数。
  auto upd = api_put(cli,
                     "/api/admin/problems/" + std::to_string(pid) +
                         "/testcases/" + std::to_string(tc),
                     fresh, R"({"output":"999\n"})");
  check(upd && upd->status == 200, "T-010 修改用例");
  auto rj = rejudge(cli, fresh, sid);
  check(rj && rj->status == 200 && parse_json(rj).value("status", "") == "WA",
        "T-010 重判结果为 WA");
  check(parse_json(rj).value("id", 0LL) == sid, "T-010 重判更新原记录 ID");
  check(scalar(env.db(), "SELECT COUNT(*) FROM submissions WHERE problem_id = " +
                             std::to_string(pid)) == 1,
        "T-010 未新增提交记录");
  check(scalar2(env.db(),
                "SELECT submit_count FROM user_problem_status WHERE user_id = ? "
                "AND problem_id = ?",
                user.id, pid) == 1,
        "T-010 提交次数不因重判增加");
  check(pass_count(cli, fresh, pid) == 0, "T-010 通过人数随重判变为 0");
  check(leaderboard_field(cli, user.id, "ac_count") == 0,
        "T-010 排行榜 AC 数变为 0");
  check(leaderboard_field(cli, user.id, "submit_count") == 1,
        "T-010 排行榜提交次数保持 1");
  check(leaderboard_field(cli, user.id, "first_ac_at") == -2,
        "T-010 首次 AC 时间清空为 null");

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-011 受控并发
// ---------------------------------------------------------------------------

void test_controlled_concurrency() {
  std::cout << "T-011 受控并发：Rejudge vs 新提交、删题 vs 提交结算\n";
  GatedExecutor exec;
  Env env("m52_conc", &exec);
  check(env.ok(), "T-011 服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "conc_user", "ConcPw1");
  std::string fresh = admin_fresh_token(cli);
  change_password(cli, fresh, kAdminPassword, kAdminNewPassword);

  std::int64_t pid = insert_problem(env.db(), "并发题", 1);
  insert_testcase(env.db(), pid, 0, "in\n", "2\n", false);

  // 初始提交（gate 为空，直接完成）。
  auto s0 = submit(cli, user.token, pid, "cpp17", "// base");
  check(s0 && parse_json(s0).value("status", "") == "AC", "T-011 初始提交 AC");
  std::int64_t sid = parse_json(s0).value("id", 0LL);

  // 打开同步门，使重判与新提交在编译阶段同时挂起。
  GatedExecutor::Gate gate;
  exec.gate = &gate;

  std::unique_ptr<httplib::Result> rj_res;
  std::unique_ptr<httplib::Result> sub_res;
  std::thread t_rej([&]() {
    httplib::Client c = make_client(env.port());
    rj_res = std::make_unique<httplib::Result>(rejudge(c, fresh, sid));
  });
  std::thread t_sub([&]() {
    httplib::Client c = make_client(env.port());
    sub_res = std::make_unique<httplib::Result>(
        submit(c, user.token, pid, "cpp17", "// concurrent"));
  });

  const bool both_entered = gate.wait_entered(2, std::chrono::seconds(10));
  check(both_entered, "T-011 重判与新提交同时进入编译阶段");
  gate.release();
  t_rej.join();
  t_sub.join();
  exec.gate = nullptr;

  check(rj_res && *rj_res && (*rj_res)->status == 200,
        "T-011 并发重判返回 200");
  check(sub_res && *sub_res && (*sub_res)->status == 200,
        "T-011 并发新提交返回 200");
  check(scalar(env.db(), "SELECT COUNT(*) FROM submissions WHERE problem_id = " +
                             std::to_string(pid)) == 2,
        "T-011 共有 2 条提交（重判不新增）");
  check(scalar2(env.db(),
                "SELECT submit_count FROM user_problem_status WHERE user_id = ? "
                "AND problem_id = ?",
                user.id, pid) == 2,
        "T-011 提交次数为 2，无丢失");
  check(pass_count(cli, fresh, pid) == 1, "T-011 通过人数为 1");

  // 删题 vs 提交结算：提交在途期间删题必须 409，结算后仍 409（已有提交）。
  std::int64_t pid2 = insert_problem(env.db(), "并发删题", 1);
  insert_testcase(env.db(), pid2, 0, "in\n", "2\n", false);
  GatedExecutor::Gate gate2;
  exec.gate = &gate2;

  std::unique_ptr<httplib::Result> sub2;
  std::thread t_sub2([&]() {
    httplib::Client c = make_client(env.port());
    sub2 = std::make_unique<httplib::Result>(
        submit(c, user.token, pid2, "cpp17", "// pending"));
  });
  check(gate2.wait_entered(1, std::chrono::seconds(10)),
        "T-011 提交进入编译阶段（已持久化在途任务）");
  auto del_inflight =
      api_delete(cli, "/api/admin/problems/" + std::to_string(pid2), fresh);
  check(del_inflight && del_inflight->status == 409,
        "T-011 在途任务存在时删题 -> 409");
  gate2.release();
  t_sub2.join();
  exec.gate = nullptr;
  check(sub2 && *sub2 && (*sub2)->status == 200, "T-011 在途提交结算成功");
  auto del_after =
      api_delete(cli, "/api/admin/problems/" + std::to_string(pid2), fresh);
  check(del_after && del_after->status == 409,
        "T-011 结算后已有提交仍拒绝删除 -> 409");
  check(count_in_flight(env.db(), pid2) == 0, "T-011 结算后在途记录已清理");

  env.stop();
  env.close_db();
}

} // namespace

int main() {
  test_admin_endpoint_permission_matrix();
  test_submission_ownership_privacy();
  test_problem_visibility_and_tags();
  test_visibility_change_history_retained();
  test_testcase_ownership_and_field_control();
  test_illegal_ids_and_error_boundaries();
  test_write_field_control_and_revocation();
  test_delete_problem_consistency();
  test_testcase_modify_judge_consistency();
  test_rejudge_statistics_consistency();
  test_controlled_concurrency();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部 M5.2 数据访问与权限回归测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
