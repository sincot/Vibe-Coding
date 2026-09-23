// 管理员题目接口集成测试（M2.1）。
//
// 使用 /tmp 下的隔离临时库与随机端口上的真实 HTTP 服务，不触碰正式数据库。
// 判题执行器可注入 FakeExecutor；本测试不验证判题正确性（由 M1.5/M1.6 负责），
// 仅用提交链路构造「已有提交」以验证删除规则与可见性变化对历史数据的影响。
//
// 覆盖：
//   - 管理员创建题目：返回有效 ID，字段与默认限制（2000ms/65536KB）保存正确
//   - 创建后可通过查询接口读取；公开样例正确，响应不混入隐藏用例
//   - 修改题面/样例/难度/标签/限制：查询与数据库一致，created_at 保留、updated_at 更新
//   - PUT 部分更新语义：未提供字段不被清空，显式空值可清空
//   - 非法参数/类型/难度/限制被拒绝，原记录不发生部分更新
//   - 游客、普通用户、未完成首次改密的管理员不能调用管理接口，数据库不变
//   - 管理员角色被撤销后旧 token 不能继续执行管理员操作
//   - 隐藏题：游客/普通用户列表不可见、详情与提交被拒；管理员仍可查看
//   - 重新公开后恢复可见，已有提交与做题状态不被可见性修改清除
//   - 删除规则：无关联数据可删除且不留孤立记录；有提交则拒绝且不部分删除
//   - 删除与提交并发时数据一致（无假成功、无未处理异常）
//   - 不存在/非法 ID 的正确响应；数据库故障不泄露内部细节
//   - 重启后新增/修改/可见性设置仍保留
//
// 运行方式：ctest --test-dir build -R admin_problems_api --output-on-failure
// 或直接执行 build/oj_admin_problems_api_test。

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

const std::string kTestSecret = "it-admin-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";
const std::string kAdminNewPassword = "AdminNewPass1";

oj::auth::JwtConfig make_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = kTestSecret;
  cfg.expires_seconds = 3600;
  return cfg;
}

// 极简可控执行器：不调用真实编译器；编译成功时写出产物文件，运行返回固定输出。
// 仅用于让提交链路快速产生一条提交记录，不关心判题结果是否正确。
class FakeExecutor : public oj::judge::IExecutor {
public:
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
    result.stdout_data = "fake-output";
    result.time_ms = 1;
    return result;
  }
};

// 门控执行器：运行阶段阻塞直到测试显式放行，用于确定性制造「提交正在判题时题目
// 被删除」的交错。仅在测试中使用，不启动真实进程。
class GatedExecutor : public oj::judge::IExecutor {
public:
  oj::judge::ProcessResult compile(
      const oj::judge::CompileRequest &request) override {
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
    entered_.store(true);
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return released_; });
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    result.stdout_data = "gated-output";
    result.time_ms = 1;
    return result;
  }

  bool entered() const { return entered_.load(); }

  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    cv_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool released_ = false;
  std::atomic<bool> entered_{false};
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

httplib::Result change_password(httplib::Client &cli, const std::string &token,
                                const std::string &old_password,
                                const std::string &new_password) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Post("/api/me/password", h,
                  "{\"old_password\":\"" + old_password +
                      "\",\"new_password\":\"" + new_password + "\"}",
                  "application/json");
}

// 管理员登录并完成首次改密，返回可用于业务接口的 token（失败返回空串）。
std::string admin_login(httplib::Client &cli) {
  int status = 0;
  std::string token = login(cli, "admin", kAdminPassword, status);
  if (token.empty()) {
    return "";
  }
  auto res = change_password(cli, token, kAdminPassword, kAdminNewPassword);
  if (!res || res->status != 200) {
    return "";
  }
  return token;
}

httplib::Result admin_create(httplib::Client &cli, const std::string &token,
                             const std::string &body) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Post("/api/admin/problems", h, body, "application/json");
}

httplib::Result admin_update(httplib::Client &cli, const std::string &token,
                             std::int64_t id, const std::string &body) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Put(("/api/admin/problems/" + std::to_string(id)).c_str(), h, body,
                 "application/json");
}

httplib::Result admin_delete(httplib::Client &cli, const std::string &token,
                             std::int64_t id) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Delete(("/api/admin/problems/" + std::to_string(id)).c_str(), h);
}

httplib::Result get_problem(httplib::Client &cli, std::int64_t id,
                            const std::string &token = "") {
  httplib::Headers h;
  if (!token.empty()) {
    h.emplace("Authorization", "Bearer " + token);
  }
  return cli.Get(("/api/problems/" + std::to_string(id)).c_str(), h);
}

