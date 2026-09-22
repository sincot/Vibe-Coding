// 管理员测试用例接口集成测试（M2.2）。
//
// 使用 /tmp 下的隔离临时库与随机端口上的真实 HTTP 服务，不触碰正式数据库。
// 判题执行器可注入可控实现：用于确认后续提交确实使用修改后的输入/期望输出，
// 以及判题期间修改用例时同一次判题使用快照而非混用版本。
//
// 覆盖：
//   - 管理员读/增/改/删用例，响应与数据库一致；新增归属由 URL 题目 ID 决定
//   - 输入/输出中空格、制表符、换行与空字符串原样保存与读取
//   - 非法 JSON/缺字段/类型错误/超长文本/非法 ord 被拒且不部分修改
//   - 不存在题目、不存在用例、“用例属于其他题目”被拒绝
//   - 游客/普通用户/未完成首改的管理员不能读取或修改
//   - 普通用户公开接口看不到新增隐藏用例，公开样例展示不变
//   - ord 规则：起始/重复/缺省/删除保留空号；管理员列表与判题执行顺序一致
//   - 后续提交实际使用修改后的输入与期望输出
//   - 判题期间修改用例不混用版本
//   - 用例修改不覆盖历史提交结果/AC 状态/提交次数
//   - 空测试集不判 AC
//   - 写入失败与并发删除不产生部分更新/孤立记录/未处理异常
//   - 重启后用例内容与顺序保留
//   - 归属只由 URL 题目 ID 决定（请求体 problem_id/id/is_sample 被忽略）
//   - 含公开样例时的缺省 ord 与空题从 0 开始；仅改 ord 不清空其它字段
//   - 非法用例 ID、伪造 token、超过 1 MiB 的请求体、ord 上限边界
//   - 并发修改与删除同一条用例的终态自洽
//
// 运行方式：ctest --test-dir build -R admin_testcases_api --output-on-failure
// 或直接执行 build/oj_admin_testcases_api_test。

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
#include "db/problems.h"
#include "db/schema.h"
#include "db/users.h"
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

const std::string kTestSecret = "it-testcase-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";
const std::string kAdminNewPassword = "AdminNewPass1";

oj::auth::JwtConfig make_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = kTestSecret;
  cfg.expires_seconds = 3600;
  return cfg;
}

// 编译成功并写出产物文件（判题器要求产物存在）。
void write_fake_program(const std::string &path) {
  std::ofstream out(path, std::ios::binary);
  out << "fake-program";
}

// 回显执行器：把标准输入原样作为标准输出返回，并记录每次运行收到的输入。
// 用于验证判题确实使用数据库中的输入与期望输出。
class EchoExecutor : public oj::judge::IExecutor {
public:
  oj::judge::ProcessResult compile(
      const oj::judge::CompileRequest &request) override {
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    write_fake_program(request.output_path);
    return result;
  }

  oj::judge::ProcessResult run(const oj::judge::RunRequest &,
                               const std::string &input) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      inputs_.push_back(input);
    }
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    result.stdout_data = input;
    result.time_ms = 1;
    return result;
  }

  std::vector<std::string> inputs() {
    std::lock_guard<std::mutex> lock(mutex_);
    return inputs_;
  }

private:
  std::mutex mutex_;
  std::vector<std::string> inputs_;
};

// 门控回显执行器：第一次运行阻塞直到显式放行，用于确定性制造「判题进行中修改用例」
// 的交错。后续运行不再阻塞。
class GatedEchoExecutor : public oj::judge::IExecutor {
public:
  oj::judge::ProcessResult compile(
      const oj::judge::CompileRequest &request) override {
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    write_fake_program(request.output_path);
    return result;
  }

  oj::judge::ProcessResult run(const oj::judge::RunRequest &,
                               const std::string &input) override {
    std::size_t index = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      index = run_count_++;
      inputs_.push_back(input);
      if (index == 0) {
        entered_ = true;
      }
    }
    if (index == 0) {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return released_; });
    }
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    result.stdout_data = input;
    result.time_ms = 1;
    return result;
  }

  bool entered() {
    std::lock_guard<std::mutex> lock(mutex_);
    return entered_;
  }

  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    cv_.notify_all();
  }

  std::vector<std::string> inputs() {
    std::lock_guard<std::mutex> lock(mutex_);
    return inputs_;
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool released_ = false;
  std::size_t run_count_ = 0;
  std::vector<std::string> inputs_;
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
  cli.set_read_timeout(30, 0);
  cli.set_write_timeout(30, 0);
  return cli;
}

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
    server_ = std::make_unique<oj::HttpServer>(
        "127.0.0.1", port_, *db_, make_config(),
        /*enable_test_routes=*/false, executor);
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

httplib::Result admin_create_problem(httplib::Client &cli,
                                     const std::string &token,
                                     const std::string &body) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Post("/api/admin/problems", h, body, "application/json");
}

httplib::Result admin_delete_problem(httplib::Client &cli,
                                     const std::string &token,
                                     std::int64_t id) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Delete(("/api/admin/problems/" + std::to_string(id)).c_str(), h);
}

httplib::Result admin_list_testcases(httplib::Client &cli,
                                     const std::string &token,
                                     std::int64_t pid) {
  httplib::Headers h;
  if (!token.empty()) {
    h.emplace("Authorization", "Bearer " + token);
  }
  return cli.Get(
      ("/api/admin/problems/" + std::to_string(pid) + "/testcases").c_str(), h);
}

httplib::Result admin_create_testcase(httplib::Client &cli,
                                      const std::string &token,
                                      std::int64_t pid,
                                      const std::string &body) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Post(
      ("/api/admin/problems/" + std::to_string(pid) + "/testcases").c_str(), h,
      body, "application/json");
}

httplib::Result admin_update_testcase(httplib::Client &cli,
                                      const std::string &token,
                                      std::int64_t pid, std::int64_t tid,
                                      const std::string &body) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Put(("/api/admin/problems/" + std::to_string(pid) +
                  "/testcases/" + std::to_string(tid))
                     .c_str(),
                 h, body, "application/json");
}

httplib::Result admin_delete_testcase(httplib::Client &cli,
                                      const std::string &token,
                                      std::int64_t pid, std::int64_t tid) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Delete(("/api/admin/problems/" + std::to_string(pid) +
                     "/testcases/" + std::to_string(tid))
                        .c_str(),
                    h);
}

httplib::Result get_problem(httplib::Client &cli, std::int64_t id,
                            const std::string &token = "") {
  httplib::Headers h;
  if (!token.empty()) {
    h.emplace("Authorization", "Bearer " + token);
  }
  return cli.Get(("/api/problems/" + std::to_string(id)).c_str(), h);
}

