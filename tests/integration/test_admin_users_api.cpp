// 管理员用户接口集成测试（M2.4）。
//
// 使用 /tmp 下的隔离临时库与随机端口上的真实 HTTP 服务，不触碰正式数据库。
// 判题执行器可注入 EchoExecutor（标准输出 = 标准输入），用于验证重置密码后的
// 首次改密标记确实会阻止普通用户提交，以及改密后恢复提交能力。
//
// 覆盖：
//   - 管理员用户列表：字段完整、稳定排序、分页与非法分页参数、无敏感字段泄露
//   - 未登录 / 普通用户 / 未完成首改的管理员不能查询或修改，数据库不变
//   - 重置密码：旧密码失效、新密码可登录、数据库保存有效哈希、reset_pwd_flag=1
//   - 重置后首改标记对普通用户生效（提交被 403 拦截，改密后恢复）
//   - 旧 token 在重置后仍可能有效至过期，且权限/标记随数据库最新值生效
//   - 普通用户提升为管理员后按新角色与首改规则处理
//   - 管理员被改为普通用户后原 token 不能继续执行管理员操作
//   - 非法新密码/缺失字段/错误类型/未知操作/不存在用户被正确处理且原数据不变
//   - 请求体夹带 account/nickname/role/reset_pwd_flag 等字段不能越权修改
//   - 自我降级、最后管理员保护与并发角色修改符合既定规则
//   - 数据库故障返回 500 且不泄露内部细节
//   - 重启后密码与角色修改仍然保留
//
// 运行方式：ctest --test-dir build -R admin_users_api --output-on-failure
// 或直接执行 build/oj_admin_users_api_test。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "auth/jwt.h"
#include "auth/password.h"
#include "db/database.h"
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

const std::string kTestSecret = "it-admin-users-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";
const std::string kAdminNewPassword = "AdminNewPass1";

oj::auth::JwtConfig make_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = kTestSecret;
  cfg.expires_seconds = 3600;
  return cfg;
}

// 回显执行器：把标准输入原样作为标准输出返回（编译恒成功）。
class EchoExecutor : public oj::judge::IExecutor {
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
                               const std::string &input) override {
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    result.stdout_data = input;
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

// 用预置管理员初始密码登录并完成首次改密，返回改密前的登录 token
// （改密不撤销 token，该 token 仍有效且角色为 admin、reset_pwd_flag=0）。
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

httplib::Result admin_get_users(httplib::Client &cli, const std::string &token,
                                const std::string &query = "") {
  std::string path = "/api/admin/users";
  if (!query.empty()) {
    path += "?" + query;
  }
  httplib::Headers h;
  if (!token.empty()) {
    h.emplace("Authorization", "Bearer " + token);
  }
  return cli.Get(path.c_str(), h);
}

httplib::Result admin_put_user(httplib::Client &cli, const std::string &token,
                               const std::string &body) {
  httplib::Headers h;
  if (!token.empty()) {
    h.emplace("Authorization", "Bearer " + token);
  }
  return cli.Put("/api/admin/users", h, body, "application/json");
}

httplib::Result admin_create_problem(httplib::Client &cli,
                                     const std::string &token,
                                     const std::string &body) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Post("/api/admin/problems", h, body, "application/json");
}

httplib::Result submit(httplib::Client &cli, const std::string &token,
                       std::int64_t id) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Post(
      ("/api/problems/" + std::to_string(id) + "/submit").c_str(), h,
      R"({"language":"cpp17","code":"int main(){}"})", "application/json");
}

// ---------------------------------------------------------------------------
// 数据库辅助
// ---------------------------------------------------------------------------

struct UserRow {
  std::int64_t id = 0;
  std::string account;
  std::string nickname;
  std::string password_hash;
  std::string role;
  int reset_pwd_flag = 0;
  std::string created_at;
  bool found = false;
};

UserRow read_user(oj::Database &db, const std::string &account) {
  UserRow row;
  oj::UserStore store(db);
  oj::UserRecord rec;
  std::string err;
  store.find_by_account(account, row.found, rec, err);
  if (row.found) {
    row.id = rec.id;
    row.account = rec.account;
    row.nickname = rec.nickname;
    row.password_hash = rec.password_hash;
    row.role = rec.role;
    row.reset_pwd_flag = rec.reset_pwd_flag;
    row.created_at = rec.created_at;
  }
  return row;
}