httplib::Result get_problems(httplib::Client &cli,
                             const std::string &token = "") {
  httplib::Headers h;
  if (!token.empty()) {
    h.emplace("Authorization", "Bearer " + token);
  }
  return cli.Get("/api/problems", h);
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
  auto res = admin_create(cli, token, body);
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

// 统计某题公开样例（is_sample=1）或隐藏用例（is_sample=0）数量。
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

// 读取 seed_key（NULL 返回空串）。
std::string read_seed_key(oj::Database &db, std::int64_t id) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT seed_key FROM problems WHERE id = ?", stmt, err)) {
    return "<error>";
  }
  stmt.bind(1, static_cast<sqlite3_int64>(id));
  if (stmt.step() != SQLITE_ROW || stmt.column_is_null(0)) {
    return "";
  }
  return stmt.column_text(0);
}

// 将题目时间戳强制设为已知历史值，用于确定性验证 updated_at 更新（避免等待）。
bool set_problem_timestamps(oj::Database &db, std::int64_t id,
                            const std::string &created_at,
                            const std::string &updated_at) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("UPDATE problems SET created_at = ?, updated_at = ? WHERE "
                  "id = ?",
                  stmt, err)) {
    return false;
  }
  stmt.bind(1, created_at);
  stmt.bind(2, updated_at);
  stmt.bind(3, static_cast<sqlite3_int64>(id));
  return stmt.step() == SQLITE_DONE;
}

bool problem_exists(oj::Database &db, std::int64_t id) {
  oj::ProblemStore store(db);
  bool found = false;
  oj::ProblemRecord record;
  std::string err;
  store.find_by_id(id, found, record, err);
  return found;
}

oj::ProblemRecord read_problem(oj::Database &db, std::int64_t id,
                               bool &found) {
  oj::ProblemStore store(db);
  oj::ProblemRecord record;
  std::string err;
  store.find_by_id(id, found, record, err);
  return record;
}

bool insert_hidden_testcase(oj::Database &db, std::int64_t problem_id,
                            int ord, const std::string &input,
                            const std::string &output) {
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

bool set_admin_role(oj::Database &db, const std::string &role) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("UPDATE users SET role = ? WHERE account = 'admin'", stmt,
                  err)) {
    return false;
  }
  stmt.bind(1, role);
  return stmt.step() == SQLITE_DONE;
}

// ---------------------------------------------------------------------------
// 测试
// ---------------------------------------------------------------------------

void test_create_defaults_and_read() {
  std::cout << "创建题目：默认限制与可见性正确，可通过查询接口读取\n";
  Env env("adm_create");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首次改密");

  auto res = admin_create(cli, admin,
                          R"({"title":"  默认限制题  ","difficulty":"easy"})");
  check(res && res->status == 201, "创建成功 201");
  std::int64_t id = res ? json::parse(res->body).value("id", -1LL) : -1;
  check(id > 0, "返回有效题目 ID");

  auto detail = get_problem(cli, id);
  check(detail && detail->status == 200, "创建后可通过详情接口读取");
  json d = detail ? json::parse(detail->body) : json{};
  check(d.value("title", "") == "默认限制题", "标题去除首尾空白后保存");
  check(d.value("description", "x") == "", "description 默认空串");
  check(d["tags"].is_array() && d["tags"].empty(), "tags 默认空数组");
  check(d.value("time_limit_ms", -1) == 2000, "时限采用默认 2000ms");
  check(d.value("memory_limit_kb", -1) == 65536, "内存采用默认 65536KB");
  check(d.value("visible", false) == true, "默认可见");
  check(d["samples"].is_array() && d["samples"].empty(), "默认无公开样例");

  bool found = false;
  oj::ProblemRecord record = read_problem(env.db(), id, found);
  check(found && record.time_limit_ms == 2000 &&
            record.memory_limit_kb == 65536 && record.visible,
        "数据库中默认限制与可见性一致");
}

void test_create_with_samples_and_no_hidden_leak() {
  std::cout << "创建含公开样例与隐藏用例：样例正确、响应不泄露隐藏用例\n";
  Env env("adm_samples");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  std::int64_t id = create_problem_id(
      cli, admin,
      R"({"title":"带样例","difficulty":"medium","tags":["数组"],
          "samples":[{"input":"2 3\n","output":"5\n"},
                     {"input":"10 20\n","output":"30\n"}]})");
  check(id > 0, "创建成功");

  // 直接写入隐藏用例，验证详情/列表都不泄露。
  insert_hidden_testcase(env.db(), id, 10, "SENTINEL-HIDDEN-IN-918273\n",
                         "SENTINEL-HIDDEN-OUT-564738\n");

  auto detail = get_problem(cli, id);
  check(detail && detail->status == 200, "详情 200");
  json d = json::parse(detail->body);
  check(d["samples"].is_array() && d["samples"].size() == 2,
        "返回 2 组公开样例");
  check(d["samples"][0].value("input", "") == "2 3\n" &&
            d["samples"][0].value("output", "") == "5\n",
        "公开样例内容正确");
  check(detail->body.find("SENTINEL-HIDDEN-IN-918273") == std::string::npos &&
            detail->body.find("SENTINEL-HIDDEN-OUT-564738") ==
                std::string::npos,
        "详情不泄露隐藏用例");

  auto list = get_problems(cli);
  check(list && list->status == 200, "列表 200");
  check(list->body.find("SENTINEL-HIDDEN-IN-918273") == std::string::npos,
        "列表不泄露隐藏用例");
}