httplib::Result submit(httplib::Client &cli, const std::string &token,
                       std::int64_t id, const std::string &code) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  std::string body = "{\"language\":\"cpp17\",\"code\":\"" + code + "\"}";
  return cli.Post(("/api/problems/" + std::to_string(id) + "/submit").c_str(),
                  h, body, "application/json");
}

std::int64_t create_problem_id(httplib::Client &cli, const std::string &token,
                               const std::string &body) {
  auto res = admin_create_problem(cli, token, body);
  if (!res || res->status != 201) {
    return -1;
  }
  return json::parse(res->body).value("id", -1LL);
}

std::int64_t create_testcase_id(httplib::Client &cli, const std::string &token,
                                std::int64_t pid, const std::string &body) {
  auto res = admin_create_testcase(cli, token, pid, body);
  if (!res || res->status != 201) {
    return -1;
  }
  return json::parse(res->body).value("id", -1LL);
}

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

std::int64_t count_testcases_flag(oj::Database &db, std::int64_t problem_id,
                                  int is_sample) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT COUNT(*) FROM testcases WHERE problem_id = ? AND "
                  "is_sample = ?",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, static_cast<sqlite3_int64>(problem_id));
  stmt.bind(2, is_sample);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::vector<oj::TestcaseRecord> read_testcases(oj::Database &db,
                                               std::int64_t problem_id) {
  oj::ProblemStore store(db);
  std::vector<oj::TestcaseRecord> records;
  std::string err;
  store.list_testcases(problem_id, records, err);
  return records;
}

bool read_testcase(oj::Database &db, std::int64_t tid, bool &found,
                   oj::TestcaseRecord &out) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT id, ord, input, output, is_sample FROM testcases "
                  "WHERE id = ?",
                  stmt, err)) {
    return false;
  }
  stmt.bind(1, static_cast<sqlite3_int64>(tid));
  if (stmt.step() == SQLITE_ROW) {
    out.id = stmt.column_int64(0);
    out.ord = stmt.column_int(1);
    out.input = stmt.column_text(2);
    out.output = stmt.column_text(3);
    out.is_sample = stmt.column_int(4) != 0;
    found = true;
    return true;
  }
  found = false;
  return true;
}

std::string read_submission_status(oj::Database &db, std::int64_t submission_id) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT status FROM submissions WHERE id = ?", stmt, err)) {
    return "<error>";
  }
  stmt.bind(1, static_cast<sqlite3_int64>(submission_id));
  if (stmt.step() != SQLITE_ROW) {
    return "<error>";
  }
  return stmt.column_text(0);
}

std::int64_t read_submit_count(oj::Database &db, std::int64_t user_id,
                               std::int64_t problem_id) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT submit_count FROM user_problem_status WHERE user_id = "
                  "? AND problem_id = ?",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, static_cast<sqlite3_int64>(user_id));
  stmt.bind(2, static_cast<sqlite3_int64>(problem_id));
  if (stmt.step() != SQLITE_ROW) {
    return -1;
  }
  return stmt.column_int64(0);
}

std::int64_t user_id_for_account(oj::Database &db, const std::string &account) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT id FROM users WHERE account = ?", stmt, err)) {
    return -1;
  }
  stmt.bind(1, account);
  if (stmt.step() != SQLITE_ROW) {
    return -1;
  }
  return stmt.column_int64(0);
}

