// M4.3 题目与做题页面后端接口集成测试。
//
// 使用 /tmp 下的隔离临时库与随机端口上的真实 HTTP 服务，不触碰正式数据库。
// 判题执行器可注入，用于稳定构造 AC/WA/RE(截断)/全局硬上限/服务取消等结果。
//
// 覆盖：
//   - GET /api/problems/{id}：游客不含 solved；登录返回本人 solved；AC 后变 true、
//     仅 WA 仍为 false；无效 token 401；隐藏题目对游客/普通用户 404；不泄露隐藏用例。
//   - POST /api/problems/{id}/submit 响应新增字段：提交级 global_deadline_hit /
//     cancelled；逐点 output_truncated；非 AC 点 actual_output 始终存在（允许空串）；
//     WA 点含 input/expected_output/actual_output。
//   - 全局硬上限（执行阶段）导致未执行全部测试点时 global_deadline_hit=true 且 results < total。
//   - 全局硬上限（编译阶段被全局预算裁剪）返回 global_deadline_hit=true、无逐点结果。
//   - 服务取消导致 cancelled=true、总体 SYSERR 且保留已得逐点。
//
// 运行方式：ctest --test-dir build -R m43_problem_page_api --output-on-failure
// 或直接执行 build/oj_m43_problem_page_api_test。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

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

const std::string kTestSecret = "it-m43-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";

oj::auth::JwtConfig make_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = kTestSecret;
  cfg.expires_seconds = 3600;
  return cfg;
}

// 可控执行器：不启动真实进程，按字段返回编译/运行结果。
class FakeExecutor : public oj::judge::IExecutor {
public:
  bool compile_launch_error = false;
  bool compile_timed_out = false;  // 编译阶段超时（用于全局预算裁剪分支）
  int compile_exit_code = 0;
  std::string compile_output;

  bool run_launch_error = false;
  bool run_cancelled = false;
  bool run_timed_out = false;
  bool run_truncated = false;
  bool run_exited = true;
  int run_exit_code = 0;
  int run_signal = 0;
  long long run_memory_kb = 0;
  long long run_time_ms = 1;
  std::string run_output;