void test_validation_rejected_without_side_effects() {
  std::cout << "非法参数被拒且不产生记录/不部分更新\n";
  Env env("adm_invalid");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  const std::int64_t before = count_rows(env.db(), "problems");
  check(admin_create(cli, admin, R"({"difficulty":"easy"})")->status == 400,
        "缺少 title 返回 400");
  check(admin_create(cli, admin,
                     R"({"title":"T","difficulty":"nope"})")->status == 400,
        "非法难度返回 400");
  check(admin_create(cli, admin,
                     R"({"title":"T","difficulty":"easy","time_limit_ms":0})")
                ->status == 400,
        "时限 0 返回 400");
  check(admin_create(cli, admin,
                     R"({"title":"T","difficulty":"easy","time_limit_ms":-1})")
                ->status == 400,
        "时限负数返回 400");
  check(admin_create(
            cli, admin,
            R"({"title":"T","difficulty":"easy","memory_limit_kb":0})")
                ->status == 400,
        "内存 0 返回 400");
  check(admin_create(cli, admin,
                     R"({"title":"T","difficulty":"easy","tags":["a,b"]})")
                ->status == 400,
        "标签含逗号返回 400");
  check(admin_create(cli, admin, R"({"title":"T","difficulty":"easy","visible":"yes"})")
                ->status == 400,
        "visible 非布尔返回 400");
  check(admin_create(cli, admin, R"({"title":"T","difficulty":"easy","samples":[{"input":"1"}]})")
                ->status == 400,
        "样例缺少 output 返回 400");
  check(admin_create(cli, admin, "not json")->status == 400,
        "非法 JSON 返回 400");
  check(count_rows(env.db(), "problems") == before,
        "所有非法创建均未写入数据库");

  // 先创建一条合法记录，再用非法 PUT 尝试修改，验证不发生部分更新。
  std::int64_t id = create_problem_id(
      cli, admin, R"({"title":"原始标题","difficulty":"easy","tags":["原始"]})");
  check(id > 0, "初始题目创建成功");
  bool found = false;
  oj::ProblemRecord original = read_problem(env.db(), id, found);
  check(found, "读取初始记录");

  // 同一请求里 title 合法、difficulty 非法：整体应被拒绝，title 不得被修改。
  check(admin_update(cli, admin, id,
                     R"({"title":"被改标题","difficulty":"nope"})")
                ->status == 400,
        "混合非法 PUT 返回 400");
  oj::ProblemRecord after = read_problem(env.db(), id, found);
  check(found && after.title == "原始标题" && after.difficulty == "easy",
        "非法 PUT 未发生部分更新");
  check(admin_update(cli, admin, id, R"({})")->status == 400,
        "空更新体返回 400");
  check(admin_update(cli, admin, id, R"({"time_limit_ms":-5})")->status == 400,
        "非法限制值返回 400");
  check(admin_update(cli, admin, 999999, R"({"title":"x"})")->status == 404,
        "更新不存在的题目返回 404");
  check(admin_update(cli, admin, 0, R"({"title":"x"})")->status == 400,
        "更新非法 ID 返回 400");
}