std::int64_t count_admins(oj::Database &db) {
  oj::Statement stmt;
  std::string err;
  if (!db.prepare("SELECT COUNT(*) FROM users WHERE role = 'admin'", stmt,
                  err)) {
    return -1;
  }
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t count_users(oj::Database &db) {
  oj::Statement stmt;
  std::string err;
  if (!db.prepare("SELECT COUNT(*) FROM users", stmt, err)) {
    return -1;
  }
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

// 直接插入一个占位用户（密码哈希为已哈希的合法字符串），仅用于构造分页边界
// 数据量，避免为纯列表分页测试付出大量 argon2 计算。不参与登录。
bool insert_dummy_user(oj::Database &db, const std::string &account,
                       const std::string &nickname) {
  oj::Statement stmt;
  std::string err;
  if (!db.prepare("INSERT INTO users (account, nickname, password_hash, role, "
                  "reset_pwd_flag) VALUES (?, ?, 'placeholder-hash', 'user', 0)",
                  stmt, err)) {
    return false;
  }
  if (!stmt.bind(1, account) || !stmt.bind(2, nickname)) {
    return false;
  }
  return stmt.step() == SQLITE_DONE;
}

std::int64_t count_submissions(oj::Database &db, std::int64_t user_id) {
  oj::Statement stmt;
  std::string err;
  if (!db.prepare("SELECT COUNT(*) FROM submissions WHERE user_id = ?", stmt,
                  err)) {
    return -1;
  }
  stmt.bind(1, static_cast<sqlite3_int64>(user_id));
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

struct StatusRow {
  bool found = false;
  std::string status;
  std::string first_ac_at;
  long long submit_count = 0;
};

StatusRow read_status(oj::Database &db, std::int64_t user_id,
                      std::int64_t problem_id) {
  StatusRow row;
  oj::Statement stmt;
  std::string err;
  if (!db.prepare("SELECT status, first_ac_at, submit_count FROM "
                  "user_problem_status WHERE user_id = ? AND problem_id = ?",
                  stmt, err)) {
    return row;
  }
  stmt.bind(1, static_cast<sqlite3_int64>(user_id));
  stmt.bind(2, static_cast<sqlite3_int64>(problem_id));
  if (stmt.step() == SQLITE_ROW) {
    row.found = true;
    row.status = stmt.column_text(0);
    row.first_ac_at = stmt.column_is_null(1) ? "" : stmt.column_text(1);
    row.submit_count = stmt.column_int64(2);
  }
  return row;
}

// ---------------------------------------------------------------------------
// 1. 用户列表字段、排序与不泄露敏感信息
// ---------------------------------------------------------------------------

void test_list_fields_order_and_no_leak() {
  std::cout << "用户列表字段、稳定排序且不泄露密码哈希\n";
  Env env("au_list");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首改");

  std::string alice_account = register_user(cli, "alice", "AlicePw123");
  std::string bob_account = register_user(cli, "bob", "BobPw12345");
  check(!alice_account.empty() && !bob_account.empty(), "注册两个普通用户");

  UserRow admin_row = read_user(env.db(), "admin");
  auto res = admin_get_users(cli, admin);
  check(res && res->status == 200, "管理员可获取用户列表");
  if (!res) {
    return;
  }
  json body = json::parse(res->body);
  check(body.contains("users") && body["users"].is_array(), "响应含 users 数组");
  check(body.value("page", 0) == 1 && body.value("page_size", 0) == 20,
        "默认 page=1、page_size=20");
  check(body.value("total", 0) == 3 && body.value("total_pages", 0) == 1,
        "total=3、total_pages=1");

  const json &users = body["users"];
  std::vector<std::int64_t> ids;
  bool fields_ok = true;
  bool sensitive = false;
  for (const auto &u : users) {
    if (!u.contains("id") || !u.contains("account") || !u.contains("nickname") ||
        !u.contains("role") || !u.contains("reset_pwd_flag") ||
        !u.contains("created_at") || u.size() != 6) {
      fields_ok = false;
    }
    ids.push_back(u.value("id", 0LL));
    if (u.contains("password") || u.contains("password_hash") ||
        u.contains("token")) {
      sensitive = true;
    }
  }
  check(fields_ok, "每条仅含 id/account/nickname/role/reset_pwd_flag/created_at");
  check(!sensitive, "列表条目不包含 password/token 字段");
  check(res->body.find("password") == std::string::npos &&
            res->body.find(admin_row.password_hash) == std::string::npos,
        "响应正文不含 \"password\" 且不含真实密码哈希");
  check(ids.size() == 3 && ids[0] == 1 && ids[1] == 2 && ids[2] == 3,
        "按 id 升序稳定排序（admin 在前）");

  bool alice_ok = false;
  for (const auto &u : users) {
    if (u.value("account", "") == alice_account) {
      alice_ok = u.value("nickname", "") == "alice" &&
                 u.value("role", "") == "user" &&
                 u.value("reset_pwd_flag", -1) == 0 &&
                 !u.value("created_at", "").empty();
    }
  }
  check(alice_ok, "普通用户字段值正确（role=user、reset_pwd_flag=0）");
}

// ---------------------------------------------------------------------------
// 2. 分页与非法分页参数
// ---------------------------------------------------------------------------

void test_pagination() {
  std::cout << "用户列表分页稳定、非法分页参数按约定处理\n";
  Env env("au_page");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首改");

  const int kTotal = 22; // admin + 21 注册用户
  for (int i = 1; i <= kTotal - 1; ++i) {
    if (register_user(cli, "pg_u" + std::to_string(i), "PgPass123").empty()) {
      check(false, "注册分页测试用户失败");
      return;
    }
  }

  auto p1 = admin_get_users(cli, admin);
  json j1 = json::parse(p1->body);
  check(p1->status == 200 && j1.value("total", 0) == kTotal &&
            j1.value("total_pages", 0) == 2 && j1["users"].size() == 20,
        "第 1 页 20 条、total=22、total_pages=2");

  auto p2 = admin_get_users(cli, admin, "page=2");
  json j2 = json::parse(p2->body);
  check(p2->status == 200 && j2["users"].size() == 2 &&
            j2.value("page", 0) == 2,
        "第 2 页 2 条");

  std::vector<std::int64_t> ids;
  for (const auto &u : j1["users"]) {
    ids.push_back(u.value("id", 0LL));
  }
  for (const auto &u : j2["users"]) {
    ids.push_back(u.value("id", 0LL));
  }
  bool sorted_unique = ids.size() == static_cast<std::size_t>(kTotal);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] != static_cast<std::int64_t>(i + 1)) {
      sorted_unique = false;
    }
  }
  check(sorted_unique, "两页合并为 id 1..22 连续、不重复、不遗漏");

  auto p3 = admin_get_users(cli, admin, "page=3");
  json j3 = json::parse(p3->body);
  check(p3->status == 200 && j3["users"].empty() &&
            j3.value("total", 0) == kTotal,
        "超出末页返回 200 空列表且 total 不变");

  auto pmax = admin_get_users(cli, admin, "page=1000000");
  check(pmax && pmax->status == 200 && json::parse(pmax->body)["users"].empty(),
        "最大页 1000000 返回空列表");

  const char *bad_pages[] = {"0", "-1", "abc", "1.5", "+1", "1000001",
                             "9999999"};
  bool all_400 = true;
  for (const char *p : bad_pages) {
    auto r = admin_get_users(cli, admin, std::string("page=") + p);
    if (!r || r->status != 400) {
      all_400 = false;
      std::cout << "    page=" << p << " -> "
                << (r ? r->status : -1) << "\n";
    }
  }
  check(all_400, "page=0/-1/abc/1.5/+1/1000001/9999999 均返回 400");
}

