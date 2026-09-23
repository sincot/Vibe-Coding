// M3.6 Rejudge 集成测试。
//
// 使用 /tmp 下的隔离临时库与随机端口上的真实 HTTP 服务，不触碰正式数据库。
// 覆盖：
//   - 管理员权限、首次改密限制、非法/不存在提交 ID
//   - 客户端不能替换原源码、语言、用户归属或指定结果
//   - 使用原提交源码/语言与当前题目配置、完整用例快照重判（真实 g++/gcc）
//   - 更新原记录而保留 ID/归属/源码/语言/created_at，不新增记录、不增加计数
//   - 状态重算：唯一 AC 失效、最早 AC 变化、通过人数一致
//   - 同一提交重复重判去重、不同提交排队、队列满载 503
//   - SYSERR 与服务取消按既定策略保留原结果与统计
//   - 重启后重判结果与统计保持一致
//
// 运行方式：ctest --test-dir build -R rejudge_api --output-on-failure
// 或直接执行 build/oj_rejudge_api_test。

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

const std::string kTestSecret = "it-rejudge-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";
const std::string kAdminNewPassword = "AdminNewPass1";

oj::auth::JwtConfig make_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = kTestSecret;
  cfg.expires_seconds = 3600;
  return cfg;
}

// 简单可控执行器：编译恒成功，运行输出固定值。
class FakeExecutor : public oj::judge::IExecutor {
public:
  bool compile_launch_error = false;
  std::string run_output = "2\n";

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

// 同步门执行器：可阻塞编译阶段，用于占用 worker / 排队场景。
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

// 可响应取消的阻塞执行器：编译阶段等待 release 或取消令牌。
class CancelAwareBlockingExecutor : public oj::judge::IExecutor {
public:
  std::atomic<bool> release{false};
  std::atomic<int> entered{0};
  std::string run_output = "2\n";