void test_update_partial_and_timestamps() {
  std::cout << "修改题目：字段/样例更新，created_at 保留、updated_at 更新\n";
  Env env("adm_update");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  std::int64_t id = create_problem_id(
      cli, admin,
      R"({"title":"旧标题","description":"旧题面","difficulty":"easy",
          "tags":["旧"],"samples":[{"input":"1\n","output":"1\n"}]})");
  check(id > 0, "创建成功");
  insert_hidden_testcase(env.db(), id, 7, "KEEP-HIDDEN-IN\n", "KEEP-HIDDEN-OUT\n");

  // 将时间戳强制设为已知历史值，确定性验证 updated_at 更新（避免等待）。
  check(set_problem_timestamps(env.db(), id, "2000-01-01 00:00:00",
                               "2000-01-01 00:00:00"),
        "注入历史时间戳");
  bool found = false;
  oj::ProblemRecord before = read_problem(env.db(), id, found);
  check(found && before.created_at == "2000-01-01 00:00:00",
        "读取更新前记录（历史时间戳）");

  auto res = admin_update(
      cli, admin, id,
      R"({"title":"新标题","description":"新题面","difficulty":"hard",
          "tags":["新","标签"],"time_limit_ms":1234,"memory_limit_kb":4321,
          "samples":[{"input":"9\n","output":"8\n"}]})");
  check(res && res->status == 200, "更新成功 200");

  oj::ProblemRecord after = read_problem(env.db(), id, found);
  check(found && after.title == "新标题" && after.description == "新题面" &&
            after.difficulty == "hard",
        "元数据更新与数据库一致");
  check(after.tags.size() == 2 && after.tags[0] == "新" && after.tags[1] == "标签",
        "标签更新与数据库一致");
  check(after.time_limit_ms == 1234 && after.memory_limit_kb == 4321,
        "限制更新与数据库一致");
  check(after.created_at == before.created_at, "created_at 保留");
  check(after.updated_at > before.updated_at, "updated_at 已更新");

  // 查询接口反映更新结果。
  auto detail = get_problem(cli, id);
  json d = json::parse(detail->body);
  check(d.value("title", "") == "新标题" && d.value("difficulty", "") == "hard",
        "详情反映修改结果");
  check(d["samples"].size() == 1 &&
            d["samples"][0].value("input", "") == "9\n",
        "公开样例已替换");

  // 隐藏用例不受样例替换影响。
  oj::ProblemStore store(env.db());
  std::vector<oj::TestcaseRecord> cases;
  std::string err;
  check(store.list_testcases(id, cases, err), "读取全部用例");
  bool hidden_kept = false;
  int sample_count = 0;
  for (const oj::TestcaseRecord &tc : cases) {
    if (!tc.is_sample && tc.input == "KEEP-HIDDEN-IN\n") {
      hidden_kept = true;
    }
    if (tc.is_sample) {
      ++sample_count;
    }
  }
  check(hidden_kept, "更新样例不影响隐藏用例");
  check(sample_count == 1, "公开样例数量与提交一致");

  // 部分更新：只改 visible，其余字段保持不变。
  check(admin_update(cli, admin, id, R"({"visible":false})")->status == 200,
        "仅更新 visible");
  oj::ProblemRecord partial = read_problem(env.db(), id, found);
  check(found && partial.title == "新标题" && !partial.visible &&
            partial.time_limit_ms == 1234 && !partial.tags.empty(),
        "未提供的字段未被清空");
}

void test_permissions() {
  std::cout << "游客/普通用户/未改密管理员均不能调用管理接口\n";
  Env env("adm_perm");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  const std::int64_t before = count_rows(env.db(), "problems");
  // 游客
  check(admin_create(cli, "", R"({"title":"x","difficulty":"easy"})")->status ==
            401,
        "游客创建 401");
  check(admin_update(cli, "", 1, R"({"title":"x"})")->status == 401,
        "游客修改 401");
  check(admin_delete(cli, "", 1)->status == 401, "游客删除 401");

  // 普通用户
  std::string account = register_user(cli, "normal", "NormalPw1");
  int status = 0;
  std::string user_token = login(cli, account, "NormalPw1", status);
  check(status == 200 && !user_token.empty(), "普通用户登录成功");
  check(admin_create(cli, user_token,
                     R"({"title":"x","difficulty":"easy"})")
                ->status == 403,
        "普通用户创建 403");
  check(admin_update(cli, user_token, 1, R"({"title":"x"})")->status == 403,
        "普通用户修改 403");
  check(admin_delete(cli, user_token, 1)->status == 403, "普通用户删除 403");

  // 未完成首次改密的管理员
  std::string fresh_admin = login(cli, "admin", kAdminPassword, status);
  check(status == 200 && !fresh_admin.empty(), "未改密管理员登录成功");
  auto res = admin_create(cli, fresh_admin,
                          R"({"title":"x","difficulty":"easy"})");
  check(res && res->status == 403, "未改密管理员创建 403");
  if (res) {
    json body = json::parse(res->body);
    check(body.value("code", "") == "PASSWORD_CHANGE_REQUIRED",
          "返回 PASSWORD_CHANGE_REQUIRED 标识");
  }
  check(admin_delete(cli, fresh_admin, 1)->status == 403,
        "未改密管理员删除 403");

  check(count_rows(env.db(), "problems") == before,
        "被拒的管理请求均未改变数据库");
}