std::int64_t sample_id(oj::Database &db, std::int64_t problem_id) {
  for (const oj::TestcaseRecord &tc : read_testcases(db, problem_id)) {
    if (tc.is_sample) {
      return tc.id;
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// 测试
// ---------------------------------------------------------------------------

void test_crud_and_db_consistency() {
  std::cout << "管理员读/增/改/删用例：响应与数据库一致\n";
  Env env("tc_crud");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  std::int64_t pid = create_problem_id(
      cli, admin,
      R"({"title":"用例CRUD","difficulty":"easy",
          "samples":[{"input":"s-in\n","output":"s-out\n"}]})");
  check(pid > 0, "创建题目成功");

  auto create_res =
      admin_create_testcase(cli, admin, pid,
                            R"({"input":"1 2\n","output":"3\n","ord":0})");
  check(create_res && create_res->status == 201, "新增用例返回 201");
  json created = create_res ? json::parse(create_res->body) : json{};
  const std::int64_t tid = created.value("id", -1LL);
  check(tid > 0, "返回新用例 ID");
  check(created.value("problem_id", -1LL) == pid, "响应含正确题目归属");
  check(created.value("ord", -1) == 0, "响应含 ord");

  bool found = false;
  oj::TestcaseRecord rec;
  check(read_testcase(env.db(), tid, found, rec) && found && !rec.is_sample &&
            rec.input == "1 2\n" && rec.output == "3\n" && rec.ord == 0,
        "数据库与响应一致且使用隐藏用例标记");

  auto list = admin_list_testcases(cli, admin, pid);
  check(list && list->status == 200, "管理员可读取完整用例列表");
  json list_json = list ? json::parse(list->body) : json{};
  check(list_json.value("problem_id", -1LL) == pid, "列表含题目归属");
  check(list_json["testcases"].size() == 2, "列表包含公开样例与隐藏用例");
  bool has_sample = false;
  bool has_hidden = false;
  for (const auto &entry : list_json["testcases"]) {
    if (entry.value("is_sample", false)) {
      has_sample = true;
    }
    if (entry.value("id", -1LL) == tid) {
      has_hidden = true;
      check(entry.value("input", "") == "1 2\n" &&
                entry.value("output", "") == "3\n",
            "隐藏用例字段完整");
    }
  }
  check(has_sample && has_hidden, "公开样例与隐藏用例均返回并带 is_sample 标记");

  auto upd = admin_update_testcase(cli, admin, pid, tid,
                                   R"({"input":"9 9\n","output":"18\n","ord":4})");
  check(upd && upd->status == 200, "修改用例返回 200");
  check(read_testcase(env.db(), tid, found, rec) && found &&
            rec.input == "9 9\n" && rec.output == "18\n" && rec.ord == 4,
        "修改后数据库一致");

  auto del = admin_delete_testcase(cli, admin, pid, tid);
  check(del && del->status == 200, "删除用例返回 200");
  check(read_testcase(env.db(), tid, found, rec) && !found, "删除后记录不存在");
}

void test_raw_text_preserved() {
  std::cout << "空格/制表符/换行/空字符串原样保存与读取\n";
  Env env("tc_raw");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");
  std::int64_t pid = create_problem_id(cli, admin,
                                       R"({"title":"原样","difficulty":"easy"})");
  check(pid > 0, "创建题目成功");

  // 输入含前后空格、制表符与多个换行；输出为空字符串。
  json body;
  body["input"] = "  a\tb\n\n c \n";
  body["output"] = "";
  auto res = admin_create_testcase(cli, admin, pid, body.dump());
  check(res && res->status == 201, "新增含特殊空白与空输出的用例");
  const std::int64_t tid = res ? json::parse(res->body).value("id", -1LL) : -1;

  bool found = false;
  oj::TestcaseRecord rec;
  check(read_testcase(env.db(), tid, found, rec) && found &&
            rec.input == "  a\tb\n\n c \n" && rec.output == "",
        "数据库原样保留空白且空输出保存为空串");

  auto list = admin_list_testcases(cli, admin, pid);
  json lj = list ? json::parse(list->body) : json{};
  bool matched = false;
  for (const auto &entry : lj["testcases"]) {
    if (entry.value("id", -1LL) == tid) {
      matched = entry.value("input", "x") == "  a\tb\n\n c \n" &&
                entry.value("output", "x") == "";
    }
  }
  check(matched, "读取接口原样返回");

  // 显式空串修改应视为清空，而非被忽略。
  auto upd = admin_update_testcase(cli, admin, pid, tid,
                                   R"({"input":""})");
  check(upd && upd->status == 200, "显式空串更新成功");
  check(read_testcase(env.db(), tid, found, rec) && found && rec.input == "",
        "显式空串清空输入（与缺失区分）");
}

void test_validation_no_partial_update() {
  std::cout << "非法输入被拒且不部分修改\n";
  Env env("tc_invalid");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");
  std::int64_t pid = create_problem_id(cli, admin,
                                       R"({"title":"校验","difficulty":"easy"})");
  check(pid > 0, "创建题目成功");

  const std::int64_t before = count_for(env.db(), "testcases", pid);
  check(admin_create_testcase(cli, admin, pid, "not json")->status == 400,
        "非法 JSON 返回 400");
  check(admin_create_testcase(cli, admin, pid, R"({"input":"x"})")->status ==
            400,
        "缺少 output 返回 400");
  check(admin_create_testcase(cli, admin, pid, R"({"output":"x"})")->status ==
            400,
        "缺少 input 返回 400");
  check(admin_create_testcase(cli, admin, pid,
                              R"({"input":1,"output":"x"})")
                ->status == 400,
        "类型错误返回 400");
  check(admin_create_testcase(cli, admin, pid,
                              R"({"input":"x","output":"y","ord":-1})")
                ->status == 400,
        "非法 ord 返回 400");
  const std::string big(70 * 1024, 'a');
  check(admin_create_testcase(
            cli, admin, pid,
            json{{"input", big}, {"output", "y"}}.dump())
                ->status == 400,
        "超长文本返回 400");
  check(count_for(env.db(), "testcases", pid) == before,
        "非法新增未写入数据库");

  const std::int64_t tid = create_testcase_id(
      cli, admin, pid, R"({"input":"orig-in","output":"orig-out","ord":2})");
  check(tid > 0, "创建基准用例");

  // 同一请求 input 合法、ord 非法：整体拒绝，input 不得被修改。
  check(admin_update_testcase(cli, admin, pid, tid,
                              R"({"input":"changed","ord":-1})")
                ->status == 400,
        "混合非法 PUT 返回 400");
  bool found = false;
  oj::TestcaseRecord rec;
  check(read_testcase(env.db(), tid, found, rec) && found &&
            rec.input == "orig-in" && rec.output == "orig-out" && rec.ord == 2,
        "非法 PUT 未发生部分更新");
  check(admin_update_testcase(cli, admin, pid, tid, R"({})")->status == 400,
        "空更新体返回 400");
}

void test_ownership_and_not_found() {
  std::cout << "不存在题目/用例与跨题目归属被拒绝\n";
  Env env("tc_owner");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  std::int64_t p1 = create_problem_id(
      cli, admin,
      R"({"title":"题一","difficulty":"easy",
          "samples":[{"input":"s\n","output":"s\n"}]})");
  std::int64_t p2 = create_problem_id(cli, admin,
                                      R"({"title":"题二","difficulty":"easy"})");
  check(p1 > 0 && p2 > 0, "创建两道题目");
  const std::int64_t tid = create_testcase_id(
      cli, admin, p1, R"({"input":"one\n","output":"one\n"})");
  check(tid > 0, "在题一新增用例");

  check(admin_create_testcase(cli, admin, 999999,
                              R"({"input":"x","output":"y"})")
                ->status == 404,
        "向不存在题目新增返回 404");
  check(admin_update_testcase(cli, admin, p1, 999999,
                              R"({"input":"x"})")
                ->status == 404,
        "修改不存在用例返回 404");
  check(admin_delete_testcase(cli, admin, p1, 999999)->status == 404,
        "删除不存在用例返回 404");
  check(admin_update_testcase(cli, admin, p2, tid, R"({"input":"hijack"})")
                ->status == 404,
        "通过其它题目 ID 修改用例返回 404");
  check(admin_delete_testcase(cli, admin, p2, tid)->status == 404,
        "通过其它题目 ID 删除用例返回 404");

  bool found = false;
  oj::TestcaseRecord rec;
  check(read_testcase(env.db(), tid, found, rec) && found &&
            rec.input == "one\n",
        "跨题目请求未修改原用例");
  check(admin_list_testcases(cli, admin, 999999)->status == 404,
        "读取不存在题目的用例返回 404");
  check(admin_create_testcase(cli, admin, 0, R"({"input":"x","output":"y"})")
                ->status == 400,
        "非法题目 ID 返回 400");
}

void test_public_sample_protected() {
  std::cout << "公开样例不被用例接口修改，公开接口不泄露隐藏用例\n";
  Env env("tc_sample");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  std::int64_t pid = create_problem_id(
      cli, admin,
      R"({"title":"样例保护","difficulty":"easy",
          "samples":[{"input":"SAMPLE-IN-111\n","output":"SAMPLE-OUT-222\n"}]})");
  check(pid > 0, "创建含公开样例的题目");
  const std::int64_t sid = sample_id(env.db(), pid);
  check(sid > 0, "找到公开样例 ID");

  check(admin_update_testcase(cli, admin, pid, sid, R"({"input":"hack"})")
                ->status == 404,
        "通过用例接口修改公开样例返回 404");
  check(admin_delete_testcase(cli, admin, pid, sid)->status == 404,
        "通过用例接口删除公开样例返回 404");
  check(count_testcases_flag(env.db(), pid, 1) == 1, "公开样例仍存在且未被修改");

  // 新增隐藏用例（含哨兵内容），公开接口（游客/普通用户）不得泄露。
  const std::int64_t hid = create_testcase_id(
      cli, admin, pid,
      R"({"input":"HIDDEN-IN-333\n","output":"HIDDEN-OUT-444\n"})");
  check(hid > 0, "新增隐藏用例");
  check((admin_list_testcases(cli, admin, pid))
            ->body.find("HIDDEN-IN-333") != std::string::npos,
        "管理员可读取隐藏用例");

  auto detail = get_problem(cli, pid);
  check(detail && detail->status == 200, "游客可读公开详情");
  check(detail->body.find("SAMPLE-IN-111") != std::string::npos,
        "公开详情展示公开样例");
  check(detail->body.find("HIDDEN-IN-333") == std::string::npos &&
            detail->body.find("HIDDEN-OUT-444") == std::string::npos,
        "公开详情不泄露隐藏用例");

  auto list = cli.Get("/api/problems");
  check(list && list->body.find("HIDDEN-IN-333") == std::string::npos,
        "公开列表不泄露隐藏用例");
}

