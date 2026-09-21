// 提交接口与持久化集成测试（M1.6）。
//
// 使用 /tmp 下的隔离临时库与随机端口上的真实 HTTP 服务，不触碰正式数据库。
// 判题执行器可注入：需要真实编译器语义的用例使用默认 LocalExecutor（真实 g++/gcc），
// 需要可控结果/故障的用例注入 FakeExecutor。
//
// 覆盖：
//   - 已登录用户提交 C++17、C11 正确程序 -> AC、提交 ID、逐点结果
//   - WA、CE 返回并入库，源码完整保存
//   - 未登录/无效 token/未改密/隐藏题/不存在题/非法参数被正确拒绝且不产生记录
//   - 客户端额外字段无法改变提交归属或判题结果
//   - 首次失败建立未 AC 状态且计数为 1
//   - 首次 AC 设置状态与首次 AC 时间；重复 AC 不覆盖；AC 后失败不清除
//   - 多用户多题互不混淆；并发提交不丢计数、不重复创建状态记录
//   - 内部判题故障 SYSERR 的记录与计数规则
//   - 持久化事务中途失败时提交记录与状态一同回滚
//   - 通过点不泄露隐藏测试输入/标准答案
//   - 重启后源码/结果/次数/AC 状态/首次 AC 时间仍保留
//
// 运行方式：ctest --test-dir build -R submit_api --output-on-failure
// 或直接执行 build/oj_submit_api_test。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

#include <atomic>
#include <chrono>
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
#include "submit/submit.h"

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

const std::string kTestSecret = "it-submit-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";

oj::auth::JwtConfig make_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = kTestSecret;
  cfg.expires_seconds = 3600;
  return cfg;
}

// 可控执行器：不启动真实进程，按字段返回编译/运行结果。
// 编译成功时创建输出文件，以满足判题器的「编译成功但无产物 -> SYSERR」检查。
//
// use_source_marker_output=true 时，编译阶段读取源码中 "OUT:<值>" 标记到行尾，
// 作为该次判题所有运行点的输出（以唯一可执行路径为键保存）。这样同一题目下不同
// 提交可得到不同结果，用于验证并发提交的结果互不混用。
class FakeExecutor : public oj::judge::IExecutor {
public:
  bool compile_launch_error = false;
  int compile_exit_code = 0;
  std::string compile_output;

  bool run_launch_error = false;
  std::string run_output;
  int run_exit_code = 0;
  long long run_time_ms = 1;

  bool use_source_marker_output = false;

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
    result.stdout_data = compile_output;
    if (compile_exit_code == 0) {
      std::ofstream out(request.output_path, std::ios::binary);
      out << "fake-program";
      out.close();
      if (use_source_marker_output) {
        std::ifstream in(request.source_path);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string content = buffer.str();
        std::string value;
        const std::string marker = "OUT:";
        std::size_t pos = content.find(marker);
        if (pos != std::string::npos) {
          std::size_t begin = pos + marker.size();
          std::size_t end = content.find('\n', begin);
          value = content.substr(begin, end == std::string::npos
                                            ? std::string::npos
                                            : end - begin);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        outputs_[request.output_path] = value;
      }
    }
    return result;
  }

  oj::judge::ProcessResult run(const oj::judge::RunRequest &request,
                               const std::string &) override {
    oj::judge::ProcessResult result;
    if (run_launch_error) {
      result.launch_error = true;
      result.launch_error_message = "fake: 无法启动";
      return result;
    }
    std::string output = run_output;
    if (use_source_marker_output) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = outputs_.find(request.executable_path);
      if (it != outputs_.end()) {
        output = it->second;
      }
    }
    result.launched = true;
    result.exited = true;
    result.exit_code = run_exit_code;
    result.stdout_data = output;
    result.time_ms = run_time_ms;
    return result;
  }

private:
  std::mutex mutex_;
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
  Env(const std::string &label, const std::string &db_path = "",
      oj::judge::IExecutor *executor = nullptr)
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
    port_ = find_free_port();
    server_ = std::make_unique<oj::HttpServer>("127.0.0.1", port_, *db_,
                                               make_config(),
                                               /*enable_test_routes=*/false,
                                               executor);
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

std::int64_t count_rows(oj::Database &db, const std::string &table) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT COUNT(*) FROM " + table, stmt, err)) {
    return -1;
  }
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

// 统计某用户某题目的状态行数（用于验证唯一状态记录）。
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
                            int visible) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("INSERT INTO problems (title, description, difficulty, tags, "
                  "visible) VALUES (?, '题面', 'easy', '测试', ?) RETURNING id",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, title);
  stmt.bind(2, visible);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

// 指定时限的题目（用于 TLE 验证）。
std::int64_t insert_problem_with_time_limit(oj::Database &db,
                                            const std::string &title,
                                            int visible, int time_limit_ms) {
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

httplib::Result submit_raw(httplib::Client &cli, const std::string &token,
                           const std::string &problem_id,
                           const std::string &body) {
  const std::string path = "/api/problems/" + problem_id + "/submit";
  if (token.empty()) {
    return cli.Post(path.c_str(), body, "application/json");
  }
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Post(path.c_str(), h, body, "application/json");
}

std::string submit_body(const std::string &language, const std::string &code) {
  json body;
  body["language"] = language;
  body["code"] = code;
  return body.dump();
}

httplib::Result submit(httplib::Client &cli, const std::string &token,
                       const std::string &problem_id, const std::string &language,
                       const std::string &code) {
  return submit_raw(cli, token, problem_id, submit_body(language, code));
}

httplib::Result change_password(httplib::Client &cli, const std::string &token,
                                const std::string &old_password,
                                const std::string &new_password) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  json body;
  body["old_password"] = old_password;
  body["new_password"] = new_password;
  return cli.Post("/api/me/password", h, body.dump(), "application/json");
}