void test_role_revoked_old_token() {
  std::cout << "管理员角色被撤销后，原 token 不能继续执行管理操作\n";
  Env env("adm_revoke");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  std::int64_t id = create_problem_id(
      cli, admin, R"({"title":"撤销前","difficulty":"easy"})");
  check(id > 0, "撤销前可创建题目");

  check(set_admin_role(env.db(), "user"), "撤销 admin 角色");
  const std::int64_t before = count_rows(env.db(), "problems");
  check(admin_create(cli, admin,
                     R"({"title":"撤销后","difficulty":"easy"})")
                ->status == 403,
        "旧 token 创建被拒 403");
  check(admin_delete(cli, admin, id)->status == 403, "旧 token 删除被拒 403");
  check(count_rows(env.db(), "problems") == before, "数据库保持不变");
}

void test_visibility_and_history_preserved() {
  std::cout << "隐藏题可见性控制，且可见性变化不清除提交与做题状态\n";
  Env env("adm_visible", "", new FakeExecutor());
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  std::int64_t id = create_problem_id(
      cli, admin,
      R"({"title":"可见性题","difficulty":"easy","samples":[{"input":"1\n","output":"1\n"}]})");
  check(id > 0, "创建成功");

  std::string account = register_user(cli, "stu", "StudentPw1");
  int status = 0;
  std::string user_token = login(cli, account, "StudentPw1", status);
  check(status == 200 && !user_token.empty(), "普通用户登录成功");

  // 先产生一条提交与做题状态。
  auto sub = submit(cli, user_token, id, "int main(){}");
  check(sub && sub->status == 200, "普通用户提交成功（产生提交记录）");
  const std::int64_t submissions_before = count_for(env.db(), "submissions", id);
  const std::int64_t status_before =
      count_for(env.db(), "user_problem_status", id);
  check(submissions_before == 1 && status_before == 1,
        "提交记录与做题状态均存在");

  // 隐藏题目。
  check(admin_update(cli, admin, id, R"({"visible":false})")->status == 200,
        "管理员设为隐藏");

  auto guest_list = get_problems(cli);
  check(guest_list && guest_list->body.find("\"title\":\"可见性题\"") ==
                          std::string::npos,
        "游客列表不含隐藏题");
  auto user_list = get_problems(cli, user_token);
  check(user_list && user_list->body.find("\"title\":\"可见性题\"") ==
                         std::string::npos,
        "普通用户列表不含隐藏题");
  check(get_problem(cli, id)->status == 404, "游客详情 404");
  check(get_problem(cli, id, user_token)->status == 404, "普通用户详情 404");
  check(submit(cli, user_token, id, "int main(){}")->status == 404,
        "普通用户向隐藏题提交被拒 404");
  check(count_for(env.db(), "submissions", id) == submissions_before,
        "被拒提交未新增记录");

  auto admin_list = get_problems(cli, admin);
  check(admin_list && admin_list->body.find("\"title\":\"可见性题\"") !=
                          std::string::npos,
        "管理员列表仍可见隐藏题");
  check(get_problem(cli, id, admin)->status == 200, "管理员可查看隐藏题详情");

  // 恢复可见。
  check(admin_update(cli, admin, id, R"({"visible":true})")->status == 200,
        "管理员重新公开");
  check(get_problem(cli, id)->status == 200, "游客恢复可见");
  auto restored = get_problems(cli);
  check(restored && restored->body.find("\"title\":\"可见性题\"") !=
                        std::string::npos,
        "游客列表恢复显示");

  check(count_for(env.db(), "submissions", id) == submissions_before &&
            count_for(env.db(), "user_problem_status", id) == status_before,
        "可见性变化不清除提交与做题状态");
}