void test_permissions() {
  std::cout << "游客/普通用户/未改密管理员均不能读取或修改用例\n";
  Env env("tc_perm");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  std::int64_t pid = create_problem_id(cli, admin,
                                       R"({"title":"权限","difficulty":"easy"})");
  check(pid > 0, "创建题目成功");
  const std::int64_t before = count_for(env.db(), "testcases", pid);

  // 游客
  check(admin_list_testcases(cli, "", pid)->status == 401, "游客读取 401");
  check(admin_create_testcase(cli, "", pid, R"({"input":"x","output":"y"})")
                ->status == 401,
        "游客新增 401");
  check(admin_update_testcase(cli, "", pid, 1, R"({"input":"x"})")->status ==
            401,
        "游客修改 401");
  check(admin_delete_testcase(cli, "", pid, 1)->status == 401, "游客删除 401");

  // 普通用户
  std::string account = register_user(cli, "normaluser", "NormalPw1");
  int status = 0;
  std::string user = login(cli, account, "NormalPw1", status);
  check(status == 200 && !user.empty(), "普通用户登录成功");
  check(admin_list_testcases(cli, user, pid)->status == 403,
        "普通用户读取 403");
  check(admin_create_testcase(cli, user, pid, R"({"input":"x","output":"y"})")
                ->status == 403,
        "普通用户新增 403");
  check(admin_update_testcase(cli, user, pid, 1, R"({"input":"x"})")->status ==
            403,
        "普通用户修改 403");
  check(admin_delete_testcase(cli, user, pid, 1)->status == 403,
        "普通用户删除 403");

  check(count_for(env.db(), "testcases", pid) == before,
        "被拒请求均未改变数据库");
}

void test_password_change_required() {
  std::cout << "未完成首次改密的管理员不能读取或修改用例\n";
  // 独立环境：保持 admin 处于 reset_pwd_flag=1，不改密。
  Env env("tc_pwdreq");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  int status = 0;
  std::string fresh = login(cli, "admin", kAdminPassword, status);
  check(status == 200 && !fresh.empty(), "未改密管理员登录成功");

  auto res = admin_create_testcase(cli, fresh, 1,
                                   R"({"input":"x","output":"y"})");
  check(res && res->status == 403, "未改密管理员新增 403");
  if (res) {
    check(json::parse(res->body).value("code", "") ==
              "PASSWORD_CHANGE_REQUIRED",
          "返回 PASSWORD_CHANGE_REQUIRED 标识");
  }
  check(admin_list_testcases(cli, fresh, 1)->status == 403,
        "未改密管理员读取 403");
  check(admin_update_testcase(cli, fresh, 1, 1, R"({"input":"x"})")->status ==
            403,
        "未改密管理员修改 403");
  check(admin_delete_testcase(cli, fresh, 1, 1)->status == 403,
        "未改密管理员删除 403");
}