// 已登录普通用户的账号与 token。
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

// ---------------------------------------------------------------------------
// 真实编译：C++17 / C11 AC
// ---------------------------------------------------------------------------

const char *kCppSum =
    "#include <iostream>\n"
    "int main(){ long long a,b; if(!(std::cin>>a>>b)) return 0; "
    "std::cout<<(a+b)<<\"\\n\"; return 0; }\n";

const char *kC11Sum =
    "#include <stdio.h>\n"
    "int main(void){ long long a,b; "
    "if(scanf(\"%lld %lld\",&a,&b)!=2) return 0; "
    "printf(\"%lld\\n\", a+b); return 0; }\n";

void test_real_cpp17_and_c11_ac() {
  std::cout << "真实编译：C++17 与 C11 正确程序返回 AC 与逐点结果\n";
  Env env("sub_cpp");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User alice = make_user(cli, env.db(), "alice_real", "AlicePw1");
  check(!alice.token.empty(), "普通用户登录成功");

  std::int64_t problem_id = insert_problem(env.db(), "A+B 真实题", 1);
  insert_testcase(env.db(), problem_id, 0, "1 2\n", "3\n", true);
  insert_testcase(env.db(), problem_id, 1, "100 -50\n", "50\n", false);
  insert_testcase(env.db(), problem_id, 2, "111111111 222222222\n",
                  "333333333\n", false);

  auto cpp = submit(cli, alice.token, std::to_string(problem_id), "cpp17",
                    kCppSum);
  check(cpp && cpp->status == 200, "C++17 提交返回 200");
  if (cpp) {
    json body = json::parse(cpp->body);
    check(body.value("status", "") == "AC", "C++17 判为 AC");
    check(body.value("id", 0LL) > 0, "返回提交 ID");
    check(body.value("passed", -1) == 3 && body.value("total", -1) == 3,
          "3/3 通过");
    check(body.contains("results") && body["results"].is_array() &&
              body["results"].size() == 3,
          "返回 3 个逐点结果");
    check(body["memory_kb"].is_null(), "未采集内存明确表示为 null");
    check(body.value("runtime_ms", -1LL) >= 0, "返回耗时");
    bool no_leak = true;
    for (const auto &item : body["results"]) {
      if (item.value("status", "") != "AC" || item.contains("input") ||
          item.contains("expected_output")) {
        no_leak = false;
      }
    }
    check(no_leak, "通过点不附带隐藏输入/标准答案");
  }

  auto c11 = submit(cli, alice.token, std::to_string(problem_id), "c11", kC11Sum);
  check(c11 && c11->status == 200 &&
            json::parse(c11->body).value("status", "") == "AC",
        "C11 判为 AC");

  // 源码完整入库。
  auto cpp_id = cpp ? json::parse(cpp->body).value("id", 0LL) : 0LL;
  oj::SubmissionStore store(env.db());
  bool found = false;
  oj::SubmissionRecord record;
  std::string err;
  check(store.find_by_id(cpp_id, found, record, err), "查询提交记录成功");
  check(found, "提交记录已入库");
  check(found && record.source_code == kCppSum, "完整源码已保存（未被裁剪/修改）");
  check(found && record.language == "cpp17", "语言按规范值保存");
  check(found && record.status == "AC", "状态入库为 AC");
  check(found && !record.created_at.empty(), "提交时间已记录");
  check(found && record.per_case.find("\"index\"") != std::string::npos,
        "逐点结果 JSON 已入库");
}

// ---------------------------------------------------------------------------
// 真实编译：WA 与 CE
// ---------------------------------------------------------------------------

const char *kCppWrong = "#include <cstdio>\n"
                        "int main(){ long long a,b; "
                        "if(scanf(\"%lld %lld\",&a,&b)!=2) return 0; "
                        "printf(\"0\\n\"); return 0; }\n";

void test_real_wa_and_ce() {
  std::cout << "真实编译：WA 与 CE 返回并正确入库\n";
  Env env("sub_wa_ce");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User bob = make_user(cli, env.db(), "bob_wa", "BobPw123");
  std::int64_t problem_id = insert_problem(env.db(), "A+B WA/CE", 1);
  insert_testcase(env.db(), problem_id, 0, "5 3\n", "8\n", false);
  insert_testcase(env.db(), problem_id, 1, "2 2\n", "4\n", false);

  auto wa = submit(cli, bob.token, std::to_string(problem_id), "cpp17",
                   kCppWrong);
  check(wa && wa->status == 200, "WA 提交返回 200（判题结果非 HTTP 故障）");
  std::int64_t wa_id = 0;
  if (wa) {
    json body = json::parse(wa->body);
    wa_id = body.value("id", 0LL);
    check(body.value("status", "") == "WA", "判为 WA");
    bool has_detail = false;
    for (const auto &item : body["results"]) {
      if (item.value("status", "") == "WA" && item.contains("input") &&
          item.contains("expected_output") && item.contains("actual_output")) {
        has_detail = true;
      }
    }
    check(has_detail, "WA 点返回输入/期望输出/实际输出");
  }

  auto ce = submit(cli, bob.token, std::to_string(problem_id), "cpp17",
                   "int main(){ this is not valid c++ }\n");
  check(ce && ce->status == 200, "CE 提交返回 200（判题结果非 HTTP 故障）");
  if (ce) {
    json body = json::parse(ce->body);
    check(body.value("status", "") == "CE", "判为 CE");
    check(!body.value("compile_output", "").empty(), "返回编译诊断");
    check(body["results"].is_array() && body["results"].empty(),
          "CE 时不执行测试点");
  }

  // WA 源码完整保存。
  oj::SubmissionStore store(env.db());
  bool found = false;
  oj::SubmissionRecord record;
  std::string err;
  store.find_by_id(wa_id, found, record, err);
  check(found && record.source_code == kCppWrong, "WA 提交的完整源码已保存");
  check(found && record.status == "WA", "WA 状态入库");
}