// ---------------------------------------------------------------------------
// 3. 权限：未登录 / 普通用户 / 未完成首改管理员
// ---------------------------------------------------------------------------

void test_permissions() {
  std::cout << "未登录/普通用户/未完成首改管理员不能查询或修改用户\n";
  Env env("au_perm");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());

  std::string account = register_user(cli, "normal", "NormalPw1");
  int status = 0;
  std::string user_token = login(cli, account, "NormalPw1", status);
  check(status == 200 && !user_token.empty(), "普通用户登录成功");

  const std::int64_t users_before = count_users(env.db());
  UserRow normal_before = read_user(env.db(), account);

  auto anon = admin_get_users(cli, "");
  check(anon && anon->status == 401, "未登录查询用户列表返回 401");

  auto user_get = admin_get_users(cli, user_token);
  check(user_get && user_get->status == 403, "普通用户查询用户列表返回 403");

  auto user_put = admin_put_user(
      cli, user_token,
      R"({"action":"change_role","user_id":1,"role":"user"})");
  check(user_put && user_put->status == 403, "普通用户修改角色返回 403");

  // 未完成首改的管理员：初始 admin 登录后未改密，reset_pwd_flag=1。
  int admin_status = 0;
  std::string raw_admin = login(cli, "admin", kAdminPassword, admin_status);
  check(admin_status == 200 && !raw_admin.empty(), "初始管理员可登录");
  auto pending_get = admin_get_users(cli, raw_admin);
  check(pending_get && pending_get->status == 403 &&
            json::parse(pending_get->body).value("code", "") ==
                "PASSWORD_CHANGE_REQUIRED",
        "未完成首改的管理员查询返回 403 PASSWORD_CHANGE_REQUIRED");
  auto pending_put = admin_put_user(
      cli, raw_admin,
      R"({"action":"reset_password","user_id":2,"new_password":"Hacked123"})");
  check(pending_put && pending_put->status == 403, "未完成首改的管理员修改被拒");

  UserRow normal_after = read_user(env.db(), account);
  check(count_users(env.db()) == users_before &&
            normal_after.password_hash == normal_before.password_hash &&
            normal_after.role == normal_before.role,
        "被拒请求不改变数据库");
  (void)normal_before;
}

// ---------------------------------------------------------------------------
// 4. 重置密码生命周期：旧/新登录、哈希、首改标记、提交限制、旧 token
// ---------------------------------------------------------------------------