void test_ord_rule_and_judge_order() {
  std::cout << "ord 规则：重复/缺省/删除保留空号，列表与判题顺序一致\n";
  std::cout << "后续提交实际使用修改后的输入与期望输出\n";
  EchoExecutor *executor = new EchoExecutor();
  Env env("tc_ord", "", executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  std::int64_t pid = create_problem_id(cli, admin,
                                       R"({"title":"排序","difficulty":"easy"})");
  check(pid > 0, "创建无样例题目");

  const std::int64_t t_e = create_testcase_id(
      cli, admin, pid, R"({"input":"e","output":"e","ord":5})");
  const std::int64_t t_a = create_testcase_id(
      cli, admin, pid, R"({"input":"a","output":"a","ord":1})");
  const std::int64_t t_c = create_testcase_id(
      cli, admin, pid, R"({"input":"c","output":"c","ord":3})");
  const std::int64_t t_d = create_testcase_id(
      cli, admin, pid, R"({"input":"d","output":"d","ord":3})");
  // 缺省 ord：追加到末尾 -> max(5)+1 = 6
  auto def = admin_create_testcase(cli, admin, pid, R"({"input":"f","output":"f"})");
  check(def && def->status == 201 && json::parse(def->body).value("ord", -1) == 6,
        "缺省 ord 追加到末尾（max+1）");
  const std::int64_t t_f = def ? json::parse(def->body).value("id", -1LL) : -1;

  auto ordered_inputs = [&](const json &body) {
    std::vector<std::string> inputs;
    for (const auto &entry : body["testcases"]) {
      inputs.push_back(entry.value("input", ""));
    }
    return inputs;
  };
  auto list1 = admin_list_testcases(cli, admin, pid);
  json l1 = json::parse(list1->body);
  std::vector<std::string> expected_a = {"a", "c", "d", "e", "f"};
  check(ordered_inputs(l1) == expected_a,
        "列表按 (ord,id) 排序：重复 ord 以 id 为稳定次序");

  // 判题执行顺序应与列表一致。
  std::string account = register_user(cli, "orduser", "OrdPw1");
  int status = 0;
  std::string user = login(cli, account, "OrdPw1", status);
  check(status == 200 && !user.empty(), "普通用户登录成功");
  auto sub = submit(cli, user, pid, "int main(){}");
  check(sub && sub->status == 200, "提交判题成功");
  check(sub && json::parse(sub->body).value("status", "") == "AC",
        "回显程序全部 AC");
  check(executor->inputs() == expected_a, "判题执行顺序与列表一致");

  // 修改一条输入的期望输出：后续提交必须使用新期望输出。
  check(admin_update_testcase(cli, admin, pid, t_c, R"({"output":"WRONG"})")
                ->status == 200,
        "修改期望输出");
  auto sub2 = submit(cli, user, pid, "int main(){}");
  check(sub2 && sub2->status == 200, "第二次提交成功");
  json s2 = sub2 ? json::parse(sub2->body) : json{};
  check(s2.value("status", "") == "WA", "使用新期望输出后判为 WA");
  bool saw_expected = false;
  for (const auto &r : s2["results"]) {
    if (r.value("status", "") == "WA" &&
        r.value("expected_output", "") == "WRONG") {
      saw_expected = true;
    }
  }
  check(saw_expected, "WA 反馈中的期望输出为修改后的值");

  // 修改输入：执行器应收到新输入。
  check(admin_update_testcase(cli, admin, pid, t_c,
                              R"({"input":"cc","output":"cc"})")
                ->status == 200,
        "修改输入与期望输出");
  auto sub3 = submit(cli, user, pid, "int main(){}");
  json s3 = sub3 ? json::parse(sub3->body) : json{};
  check(sub3 && s3.value("status", "") == "AC", "使用新输入/期望输出后 AC");
  std::vector<std::string> all = executor->inputs();
  check(all.size() >= 15, "执行器记录了三次提交的输入");
  // 第三次提交的输入序列应包含 "cc" 而非 "c"。
  bool saw_cc = false;
  for (std::size_t i = all.size() - 5; i < all.size(); ++i) {
    if (all[i] == "cc") {
      saw_cc = true;
    }
  }
  check(saw_cc, "后续提交实际传入修改后的输入");

  // 更新 ord 改变顺序。
  check(admin_update_testcase(cli, admin, pid, t_e, R"({"ord":0})")->status ==
            200,
        "将 e 的 ord 改为 0");
  auto list2 = admin_list_testcases(cli, admin, pid);
  std::vector<std::string> expected_b = {"e", "a", "cc", "d", "f"};
  check(ordered_inputs(json::parse(list2->body)) == expected_b,
        "修改 ord 后列表顺序更新");

  // 删除不重排、保留空号。
  check(admin_delete_testcase(cli, admin, pid, t_a)->status == 200,
        "删除用例 a");
  auto list3 = admin_list_testcases(cli, admin, pid);
  json l3 = json::parse(list3->body);
  std::vector<std::string> expected_c = {"e", "cc", "d", "f"};
  check(ordered_inputs(l3) == expected_c, "删除后顺序保持，不重排");
  bool gaps_kept = true;
  for (const auto &entry : l3["testcases"]) {
    const std::string in = entry.value("input", "");
    const int ord = entry.value("ord", -1);
    if ((in == "e" && ord != 0) || (in == "cc" && ord != 3) ||
        (in == "d" && ord != 3) || (in == "f" && ord != 6)) {
      gaps_kept = false;
    }
  }
  check(gaps_kept, "删除后其余 ord 保持不变（保留空号）");
}

void test_modify_during_judge_snapshot() {
  std::cout << "判题期间修改用例：同一次判题使用快照，不混用版本\n";
  GatedEchoExecutor executor;
  Env env("tc_gated", "", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  std::int64_t pid = create_problem_id(cli, admin,
                                       R"({"title":"门控","difficulty":"easy"})");
  check(pid > 0, "创建题目成功");
  const std::int64_t t1 =
      create_testcase_id(cli, admin, pid, R"({"input":"X","output":"X","ord":0})");
  const std::int64_t t2 =
      create_testcase_id(cli, admin, pid, R"({"input":"Y","output":"Y","ord":1})");
  check(t1 > 0 && t2 > 0, "创建两条用例");

  std::string account = register_user(cli, "gateduser", "GatedPw1");
  int status = 0;
  std::string user = login(cli, account, "GatedPw1", status);
  check(status == 200 && !user.empty(), "普通用户登录成功");

  int submit_status = 0;
  std::string submit_body;
  std::thread submitter([&]() {
    httplib::Client c = make_client(env.port());
    auto res = submit(c, user, pid, "int main(){}");
    submit_status = res ? res->status : -1;
    submit_body = res ? res->body : "";
  });

  bool entered = false;
  for (int i = 0; i < 5000; ++i) {
    if (executor.entered()) {
      entered = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(entered, "提交已进入判题阶段");
  if (!entered) {
    executor.release();
    submitter.join();
    return;
  }

  // 判题进行中修改第二条用例（此时尚未执行到它）。
  check(admin_update_testcase(cli, admin, pid, t2,
                              R"({"input":"Z","output":"Z"})")
                ->status == 200,
        "判题期间修改用例成功");
  executor.release();
  submitter.join();

  check(submit_status == 200, "提交正常返回");
  json s = json::parse(submit_body);
  check(s.value("status", "") == "AC",
        "本次判题使用修改前的快照（旧输入 Y 与旧期望 Y 匹配）");

  // 修改后的提交（快照 = X,Z）应全部 AC。
  executor.release();
  auto sub2 = submit(cli, user, pid, "int main(){}");
  json s2 = sub2 ? json::parse(sub2->body) : json{};
  check(sub2 && s2.value("status", "") == "AC", "后续提交使用修改后的用例");

  std::vector<std::string> all = executor.inputs();
  // 第一次提交：X, Y；第二次提交：X, Z。
  check(all.size() >= 4 && all[0] == "X" && all[1] == "Y" &&
            all[all.size() - 2] == "X" && all[all.size() - 1] == "Z",
        "同一次判题未混用修改前后的用例");
}

void test_history_not_overwritten() {
  std::cout << "用例修改不覆盖历史提交/AC 状态/提交次数\n";
  EchoExecutor *executor = new EchoExecutor();
  Env env("tc_history", "", executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  std::int64_t pid = create_problem_id(cli, admin,
                                       R"({"title":"历史","difficulty":"easy"})");
  check(pid > 0, "创建题目成功");
  const std::int64_t tid =
      create_testcase_id(cli, admin, pid, R"({"input":"q","output":"q"})");
  check(tid > 0, "创建用例");

  std::string account = register_user(cli, "histuser", "HistPw1");
  int status = 0;
  std::string user = login(cli, account, "HistPw1", status);
  check(status == 200 && !user.empty(), "普通用户登录成功");

  auto sub = submit(cli, user, pid, "int main(){}");
  check(sub && sub->status == 200, "首次提交成功");
  const std::int64_t submission_id =
      sub ? json::parse(sub->body).value("id", -1LL) : -1;
  check(sub && json::parse(sub->body).value("status", "") == "AC",
        "首次提交 AC");
  const std::int64_t uid = user_id_for_account(env.db(), account);
  check(uid > 0, "读取用户 ID");
  const std::int64_t count_before = read_submit_count(env.db(), uid, pid);
  const std::int64_t subs_before = count_for(env.db(), "submissions", pid);

  check(admin_update_testcase(cli, admin, pid, tid,
                              R"({"input":"q2","output":"other"})")
                ->status == 200,
        "修改用例");
  check(admin_delete_testcase(cli, admin, pid, tid)->status == 200,
        "删除用例");

  check(read_submission_status(env.db(), submission_id) == "AC",
        "历史提交状态未被覆盖");
  check(count_for(env.db(), "submissions", pid) == subs_before,
        "提交记录数量未变化");
  check(read_submit_count(env.db(), uid, pid) == count_before,
        "提交次数未变化");
}

void test_empty_testset_not_ac() {
  std::cout << "空测试集不判 AC，返回约定的不可判题结果并正常持久化\n";
  EchoExecutor *executor = new EchoExecutor();
  Env env("tc_empty", "", executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  std::int64_t pid = create_problem_id(cli, admin,
                                       R"({"title":"空集","difficulty":"easy"})");
  check(pid > 0, "创建无样例题目");
  const std::int64_t tid =
      create_testcase_id(cli, admin, pid, R"({"input":"x","output":"x"})");
  check(tid > 0, "创建唯一用例");
  check(admin_delete_testcase(cli, admin, pid, tid)->status == 200,
        "删除唯一用例，测试集为空");
  check(count_for(env.db(), "testcases", pid) == 0, "确认零测试点");

  std::string account = register_user(cli, "emptyuser", "EmptyPw1");
  int status = 0;
  std::string user = login(cli, account, "EmptyPw1", status);
  auto sub = submit(cli, user, pid, "int main(){}");
  check(sub && sub->status == 200, "提交返回 200（判题结果为业务结果）");
  json s = sub ? json::parse(sub->body) : json{};
  check(s.value("status", "") != "AC", "零测试点不判 AC");
  check(s.value("status", "") == "SYSERR", "返回约定的 SYSERR（不可判题）");
  check(s.value("total", -1) == 0, "测试点数为 0");
}

void test_concurrent_delete_problem() {
  std::cout << "并发删除题目与新增用例：不产生孤立记录/未处理异常\n";
  EchoExecutor *executor = new EchoExecutor();
  Env env("tc_race", "", executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);

  bool all_consistent = true;
  for (int i = 0; i < 5; ++i) {
    std::int64_t pid = create_problem_id(
        cli, admin, R"({"title":"竞态","difficulty":"easy"})");
    if (pid <= 0) {
      all_consistent = false;
      break;
    }
    std::atomic<bool> go{false};
    int create_status = 0;
    int delete_status = 0;
    std::thread creator([&]() {
      httplib::Client c = make_client(env.port());
      while (!go.load()) {
        std::this_thread::yield();
      }
      auto res = admin_create_testcase(c, admin, pid,
                                       R"({"input":"1","output":"1"})");
      create_status = res ? res->status : -1;
    });
    std::thread deleter([&]() {
      httplib::Client c = make_client(env.port());
      while (!go.load()) {
        std::this_thread::yield();
      }
      auto res = admin_delete_problem(c, admin, pid);
      delete_status = res ? res->status : -1;
    });
    go = true;
    creator.join();
    deleter.join();

    const std::int64_t testcases = count_for(env.db(), "testcases", pid);
    const bool problem_gone = (json::parse(
        admin_list_testcases(cli, admin, pid)->body).value("problem_id", -1LL) !=
                              pid);
    // 无提交，删除总应成功；题目最终不存在，且无孤立用例。
    if (!(delete_status == 200 && (create_status == 201 || create_status == 404) &&
          testcases == 0 && problem_gone)) {
      all_consistent = false;
      std::cout << "    迭代 " << i << " 不一致：create=" << create_status
                << " delete=" << delete_status << " testcases=" << testcases
                << "\n";
    }
  }
  check(all_consistent, "并发删题与新增用例的所有迭代均一致");

  // 数据库关闭后写入失败返回 500，不产生部分更新/未处理异常。
  std::int64_t pid = create_problem_id(
      cli, admin, R"({"title":"故障","difficulty":"easy"})");
  check(pid > 0, "创建故障测试题");
  env.close_db();
  auto res = admin_create_testcase(cli, admin, pid,
                                   R"({"input":"x","output":"y"})");
  check(res && res->status == 500, "数据库故障新增返回 500");
  if (res) {
    check(res->body.find("sqlite") == std::string::npos &&
              res->body.find("SELECT") == std::string::npos,
          "错误响应不泄露 SQL/细节");
  }
}

void test_persistence_restart() {
  std::cout << "重启后用例内容与顺序保留\n";
  TempDir dir("tc_persist");
  const std::string dbpath = dir.db_path();
  std::int64_t pid = -1;

  {
    Env env("tc_persist_1", dbpath);
    check(env.ok(), "首次启动成功");
    httplib::Client cli = make_client(env.port());
    std::string admin = admin_login(cli);
    pid = create_problem_id(cli, admin,
                            R"({"title":"持久化用例","difficulty":"easy"})");
    check(pid > 0, "创建题目");
    check(create_testcase_id(cli, admin, pid,
                             R"({"input":"second","output":"2","ord":9})") > 0,
          "新增 ord=9 用例");
    check(create_testcase_id(cli, admin, pid,
                             R"({"input":"first","output":"1","ord":2})") > 0,
          "新增 ord=2 用例");
    env.stop();
    env.close_db();
  }

  {
    Env env("tc_persist_2", dbpath);
    check(env.ok(), "重启成功");
    httplib::Client cli = make_client(env.port());
    int status = 0;
    std::string admin = login(cli, "admin", kAdminNewPassword, status);
    check(status == 200 && !admin.empty(), "重启后管理员登录成功");

    auto list = admin_list_testcases(cli, admin, pid);
    check(list && list->status == 200, "重启后可读取用例");
    json lj = list ? json::parse(list->body) : json{};
    check(lj["testcases"].size() == 2, "用例数量保留");
    check(lj["testcases"][0].value("input", "") == "first" &&
              lj["testcases"][0].value("ord", -1) == 2 &&
              lj["testcases"][1].value("input", "") == "second" &&
              lj["testcases"][1].value("ord", -1) == 9,
          "内容与顺序保留");
    env.stop();
    env.close_db();
  }
}

// 新增用例的归属只由 URL 题目 ID 决定；请求体中的 problem_id/id/is_sample 一律忽略，
// 防止客户端把用例挂到别的题目或伪造为公开样例。
void test_create_ownership_body_ignored() {
  std::cout << "新增用例：归属取 URL 题目 ID，请求体同名字段被忽略\n";
  Env env("tc_own_body");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  std::int64_t p1 = create_problem_id(cli, admin,
                                      R"({"title":"归属一","difficulty":"easy"})");
  std::int64_t p2 = create_problem_id(cli, admin,
                                      R"({"title":"归属二","difficulty":"easy"})");
  check(p1 > 0 && p2 > 0, "创建两道题目");

  json body;
  body["input"] = "o";
  body["output"] = "o";
  body["problem_id"] = p2;
  body["id"] = 123456;
  body["is_sample"] = 1;
  auto res = admin_create_testcase(cli, admin, p1, body.dump());
  check(res && res->status == 201, "在题一下新增（请求体伪造成题二/公开样例）");
  json created = res ? json::parse(res->body) : json{};
  const std::int64_t tid = created.value("id", -1LL);
  check(created.value("problem_id", -1LL) == p1 && tid != 123456,
        "响应归属为 URL 题目，ID 由服务端分配");

  bool found = false;
  oj::TestcaseRecord rec;
  check(read_testcase(env.db(), tid, found, rec) && found && !rec.is_sample,
        "记录归属题一且为隐藏用例（is_sample 未被伪造）");
  auto l2 = admin_list_testcases(cli, admin, p2);
  check(l2 && l2->status == 200 &&
            json::parse(l2->body)["testcases"].empty(),
        "题二列表不含伪造归属的用例");
}

// 缺省 ord：空题从 0 开始；含公开样例时追加到样例之后（max+1，含样例占位）。
// 同时验证管理员列表顺序与判题执行顺序在样例/隐藏混排下仍一致。
void test_default_ord_with_samples_and_empty() {
  std::cout << "缺省 ord：空题从 0，含样例时排在其后；列表与判题顺序一致\n";
  EchoExecutor *executor = new EchoExecutor();
  Env env("tc_deford", "", executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  // 空题：列表 200 且为空，首条缺省 ord=0。
  std::int64_t empty_pid = create_problem_id(
      cli, admin, R"({"title":"空题","difficulty":"easy"})");
  auto el = admin_list_testcases(cli, admin, empty_pid);
  check(el && el->status == 200 &&
            json::parse(el->body)["testcases"].empty(),
        "无用例题目列表为 200 且为空");
  auto first = admin_create_testcase(cli, admin, empty_pid,
                                     R"({"input":"z","output":"z"})");
  check(first && first->status == 201 &&
            json::parse(first->body).value("ord", -1) == 0,
        "空题首个用例缺省 ord=0");

  // 含两个公开样例：样例 ord 由 M2.1 从 0 连续编号，隐藏用例追加到其后。
  std::int64_t pid = create_problem_id(
      cli, admin,
      R"({"title":"样例在前","difficulty":"easy",
          "samples":[{"input":"s0","output":"s0"},{"input":"s1","output":"s1"}]})");
  auto l0 = admin_list_testcases(cli, admin, pid);
  json lj0 = json::parse(l0->body);
  check(lj0["testcases"].size() == 2 &&
            lj0["testcases"][0].value("is_sample", false) &&
            lj0["testcases"][0].value("ord", -1) == 0 &&
            lj0["testcases"][1].value("ord", -1) == 1,
        "公开样例缺省 ord 为 0,1");

  auto h2 = admin_create_testcase(cli, admin, pid,
                                  R"({"input":"h2","output":"h2"})");
  auto h3 = admin_create_testcase(cli, admin, pid,
                                  R"({"input":"h3","output":"h3"})");
  check(h2 && json::parse(h2->body).value("ord", -1) == 2,
        "首个隐藏用例缺省 ord = 样例占位后 max+1 = 2");
  check(h3 && json::parse(h3->body).value("ord", -1) == 3,
        "第二个隐藏用例缺省 ord = 3");

  auto l1 = admin_list_testcases(cli, admin, pid);
  json lj1 = json::parse(l1->body);
  std::vector<std::string> order;
  for (const auto &e : lj1["testcases"]) {
    order.push_back(e.value("input", ""));
  }
  std::vector<std::string> expected = {"s0", "s1", "h2", "h3"};
  check(order == expected, "列表按 (ord,id) 混合排序：样例在前、隐藏在后");

  std::string account = register_user(cli, "deford", "DefOrdPw1");
  int status = 0;
  std::string user = login(cli, account, "DefOrdPw1", status);
  auto sub = submit(cli, user, pid, "int main(){}");
  check(sub && json::parse(sub->body).value("status", "") == "AC",
        "回显程序全部 AC");
  check(executor->inputs() == expected,
        "判题执行顺序与管理员列表一致（含公开样例）");
}

// 仅更新 ord 时不得清空输入/期望输出（部分更新语义）。
void test_ord_only_update_preserves_fields() {
  std::cout << "仅更新 ord 不清空输入/期望输出\n";
  Env env("tc_ordonly");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  std::int64_t pid = create_problem_id(cli, admin,
                                       R"({"title":"仅ord","difficulty":"easy"})");
  check(pid > 0, "创建题目");
  const std::int64_t tid = create_testcase_id(
      cli, admin, pid, R"({"input":"keep-in","output":"keep-out","ord":1})");
  check(tid > 0, "创建用例");

  auto upd = admin_update_testcase(cli, admin, pid, tid, R"({"ord":9})");
  check(upd && upd->status == 200 &&
            json::parse(upd->body).value("ord", -1) == 9,
        "仅更新 ord 返回新 ord");
  bool found = false;
  oj::TestcaseRecord rec;
  check(read_testcase(env.db(), tid, found, rec) && found &&
            rec.input == "keep-in" && rec.output == "keep-out" && rec.ord == 9,
        "输入/期望输出保持原值");
}

// 非法用例 ID 与伪造 token。
void test_invalid_ids_and_forged_token() {
  std::cout << "非法用例 ID 与伪造 token 被拒绝\n";
  Env env("tc_badids");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  std::int64_t pid = create_problem_id(cli, admin,
                                       R"({"title":"非法ID","difficulty":"easy"})");
  check(pid > 0, "创建题目");
  const std::int64_t tid = create_testcase_id(
      cli, admin, pid, R"({"input":"a","output":"a"})");
  check(tid > 0, "创建用例");
  const std::int64_t before = count_for(env.db(), "testcases", pid);

  httplib::Headers auth{{"Authorization", "Bearer " + admin}};
  const std::string base =
      "/api/admin/problems/" + std::to_string(pid) + "/testcases";
  check(cli.Put((base + "/abc").c_str(), auth, R"({"input":"x"})",
                "application/json")
                ->status == 400,
        "非数字用例 ID 返回 400");
  check(cli.Put((base + "/0").c_str(), auth, R"({"input":"x"})",
                "application/json")
                ->status == 400,
        "用例 ID 0 返回 400");
  check(cli.Delete((base + "/-1").c_str(), auth)->status == 400,
        "负数用例 ID 返回 400");
  check(cli.Get("/api/admin/problems/abc/testcases", auth)->status == 400,
        "非数字题目 ID 读取返回 400");
  check(cli.Post("/api/admin/problems/0/testcases", auth,
                 R"({"input":"x","output":"y"})", "application/json")
                ->status == 400,
        "题目 ID 0 新增返回 400");

  // 伪造 / 无效 token：401，且不得被当作游客或管理员。
  httplib::Headers bad{{"Authorization", "Bearer not-a-valid-jwt"}};
  check(cli.Get(base.c_str(), bad)->status == 401, "伪造 token 读取 401");
  check(cli.Post(base.c_str(), bad, R"({"input":"x","output":"y"})",
                 "application/json")
                ->status == 401,
        "伪造 token 新增 401");
  check(cli.Put((base + "/" + std::to_string(tid)).c_str(), bad,
                R"({"input":"x"})", "application/json")
                ->status == 401,
        "伪造 token 修改 401");
  check(cli.Delete((base + "/" + std::to_string(tid)).c_str(), bad)->status ==
            401,
        "伪造 token 删除 401");
  check(count_for(env.db(), "testcases", pid) == before,
        "非法 ID / 伪造 token 请求均未改变数据库");
}

// 请求体超过服务器上限（1 MiB）应在进入业务处理前被拒绝。
void test_payload_too_large() {
  std::cout << "请求体超过 1 MiB 返回 413 且不写入\n";
  Env env("tc_413");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  std::int64_t pid = create_problem_id(cli, admin,
                                       R"({"title":"超体","difficulty":"easy"})");
  check(pid > 0, "创建题目");

  json body;
  body["input"] = std::string(2 * 1024 * 1024, 'a');
  body["output"] = "y";
  auto res = admin_create_testcase(cli, admin, pid, body.dump());
  check(res && res->status == 413, "超大请求体返回 413");
  check(count_for(env.db(), "testcases", pid) == 0, "未写入记录");
}

// ord 上限边界：显式最大值可保存；显式超界 400；缺省追加耗尽返回 409（约束冲突）。
void test_ord_cap_boundary() {
  std::cout << "ord 上限边界：显式最大值可保存，自动追加耗尽返回 409\n";
  Env env("tc_ordcap");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  std::int64_t pid = create_problem_id(cli, admin,
                                       R"({"title":"ord上限","difficulty":"easy"})");
  check(pid > 0, "创建题目");

  const std::int64_t tid = create_testcase_id(
      cli, admin, pid, R"({"input":"max","output":"max","ord":1000000})");
  check(tid > 0, "显式 ord=1000000 可保存");
  check(admin_create_testcase(cli, admin, pid,
                              R"({"input":"x","output":"x","ord":1000001})")
                ->status == 400,
        "显式 ord=1000001 返回 400");
  auto over =
      admin_create_testcase(cli, admin, pid, R"({"input":"y","output":"y"})");
  check(over && over->status == 409, "自动追加 ord 耗尽返回 409");
  check(count_for(env.db(), "testcases", pid) == 1,
        "超界/耗尽失败均未写入记录");
  bool found = false;
  oj::TestcaseRecord rec;
  check(read_testcase(env.db(), tid, found, rec) && found && rec.ord == 1000000,
        "已保存用例的 ord 未受影响");
}

// 数据库不可用时读/写接口均返回 500 通用文案，不产生未处理异常。
void test_db_failure_update_delete() {
  std::cout << "数据库故障时修改/删除返回 500 且不泄露内部细节\n";
  Env env("tc_dbfail");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  std::int64_t pid = create_problem_id(cli, admin,
                                       R"({"title":"故障","difficulty":"easy"})");
  const std::int64_t tid = create_testcase_id(
      cli, admin, pid, R"({"input":"a","output":"a"})");
  check(pid > 0 && tid > 0, "创建题目与用例");

  env.close_db();
  auto u = admin_update_testcase(cli, admin, pid, tid, R"({"input":"x"})");
  auto d = admin_delete_testcase(cli, admin, pid, tid);
  auto l = admin_list_testcases(cli, admin, pid);
  check(u && u->status == 500, "数据库故障修改返回 500");
  check(d && d->status == 500, "数据库故障删除返回 500");
  check(l && l->status == 500, "数据库故障读取返回 500");
  for (httplib::Result *r : {&u, &d, &l}) {
    if (*r) {
      check((*r)->body.find("sqlite") == std::string::npos &&
                (*r)->body.find("SELECT") == std::string::npos &&
                (*r)->body.find("testcases") == std::string::npos,
            "错误响应不泄露 SQL/表名/路径");
    }
  }
}

// 并发修改与删除同一条用例：两个操作均被事务串行化，终态自洽（记录最终不存在，
// DELETE 必成功，PUT 为 200 或 404），不产生未处理异常。
void test_concurrent_update_delete_same_testcase() {
  std::cout << "并发修改与删除同一用例：终态自洽、无未处理异常\n";
  Env env("tc_upddel");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);

  bool all_consistent = true;
  for (int i = 0; i < 5; ++i) {
    std::int64_t pid = create_problem_id(
        cli, admin, R"({"title":"改删竞态","difficulty":"easy"})");
    std::int64_t tid = create_testcase_id(
        cli, admin, pid, R"({"input":"a","output":"a"})");
    if (pid <= 0 || tid <= 0) {
      all_consistent = false;
      break;
    }
    std::atomic<bool> go{false};
    int put_status = 0;
    int del_status = 0;
    std::thread putter([&]() {
      httplib::Client c = make_client(env.port());
      while (!go.load()) {
        std::this_thread::yield();
      }
      auto r = admin_update_testcase(c, admin, pid, tid, R"({"input":"b"})");
      put_status = r ? r->status : -1;
    });
    std::thread deleter([&]() {
      httplib::Client c = make_client(env.port());
      while (!go.load()) {
        std::this_thread::yield();
      }
      auto r = admin_delete_testcase(c, admin, pid, tid);
      del_status = r ? r->status : -1;
    });
    go = true;
    putter.join();
    deleter.join();

    bool found = false;
    oj::TestcaseRecord rec;
    read_testcase(env.db(), tid, found, rec);
    if (!(del_status == 200 && (put_status == 200 || put_status == 404) &&
          !found)) {
      all_consistent = false;
      std::cout << "    迭代 " << i << " 不一致：put=" << put_status
                << " del=" << del_status << " exists=" << found << "\n";
    }
  }
  check(all_consistent, "并发 PUT/DELETE 同一用例的所有迭代终态自洽");
}

} // namespace

int main() {
  test_crud_and_db_consistency();
  test_raw_text_preserved();
  test_validation_no_partial_update();
  test_ownership_and_not_found();
  test_public_sample_protected();
  test_permissions();
  test_password_change_required();
  test_ord_rule_and_judge_order();
  test_modify_during_judge_snapshot();
  test_history_not_overwritten();
  test_empty_testset_not_ac();
  test_concurrent_delete_problem();
  test_persistence_restart();
  test_create_ownership_body_ignored();
  test_default_ord_with_samples_and_empty();
  test_ord_only_update_preserves_fields();
  test_invalid_ids_and_forged_token();
  test_payload_too_large();
  test_ord_cap_boundary();
  test_db_failure_update_delete();
  test_concurrent_update_delete_same_testcase();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部管理员测试用例接口集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