void test_delete_rules() {
  std::cout << "删除规则：无提交可删且不留孤立记录，有提交则拒绝\n";
  Env env("adm_delete", "", new FakeExecutor());
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  // 无关联数据：可删除，且用例/状态一并清除。
  std::int64_t id = create_problem_id(
      cli, admin,
      R"({"title":"待删除","difficulty":"easy","samples":[{"input":"1\n","output":"1\n"}]})");
  insert_hidden_testcase(env.db(), id, 5, "HIDDEN\n", "HIDDEN\n");
  check(count_for(env.db(), "testcases", id) == 2, "删除前有 2 条用例");
  auto del = admin_delete(cli, admin, id);
  check(del && del->status == 200, "无提交时删除成功 200");
  check(!problem_exists(env.db(), id), "题目已从数据库删除");
  check(count_for(env.db(), "testcases", id) == 0, "测试用例已删除");
  check(count_for(env.db(), "user_problem_status", id) == 0,
        "做题状态已删除");
  check(get_problem(cli, id)->status == 404, "删除后详情 404");

  // 有提交：拒绝删除，且不发生部分删除。
  std::int64_t id2 = create_problem_id(
      cli, admin,
      R"({"title":"有提交","difficulty":"easy","samples":[{"input":"1\n","output":"1\n"}]})");
  std::string account = register_user(cli, "dstudent", "StudentPw1");
  int status = 0;
  std::string user_token = login(cli, account, "StudentPw1", status);
  check(status == 200, "普通用户登录成功");
  check(submit(cli, user_token, id2, "int main(){}")->status == 200,
        "产生一条提交");
  const std::int64_t problems_before = count_rows(env.db(), "problems");
  const std::int64_t testcases_before = count_for(env.db(), "testcases", id2);
  const std::int64_t submissions_before =
      count_for(env.db(), "submissions", id2);

  auto del2 = admin_delete(cli, admin, id2);
  check(del2 && del2->status == 409, "有提交时删除返回 409");
  check(problem_exists(env.db(), id2), "被拒删除后题目仍存在");
  check(count_for(env.db(), "testcases", id2) == testcases_before,
        "被拒删除后测试用例未被部分删除");
  check(count_for(env.db(), "submissions", id2) == submissions_before,
        "被拒删除后提交记录完好");
  check(count_rows(env.db(), "problems") == problems_before,
        "被拒删除后题目数量不变");

  // 不存在 / 非法 ID。
  check(admin_delete(cli, admin, 999999)->status == 404,
        "删除不存在题目 404");
  auto bad = admin_delete(cli, admin, 0);
  check(bad && bad->status == 400, "删除非法 ID 400");
}

void test_server_managed_fields_ignored() {
  std::cout << "服务端管理字段无法由客户端指定\n";
  Env env("adm_serverfields");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  auto res = admin_create(
      cli, admin,
      R"({"title":"服务端字段","difficulty":"easy","id":123456,
          "seed_key":"forged","created_at":"1999-01-01 00:00:00",
          "updated_at":"1999-01-01 00:00:00"})");
  check(res && res->status == 201, "含越权字段仍按服务端规则创建");
  std::int64_t id = res ? json::parse(res->body).value("id", -1LL) : -1;
  check(id > 0 && id != 123456, "返回服务端分配的 ID，非客户端指定值");
  check(read_seed_key(env.db(), id).empty(), "seed_key 为 NULL（普通题）");
  bool found = false;
  oj::ProblemRecord created = read_problem(env.db(), id, found);
  check(found && created.created_at != "1999-01-01 00:00:00" &&
            created.updated_at != "1999-01-01 00:00:00",
        "时间戳为服务端当前时间");

  // PUT 仅含服务端字段：无可更新字段 -> 400，时间戳不变。
  check(admin_update(cli, admin, id,
                     R"({"id":999,"created_at":"1999-01-01 00:00:00",
                         "updated_at":"1999-01-01 00:00:00"})")
                ->status == 400,
        "仅越权字段的 PUT 返回 400");
  oj::ProblemRecord after = read_problem(env.db(), id, found);
  check(found && after.created_at == created.created_at &&
            after.updated_at == created.updated_at,
        "越权 PUT 未改变时间戳");

  // PUT 合法字段 + 越权 created_at：created_at 不被篡改。
  check(admin_update(cli, admin, id,
                     R"({"title":"改名","created_at":"1999-01-01 00:00:00"})")
                ->status == 200,
        "合法字段更新成功");
  oj::ProblemRecord renamed = read_problem(env.db(), id, found);
  check(found && renamed.title == "改名" &&
            renamed.created_at == created.created_at,
        "允许改动标题但 created_at 不被篡改");
  check(read_seed_key(env.db(), id).empty(), "seed_key 仍为 NULL");
}

void test_create_atomic_and_tags_roundtrip() {
  std::cout << "新建题：无隐藏用例、公开样例标记正确、标签数组往返一致\n";
  Env env("adm_create_shape");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  std::int64_t id = create_problem_id(
      cli, admin,
      R"({"title":"结构题","difficulty":"medium","tags":["数组","入门"],
          "samples":[{"input":"1\n","output":"1\n"},
                     {"input":"2\n","output":"2\n"}]})");
  check(id > 0, "创建成功");
  check(count_testcases_flag(env.db(), id, 1) == 2, "公开样例 2 条");
  check(count_testcases_flag(env.db(), id, 0) == 0, "未产生隐藏用例");

  auto list = get_problems(cli);
  check(list && list->status == 200, "列表 200");
  json list_json = json::parse(list->body);
  const json *entry = nullptr;
  for (const auto &p : list_json["problems"]) {
    if (p.value("id", -1LL) == id) {
      entry = &p;
    }
  }
  check(entry != nullptr, "新题出现在列表");
  check(entry != nullptr && (*entry)["tags"].is_array() &&
            (*entry)["tags"].size() == 2 && (*entry)["tags"][0] == "数组" &&
            (*entry)["tags"][1] == "入门",
        "列表 tags 数组与顺序正确");

  auto detail = get_problem(cli, id);
  json d = json::parse(detail->body);
  check(d["tags"].is_array() && d["tags"].size() == 2 && d["tags"][0] == "数组",
        "详情 tags 数组正确");
  check(d["samples"].is_array() && d["samples"].size() == 2,
        "详情公开样例 2 组");
}