  oj::judge::ProcessResult compile(
      const oj::judge::CompileRequest &request) override {
    entered.fetch_add(1);
    while (!release.load()) {
      if (request.cancel != nullptr && request.cancel->cancelled()) {
        oj::judge::ProcessResult result;
        result.cancelled = true;
        result.termination = oj::judge::TerminationReason::Cancelled;
        return result;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
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

// 隔离临时库 + 随机端口真实 HTTP + 可选注入执行器 + 可配置调度器选项。
class Env {
public:
  Env(const std::string &label, oj::judge::IExecutor *executor = nullptr,
      const std::string &db_path = "",
      JudgeManager::Options manager_options = JudgeManager::Options())
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

std::int64_t user_id(oj::Database &db, const std::string &account) {
  std::string err;
  oj::Statement stmt;
  db.prepare("SELECT id FROM users WHERE account = ?", stmt, err);
  stmt.bind(1, account);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t insert_problem(oj::Database &db, const std::string &title,
                            int visible = 1) {
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

bool update_testcase_output(oj::Database &db, std::int64_t problem_id, int ord,
                            const std::string &output) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("UPDATE testcases SET output = ? WHERE problem_id = ? AND "
                  "ord = ?",
                  stmt, err)) {
    return false;
  }
  stmt.bind(1, output);
  stmt.bind(2, static_cast<sqlite3_int64>(problem_id));
  stmt.bind(3, ord);
  return stmt.step() == SQLITE_DONE;
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

bool read_status(oj::Database &db, std::int64_t uid, std::int64_t pid,
                 bool &found, oj::UserProblemStatusRecord &out) {
  oj::UserProblemStatusStore store(db);
  std::string err;
  return store.find(uid, pid, found, out, err);
}

bool read_submission(oj::Database &db, std::int64_t id, bool &found,
                     oj::SubmissionRecord &out) {
  oj::SubmissionStore store(db);
  std::string err;
  return store.find_by_id(id, found, out, err);
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

httplib::Result change_password(httplib::Client &cli, const std::string &token,
                                const std::string &old_password,
                                const std::string &new_password) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  json body;
  body["old_password"] = old_password;
  body["new_password"] = new_password;
  return cli.Post("/api/me/password", h, body.dump(), "application/json");
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

std::string make_admin_token(httplib::Client &cli) {
  int status = 0;
  std::string token = login(cli, "admin", kAdminPassword, status);
  if (token.empty()) {
    return "";
  }
  auto changed = change_password(cli, token, kAdminPassword, kAdminNewPassword);
  if (!changed || changed->status != 200) {
    return "";
  }
  token = login(cli, "admin", kAdminNewPassword, status);
  return token;
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

httplib::Result rejudge_raw(httplib::Client &cli, const std::string &token,
                            const std::string &submission_id,
                            const std::string &body) {
  const std::string path =
      "/api/admin/submissions/" + submission_id + "/rejudge";
  if (token.empty()) {
    return cli.Post(path.c_str(), body, "application/json");
  }
  httplib::Headers headers{{"Authorization", "Bearer " + token}};
  return cli.Post(path.c_str(), headers, body, "application/json");
}

httplib::Result rejudge(httplib::Client &cli, const std::string &token,
                        std::int64_t submission_id) {
  return rejudge_raw(cli, token, std::to_string(submission_id), "{}");
}

// 通过管理员列表查询某题通过人数（pass_count）。
long long pass_count(httplib::Client &cli, const std::string &admin_token,
                     std::int64_t problem_id) {
  httplib::Headers headers{{"Authorization", "Bearer " + admin_token}};
  auto res = cli.Get("/api/problems?visible=all", headers);
  if (!res || res->status != 200) {
    return -1;
  }
  json body = json::parse(res->body);
  for (const auto &p : body["problems"]) {
    if (p.value("id", 0LL) == problem_id) {
      return p.value("pass_count", -1LL);
    }
  }
  return -1;
}

// 以原始 socket 发送重判请求并保持连接打开（用于客户端断连场景）。
bool send_raw_rejudge_keep_open(int port, const std::string &token,
                                std::int64_t submission_id, int &sock_out) {
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
  const std::string payload = "{}";
  std::ostringstream req;
  req << "POST /api/admin/submissions/" << submission_id
      << "/rejudge HTTP/1.1\r\n"
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

bool wait_until(std::function<bool()> predicate,
                std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

constexpr auto kShort = std::chrono::milliseconds(5000);

// ---------------------------------------------------------------------------
// T-001 权限、首改限制与非法/不存在 ID
// ---------------------------------------------------------------------------

void test_permissions_and_invalid_id() {
  std::cout << "权限/首改限制/非法 ID：401/403/400/404 且不修改数据\n";
  FakeExecutor fake;
  Env env("rej_perms", &fake);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "rej_user1", "UserPw123");
  std::int64_t pid = insert_problem(env.db(), "权限题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  auto sub = submit(cli, user.token, std::to_string(pid), "cpp17", "x");
  check(sub && sub->status == 200, "准备提交成功");
  const std::int64_t sid =
      sub ? json::parse(sub->body).value("id", 0LL) : 0LL;
  check(sid > 0, "取得提交 ID");

  const std::int64_t submissions_before = count_rows(env.db(), "submissions");

  // 未登录。
  auto no_auth = rejudge_raw(cli, "", std::to_string(sid), "{}");
  check(no_auth && no_auth->status == 401, "未登录重判 401");

  // 普通用户。
  auto normal = rejudge(cli, user.token, sid);
  check(normal && normal->status == 403, "普通用户重判 403");

  // 未完成首次改密的管理员。
  int status = 0;
  std::string admin_token = login(cli, "admin", kAdminPassword, status);
  check(status == 200 && !admin_token.empty(), "admin 登录成功");
  auto blocked = rejudge(cli, admin_token, sid);
  check(blocked && blocked->status == 403, "未改密 admin 重判 403");
  if (blocked) {
    check(json::parse(blocked->body).value("code", "") ==
              "PASSWORD_CHANGE_REQUIRED",
          "返回 PASSWORD_CHANGE_REQUIRED");
  }

  // 完成首改。
  admin_token = make_admin_token(cli);
  check(!admin_token.empty(), "admin 完成首次改密并取得新 token");

  // 非法 ID。
  check(rejudge_raw(cli, admin_token, "abc", "{}")->status == 400,
        "非数字 ID 400");
  check(rejudge_raw(cli, admin_token, "0", "{}")->status == 400, "0 ID 400");
  check(rejudge_raw(cli, admin_token, "-1", "{}")->status == 400,
        "负数 ID 400");
  // 不存在的 ID。
  auto missing = rejudge(cli, admin_token, 999999999);
  check(missing && missing->status == 404, "不存在提交 ID 404");

  // 权限/参数错误不产生任何数据变化。
  check(count_rows(env.db(), "submissions") == submissions_before,
        "上述失败请求未新增提交记录");

  // 合法重判可用（准备后续用例）。
  auto ok = rejudge(cli, admin_token, sid);
  check(ok && ok->status == 200, "合法重判返回 200");
}

// ---------------------------------------------------------------------------
// T-002 客户端字段不可替换 + 原记录保留（真实 C++17）
// ---------------------------------------------------------------------------

const char *kCppPrintOne =
    "#include <cstdio>\n"
    "int main(){ long long x; if(scanf(\"%lld\",&x)!=1) return 0; "
    "printf(\"1\\n\"); return 0; }\n";

void test_client_fields_cannot_replace() {
  std::cout << "客户端字段不可替换：使用原源码/语言/归属重判\n";
  Env env("rej_fields");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User owner = make_user(cli, env.db(), "rej_owner", "OwnerPw1");
  User other = make_user(cli, env.db(), "rej_other", "OtherPw1");
  std::int64_t pid = insert_problem(env.db(), "字段保护题");
  insert_testcase(env.db(), pid, 0, "0\n", "1\n");

  auto sub = submit(cli, owner.token, std::to_string(pid), "cpp17",
                    kCppPrintOne);
  check(sub && json::parse(sub->body).value("status", "") == "AC",
        "原提交 AC");
  const std::int64_t sid =
      sub ? json::parse(sub->body).value("id", 0LL) : 0LL;

  bool found = false;
  oj::SubmissionRecord before;
  check(read_submission(env.db(), sid, found, before) && found,
        "读取原提交记录");
  const std::string original_created_at = before.created_at;

  std::string admin_token = make_admin_token(cli);
  check(!admin_token.empty(), "admin 就绪");

  json evil_body;
  evil_body["user_id"] = other.id;
  evil_body["language"] = "c11";
  evil_body["code"] = "int main(){return 0;}\n";
  evil_body["status"] = "WA";
  json fake_case;
  fake_case["input"] = "0\n";
  fake_case["output"] = "999\n";
  evil_body["testcases"] = json::array({fake_case});

  auto res = rejudge_raw(cli, admin_token, std::to_string(sid),
                         evil_body.dump());
  check(res && res->status == 200, "重判返回 200");
  if (res) {
    json body = json::parse(res->body);
    check(body.value("status", "") == "AC",
          "结果仍按数据库原源码/语言计算（AC，非伪造 WA）");
    check(body.value("id", 0LL) == sid, "返回原提交 ID");
    check(body.value("created_at", "") == original_created_at,
          "返回原提交时间");
  }

  oj::SubmissionRecord after;
  check(read_submission(env.db(), sid, found, after) && found,
        "重判后记录仍在");
  check(after.id == sid && after.user_id == owner.id &&
            after.problem_id == pid,
        "提交 ID 与归属不变");
  check(after.language == "cpp17" && after.source_code == kCppPrintOne,
        "语言与源码不变");
  check(after.created_at == original_created_at, "created_at 不变");
  check(count_rows(env.db(), "submissions") == 1, "未新增提交记录");

  bool st_found = false;
  oj::UserProblemStatusRecord st;
  check(read_status(env.db(), owner.id, pid, st_found, st) && st_found,
        "状态记录存在");
  check(st.submit_count == 1 && st.accepted,
        "submit_count 不因重判增加，AC 状态保留");
  check(st.first_ac_at == original_created_at, "首次 AC 时间不变");
}

// ---------------------------------------------------------------------------
// T-003 C++17：AC -> WA -> AC（当前用例快照）
// ---------------------------------------------------------------------------

void test_cpp17_ac_wa_ac() {
  std::cout << "C++17：随当前用例重判 AC->WA->AC\n";
  Env env("rej_cpp17");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "rej_cpp_user", "CppPw123");
  std::int64_t pid = insert_problem(env.db(), "C++17 重判题");
  insert_testcase(env.db(), pid, 0, "0\n", "1\n");

  auto sub = submit(cli, user.token, std::to_string(pid), "cpp17",
                    kCppPrintOne);
  check(sub && json::parse(sub->body).value("status", "") == "AC",
        "初始 AC");
  const std::int64_t sid =
      sub ? json::parse(sub->body).value("id", 0LL) : 0LL;
  bool found = false;
  oj::SubmissionRecord rec;
  read_submission(env.db(), sid, found, rec);
  const std::string created_at = rec.created_at;

  std::string admin_token = make_admin_token(cli);

  // 修改期望输出 -> 重判为 WA。
  check(update_testcase_output(env.db(), pid, 0, "2\n"), "修改用例期望为 2");
  auto wa = rejudge(cli, admin_token, sid);
  check(wa && json::parse(wa->body).value("status", "") == "WA",
        "用例变更后重判为 WA");
  read_submission(env.db(), sid, found, rec);
  check(found && rec.status == "WA", "原记录状态更新为 WA");
  check(found && rec.created_at == created_at, "created_at 仍保留");
  bool st_found = false;
  oj::UserProblemStatusRecord st;
  read_status(env.db(), user.id, pid, st_found, st);
  check(st_found && !st.accepted && !st.has_first_ac_at,
        "唯一 AC 失效后状态清空");
  check(st_found && st.submit_count == 1, "submit_count 不变");

  // 改回期望 -> 重判回 AC。
  check(update_testcase_output(env.db(), pid, 0, "1\n"), "修改用例期望回 1");
  auto ac = rejudge(cli, admin_token, sid);
  check(ac && json::parse(ac->body).value("status", "") == "AC",
        "用例恢复后重判回 AC");
  read_status(env.db(), user.id, pid, st_found, st);
  check(st_found && st.accepted && st.first_ac_at == created_at,
        "恢复 AC 后首次 AC 时间取原提交时间");
  check(st_found && st.submit_count == 1, "submit_count 仍不变");
}

// ---------------------------------------------------------------------------
// T-004 C11：WA -> AC -> WA
// ---------------------------------------------------------------------------

const char *kC11PrintOne =
    "#include <stdio.h>\n"
    "int main(void){ long long x; if(scanf(\"%lld\",&x)!=1) return 0; "
    "printf(\"1\\n\"); return 0; }\n";

void test_c11_wa_ac_wa() {
  std::cout << "C11：随当前用例重判 WA->AC->WA\n";
  Env env("rej_c11");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "rej_c_user", "CPw12345");
  std::int64_t pid = insert_problem(env.db(), "C11 重判题");
  insert_testcase(env.db(), pid, 0, "0\n", "2\n");

  auto sub = submit(cli, user.token, std::to_string(pid), "c11",
                    kC11PrintOne);
  check(sub && json::parse(sub->body).value("status", "") == "WA",
        "初始 WA");
  const std::int64_t sid =
      sub ? json::parse(sub->body).value("id", 0LL) : 0LL;

  std::string admin_token = make_admin_token(cli);

  check(update_testcase_output(env.db(), pid, 0, "1\n"), "修改用例期望为 1");
  auto ac = rejudge(cli, admin_token, sid);
  check(ac && json::parse(ac->body).value("status", "") == "AC",
        "C11 重判为 AC");
  bool st_found = false;
  oj::UserProblemStatusRecord st;
  read_status(env.db(), user.id, pid, st_found, st);
  check(st_found && st.accepted && st.submit_count == 1,
        "C11 AC 后状态 accepted 且计数不变");

  check(update_testcase_output(env.db(), pid, 0, "2\n"), "修改用例期望回 2");
  auto wa = rejudge(cli, admin_token, sid);
  check(wa && json::parse(wa->body).value("status", "") == "WA",
        "C11 重判回 WA");
  read_status(env.db(), user.id, pid, st_found, st);
  check(st_found && !st.accepted && !st.has_first_ac_at,
        "C11 WA 后状态清空");
}

// ---------------------------------------------------------------------------
// T-005 状态重算：最早 AC 变化 / 唯一 AC 失效 / 通过人数
// ---------------------------------------------------------------------------

const char *kCppAlwaysOne =
    "#include <cstdio>\n"
    "int main(){ long long x; if(scanf(\"%lld\",&x)!=1) return 0; "
    "printf(\"1\\n\"); return 0; }\n";

const char *kCppTwoWhenNine =
    "#include <cstdio>\n"
    "int main(){ long long x; if(scanf(\"%lld\",&x)!=1) return 0; "
    "printf(\"%lld\\n\", x==9?2:1); return 0; }\n";

void test_status_recompute_earliest_and_clear() {
  std::cout << "状态重算：最早 AC 变化、唯一 AC 失效、通过人数一致\n";
  Env env("rej_recompute");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "rej_recompute_user", "RePw1234");
  std::int64_t pid = insert_problem(env.db(), "状态重算题");
  insert_testcase(env.db(), pid, 0, "0\n", "1\n");

  const std::string t1 = "2026-01-01 00:00:01";
  const std::string t3 = "2026-01-01 00:00:03";
  const std::int64_t sid_a = insert_submission(
      env.db(), user.id, pid, "cpp17", kCppAlwaysOne, "AC", t1);
  const std::int64_t sid_b = insert_submission(
      env.db(), user.id, pid, "cpp17", kCppTwoWhenNine, "AC", t3);
  check(sid_a > 0 && sid_b > 0, "直接插入两条 AC 提交");

  std::string admin_token = make_admin_token(cli);

  // 初始重判 A（仅 T1）：仍为 AC，状态被创建/重算。
  auto first = rejudge(cli, admin_token, sid_a);
  check(first && json::parse(first->body).value("status", "") == "AC",
        "A 重判仍为 AC");
  bool st_found = false;
  oj::UserProblemStatusRecord st;
  read_status(env.db(), user.id, pid, st_found, st);
  check(st_found && st.accepted && st.first_ac_at == t1,
        "首次 AC 时间为最早 AC 提交 t1");
  check(st_found && st.submit_count == 2, "submit_count 按实际提交数保持为 2");
  check(pass_count(cli, admin_token, pid) == 1, "通过人数为 1");

  // 新增 T2（input=9, output=2）：A 重判变 WA，B 的已存 AC 仍然有效。
  check(insert_testcase(env.db(), pid, 1, "9\n", "2\n"), "新增第二用例");
  auto wa_a = rejudge(cli, admin_token, sid_a);
  check(wa_a && json::parse(wa_a->body).value("status", "") == "WA",
        "A 在新用例下重判为 WA");
  read_status(env.db(), user.id, pid, st_found, st);
  check(st_found && st.accepted && st.first_ac_at == t3,
        "最早 AC 失效后首次 AC 时间更新为 t3");
  check(st_found && st.submit_count == 2, "submit_count 不因重判变化");
  check(pass_count(cli, admin_token, pid) == 1, "通过人数仍为 1");

  // 修改 T2 期望为 1：B 重判也变 WA，唯一 AC 失效。
  check(update_testcase_output(env.db(), pid, 1, "1\n"), "修改第二用例期望");
  auto wa_b = rejudge(cli, admin_token, sid_b);
  check(wa_b && json::parse(wa_b->body).value("status", "") == "WA",
        "B 在新用例下重判为 WA");
  read_status(env.db(), user.id, pid, st_found, st);
  check(st_found && !st.accepted && !st.has_first_ac_at,
        "唯一 AC 失效后状态为 none、首次 AC 时间清空");
  check(st_found && st.submit_count == 2, "submit_count 仍不变");
  check(pass_count(cli, admin_token, pid) == 0, "通过人数变为 0");

  // 记录本身未被替换。
  bool found = false;
  oj::SubmissionRecord rec_a;
  read_submission(env.db(), sid_a, found, rec_a);
  check(found && rec_a.id == sid_a && rec_a.created_at == t1 &&
            rec_a.source_code == kCppAlwaysOne && rec_a.status == "WA",
        "A 记录保留 ID/源码/created_at，状态更新为 WA");
  oj::SubmissionRecord rec_b;
  read_submission(env.db(), sid_b, found, rec_b);
  check(found && rec_b.id == sid_b && rec_b.created_at == t3 &&
            rec_b.source_code == kCppTwoWhenNine && rec_b.status == "WA",
        "B 记录保留 ID/源码/created_at，状态更新为 WA");
  check(count_rows(env.db(), "submissions") == 2, "未新增提交记录");
}

// ---------------------------------------------------------------------------
// T-006 并发去重与队列满载
// ---------------------------------------------------------------------------

void test_concurrent_dedup_and_queue_full() {
  std::cout << "并发：同一提交去重、不同提交排队、队列满载 503、完成后释放\n";
  GatedExecutor executor;
  GatedExecutor::Gate gate;
  executor.gate = &gate;
  Env env("rej_dedup", &executor, "",
          JudgeManager::Options(/*capacity=*/1, /*workers=*/1));
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "rej_dedup_user", "DedupPw1");
  std::int64_t pid = insert_problem(env.db(), "并发去重题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  const std::int64_t s1 = insert_submission(env.db(), user.id, pid, "cpp17",
                                            "x1", "WA", "2026-01-01 00:00:01");
  const std::int64_t s2 = insert_submission(env.db(), user.id, pid, "cpp17",
                                            "x2", "WA", "2026-01-01 00:00:02");
  const std::int64_t s3 = insert_submission(env.db(), user.id, pid, "cpp17",
                                            "x3", "WA", "2026-01-01 00:00:03");
  check(s1 > 0 && s2 > 0 && s3 > 0, "准备三条提交");

  std::string admin_token = make_admin_token(cli);

  // S1 占住唯一 worker。
  std::string status1;
  int code1 = 0;
  std::thread t1([&]() {
    httplib::Client thread_cli = make_client(env.port());
    auto res = rejudge(thread_cli, admin_token, s1);
    if (res) {
      code1 = res->status;
      if (res->status == 200) {
        status1 = json::parse(res->body).value("status", "");
      }
    }
  });
  check(gate.wait_entered(1, kShort), "S1 重判开始执行（占用 worker）");

  // S2 进入唯一等待槽位。
  std::string status2;
  int code2 = 0;
  std::thread t2([&]() {
    httplib::Client thread_cli = make_client(env.port());
    auto res = rejudge(thread_cli, admin_token, s2);
    if (res) {
      code2 = res->status;
      if (res->status == 200) {
        status2 = json::parse(res->body).value("status", "");
      }
    }
  });
  check(wait_until(
            [&]() { return env.server()->judge_manager()->queued_count() == 1; },
            kShort),
        "S2 重判进入等待队列");

  // 同一提交 S1 再次重判：去重冲突。
  httplib::Client dup_cli = make_client(env.port());
  auto dup = rejudge(dup_cli, admin_token, s1);
  check(dup && dup->status == 409, "同一提交重复重判返回 409");
  if (dup) {
    check(json::parse(dup->body).value("code", "") == "REJUDGE_IN_PROGRESS",
          "返回 REJUDGE_IN_PROGRESS");
  }

  // 第三个不同提交 S3：队列满载。
  httplib::Client full_cli = make_client(env.port());
  auto full = rejudge(full_cli, admin_token, s3);
  check(full && full->status == 503, "队列满载返回 503");
  if (full) {
    check(json::parse(full->body).value("code", "") == "JUDGE_QUEUE_FULL",
          "返回 JUDGE_QUEUE_FULL");
  }

  gate.release();
  t1.join();
  t2.join();
  check(code1 == 200 && status1 == "AC", "S1 重判完成且为 AC");
  check(code2 == 200 && status2 == "AC", "S2 重判完成且为 AC");

  // 去重已释放：S1 可再次重判。
  auto again = rejudge(cli, admin_token, s1);
  check(again && again->status == 200, "完成后同一提交可再次重判");

  // 统计不被重判改变。
  bool st_found = false;
  oj::UserProblemStatusRecord st;
  read_status(env.db(), user.id, pid, st_found, st);
  check(st_found && st.submit_count == 3, "submit_count 不因重判增加");
  check(count_rows(env.db(), "submissions") == 3, "未新增提交记录");
}

// ---------------------------------------------------------------------------
// T-007 SYSERR：按既定策略保留原结果与统计
// ---------------------------------------------------------------------------

void test_syserr_preserves_original() {
  std::cout << "SYSERR：重判无法完成时保留原结果与统计\n";
  FakeExecutor fake;
  fake.compile_launch_error = true;
  Env env("rej_syserr", &fake);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "rej_syserr_user", "SysPw123");
  std::int64_t pid = insert_problem(env.db(), "SYSERR 保留题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  const std::int64_t sid = insert_submission(
      env.db(), user.id, pid, "cpp17", "x", "AC", "2026-01-01 00:00:01");
  check(sid > 0, "准备原 AC 提交");

  std::string admin_token = make_admin_token(cli);
  auto res = rejudge(cli, admin_token, sid);
  check(res && res->status == 500, "SYSERR 重判返回 500（不声称成功）");

  bool found = false;
  oj::SubmissionRecord rec;
  read_submission(env.db(), sid, found, rec);
  check(found && rec.status == "AC" && rec.created_at == "2026-01-01 00:00:01",
        "原记录状态与时间被保留");
  check(count_rows(env.db(), "submissions") == 1, "未新增提交记录");

  bool st_found = false;
  oj::UserProblemStatusRecord st;
  read_status(env.db(), user.id, pid, st_found, st);
  check(!st_found, "失败的重判未创建/修改状态记录");

  // 失败路径同样释放去重占用：再次重判应得到 500（而非 409 冲突）。
  auto retry = rejudge(cli, admin_token, sid);
  check(retry && retry->status == 500,
        "SYSERR 失败后去重已释放，可再次发起（仍按策略 500）");
}

// ---------------------------------------------------------------------------
// T-008 服务取消：停止时取消重判，保留原结果
// ---------------------------------------------------------------------------

void test_service_stop_cancels_rejudge_preserving_original() {
  std::cout << "服务取消：停止时取消重判，保留原结果与统计\n";
  CancelAwareBlockingExecutor executor;
  Env env("rej_cancel", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "rej_cancel_user", "CancelPw1");
  std::int64_t pid = insert_problem(env.db(), "取消保留题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  const std::int64_t sid = insert_submission(
      env.db(), user.id, pid, "cpp17", "x", "AC", "2026-01-01 00:00:01");
  check(sid > 0, "准备原 AC 提交");

  std::string admin_token = make_admin_token(cli);
  std::atomic<int> response_code{0};
  std::thread t([&]() {
    httplib::Client thread_cli = make_client(env.port());
    auto res = rejudge(thread_cli, admin_token, sid);
    if (res) {
      response_code.store(res->status);
    }
  });
  check(wait_until([&]() { return executor.entered.load() >= 1; }, kShort),
        "重判已进入阻塞执行");

  env.stop(); // 触发取消；执行器看到取消令牌后返回 cancelled。
  t.join();
  check(response_code.load() == 500 || response_code.load() == 0,
        "停止期间重判未返回成功结果");

  bool found = false;
  oj::SubmissionRecord rec;
  read_submission(env.db(), sid, found, rec);
  check(found && rec.status == "AC" && rec.created_at == "2026-01-01 00:00:01",
        "取消后原记录保持不变");
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-008b 客户端断开：已接收的重判任务继续执行并更新原记录，去重可靠释放
// ---------------------------------------------------------------------------

void test_client_disconnect_keeps_rejudge_result() {
  std::cout << "客户端断开：重判任务继续执行、更新原记录且去重释放\n";
  GatedExecutor executor;
  GatedExecutor::Gate gate;
  executor.gate = &gate;
  Env env("rej_disconnect", &executor, "",
          JudgeManager::Options(/*capacity=*/8, /*workers=*/1));
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  User user = make_user(cli, env.db(), "rej_disc_user", "DiscPw12");
  std::int64_t pid = insert_problem(env.db(), "断连重判题");
  insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
  const std::string created_at = "2026-01-01 00:00:01";
  const std::int64_t sid = insert_submission(env.db(), user.id, pid, "cpp17",
                                             "x", "WA", created_at);
  check(sid > 0, "准备原 WA 提交");

  std::string admin_token = make_admin_token(cli);
  int sock = -1;
  check(send_raw_rejudge_keep_open(env.port(), admin_token, sid, sock),
        "发送原始重判请求");
  check(gate.wait_entered(1, kShort), "重判已被接收并在执行");
  ::close(sock); // 客户端在结果返回前断开

  gate.release();
  check(wait_until(
            [&]() {
              bool found = false;
              oj::SubmissionRecord rec;
              if (!read_submission(env.db(), sid, found, rec)) {
                return false;
              }
              return found && rec.status == "AC";
            },
            kShort),
        "客户端断开后重判仍完成并更新原记录");

  bool found = false;
  oj::SubmissionRecord rec;
  read_submission(env.db(), sid, found, rec);
  check(found && rec.status == "AC" && rec.created_at == created_at &&
            rec.source_code == "x",
        "原记录被更新且保留 created_at 与源码");
  check(count_rows(env.db(), "submissions") == 1, "未新增提交记录");

  bool st_found = false;
  oj::UserProblemStatusRecord st;
  read_status(env.db(), user.id, pid, st_found, st);
  check(st_found && st.accepted && st.first_ac_at == created_at &&
            st.submit_count == 1,
        "状态被重算且 submit_count 不变");

  // 去重占用在断开后同样可靠释放：可再次正常重判（有界重试以覆盖 HTTP 线程
  // 唤醒与 guard 析构的极短窗口）。
  check(wait_until(
            [&]() {
              httplib::Client retry_cli = make_client(env.port());
              auto again = rejudge(retry_cli, admin_token, sid);
              return again && again->status == 200;
            },
            kShort),
        "断开后去重已释放，可再次重判");

  env.stop();
  env.close_db();
}

// ---------------------------------------------------------------------------
// T-009a 管理员可重判隐藏题目的提交
// ---------------------------------------------------------------------------

void test_rejudge_hidden_problem_submission() {
  std::cout << "管理员可重判隐藏题目的提交\n";
  FakeExecutor fake;
  Env env("rej_hidden", &fake);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  std::int64_t hidden_pid = insert_problem(env.db(), "隐藏题重判", 0);
  insert_testcase(env.db(), hidden_pid, 0, "1 1\n", "2\n");

  std::string admin_token = make_admin_token(cli);
  check(!admin_token.empty(), "admin 就绪");

  // 管理员向隐藏题提交（既有权限），再重判该提交。
  auto sub = submit(cli, admin_token, std::to_string(hidden_pid), "cpp17", "x");
  check(sub && sub->status == 200 &&
            json::parse(sub->body).value("status", "") == "AC",
        "管理员向隐藏题提交成功");
  const std::int64_t sid =
      sub ? json::parse(sub->body).value("id", 0LL) : 0LL;

  auto re = rejudge(cli, admin_token, sid);
  check(re && re->status == 200 &&
            json::parse(re->body).value("status", "") == "AC",
        "管理员可重判隐藏题目的提交");
}

// ---------------------------------------------------------------------------
// T-009 重启持久化
// ---------------------------------------------------------------------------

void test_persistence_restart() {
  std::cout << "重启后：重判结果、原时间与统计保持一致\n";
  TempDir dir("rej_persist");
  const std::string dbpath = dir.db_path();
  std::int64_t pid = 0;
  std::int64_t sid = 0;
  std::int64_t uid = 0;
  std::string created_at;

  {
    FakeExecutor fake;
    Env env("rej_persist_first", &fake, dbpath);
    check(env.ok(), "首次启动成功");
    httplib::Client cli = make_client(env.port());
    User user = make_user(cli, env.db(), "rej_persist_user", "PersistPw1");
    uid = user.id;
    pid = insert_problem(env.db(), "重启持久化题");
    insert_testcase(env.db(), pid, 0, "1 1\n", "2\n");
    auto sub = submit(cli, user.token, std::to_string(pid), "cpp17", "x");
    check(sub && json::parse(sub->body).value("status", "") == "AC",
          "初始 AC");
    sid = sub ? json::parse(sub->body).value("id", 0LL) : 0LL;
    created_at = json::parse(sub->body).value("created_at", "");

    std::string admin_token = make_admin_token(cli);
    auto re = rejudge(cli, admin_token, sid);
    check(re && re->status == 200 &&
              json::parse(re->body).value("status", "") == "AC",
          "重判完成");
    env.stop();
    env.close_db();
  }

  {
    FakeExecutor fake;
    Env env("rej_persist_second", &fake, dbpath);
    check(env.ok(), "重启成功");
    bool found = false;
    oj::SubmissionRecord rec;
    read_submission(env.db(), sid, found, rec);
    check(found && rec.id == sid && rec.user_id == uid &&
              rec.problem_id == pid,
        "重启后提交记录与归属保留");
    check(found && rec.status == "AC" && rec.created_at == created_at,
          "重启后状态与原提交时间保留");
    check(found && rec.language == "cpp17" && rec.source_code == "x",
          "重启后语言与源码保留");

    bool st_found = false;
    oj::UserProblemStatusRecord st;
    read_status(env.db(), uid, pid, st_found, st);
    check(st_found && st.accepted && st.submit_count == 1 &&
              st.first_ac_at == created_at,
          "重启后做题状态与首次 AC 时间保留");
    env.stop();
    env.close_db();
  }
}

} // namespace

int main() {
  test_permissions_and_invalid_id();
  test_client_fields_cannot_replace();
  test_cpp17_ac_wa_ac();
  test_c11_wa_ac_wa();
  test_status_recompute_earliest_and_clear();
  test_concurrent_dedup_and_queue_full();
  test_syserr_preserves_original();
  test_service_stop_cancels_rejudge_preserving_original();
  test_client_disconnect_keeps_rejudge_result();
  test_rejudge_hidden_problem_submission();
  test_persistence_restart();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部 Rejudge 集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