  oj::judge::ProcessResult compile(
      const oj::judge::CompileRequest &request) override {
    oj::judge::ProcessResult result;
    if (compile_launch_error) {
      result.launch_error = true;
      result.launch_error_message = "fake: 编译器不可用";
      return result;
    }
    result.launched = true;
    result.exited = true;
    result.exit_code = compile_exit_code;
    result.timed_out = compile_timed_out;
    result.stdout_data = compile_output;
    if (compile_exit_code == 0 && !compile_timed_out) {
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
    result.exited = run_exited;
    result.exit_code = run_exit_code;
    result.term_signal = run_signal;
    result.cancelled = run_cancelled;
    result.timed_out = run_timed_out;
    result.stdout_truncated = run_truncated;
    result.stdout_data = run_output;
    result.time_ms = run_time_ms;
    result.memory_kb = run_memory_kb;
    if (run_cancelled) {
      result.termination = oj::judge::TerminationReason::Cancelled;
    } else if (run_timed_out) {
      result.termination = oj::judge::TerminationReason::TimedOut;
    } else if (run_exit_code != 0 || run_signal != 0 || !run_exited) {
      result.termination = run_signal != 0
                               ? oj::judge::TerminationReason::Signaled
                               : oj::judge::TerminationReason::NonZeroExit;
    } else {
      result.termination = oj::judge::TerminationReason::Completed;
    }
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

class Env {
public:
  Env(const std::string &label, oj::judge::IExecutor *executor = nullptr,
      oj::judge::JudgeOptions options = {})
      : dir_(label) {
    std::string err;
    db_ = oj::Database::open(dir_.db_path(), err);
    if (!db_) {
      return;
    }
    if (!oj::initialize_schema(*db_, kAdminPassword, err)) {
      return;
    }
    port_ = find_free_port();
    server_ = std::make_unique<oj::HttpServer>("127.0.0.1", port_, *db_,
                                               make_config(),
                                               /*enable_test_routes=*/false,
                                               executor, options);
    if (!server_->start(err)) {
      return;
    }
    started_ = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  bool ok() const { return started_; }
  int port() const { return port_; }
  oj::Database &db() { return *db_; }
  void stop() {
    if (server_) {
      server_->stop();
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
// 数据库 / HTTP 辅助
// ---------------------------------------------------------------------------

std::int64_t user_id(oj::Database &db, const std::string &account) {
  std::string err;
  oj::Statement stmt;
  db.prepare("SELECT id FROM users WHERE account = ?", stmt, err);
  stmt.bind(1, account);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t insert_problem(oj::Database &db, const std::string &title,
                            int visible, int time_limit_ms = 2000) {
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
                     const std::string &input, const std::string &output,
                     bool is_sample) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("INSERT INTO testcases (problem_id, ord, input, output, "
                  "is_sample) VALUES (?, ?, ?, ?, ?)",
                  stmt, err)) {
    return false;
  }
  stmt.bind(1, static_cast<sqlite3_int64>(problem_id));
  stmt.bind(2, ord);
  stmt.bind(3, input);
  stmt.bind(4, output);
  stmt.bind(5, is_sample ? 1 : 0);
  return stmt.step() == SQLITE_DONE;
}

std::string register_user(httplib::Client &cli, const std::string &nickname,
                          const std::string &password) {
  json body;
  body["nickname"] = nickname;
  body["password"] = password;
  auto res = cli.Post("/api/register", body.dump(), "application/json");
  if (!res || res->status != 201) {
    return "";
  }
  return json::parse(res->body).value("account", "");
}

std::string login(httplib::Client &cli, const std::string &account,
                  const std::string &password) {
  json body;
  body["account"] = account;
  body["password"] = password;
  auto res = cli.Post("/api/login", body.dump(), "application/json");
  if (!res || res->status != 200) {
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
  user.token = login(cli, user.account, pw);
  user.id = user_id(db, user.account);
  return user;
}

httplib::Result get_detail(httplib::Client &cli, std::int64_t id,
                           const std::string &token = "") {
  const std::string path = "/api/problems/" + std::to_string(id);
  if (token.empty()) {
    return cli.Get(path.c_str());
  }
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Get(path.c_str(), h);
}

httplib::Result submit(httplib::Client &cli, const std::string &token,
                       std::int64_t id, const std::string &language,
                       const std::string &code) {
  json body;
  body["language"] = language;
  body["code"] = code;
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Post(("/api/problems/" + std::to_string(id) + "/submit").c_str(),
                  h, body.dump(), "application/json");
}

// ---------------------------------------------------------------------------
// T-005 / T-006 / T-007：详情本人状态与鉴权、可见性
// ---------------------------------------------------------------------------

void test_detail_owner_status() {
  std::cout << "\n== M43-1 详情本人状态（游客/登录/AC/WA）与鉴权可见性 ==\n";
  FakeExecutor fake;
  fake.run_output = "5\n";
  Env env("m43_detail", &fake);
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());

  const std::int64_t pid = insert_problem(env.db(), "M43 详情题", 1);
  check(pid > 0, "插入题目");
  check(insert_testcase(env.db(), pid, 0, "2 3\n", "5\n", true), "插入公开样例");
  // 隐藏用例期望输出统一为 "5\n"，便于用单一 fake 输出稳定构造 AC。
  check(insert_testcase(env.db(), pid, 1, "10 20\n", "5\n", false),
        "插入隐藏用例 1");
  check(insert_testcase(env.db(), pid, 2, "7 8\n", "5\n", false),
        "插入隐藏用例 2");

  // 游客：无 solved，只有公开样例，不含隐藏用例。
  {
    auto res = get_detail(cli, pid);
    check(res && res->status == 200, "游客获取详情 200");
    json body = res ? json::parse(res->body) : json();
    check(body.contains("samples") && body["samples"].size() == 1,
          "详情仅返回公开样例（1 组）");
    check(!body.contains("solved"), "游客响应不含 solved 字段");
    const std::string text = body.dump();
    check(text.find("10 20") == std::string::npos &&
              text.find("7 8") == std::string::npos,
          "详情不泄露隐藏用例输入");
  }

  // 无效 token：沿用既有约定 401。
  {
    auto res = get_detail(cli, pid, "not-a-real-token");
    check(res && res->status == 401, "无效 token 获取详情返回 401");
  }

  User alice = make_user(cli, env.db(), "m43_alice", "UserPass123");
  check(!alice.token.empty(), "注册并登录 alice");

  // 登录用户初始未 AC。
  {
    auto res = get_detail(cli, pid, alice.token);
    check(res && res->status == 200, "登录用户获取详情 200");
    json body = res ? json::parse(res->body) : json();
    check(body.contains("solved") && body["solved"] == false,
          "登录用户未 AC 时 solved=false");
  }

  // 提交 AC 后 solved=true。
  {
    auto res = submit(cli, alice.token, pid, "cpp17", "int main(){}");
    check(res && res->status == 200 &&
              json::parse(res->body).value("status", "") == "AC",
          "alice 提交 AC");
    auto detail = get_detail(cli, pid, alice.token);
    json body = detail ? json::parse(detail->body) : json();
    check(body.value("solved", false) == true, "AC 后 solved=true");
  }

  // 历史 AC 不因后续 WA 失效。
  User bob = make_user(cli, env.db(), "m43_bob", "UserPass123");
  check(!bob.token.empty(), "注册并登录 bob");
  {
    auto res = submit(cli, bob.token, pid, "cpp17", "int main(){}");
    check(res && json::parse(res->body).value("status", "") == "AC",
          "bob 首次以匹配输出提交为 AC");
  }
  {
    fake.run_output = "0\n";  // 后续提交输出不匹配 -> WA
    auto res = submit(cli, bob.token, pid, "cpp17", "int main(){}");
    check(res && json::parse(res->body).value("status", "") == "WA",
          "bob 第二次提交为 WA");
    auto detail = get_detail(cli, pid, bob.token);
    json body = detail ? json::parse(detail->body) : json();
    check(body.value("solved", false) == true,
          "bob 已 AC 后仍为 solved=true（WA 不清除历史 AC）");
  }

  // 新用户仅 WA → false。
  User carol = make_user(cli, env.db(), "m43_carol", "UserPass123");
  {
    auto res = submit(cli, carol.token, pid, "cpp17", "int main(){}");
    check(res && json::parse(res->body).value("status", "") == "WA",
          "carol 仅 WA");
    auto detail = get_detail(cli, pid, carol.token);
    json body = detail ? json::parse(detail->body) : json();
    check(body.value("solved", true) == false, "仅 WA 的用户 solved=false");
  }

  // 隐藏题目：游客/普通用户 404。
  {
    const std::int64_t hidden = insert_problem(env.db(), "M43 隐藏题", 0);
    insert_testcase(env.db(), hidden, 0, "1\n", "1\n", false);
    auto guest = get_detail(cli, hidden);
    check(guest && guest->status == 404, "游客访问隐藏题详情 404");
    auto normal = get_detail(cli, hidden, alice.token);
    check(normal && normal->status == 404, "普通用户访问隐藏题详情 404");
  }

  env.stop();
}

// ---------------------------------------------------------------------------
// T-008 / T-009 / T-010：提交响应新字段
// ---------------------------------------------------------------------------

void test_submit_response_fields() {
  std::cout << "\n== M43-2 提交响应字段（WA 空输出/截断/CE 与新增布尔字段）==\n";
  FakeExecutor fake;
  Env env("m43_fields", &fake);
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m43_fields_u", "UserPass123");
  check(!u.token.empty(), "注册并登录用户");

  const std::int64_t pid = insert_problem(env.db(), "M43 字段题", 1);
  insert_testcase(env.db(), pid, 0, "1 2\n", "3\n", false);
  insert_testcase(env.db(), pid, 1, "2 2\n", "4\n", false);

  // WA 且实际输出为空串：actual_output 字段必须存在且等于 ""。
  fake.compile_exit_code = 0;
  fake.compile_output = "";
  fake.run_output = "";
  fake.run_exited = true;
  fake.run_exit_code = 0;
  fake.run_truncated = false;
  {
    auto res = submit(cli, u.token, pid, "cpp17", "int main(){}");
    check(res && res->status == 200, "WA 提交返回 200");
    json body = res ? json::parse(res->body) : json();
    check(body.value("status", "") == "WA", "状态为 WA");
    check(body.contains("global_deadline_hit") &&
              body["global_deadline_hit"].is_boolean() &&
              body["global_deadline_hit"] == false,
          "提交级 global_deadline_hit=false 存在");
    check(body.contains("cancelled") && body["cancelled"].is_boolean() &&
              body["cancelled"] == false,
          "提交级 cancelled=false 存在");
    bool all_have = !body["results"].empty();
    for (const auto &item : body["results"]) {
      if (!item.contains("input") || !item.contains("expected_output") ||
          !item.contains("actual_output")) {
        all_have = false;
      }
    }
    check(all_have, "WA 点含 input/expected_output/actual_output");
    check(body["results"][0].contains("actual_output") &&
              body["results"][0]["actual_output"].is_string() &&
              body["results"][0]["actual_output"].get<std::string>().empty(),
          "空实际输出以空字符串返回（区别于缺失）");
  }

  // 标准输出截断：状态 RE，逐点 output_truncated=true。
  fake.run_output = "this output is truncated";
  fake.run_truncated = true;
  {
    auto res = submit(cli, u.token, pid, "cpp17", "int main(){}");
    json body = res ? json::parse(res->body) : json();
    check(body.value("status", "") == "RE", "输出截断判为 RE");
    check(!body["results"].empty() &&
              body["results"][0].value("output_truncated", false) == true,
          "逐点 output_truncated=true");
    check(body["results"][0].contains("actual_output"),
          "截断点仍返回 actual_output");
  }

  // 编译失败：CE，results 为空，compile_output 存在。
  fake.run_truncated = false;
  fake.compile_exit_code = 1;
  fake.compile_output = "main.cpp:1:1: error: expected ';'";
  {
    auto res = submit(cli, u.token, pid, "cpp17", "bad code");
    json body = res ? json::parse(res->body) : json();
    check(body.value("status", "") == "CE", "编译失败判为 CE");
    check(body.value("compile_ok", true) == false, "compile_ok=false");
    check(body.contains("compile_output") &&
              body["compile_output"].get<std::string>().find("error") !=
                  std::string::npos,
          "返回编译器诊断");
    check(body["results"].is_array() && body["results"].empty(),
          "编译失败时无逐点结果");
  }

  env.stop();
}

// ---------------------------------------------------------------------------
// T-011：全局硬上限导致未全部执行
// ---------------------------------------------------------------------------

void test_global_deadline_partial() {
  std::cout << "\n== M43-3 全局硬上限未执行全部测试点 ==\n";
  FakeExecutor fake;
  fake.run_output = "3\n";
  fake.run_timed_out = true;  // 触发全局/单点超时分类

  oj::judge::JudgeOptions options;
  options.global_time_limit_ms = 5;

  Env env("m43_global", &fake, options);
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m43_global_u", "UserPass123");

  const std::int64_t pid = insert_problem(env.db(), "M43 全局上限题", 1, 60000);
  insert_testcase(env.db(), pid, 0, "1 2\n", "3\n", false);
  insert_testcase(env.db(), pid, 1, "2 2\n", "4\n", false);
  insert_testcase(env.db(), pid, 2, "3 3\n", "6\n", false);

  auto res = submit(cli, u.token, pid, "cpp17", "int main(){}");
  check(res && res->status == 200, "提交返回 200（判题结果非 HTTP 故障）");
  json body = res ? json::parse(res->body) : json();
  check(body.value("global_deadline_hit", false) == true,
        "global_deadline_hit=true");
  check(body.value("cancelled", false) == false, "cancelled=false");
  check(body.value("total", 0) == 3, "total=3");
  check(body["results"].size() < 3u,
        "未执行全部测试点（results < total）");
  check(body.value("status", "") == "SYSERR", "总体状态为 SYSERR");

  env.stop();
}

// ---------------------------------------------------------------------------
// T-011b：编译阶段被全局预算裁剪导致的硬上限
// ---------------------------------------------------------------------------

void test_compile_phase_global_cap() {
  std::cout << "\n== M43-5 编译阶段触发全局硬上限 ==\n";
  FakeExecutor fake;
  fake.compile_timed_out = true;  // 编译超时，且编译预算被全局剩余预算裁剪

  oj::judge::JudgeOptions options;
  // 全局预算 2000ms < 编译保护超时 10000ms 且 >0：编译阶段即被全局裁剪。
  options.global_time_limit_ms = 2000;

  Env env("m43_ccap", &fake, options);
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m43_ccap_u", "UserPass123");

  const std::int64_t pid = insert_problem(env.db(), "M43 编译全局裁剪", 1);
  insert_testcase(env.db(), pid, 0, "1\n", "1\n", false);
  insert_testcase(env.db(), pid, 1, "2\n", "2\n", false);

  auto res = submit(cli, u.token, pid, "cpp17", "int main(){}");
  check(res && res->status == 200, "提交返回 200（内部兜底非 HTTP 故障）");
  json body = res ? json::parse(res->body) : json();
  check(body.value("global_deadline_hit", false) == true,
        "编译阶段裁剪 → global_deadline_hit=true");
  check(body.value("cancelled", false) == false, "cancelled=false");
  check(body.value("status", "") == "SYSERR", "总体状态为 SYSERR");
  check(body.value("total", 0) == 2, "total=2");
  check(body["results"].is_array() && body["results"].empty(),
        "编译阶段终止：无逐点结果，未伪造成通过");

  env.stop();
}

// ---------------------------------------------------------------------------
// T-012：服务取消透传
// ---------------------------------------------------------------------------

void test_cancelled_passthrough() {
  std::cout << "\n== M43-4 服务取消透传（cancelled）==\n";
  FakeExecutor fake;
  fake.run_cancelled = true;
  fake.run_exited = false;

  Env env("m43_cancel", &fake);
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m43_cancel_u", "UserPass123");

  const std::int64_t pid = insert_problem(env.db(), "M43 取消题", 1);
  insert_testcase(env.db(), pid, 0, "1\n", "1\n", false);
  insert_testcase(env.db(), pid, 1, "2\n", "2\n", false);

  auto res = submit(cli, u.token, pid, "cpp17", "int main(){}");
  check(res && res->status == 200, "提交返回 200");
  json body = res ? json::parse(res->body) : json();
  check(body.value("cancelled", false) == true, "cancelled=true");
  check(body.value("status", "") == "SYSERR", "取消总体状态为 SYSERR");
  check(!body["results"].empty(), "保留已获得的逐点结果");
  check(body["results"][0].value("status", "") == "SYSERR",
        "被取消的测试点状态为 SYSERR");

  env.stop();
}

} // namespace

int main() {
  std::cout << "M4.3 题目与做题页面后端接口集成测试\n";
  test_detail_owner_status();
  test_submit_response_fields();
  test_global_deadline_partial();
  test_compile_phase_global_cap();
  test_cancelled_passthrough();
  std::cout << "\n失败数：" << g_failures << "\n";
  return g_failures == 0 ? 0 : 1;
}