void test_reset_password_lifecycle() {
  std::cout << "管理员重置密码：旧密码失效、新密码可登录、标记与旧 token 行为\n";
  EchoExecutor executor;
  Env env("au_reset", "", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首改");

  std::string account = register_user(cli, "carol", "CarolPw1");
  int status = 0;
  std::string old_token = login(cli, account, "CarolPw1", status);
  check(status == 200 && !old_token.empty(), "carol 以旧密码登录成功");
  UserRow before = read_user(env.db(), account);
  const std::string old_hash = before.password_hash;
  check(before.reset_pwd_flag == 0, "重置前 reset_pwd_flag=0");

  auto put = admin_put_user(cli, admin,
                            "{\"action\":\"reset_password\",\"user_id\":" +
                                std::to_string(before.id) +
                                ",\"new_password\":\"CarolNew1\"}");
  check(put && put->status == 200 &&
            json::parse(put->body).value("user_id", 0LL) == before.id,
        "重置密码返回 200 + user_id");
  check(put && put->body.find("CarolNew1") == std::string::npos,
        "成功响应不回显新密码");

  UserRow after = read_user(env.db(), account);
  std::string verify_err;
  check(after.password_hash != old_hash &&
            oj::auth::verify_password(after.password_hash, "CarolNew1",
                                      verify_err),
        "数据库保存的是新密码的有效 argon2id 哈希");
  check(after.reset_pwd_flag == 1, "重置后 reset_pwd_flag=1");

  int s_old = 0;
  login(cli, account, "CarolPw1", s_old);
  check(s_old == 401, "旧密码不能登录");
  int s_new = 0;
  std::string new_token = login(cli, account, "CarolNew1", s_new);
  check(s_new == 200 && !new_token.empty(), "新密码可以登录");

  // 旧 token 未被撤销（沿用现有会话策略），但权限/标记按数据库最新值生效。
  httplib::Headers old_h{{"Authorization", "Bearer " + old_token}};
  auto me = cli.Get("/api/me", old_h);
  check(me && me->status == 200 &&
            json::parse(me->body).value("reset_pwd_flag", -1) == 1,
        "重置后旧 token 仍可用且看到数据库最新标记（未声称失效）");

  // 首改标记对普通用户生效：提交被 403 拦截。
  auto pid_res = admin_create_problem(
      cli, admin,
      R"({"title":"标记题","difficulty":"easy","samples":[{"input":"x","output":"x"}]})");
  check(pid_res && pid_res->status == 201, "管理员创建题目");
  std::int64_t pid = json::parse(pid_res->body).value("id", -1LL);

  auto blocked = submit(cli, new_token, pid);
  check(blocked && blocked->status == 403 &&
            json::parse(blocked->body).value("code", "") ==
                "PASSWORD_CHANGE_REQUIRED",
        "重置后普通用户提交被 403 PASSWORD_CHANGE_REQUIRED 拦截");

  httplib::Headers new_h{{"Authorization", "Bearer " + new_token}};
  auto chg = cli.Post("/api/me/password", new_h,
                      R"({"old_password":"CarolNew1","new_password":"CarolFinal1"})",
                      "application/json");
  check(chg && chg->status == 200, "carol 改密成功");
  auto ok_sub = submit(cli, new_token, pid);
  check(ok_sub && ok_sub->status == 200 &&
            json::parse(ok_sub->body).value("status", "") == "AC",
        "改密后同一 token 恢复提交能力（判题返回 AC）");
  check(read_user(env.db(), account).reset_pwd_flag == 0, "改密后标记清除");
}

// ---------------------------------------------------------------------------
// 5. 角色提升与降级
// ---------------------------------------------------------------------------

void test_role_change_promote_and_demote() {
  std::cout << "提升按新角色生效，降级后原 token 失去管理员权限\n";
  Env env("au_roles");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首改");

  std::string account = register_user(cli, "dave", "DavePass1");
  int status = 0;
  std::string dave_token = login(cli, account, "DavePass1", status);
  check(status == 200, "dave 登录成功");
  UserRow dave = read_user(env.db(), account);

  auto denied = admin_get_users(cli, dave_token);
  check(denied && denied->status == 403, "提升前 dave 无法访问管理员接口");

  auto promote = admin_put_user(
      cli, admin,
      "{\"action\":\"change_role\",\"user_id\":" + std::to_string(dave.id) +
          ",\"role\":\"admin\"}");
  check(promote && promote->status == 200 &&
            json::parse(promote->body).value("role", "") == "admin",
        "提升为管理员返回 200");
  check(read_user(env.db(), account).role == "admin", "数据库角色已更新");

  auto allowed = admin_get_users(cli, dave_token);
  check(allowed && allowed->status == 200,
        "提升后 dave 原 token 立即可访问管理员接口（角色按数据库）");

  // 已改密标记为 1 的用户即使被提升为管理员也须先改密。
  auto reset = admin_put_user(
      cli, admin,
      "{\"action\":\"reset_password\",\"user_id\":" + std::to_string(dave.id) +
          ",\"new_password\":\"DaveTemp1\"}");
  check(reset && reset->status == 200, "重置 dave 密码（标记=1）");
  auto flagged = admin_get_users(cli, dave_token);
  check(flagged && flagged->status == 403 &&
            json::parse(flagged->body).value("code", "") ==
                "PASSWORD_CHANGE_REQUIRED",
        "管理员且标记=1 时按首改规则返回 PASSWORD_CHANGE_REQUIRED");

  // 用新密码登录并改密清除标记，然后被降级。
  int s = 0;
  std::string dave2 = login(cli, account, "DaveTemp1", s);
  check(s == 200 && !dave2.empty(), "dave 用重置后新密码登录");
  httplib::Headers h2{{"Authorization", "Bearer " + dave2}};
  check(cli.Post("/api/me/password", h2,
                 R"({"old_password":"DaveTemp1","new_password":"DaveFinal1"})",
                 "application/json")
            ->status == 200,
        "dave 改密清除标记");
  check(admin_get_users(cli, dave2)->status == 200, "改密后 dave 可访问管理员接口");

  auto demote = admin_put_user(
      cli, admin,
      "{\"action\":\"change_role\",\"user_id\":" + std::to_string(dave.id) +
          ",\"role\":\"user\"}");
  check(demote && demote->status == 200, "降级为普通用户返回 200");
  auto after_demote = admin_get_users(cli, dave2);
  check(after_demote && after_demote->status == 403,
        "降级后原 token 不能继续执行管理员操作");
}

// ---------------------------------------------------------------------------
// 6. 非法请求与原数据不变
// ---------------------------------------------------------------------------

void test_invalid_requests_no_change() {
  std::cout << "非法请求/未知操作/不存在用户被拒且原数据不变\n";
  Env env("au_invalid");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首改");

  std::string account = register_user(cli, "erin", "ErinPass1");
  UserRow erin = read_user(env.db(), account);
  const std::string hash_before = erin.password_hash;

  struct Case {
    std::string body;
    int status;
    std::string label;
  };
  const std::vector<Case> cases = {
      {R"({"action":"reset_password","user_id":)" + std::to_string(erin.id) +
           R"(,"new_password":""})",
       400, "空新密码 400"},
      {R"({"action":"reset_password","user_id":)" + std::to_string(erin.id) +
           R"(})",
       400, "缺 new_password 400"},
      {R"({"action":"reset_password","user_id":)" + std::to_string(erin.id) +
           R"(,"new_password":123})",
       400, "new_password 类型错误 400"},
      {"{\"action\":\"reset_password\",\"user_id\":" +
           std::to_string(erin.id) + ",\"new_password\":\"" +
           std::string(200, 'a') + "\"}",
       400, "新密码过长 400"},
      {R"({"action":"change_role","user_id":)" + std::to_string(erin.id) +
           R"(,"role":"superadmin"})",
       400, "非法角色 400"},
      {R"({"action":"change_role","user_id":)" + std::to_string(erin.id) +
           R"(})",
       400, "缺 role 400"},
      {R"({"action":"unknown","user_id":1})", 400, "未知操作 400"},
      {R"({"action":"change_role","user_id":0,"role":"admin"})", 400,
       "user_id=0 400"},
      {"{\"action\":\"reset_password\",\"user_id\":999999,\"new_password\":\"Valid123\"}",
       404, "不存在用户重置 404"},
      {"{\"action\":\"change_role\",\"user_id\":999999,\"role\":\"admin\"}",
       404, "不存在用户改角色 404"},
      {"not-json", 400, "非法 JSON 400"},
  };

  bool all_ok = true;
  for (const Case &c : cases) {
    auto r = admin_put_user(cli, admin, c.body);
    if (!r || r->status != c.status) {
      all_ok = false;
      std::cout << "    " << c.label << " -> " << (r ? r->status : -1) << "\n";
    }
  }
  check(all_ok, "全部非法请求返回约定状态码");

  UserRow erin_after = read_user(env.db(), account);
  check(erin_after.password_hash == hash_before &&
            erin_after.role == "user" && erin_after.reset_pwd_flag == 0,
        "非法请求后密码/角色/标记均未改变");
}

// ---------------------------------------------------------------------------
// 7. 越权字段被忽略
// ---------------------------------------------------------------------------

void test_extra_fields_ignored() {
  std::cout << "请求体夹带账号/昵称/角色等字段不能越权修改\n";
  Env env("au_extra");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首改");

  std::string account = register_user(cli, "frank", "FrankPass1");
  UserRow frank = read_user(env.db(), account);
  const std::string hash_before = frank.password_hash;

  // reset_password 夹带 role/account/nickname/reset_pwd_flag：只应改密码与标记。
  auto reset = admin_put_user(
      cli, admin,
      "{\"action\":\"reset_password\",\"user_id\":" + std::to_string(frank.id) +
          ",\"new_password\":\"FrankNew1\",\"role\":\"admin\","
          "\"account\":\"admin\",\"nickname\":\"hacker\","
          "\"reset_pwd_flag\":0,\"password_hash\":\"injected\"}");
  check(reset && reset->status == 200, "夹带字段的重置请求成功但只改密码");
  UserRow after_reset = read_user(env.db(), account);
  check(after_reset.role == "user" && after_reset.account == account &&
            after_reset.nickname == "frank" &&
            after_reset.reset_pwd_flag == 1 &&
            after_reset.password_hash != "injected" &&
            after_reset.password_hash != hash_before,
        "角色/账号/昵称未被夹带字段改变，仅密码与标记更新");

  // change_role 夹带 new_password：只应改角色，密码不变。
  const std::string hash_after_reset = after_reset.password_hash;
  auto role = admin_put_user(
      cli, admin,
      "{\"action\":\"change_role\",\"user_id\":" + std::to_string(frank.id) +
          ",\"role\":\"admin\",\"new_password\":\"ShouldNotApply1\","
          "\"account\":\"root\",\"reset_pwd_flag\":0}");
  check(role && role->status == 200, "夹带 new_password 的改角色请求成功");
  UserRow after_role = read_user(env.db(), account);
  check(after_role.role == "admin" &&
            after_role.password_hash == hash_after_reset &&
            after_role.account == account && after_role.nickname == "frank",
        "改角色只更新 role，密码/账号/昵称不变");
}

// ---------------------------------------------------------------------------
// 8. 密码/角色修改不影响账号、历史提交与做题状态
// ---------------------------------------------------------------------------

void test_side_data_unchanged() {
  std::cout << "密码/角色修改不改变账号、历史提交与做题状态\n";
  EchoExecutor executor;
  Env env("au_side", "", &executor);
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首改");

  auto pid_res = admin_create_problem(
      cli, admin,
      R"({"title":"旁路","difficulty":"easy","samples":[{"input":"x","output":"x"}]})");
  std::int64_t pid = json::parse(pid_res->body).value("id", -1LL);
  check(pid > 0, "创建题目");

  std::string account = register_user(cli, "judy", "JudyPass1");
  int s = 0;
  std::string token = login(cli, account, "JudyPass1", s);
  check(s == 200 && !token.empty(), "judy 登录");
  auto sub = submit(cli, token, pid);
  check(sub && json::parse(sub->body).value("status", "") == "AC",
        "judy 提交获得 AC");

  UserRow before = read_user(env.db(), account);
  const StatusRow status_before = read_status(env.db(), before.id, pid);
  const std::int64_t subs_before = count_submissions(env.db(), before.id);
  check(status_before.found && status_before.status == "accepted" &&
            status_before.submit_count == 1 && subs_before == 1,
        "记录初始提交与 AC 状态");

  check(admin_put_user(cli, admin,
                       "{\"action\":\"reset_password\",\"user_id\":" +
                           std::to_string(before.id) +
                           ",\"new_password\":\"JudyNew1\"}")
            ->status == 200,
        "重置 judy 密码");
  check(admin_put_user(cli, admin,
                       "{\"action\":\"change_role\",\"user_id\":" +
                           std::to_string(before.id) + ",\"role\":\"admin\"}")
            ->status == 200,
        "提升 judy 为管理员");

  UserRow after = read_user(env.db(), account);
  StatusRow status_after = read_status(env.db(), before.id, pid);
  check(after.account == before.account && after.nickname == before.nickname &&
            after.created_at == before.created_at,
        "账号/昵称/注册时间均未改变");
  check(count_submissions(env.db(), before.id) == subs_before &&
            status_after.found && status_after.status == "accepted" &&
            status_after.first_ac_at == status_before.first_ac_at &&
            status_after.submit_count == status_before.submit_count,
        "历史提交数量、AC 状态、首次 AC 时间与提交次数均不变");
}

// ---------------------------------------------------------------------------
// 9. 自我降级与最后管理员保护
// ---------------------------------------------------------------------------

void test_last_admin_and_self_demotion() {
  std::cout << "最后管理员保护与自我降级规则\n";
  Env env("au_lastadmin");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首改");

  UserRow preset = read_user(env.db(), "admin");
  auto self_demote = admin_put_user(
      cli, admin,
      "{\"action\":\"change_role\",\"user_id\":" +
          std::to_string(preset.id) + ",\"role\":\"user\"}");
  check(self_demote && self_demote->status == 409,
        "唯一管理员自我降级返回 409");
  check(read_user(env.db(), "admin").role == "admin" &&
            count_admins(env.db()) == 1,
        "被拒后预置 admin 仍为管理员");
  // 保护未使 token 失效。
  check(admin_get_users(cli, admin)->status == 200, "被拒的管理员仍可访问");

  // 增加第二个管理员后可自我降级；降级后原 token 失效管理权限。
  std::string account = register_user(cli, "grace", "GracePass1");
  UserRow grace = read_user(env.db(), account);
  check(admin_put_user(cli, admin,
                       "{\"action\":\"change_role\",\"user_id\":" +
                           std::to_string(grace.id) + ",\"role\":\"admin\"}")
            ->status == 200,
        "提升 grace 为管理员（现有 2 名管理员）");
  auto now_ok = admin_put_user(
      cli, admin,
      "{\"action\":\"change_role\",\"user_id\":" +
          std::to_string(preset.id) + ",\"role\":\"user\"}");
  check(now_ok && now_ok->status == 200, "非最后一名管理员自我降级成功");
  check(count_admins(env.db()) == 1, "降级后仅剩 1 名管理员");
  check(admin_get_users(cli, admin)->status == 403,
        "自我降级后原 token 失去管理员权限");
}

// ---------------------------------------------------------------------------
// 10. 并发角色修改：最后管理员保护不可被绕过
// ---------------------------------------------------------------------------

void test_concurrent_role_changes() {
  std::cout << "并发降级不会把管理员清零\n";
  Env env("au_conc");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首改");

  std::string account = register_user(cli, "heidi", "HeidiPass1");
  UserRow heidi = read_user(env.db(), account);
  check(admin_put_user(cli, admin,
                       "{\"action\":\"change_role\",\"user_id\":" +
                           std::to_string(heidi.id) + ",\"role\":\"admin\"}")
            ->status == 200,
        "提升 heidi 为管理员");
  int s = 0;
  std::string heidi_token = login(cli, account, "HeidiPass1", s);
  check(s == 200 && !heidi_token.empty(), "heidi 登录");

  UserRow preset = read_user(env.db(), "admin");
  bool invariant_ok = true;
  for (int i = 0; i < 3; ++i) {
    // 重置为两名管理员后再并发互降。
    UserRow cur_preset = read_user(env.db(), "admin");
    UserRow cur_heidi = read_user(env.db(), account);
    if (cur_preset.role != "admin") {
      admin_put_user(cli, heidi_token,
                     "{\"action\":\"change_role\",\"user_id\":" +
                         std::to_string(cur_preset.id) + ",\"role\":\"admin\"}");
    }
    if (cur_heidi.role != "admin") {
      admin_put_user(cli, admin,
                     "{\"action\":\"change_role\",\"user_id\":" +
                         std::to_string(cur_heidi.id) + ",\"role\":\"admin\"}");
    }

    std::atomic<bool> go{false};
    int st_preset = 0;
    int st_heidi = 0;
    std::thread a([&]() {
      httplib::Client c = make_client(env.port());
      while (!go.load()) {
        std::this_thread::yield();
      }
      auto r = admin_put_user(
          c, admin, "{\"action\":\"change_role\",\"user_id\":" +
                        std::to_string(heidi.id) + ",\"role\":\"user\"}");
      st_preset = r ? r->status : -1;
    });
    std::thread b([&]() {
      httplib::Client c = make_client(env.port());
      while (!go.load()) {
        std::this_thread::yield();
      }
      auto r = admin_put_user(
          c, heidi_token,
          "{\"action\":\"change_role\",\"user_id\":" +
              std::to_string(preset.id) + ",\"role\":\"user\"}");
      st_heidi = r ? r->status : -1;
    });
    go = true;
    a.join();
    b.join();

    const std::int64_t admins = count_admins(env.db());
    const bool one_success = (st_preset == 200) != (st_heidi == 200);
    const bool other_rejected =
        (st_preset == 200 ? (st_heidi == 403 || st_heidi == 409)
                          : (st_preset == 403 || st_preset == 409));
    if (admins != 1 || !one_success || !other_rejected) {
      invariant_ok = false;
      std::cout << "    迭代 " << i << "：preset=" << st_preset
                << " heidi=" << st_heidi << " admins=" << admins << "\n";
    }
  }
  check(invariant_ok, "3 轮并发互降后始终恰好保留 1 名管理员");
}

// ---------------------------------------------------------------------------
// 11. 数据库故障
// ---------------------------------------------------------------------------

void test_db_failure() {
  std::cout << "数据库故障返回 500 且不泄露内部细节\n";
  Env env("au_dbfail");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首改");

  env.close_db();
  auto g = admin_get_users(cli, admin);
  auto p = admin_put_user(
      cli, admin,
      R"({"action":"change_role","user_id":1,"role":"user"})");
  check(g && g->status == 500, "数据库故障查询返回 500");
  check(p && p->status == 500, "数据库故障修改返回 500");
  for (httplib::Result *r : {&g, &p}) {
    if (*r) {
      check((*r)->body.find("sqlite") == std::string::npos &&
                (*r)->body.find("SELECT") == std::string::npos &&
                (*r)->body.find("users") == std::string::npos,
            "错误响应不泄露 SQL/表名");
    }
  }
}

// ---------------------------------------------------------------------------
// 12. 重启持久化
// ---------------------------------------------------------------------------

void test_persistence_restart() {
  std::cout << "重启后密码与角色修改仍然保留\n";
  TempDir dir("au_persist");
  const std::string dbpath = dir.db_path();
  std::int64_t target_id = -1;
  std::string ivan_account;

  {
    Env env("au_persist_1", dbpath);
    check(env.ok(), "首次启动成功");
    httplib::Client cli = make_client(env.port());
    std::string admin = admin_login(cli);
    check(!admin.empty(), "管理员登录并完成首改");
    std::string account = register_user(cli, "ivan", "IvanPass1");
    ivan_account = account;
    UserRow ivan = read_user(env.db(), account);
    target_id = ivan.id;
    check(admin_put_user(cli, admin,
                         "{\"action\":\"reset_password\",\"user_id\":" +
                             std::to_string(ivan.id) +
                             ",\"new_password\":\"IvanNew1\"}")
              ->status == 200,
          "重置 ivan 密码");
    check(admin_put_user(cli, admin,
                         "{\"action\":\"change_role\",\"user_id\":" +
                             std::to_string(ivan.id) + ",\"role\":\"admin\"}")
              ->status == 200,
          "提升 ivan 为管理员");
    env.stop();
    env.close_db();
  }

  {
    Env env("au_persist_2", dbpath);
    check(env.ok(), "重启成功");
    httplib::Client cli = make_client(env.port());
    UserRow ivan = read_user(env.db(), ivan_account);
    check(ivan.id == target_id && ivan.role == "admin" &&
              ivan.reset_pwd_flag == 1,
          "重启后角色与首次改密标记保留");
    std::string verify_err;
    check(oj::auth::verify_password(ivan.password_hash, "IvanNew1",
                                    verify_err),
          "重启后仍保存新密码哈希");
    int s_old = 0;
    login(cli, ivan_account, "IvanPass1", s_old);
    check(s_old == 401, "重启后旧密码仍不能登录");
    int s_new = 0;
    std::string token = login(cli, ivan_account, "IvanNew1", s_new);
    check(s_new == 200 && !token.empty(), "重启后新密码可登录");
    env.stop();
    env.close_db();
  }
}

// ---------------------------------------------------------------------------
// 13. 查询参数、分页整数倍边界、同角色 no-op 与超大请求体
// ---------------------------------------------------------------------------

void test_query_param_and_boundary_edges() {
  std::cout << "查询参数忽略、分页整数倍边界、同角色 no-op 与 413\n";
  Env env("au_edges");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首改");

  std::string account = register_user(cli, "karl", "KarlPass1");
  UserRow karl = read_user(env.db(), account);

  // 客户端自带的 page_size 与未知参数应被忽略：page_size 恒为 20。
  auto ignored = admin_get_users(cli, admin, "page_size=1000&foo=bar&page=1");
  json ignored_json = json::parse(ignored->body);
  check(ignored && ignored->status == 200 &&
            ignored_json.value("page_size", 0) == 20,
        "忽略客户端 page_size/未知查询参数，page_size 恒为 20");

  // 同角色 no-op：唯一管理员 admin->admin 不应触发最后管理员保护。
  UserRow preset = read_user(env.db(), "admin");
  auto admin_noop = admin_put_user(
      cli, admin,
      "{\"action\":\"change_role\",\"user_id\":" +
          std::to_string(preset.id) + ",\"role\":\"admin\"}");
  check(admin_noop && admin_noop->status == 200 &&
            read_user(env.db(), "admin").role == "admin",
        "唯一管理员改为 admin（no-op）返回 200");
  // 普通用户 user->user（no-op）。
  auto user_noop = admin_put_user(
      cli, admin,
      "{\"action\":\"change_role\",\"user_id\":" + std::to_string(karl.id) +
          ",\"role\":\"user\"}");
  check(user_noop && user_noop->status == 200 &&
            read_user(env.db(), account).role == "user",
        "普通用户改为 user（no-op）返回 200");

  // 分页整数倍边界：构造恰好 40 个用户（total_pages 应为 2）。
  const std::int64_t before = count_users(env.db());
  for (std::int64_t i = before; i < 40; ++i) {
    if (!insert_dummy_user(env.db(), "d" + std::to_string(i),
                           "dn" + std::to_string(i))) {
      check(false, "构造分页边界数据失败");
      return;
    }
  }
  auto e1 = admin_get_users(cli, admin, "page=1");
  auto e2 = admin_get_users(cli, admin, "page=2");
  auto e3 = admin_get_users(cli, admin, "page=3");
  json j1 = json::parse(e1->body);
  json j2 = json::parse(e2->body);
  json j3 = json::parse(e3->body);
  check(j1.value("total", 0) == 40 && j1.value("total_pages", 0) == 2 &&
            j1["users"].size() == 20 && j2["users"].size() == 20 &&
            j3["users"].empty(),
        "total=40 整除边界：两页各 20、末页之后为空、total_pages=2");
  std::vector<std::int64_t> ids;
  for (const auto &u : j1["users"]) {
    ids.push_back(u.value("id", 0LL));
  }
  for (const auto &u : j2["users"]) {
    ids.push_back(u.value("id", 0LL));
  }
  bool contiguous = ids.size() == 40;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] != static_cast<std::int64_t>(i + 1)) {
      contiguous = false;
    }
  }
  check(contiguous, "整数倍分页跨页仍为 id 1..40 连续、不重不漏");

  // 超大请求体（>1 MiB）由服务器在解析前拒绝，返回 413，不产生副作用。
  json big;
  big["action"] = "reset_password";
  big["user_id"] = karl.id;
  big["new_password"] = std::string(2 * 1024 * 1024, 'a');
  const std::string hash_before = read_user(env.db(), account).password_hash;
  auto too_large = admin_put_user(cli, admin, big.dump());
  check(too_large && too_large->status == 413, "超大请求体返回 413");
  check(read_user(env.db(), account).password_hash == hash_before &&
            read_user(env.db(), account).reset_pwd_flag == 0,
        "413 后目标用户密码与标记未被修改");
}