// ---------------------------------------------------------------------------
// 已有异常结果：TLE / RE
// ---------------------------------------------------------------------------

void test_real_tle_and_re() {
  std::cout << "真实编译：TLE 与 RE 返回并正确入库计数\n";
  Env env("sub_tle_re");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "tle_user", "TlePw1234");
  std::int64_t problem_id =
      insert_problem_with_time_limit(env.db(), "TLE/RE 题", 1, 300);
  insert_testcase(env.db(), problem_id, 0, "", "1\n", false);
  const std::string pid = std::to_string(problem_id);

  const char *loop =
      "int main(){ volatile unsigned long long c=0; while(true){ c++; } "
      "return 0; }\n";
  auto tle = submit(cli, user.token, pid, "cpp17", loop);
  check(tle && tle->status == 200, "TLE 提交返回 200（判题结果非 HTTP 故障）");
  std::int64_t tle_id = 0;
  if (tle) {
    json body = json::parse(tle->body);
    tle_id = body.value("id", 0LL);
    check(body.value("status", "") == "TLE", "死循环判为 TLE");
  }

  const char *crash = "#include <cstdlib>\nint main(){ std::abort(); }\n";
  auto re = submit(cli, user.token, pid, "cpp17", crash);
  check(re && re->status == 200, "RE 提交返回 200");
  if (re) {
    check(json::parse(re->body).value("status", "") == "RE", "崩溃判为 RE");
  }

  oj::SubmissionStore store(env.db());
  bool found = false;
  oj::SubmissionRecord record;
  std::string err;
  store.find_by_id(tle_id, found, record, err);
  check(found && record.status == "TLE", "TLE 结果已入库");

  oj::UserProblemStatusRecord st;
  read_status(env.db(), user.id, problem_id, found, st);
  check(found && st.submit_count == 2 && !st.accepted,
        "TLE/RE 均计入 submit_count 且不置 AC");
}

// ---------------------------------------------------------------------------
// language 大小写与规范入库
// ---------------------------------------------------------------------------

void test_language_case_insensitive_and_canonical() {
  std::cout << "language 大小写不敏感，入库为规范小写\n";
  FakeExecutor fake;
  fake.run_output = "2\n";
  Env env("sub_lang", "", &fake);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "lang_user", "LangPw12");
  std::int64_t problem_id = insert_problem(env.db(), "语言规范题", 1);
  insert_testcase(env.db(), problem_id, 0, "1 1\n", "2\n", false);
  const std::string pid = std::to_string(problem_id);

  oj::SubmissionStore store(env.db());
  bool found = false;
  oj::SubmissionRecord record;
  std::string err;

  auto upper = submit(cli, user.token, pid, "CPP17", "x");
  check(upper && upper->status == 200 &&
            json::parse(upper->body).value("status", "") == "AC",
        "CPP17（大写）被接受");
  if (upper) {
    store.find_by_id(json::parse(upper->body).value("id", 0LL), found, record,
                     err);
    check(found && record.language == "cpp17", "入库规范为 cpp17");
  }

  auto c11 = submit(cli, user.token, pid, "C11", "x");
  check(c11 && c11->status == 200 &&
            json::parse(c11->body).value("status", "") == "AC",
        "C11（大写）被接受");
  if (c11) {
    store.find_by_id(json::parse(c11->body).value("id", 0LL), found, record,
                     err);
    check(found && record.language == "c11", "入库规范为 c11");
  }
}

// ---------------------------------------------------------------------------
// 编译信息与耗时持久化
// ---------------------------------------------------------------------------