void test_update_explicit_clear() {
  std::cout << "PUT 显式空值清空字段，未提供字段保留且不动隐藏用例\n";
  Env env("adm_clear");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  std::int64_t id = create_problem_id(
      cli, admin,
      R"({"title":"清空题","description":"有题面","difficulty":"easy",
          "tags":["a","b"],"samples":[{"input":"1\n","output":"1\n"}]})");
  check(id > 0, "创建成功");
  insert_hidden_testcase(env.db(), id, 3, "HIDDEN-CLEAR-IN\n",
                         "HIDDEN-CLEAR-OUT\n");

  check(admin_update(cli, admin, id,
                     R"({"description":"","tags":[],"samples":[]})")
                ->status == 200,
        "显式清空 description/tags/samples");
  bool found = false;
  oj::ProblemRecord record = read_problem(env.db(), id, found);
  check(found && record.description.empty() && record.tags.empty(),
        "description 与 tags 已清空");
  check(found && record.title == "清空题" && record.difficulty == "easy",
        "未提供字段保留");
  check(count_testcases_flag(env.db(), id, 1) == 0, "公开样例已清空");
  check(count_testcases_flag(env.db(), id, 0) == 1, "隐藏用例未被清空");

  auto detail = get_problem(cli, id);
  json d = json::parse(detail->body);
  check(d["samples"].is_array() && d["samples"].empty(),
        "详情公开样例为空数组");
}