// ---------------------------------------------------------------------------
// 14. 并发重置密码 + 改角色（同一用户）
// ---------------------------------------------------------------------------

void test_concurrent_reset_and_role() {
  std::cout << "并发重置密码与改角色：各自生效、终态自洽\n";
  Env env("au_conc_rr");
  check(env.ok(), "服务启动成功");
  httplib::Client cli = make_client(env.port());
  std::string admin = admin_login(cli);
  check(!admin.empty(), "管理员登录并完成首改");

  std::string account = register_user(cli, "nina", "NinaPass1");
  UserRow nina = read_user(env.db(), account);
  check(nina.id > 0, "注册目标用户");

  bool all_ok = true;
  for (int i = 0; i < 3; ++i) {
    const std::string new_pw = "NinaNew" + std::to_string(i);
    const std::string new_role = (i % 2 == 0) ? "admin" : "user";
    std::atomic<bool> go{false};
    int reset_status = 0;
    int role_status = 0;

    std::thread resetter([&]() {
      httplib::Client c = make_client(env.port());
      while (!go.load()) {
        std::this_thread::yield();
      }
      auto r = admin_put_user(
          c, admin,
          "{\"action\":\"reset_password\",\"user_id\":" +
              std::to_string(nina.id) + ",\"new_password\":\"" + new_pw + "\"}");
      reset_status = r ? r->status : -1;
    });
    std::thread role_changer([&]() {
      httplib::Client c = make_client(env.port());
      while (!go.load()) {
        std::this_thread::yield();
      }
      auto r = admin_put_user(
          c, admin,
          "{\"action\":\"change_role\",\"user_id\":" +
              std::to_string(nina.id) + ",\"role\":\"" + new_role + "\"}");
      role_status = r ? r->status : -1;
    });
    go = true;
    resetter.join();
    role_changer.join();

    UserRow cur = read_user(env.db(), account);
    std::string verify_err;
    const bool pw_ok =
        oj::auth::verify_password(cur.password_hash, new_pw, verify_err);
    if (!(reset_status == 200 && role_status == 200 &&
          cur.role == new_role && cur.reset_pwd_flag == 1 && pw_ok)) {
      all_ok = false;
      std::cout << "    迭代 " << i << "：reset=" << reset_status
                << " role=" << role_status << " cur_role=" << cur.role
                << " flag=" << cur.reset_pwd_flag << " pw_ok=" << pw_ok << "\n";
    }
  }
  check(all_ok, "3 轮并发重置+改角色均 200，角色/密码/标记终态一致");
}

} // namespace

int main() {
  test_list_fields_order_and_no_leak();
  test_pagination();
  test_permissions();
  test_reset_password_lifecycle();
  test_role_change_promote_and_demote();
  test_invalid_requests_no_change();
  test_extra_fields_ignored();
  test_side_data_unchanged();
  test_last_admin_and_self_demotion();
  test_concurrent_role_changes();
  test_query_param_and_boundary_edges();
  test_concurrent_reset_and_role();
  test_db_failure();
  test_persistence_restart();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部管理员用户接口集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