void test_compile_msg_and_runtime_persisted() {
  std::cout << "编译诊断与耗时按提交结果入库\n";
  FakeExecutor fake;
  fake.compile_exit_code = 1;
  fake.compile_output = "diag-marker";
  Env env("sub_meta", "", &fake);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "meta_user", "MetaPw12");
  std::int64_t problem_id = insert_problem(env.db(), "元数据题", 1);
  // 两个测试点期望一致（Fake 对所有点输出同一内容），便于验证逐点耗时求和。
  insert_testcase(env.db(), problem_id, 0, "1 1\n", "2\n", false);
  insert_testcase(env.db(), problem_id, 1, "2 2\n", "2\n", false);
  const std::string pid = std::to_string(problem_id);

  oj::SubmissionStore store(env.db());
  bool found = false;
  oj::SubmissionRecord record;
  std::string err;

  // CE：compile_msg 入库，未执行测试点，耗时为 0。
  auto ce = submit(cli, user.token, pid, "cpp17", "x");
  check(ce && json::parse(ce->body).value("status", "") == "CE", "判为 CE");
  if (ce) {
    store.find_by_id(json::parse(ce->body).value("id", 0LL), found, record,
                     err);
    check(found && record.compile_msg.find("diag-marker") != std::string::npos,
          "CE 编译诊断已入库");
    check(found && record.runtime_ms == 0, "CE 耗时为 0");
  }

  // AC：逐点耗时求和入库（2 点 * 7ms = 14ms）。
  fake.compile_exit_code = 0;
  fake.compile_output.clear();
  fake.run_output = "2\n";
  fake.run_time_ms = 7;
  auto ac = submit(cli, user.token, pid, "cpp17", "x");
  check(ac && json::parse(ac->body).value("status", "") == "AC", "判为 AC");
  if (ac) {
    store.find_by_id(json::parse(ac->body).value("id", 0LL), found, record, err);
    check(found && record.runtime_ms == 14, "逐点耗时求和入库（14ms）");
    check(found && record.memory_kb == 0,
          "未采集内存以 0 哨兵入库（响应侧为 null）");
  }
}

// ---------------------------------------------------------------------------
// 空用例集：SYSERR 而非 AC
// ---------------------------------------------------------------------------

void test_empty_testcase_is_syserr() {
  std::cout << "题目无测试用例：SYSERR 而非 AC，且入库计数\n";
  Env env("sub_empty");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "empty_user", "EmptyPw1");
  std::int64_t problem_id = insert_problem(env.db(), "零用例题", 1);
  // 不插入任何 testcase。

  auto res = submit(cli, user.token, std::to_string(problem_id), "cpp17",
                    "int main(){return 0;}\n");
  check(res && res->status == 200, "提交返回 200");
  std::int64_t id = 0;
  if (res) {
    json body = json::parse(res->body);
    id = body.value("id", 0LL);
    check(body.value("status", "") == "SYSERR", "空用例集判为 SYSERR 而非 AC");
    check(body.value("total", -1) == 0, "总测试点数为 0");
    check(body["results"].is_array() && body["results"].empty(),
          "无逐点结果");
  }

  oj::SubmissionStore store(env.db());
  bool found = false;
  oj::SubmissionRecord record;
  std::string err;
  store.find_by_id(id, found, record, err);
  check(found && record.status == "SYSERR", "SYSERR 已入库");

  oj::UserProblemStatusRecord st;
  read_status(env.db(), user.id, problem_id, found, st);
  check(found && st.submit_count == 1 && !st.accepted,
        "空用例 SYSERR 计数 1 且不置 AC");
}

// ---------------------------------------------------------------------------
// 并发混合结果：互不混用
// ---------------------------------------------------------------------------