// 门控执行器：确定性制造「提交正在判题时尝试删除题目」的交错。
// M3.7 起，题目存在未结算在途任务（判题进行中）时删除被拒（409），已接收任务正常
// 完成并保存终态（避免因删题丢失已接收任务）。
void test_delete_during_judge_gated() {
  std::cout << "判题期间删除题目：在途任务存在时删除被拒（409），提交正常完成\n";
  GatedExecutor executor;
  Env env("adm_gated", "", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  std::string account = register_user(cli, "gated", "GatedPw1");
  int status = 0;
  std::string user_token = login(cli, account, "GatedPw1", status);
  check(status == 200 && !user_token.empty(), "普通用户登录成功");

  std::int64_t id = create_problem_id(
      cli, admin,
      R"({"title":"门控题","difficulty":"easy","samples":[{"input":"1\n","output":"1\n"}]})");
  check(id > 0, "创建成功");

  int submit_status = 0;
  std::thread submitter([&]() {
    httplib::Client c = make_client(env.port());
    auto res = submit(c, user_token, id, "int main(){}");
    submit_status = res ? res->status : -1;
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

  auto del = admin_delete(cli, admin, id);
  check(del && del->status == 409, "判题期间删除题目被拒（存在未结算在途任务）");
  check(problem_exists(env.db(), id), "题目未被删除");

  executor.release();
  submitter.join();

  check(submit_status == 200, "题目仍在，提交正常完成并返回 200");
  check(count_for(env.db(), "submissions", id) == 1, "提交持久化 1 条");
  check(count_for(env.db(), "testcases", id) >= 1, "用例保留");
}

// 并发删除与提交：最终状态必须自洽——题目存在则有提交且删除被拒，题目删除则提交
// 未成功且无孤立记录。
void test_concurrent_delete_and_submit() {
  std::cout << "并发删除与提交：数据一致，无假成功/未处理异常\n";
  Env env("adm_race", "", new FakeExecutor());
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  std::string account = register_user(cli, "racer", "RacerPw1");
  int status = 0;
  std::string user_token = login(cli, account, "RacerPw1", status);
  check(status == 200 && !user_token.empty(), "普通用户登录成功");

  bool all_consistent = true;
  for (int i = 0; i < 5; ++i) {
    std::int64_t id = create_problem_id(
        cli, admin,
        R"({"title":"竞态题","difficulty":"easy","samples":[{"input":"1\n","output":"1\n"}]})");
    if (id <= 0) {
      all_consistent = false;
      break;
    }

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    int submit_status = 0;
    int delete_status = 0;

    std::thread submitter([&]() {
      httplib::Client c = make_client(env.port());
      ready.fetch_add(1);
      while (!go.load()) {
        std::this_thread::yield();
      }
      auto res = submit(c, user_token, id, "int main(){}");
      submit_status = res ? res->status : -1;
    });
    std::thread deleter([&]() {
      httplib::Client c = make_client(env.port());
      ready.fetch_add(1);
      while (!go.load()) {
        std::this_thread::yield();
      }
      auto res = admin_delete(c, admin, id);
      delete_status = res ? res->status : -1;
    });
    while (ready.load() < 2) {
      std::this_thread::yield();
    }
    go = true;
    submitter.join();
    deleter.join();

    const bool problem_gone = !problem_exists(env.db(), id);
    const std::int64_t submissions = count_for(env.db(), "submissions", id);
    const std::int64_t testcases = count_for(env.db(), "testcases", id);
    const bool submit_ok = submit_status == 200;
    const bool delete_removed = delete_status == 200;
    const bool delete_rejected = delete_status == 409;

    if (problem_gone) {
      // 删除成功：提交不得成功，且不得留下孤立提交/用例。
      if (!delete_removed || submit_ok || submissions != 0 || testcases != 0) {
        all_consistent = false;
        std::cout << "    迭代 " << i << " 不一致：题目已删但 delete="
                  << delete_status << " submit=" << submit_status
                  << " submissions=" << submissions
                  << " testcases=" << testcases << "\n";
      }
    } else {
      // 题目仍在：删除必须因已有提交被拒（409），提交成功并留下 1 条记录与用例。
      if (!delete_rejected || !submit_ok || submissions != 1 || testcases < 1) {
        all_consistent = false;
        std::cout << "    迭代 " << i << " 不一致：题目仍在 submit="
                  << submit_status << " delete=" << delete_status
                  << " submissions=" << submissions
                  << " testcases=" << testcases << "\n";
      }
    }
  }
  check(all_consistent, "并发删题与提交的所有迭代均保持一致");
}

void test_persistence_restart() {
  std::cout << "重启后新增/修改/可见性设置仍保留\n";
  TempDir dir("adm_persist");
  const std::string dbpath = dir.db_path();
  std::int64_t id = -1;

  {
    Env env("adm_persist_1", dbpath);
    check(env.ok(), "首次启动成功");
    httplib::Client cli = make_client(env.port());
    std::string admin = admin_login(cli);
    id = create_problem_id(
        cli, admin,
        R"({"title":"持久化题","difficulty":"medium","tags":["持久"],
            "samples":[{"input":"1\n","output":"1\n"}]})");
    check(id > 0, "创建成功");
    check(admin_update(cli, admin, id,
                       R"({"description":"更新后的题面","visible":false})")
                  ->status == 200,
          "修改并设为隐藏");
    env.stop();
    env.close_db();
  }

  {
    Env env("adm_persist_2", dbpath);
    check(env.ok(), "重启成功");
    httplib::Client cli = make_client(env.port());
    // 首次启动已改密，重启后用新密码直接登录。
    int status = 0;
    std::string admin = login(cli, "admin", kAdminNewPassword, status);
    check(status == 200 && !admin.empty(), "重启后管理员登录成功");

    bool found = false;
    oj::ProblemRecord record = read_problem(env.db(), id, found);
    check(found && record.description == "更新后的题面" && !record.visible,
          "重启后修改与可见性设置保留");
    check(record.tags.size() == 1 && record.tags[0] == "持久",
          "重启后标签保留");
    check(get_problem(cli, id, admin)->status == 200,
          "管理员重启后仍可查看隐藏题");
    check(get_problem(cli, id)->status == 404, "游客重启后仍看不到隐藏题");
    env.stop();
    env.close_db();
  }
}

void test_internal_error_no_leak() {
  std::cout << "数据库故障时管理接口返回 500 且不泄露内部细节\n";
  Env env("adm_internal");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员就绪");

  env.close_db();
  auto res = admin_create(cli, admin,
                          R"({"title":"x","difficulty":"easy"})");
  check(res && res->status == 500, "数据库故障创建返回 500");
  if (res) {
    json body = json::parse(res->body);
    check(body.value("error", "") == "内部错误", "错误文案通用");
    check(res->body.find("sqlite") == std::string::npos &&
              res->body.find("SELECT") == std::string::npos &&
              res->body.find("problems") == std::string::npos,
          "不泄露 SQL/表名/路径");
  }
}

} // namespace

int main() {
  test_create_defaults_and_read();
  test_create_with_samples_and_no_hidden_leak();
  test_server_managed_fields_ignored();
  test_create_atomic_and_tags_roundtrip();
  test_validation_rejected_without_side_effects();
  test_update_partial_and_timestamps();
  test_update_explicit_clear();
  test_permissions();
  test_role_revoked_old_token();
  test_visibility_and_history_preserved();
  test_delete_rules();
  test_delete_during_judge_gated();
  test_concurrent_delete_and_submit();
  test_persistence_restart();
  test_internal_error_no_leak();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部管理员题目接口集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