void test_concurrent_mixed_results() {
  std::cout << "并发提交不同正确性的程序：结果互不混用\n";
  FakeExecutor fake;
  fake.use_source_marker_output = true; // 按源码标记决定该次输出
  Env env("sub_mix", "", &fake);
  check(env.ok(), "服务启动成功");
  const int port = env.port();
  httplib::Client cli = make_client(port);

  User user = make_user(cli, env.db(), "mix_user", "MixPw1234");
  std::int64_t problem_id = insert_problem(env.db(), "并发混合题", 1);
  insert_testcase(env.db(), problem_id, 0, "1 1\n", "RIGHT\n", false);
  const std::string pid = std::to_string(problem_id);

  const int kThreads = 8;
  std::vector<std::string> statuses(kThreads);
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      httplib::Client thread_cli = make_client(port);
      const std::string marker = (i % 2 == 0) ? "RIGHT" : "WRONG";
      const std::string source = "// OUT:" + marker + "\n";
      auto res = submit(thread_cli, user.token, pid, "cpp17", source);
      if (res && res->status == 200) {
        statuses[i] = json::parse(res->body).value("status", "");
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  bool all_expected = true;
  for (int i = 0; i < kThreads; ++i) {
    const std::string expected = (i % 2 == 0) ? "AC" : "WA";
    if (statuses[i] != expected) {
      all_expected = false;
      std::cout << "    [INFO] 线程 " << i << " 期望 " << expected << " 实得 "
                << statuses[i] << "\n";
    }
  }
  check(all_expected, "每个并发提交都得到各自程序的判题结果（无混用）");

  oj::UserProblemStatusRecord st;
  bool found = false;
  read_status(env.db(), user.id, problem_id, found, st);
  check(found && st.submit_count == kThreads, "并发混合提交计数正确");
  check(count_status_rows(env.db(), user.id, problem_id) == 1,
        "并发混合提交只有一条状态记录");
}

// ---------------------------------------------------------------------------
// 认证、权限与参数校验：拒绝且不产生记录
// ---------------------------------------------------------------------------

void test_rejections_do_not_persist() {
  std::cout << "认证/权限/参数错误被拒绝，且不产生提交记录或计数变化\n";
  Env env("sub_reject");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User carol = make_user(cli, env.db(), "carol_rej", "CarolPw1");
  std::int64_t problem_id = insert_problem(env.db(), "拒绝对照题", 1);
  insert_testcase(env.db(), problem_id, 0, "1 1\n", "2\n", false);
  std::int64_t hidden_id = insert_problem(env.db(), "隐藏题", 0);
  insert_testcase(env.db(), hidden_id, 0, "1 1\n", "2\n", false);

  const std::int64_t submissions_before = count_rows(env.db(), "submissions");

  // 未登录 / 无效 token。
  check(submit(cli, "", std::to_string(problem_id), "cpp17", kCppSum)->status ==
            401,
        "未登录提交 401");
  check(submit(cli, "not-a-jwt", std::to_string(problem_id), "cpp17",
               kCppSum)
                ->status == 401,
        "无效 token 提交 401");

  const std::string pid = std::to_string(problem_id);
  // 非法 JSON / 非对象 / 字段问题。
  check(submit_raw(cli, carol.token, pid, "{bad json")->status == 400,
        "非法 JSON 400");
  check(submit_raw(cli, carol.token, pid, "[1,2,3]")->status == 400,
        "非对象请求体 400");
  check(submit_raw(cli, carol.token, pid, R"({"language":"cpp17"})")->status ==
            400,
        "缺少 code 400");
  check(submit_raw(cli, carol.token, pid, R"({"code":"x"})")->status == 400,
        "缺少 language 400");
  check(submit_raw(cli, carol.token, pid,
                   R"({"language":123,"code":"x"})")
                ->status == 400,
        "language 类型错误 400");
  check(submit_raw(cli, carol.token, pid,
                   R"({"language":"cpp17","code":42})")
                ->status == 400,
        "code 类型错误 400");
  check(submit(cli, carol.token, pid, "python", "print(1)")->status == 400,
        "不支持的语言 400");
  check(submit(cli, carol.token, pid, "cpp17", "")->status == 400,
        "空源码 400");
  check(submit(cli, carol.token, pid, "cpp17", "   \n\t  ")->status == 400,
        "纯空白源码 400");
  check(submit(cli, carol.token, pid, "cpp17",
               std::string(oj::submit::kMaxSourceBytes + 1, 'a'))
                ->status == 400,
        "超长源码 400");
  // 请求体整体超过服务器上限：在进入业务处理前被拒绝。
  check(submit(cli, carol.token, pid, "cpp17",
               std::string(2 * 1024 * 1024, 'a'))
                ->status == 413,
        "请求体超限 413");

  // 题目 ID 非法 / 不存在 / 隐藏。
  check(submit(cli, carol.token, "abc", "cpp17", kCppSum)->status == 400,
        "非法 ID 400");
  check(submit(cli, carol.token, "0", "cpp17", kCppSum)->status == 400,
        "0 ID 400");
  check(submit(cli, carol.token, "999999999", "cpp17", kCppSum)->status == 404,
        "不存在题目 404");
  check(submit(cli, carol.token, std::to_string(hidden_id), "cpp17", kCppSum)
                ->status == 404,
        "普通用户向隐藏题提交 404");

  // admin 未完成首次改密 -> 403 且带稳定 code。
  int admin_status = 0;
  std::string admin_token = login(cli, "admin", kAdminPassword, admin_status);
  check(admin_status == 200 && !admin_token.empty(), "admin 登录成功");
  auto admin_blocked =
      submit(cli, admin_token, pid, "cpp17", kCppSum);
  check(admin_blocked && admin_blocked->status == 403,
        "未改密 admin 提交 403");
  if (admin_blocked) {
    json body = json::parse(admin_blocked->body);
    check(body.value("code", "") == "PASSWORD_CHANGE_REQUIRED",
          "返回 PASSWORD_CHANGE_REQUIRED");
  }

  // 以上全部被拒：无提交记录、无状态记录、计数不变。
  check(count_rows(env.db(), "submissions") == submissions_before,
        "被拒请求不产生提交记录");
  bool found = false;
  oj::UserProblemStatusRecord st;
  check(read_status(env.db(), carol.id, problem_id, found, st) && !found,
        "被拒请求不创建做题状态");
}

// ---------------------------------------------------------------------------
// 管理员可向隐藏题提交
// ---------------------------------------------------------------------------

void test_admin_can_submit_hidden() {
  std::cout << "已改密管理员可向隐藏题提交\n";
  FakeExecutor fake;
  fake.run_output = "2\n";
  Env env("sub_admin", "", &fake);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  std::int64_t hidden_id = insert_problem(env.db(), "管理员隐藏题", 0);
  insert_testcase(env.db(), hidden_id, 0, "1 1\n", "2\n", false);

  int status = 0;
  std::string admin_token = login(cli, "admin", kAdminPassword, status);
  check(status == 200, "admin 登录成功");
  auto changed = change_password(cli, admin_token, kAdminPassword, "AdminNewPass1");
  check(changed && changed->status == 200, "admin 完成首次改密");

  // 用新密码重新登录取得最新 token。
  admin_token = login(cli, "admin", "AdminNewPass1", status);
  check(status == 200 && !admin_token.empty(), "新密码登录成功");
  auto res = submit(cli, admin_token, std::to_string(hidden_id), "cpp17", "x");
  check(res && res->status == 200 &&
            json::parse(res->body).value("status", "") == "AC",
        "已改密 admin 可向隐藏题提交并得到 AC");
}

// ---------------------------------------------------------------------------
// 额外字段不能改变归属/结果
// ---------------------------------------------------------------------------

void test_extra_fields_ignored() {
  std::cout << "客户端额外字段不能改变提交归属或判题结果\n";
  FakeExecutor fake;
  fake.run_output = "1\n"; // 与期望不同 -> WA
  Env env("sub_extra", "", &fake);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User dave = make_user(cli, env.db(), "dave_extra", "DavePw12");
  User evil = make_user(cli, env.db(), "evil_extra", "EvilPw12");
  check(dave.id > 0 && evil.id > 0, "两个用户就绪");

  std::int64_t problem_id = insert_problem(env.db(), "额外字段题", 1);
  insert_testcase(env.db(), problem_id, 0, "1 1\n", "2\n", false);

  json body;
  body["language"] = "cpp17";
  body["code"] = "source-marker";
  body["user_id"] = evil.id;          // 试图伪造归属
  body["id"] = evil.id;
  body["status"] = "AC";              // 试图伪造判题状态
  json injected_case;
  injected_case["input"] = "1 1\n";
  injected_case["output"] = "1\n";    // 若被采纳则会判 AC
  body["testcases"] = json::array({injected_case});
  body["expected_output"] = "1\n";
  body["time_limit_ms"] = 999999999;

  auto res = submit_raw(cli, dave.token, std::to_string(problem_id),
                        body.dump());
  check(res && res->status == 200, "带额外字段的提交仍返回 200");
  std::int64_t id = 0;
  if (res) {
    json resp = json::parse(res->body);
    id = resp.value("id", 0LL);
    check(resp.value("status", "") == "WA",
          "判题结果仍按数据库用例计算（WA，而非伪造的 AC）");
  }

  oj::SubmissionStore store(env.db());
  bool found = false;
  oj::SubmissionRecord record;
  std::string err;
  store.find_by_id(id, found, record, err);
  check(found && record.user_id == dave.id, "提交归属仍为当前登录用户");
  check(found && record.source_code == "source-marker", "源码未被篡改");
  check(found && record.status == "WA", "入库状态未被客户端字段影响");
}

// ---------------------------------------------------------------------------
// 状态计算：首次失败 / 首次 AC / 重复 AC / AC 后失败
// ---------------------------------------------------------------------------

void test_status_transitions() {
  std::cout << "做题状态：首次失败、首次 AC、重复 AC、AC 后失败\n";
  FakeExecutor fake;
  fake.run_output = "9\n"; // 先制造失败
  Env env("sub_status", "", &fake);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User erin = make_user(cli, env.db(), "erin_status", "ErinPw12");
  std::int64_t problem_id = insert_problem(env.db(), "状态题", 1);
  insert_testcase(env.db(), problem_id, 0, "1 1\n", "2\n", false);
  const std::string pid = std::to_string(problem_id);

  // 首次失败。
  auto fail1 = submit(cli, erin.token, pid, "cpp17", "source");
  check(fail1 && json::parse(fail1->body).value("status", "") == "WA",
        "首次提交 WA");
  bool found = false;
  oj::UserProblemStatusRecord st;
  read_status(env.db(), erin.id, problem_id, found, st);
  check(found && !st.accepted && st.submit_count == 1,
        "首次失败建立未 AC 状态且 submit_count=1");
  check(found && !st.has_first_ac_at, "未 AC 时不写首次 AC 时间");

  // 切换为 AC。
  fake.run_output = "2\n";
  auto ac1 = submit(cli, erin.token, pid, "cpp17", "source");
  check(ac1 && json::parse(ac1->body).value("status", "") == "AC",
        "随后提交 AC");
  read_status(env.db(), erin.id, problem_id, found, st);
  const std::string first_ac_at = st.first_ac_at;
  check(found && st.accepted && st.submit_count == 2,
        "首次 AC 设置状态并计数为 2");
  check(found && st.has_first_ac_at && !st.first_ac_at.empty(),
        "首次 AC 记录时间");
  std::string first_created_at =
      json::parse(ac1->body).value("created_at", "");
  check(first_ac_at == first_created_at,
        "首次 AC 时间与本次提交时间同口径");

  // 等待 1 秒后重复 AC：时间不得被覆盖。
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  auto ac2 = submit(cli, erin.token, pid, "cpp17", "source");
  check(ac2 && json::parse(ac2->body).value("status", "") == "AC",
        "重复 AC");
  read_status(env.db(), erin.id, problem_id, found, st);
  check(found && st.submit_count == 3, "重复 AC 计数累加");
  check(found && st.first_ac_at == first_ac_at, "重复 AC 不覆盖首次 AC 时间");
  check(found && st.accepted, "重复 AC 保持 accepted");

  // AC 后失败：不清除 AC 状态与首次 AC 时间。
  fake.run_output = "9\n";
  auto fail2 = submit(cli, erin.token, pid, "cpp17", "source");
  check(fail2 && json::parse(fail2->body).value("status", "") == "WA",
        "AC 后提交 WA");
  read_status(env.db(), erin.id, problem_id, found, st);
  check(found && st.accepted && st.first_ac_at == first_ac_at,
        "AC 后失败不清除已通过状态与首次 AC 时间");
  check(found && st.submit_count == 4, "AC 后失败仍累加计数");

  // AC 后 CE：同样不清除。
  fake.compile_exit_code = 1;
  fake.compile_output = "error: boom";
  auto ce = submit(cli, erin.token, pid, "cpp17", "source");
  check(ce && json::parse(ce->body).value("status", "") == "CE", "AC 后 CE");
  read_status(env.db(), erin.id, problem_id, found, st);
  check(found && st.accepted && st.first_ac_at == first_ac_at,
        "AC 后 CE 不清除已通过状态");
  check(found && st.submit_count == 5, "CE 计入提交次数");
}

// ---------------------------------------------------------------------------
// 多用户多题 + 并发计数
// ---------------------------------------------------------------------------

void test_multi_user_problem_and_concurrency() {
  std::cout << "多用户多题独立；并发提交不丢计数、不重复创建状态\n";
  FakeExecutor fake;
  fake.run_output = "2\n";
  Env env("sub_concurrent", "", &fake);
  check(env.ok(), "服务启动成功");
  int port = env.port();
  httplib::Client cli = make_client(port);

  User u1 = make_user(cli, env.db(), "u1_conc", "U1Pw1234");
  User u2 = make_user(cli, env.db(), "u2_conc", "U2Pw1234");
  std::int64_t p1 = insert_problem(env.db(), "并发题1", 1);
  std::int64_t p2 = insert_problem(env.db(), "并发题2", 1);
  insert_testcase(env.db(), p1, 0, "1 1\n", "2\n", false);
  insert_testcase(env.db(), p2, 0, "1 1\n", "2\n", false);

  // 多用户多题互不混淆。
  submit(cli, u1.token, std::to_string(p1), "cpp17", "a");
  submit(cli, u1.token, std::to_string(p2), "cpp17", "a");
  submit(cli, u2.token, std::to_string(p1), "cpp17", "a");
  bool found = false;
  oj::UserProblemStatusRecord st;
  read_status(env.db(), u1.id, p1, found, st);
  check(found && st.submit_count == 1, "u1/p1 计数为 1");
  read_status(env.db(), u1.id, p2, found, st);
  check(found && st.submit_count == 1, "u1/p2 计数为 1");
  read_status(env.db(), u2.id, p1, found, st);
  check(found && st.submit_count == 1, "u2/p1 计数为 1");

  // 并发提交同一用户同一题目。
  const int kThreads = 8;
  std::atomic<int> ok_count{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      httplib::Client thread_cli = make_client(port);
      auto res = submit(thread_cli, u1.token, std::to_string(p2), "cpp17",
                        "concurrent-" + std::to_string(i));
      if (res && res->status == 200) {
        ok_count.fetch_add(1);
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  check(ok_count.load() == kThreads, "并发提交全部返回 200");

  read_status(env.db(), u1.id, p2, found, st);
  check(found && st.submit_count == 1 + kThreads,
        "并发提交未丢失计数（submit_count 累加正确）");
  check(count_rows(env.db(), "user_problem_status") >= 0 &&
            count_status_rows(env.db(), u1.id, p2) == 1,
        "同一用户题目只有一条状态记录");
}

// ---------------------------------------------------------------------------
// SYSERR 记录与计数
// ---------------------------------------------------------------------------

void test_syserr_recorded_and_counted() {
  std::cout << "内部判题故障 SYSERR：记录并计数，非 HTTP 500\n";
  FakeExecutor fake;
  fake.compile_launch_error = true; // 编译环境故障 -> SYSERR
  Env env("sub_syserr", "", &fake);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User frank = make_user(cli, env.db(), "frank_sys", "FrankPw1");
  std::int64_t problem_id = insert_problem(env.db(), "SYSERR 题", 1);
  insert_testcase(env.db(), problem_id, 0, "1 1\n", "2\n", false);

  auto res = submit(cli, frank.token, std::to_string(problem_id), "cpp17", "x");
  check(res && res->status == 200, "SYSERR 作为判题结果返回 200");
  std::int64_t id = 0;
  if (res) {
    json body = json::parse(res->body);
    id = body.value("id", 0LL);
    check(body.value("status", "") == "SYSERR", "状态为 SYSERR");
  }

  oj::SubmissionStore store(env.db());
  bool found = false;
  oj::SubmissionRecord record;
  std::string err;
  store.find_by_id(id, found, record, err);
  check(found && record.status == "SYSERR", "SYSERR 提交已入库");

  oj::UserProblemStatusRecord st;
  read_status(env.db(), frank.id, problem_id, found, st);
  check(found && !st.accepted && st.submit_count == 1,
        "SYSERR 按声明规则计数且不置 AC");
}

// ---------------------------------------------------------------------------
// 事务中途失败整体回滚
// ---------------------------------------------------------------------------

void test_transaction_rollback() {
  std::cout << "持久化事务中途失败：提交记录与做题状态一同回滚\n";
  FakeExecutor fake;
  fake.run_output = "2\n";
  Env env("sub_rollback", "", &fake);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User grace = make_user(cli, env.db(), "grace_rb", "GracePw1");
  std::int64_t problem_id = insert_problem(env.db(), "回滚题", 1);
  insert_testcase(env.db(), problem_id, 0, "1 1\n", "2\n", false);

  const std::int64_t submissions_before = count_rows(env.db(), "submissions");
  const std::int64_t status_before = count_rows(env.db(), "user_problem_status");

  // 用触发器强制状态写入失败：提交记录插入成功后将回滚。
  std::string err;
  check(env.db().exec("CREATE TRIGGER fail_ups BEFORE INSERT ON "
                      "user_problem_status BEGIN SELECT RAISE(FAIL, 'forced'); "
                      "END;",
                      err),
        "创建强制失败触发器");

  auto res = submit(cli, grace.token, std::to_string(problem_id), "cpp17", "x");
  check(res && res->status == 500, "状态写入失败返回 500");
  if (res) {
    check(res->body.find("sqlite") == std::string::npos &&
              res->body.find("forced") == std::string::npos,
          "500 响应不泄露 SQL/内部细节");
  }
  check(count_rows(env.db(), "submissions") == submissions_before,
        "提交记录已随事务回滚");
  check(count_rows(env.db(), "user_problem_status") == status_before,
        "做题状态已随事务回滚");

  // 故障消除后可正常提交。
  check(env.db().exec("DROP TRIGGER fail_ups;", err), "移除触发器");
  auto ok = submit(cli, grace.token, std::to_string(problem_id), "cpp17", "x");
  check(ok && ok->status == 200 &&
            json::parse(ok->body).value("status", "") == "AC",
        "故障消除后提交成功");
  check(count_rows(env.db(), "submissions") == submissions_before + 1,
        "恢复后提交记录 +1");
}

// ---------------------------------------------------------------------------
// 隐藏用例不泄露
// ---------------------------------------------------------------------------

void test_hidden_leak_in_submit_response() {
  std::cout << "提交响应：WA 点给出详情，通过点不泄露隐藏输入/答案\n";
  FakeExecutor fake;
  fake.run_output = "1\n"; // 第 1 点 AC，第 2 点 WA
  Env env("sub_leak", "", &fake);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User henry = make_user(cli, env.db(), "henry_leak", "HenryPw1");
  std::int64_t problem_id = insert_problem(env.db(), "泄露检查题", 1);
  insert_testcase(env.db(), problem_id, 0, "PASSTOKEN12345\n", "1\n", false);
  insert_testcase(env.db(), problem_id, 1, "FAILTOKEN67890\n", "2\n", false);

  auto res =
      submit(cli, henry.token, std::to_string(problem_id), "cpp17", "x");
  check(res && res->status == 200, "提交返回 200");
  if (res) {
    json body = json::parse(res->body);
    const std::string raw = res->body;
    check(body.value("status", "") == "WA", "汇总为 WA");
    check(raw.find("FAILTOKEN67890") != std::string::npos,
          "WA 点返回其隐藏输入详情");
    check(raw.find("PASSTOKEN12345") == std::string::npos,
          "通过点不泄露其隐藏输入");
    // 通过点条目不得包含 input/expected_output/actual_output。
    for (const auto &item : body["results"]) {
      if (item.value("status", "") == "AC") {
        check(!item.contains("input") && !item.contains("expected_output") &&
                  !item.contains("actual_output"),
              "AC 点条目不含隐藏用例字段");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 重启持久化
// ---------------------------------------------------------------------------

void test_persistence_restart() {
  std::cout << "重启后源码/结果/次数/AC 状态/首次 AC 时间仍保留\n";
  TempDir dir("sub_persist");
  const std::string dbpath = dir.db_path();
  std::int64_t problem_id = 0;
  std::int64_t submission_id = 0;
  std::int64_t uid = 0;
  std::string first_ac_at;
  const std::string source = "persist-source-marker";

  {
    FakeExecutor fake;
    fake.run_output = "2\n";
    Env env("sub_persist_first", dbpath, &fake);
    check(env.ok(), "首次启动成功");
    httplib::Client cli = make_client(env.port());
    User ivy = make_user(cli, env.db(), "ivy_persist", "IvyPw1234");
    uid = ivy.id;
    problem_id = insert_problem(env.db(), "持久化题", 1);
    insert_testcase(env.db(), problem_id, 0, "1 1\n", "2\n", false);

    auto res =
        submit(cli, ivy.token, std::to_string(problem_id), "cpp17", source);
    check(res && json::parse(res->body).value("status", "") == "AC",
          "首次提交 AC");
    if (res) {
      json body = json::parse(res->body);
      submission_id = body.value("id", 0LL);
      first_ac_at = body.value("created_at", "");
    }
    env.stop();
    env.close_db();
  }

  {
    Env env("sub_persist_second", dbpath, nullptr);
    check(env.ok(), "重启成功");
    oj::SubmissionStore store(env.db());
    bool found = false;
    oj::SubmissionRecord record;
    std::string err;
    store.find_by_id(submission_id, found, record, err);
    check(found, "重启后提交记录仍在");
    check(found && record.source_code == source, "源码仍完整保留");
    check(found && record.status == "AC", "结果状态仍保留");
    check(found && record.per_case.find("\"index\"") != std::string::npos,
          "逐点结果仍保留");

    oj::UserProblemStatusRecord st;
    read_status(env.db(), uid, problem_id, found, st);
    check(found && st.accepted, "AC 状态仍保留");
    check(found && st.submit_count == 1, "提交次数仍保留");
    check(found && st.first_ac_at == first_ac_at, "首次 AC 时间仍保留");
    env.stop();
    env.close_db();
  }
}

} // namespace

int main() {
  test_real_cpp17_and_c11_ac();
  test_real_wa_and_ce();
  test_real_tle_and_re();
  test_language_case_insensitive_and_canonical();
  test_compile_msg_and_runtime_persisted();
  test_empty_testcase_is_syserr();
  test_rejections_do_not_persist();
  test_admin_can_submit_hidden();
  test_extra_fields_ignored();
  test_status_transitions();
  test_multi_user_problem_and_concurrency();
  test_concurrent_mixed_results();
  test_syserr_recorded_and_counted();
  test_transaction_rollback();
  test_hidden_leak_in_submit_response();
  test_persistence_restart();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部提交接口与持久化集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
