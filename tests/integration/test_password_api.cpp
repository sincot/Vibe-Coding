// 改密与权限检查集成测试（M1.3）。
//
// 通过 httplib::Client 访问真实运行的 HTTP 服务（隔离端口 + /tmp 临时库 + 专用
// 测试密钥 + 测试路由 /api/test/admin-only），覆盖：
//   - 普通用户改密成功、旧密码失效、新密码可登录
//   - 错误旧密码 / 非法新密码 / 与旧密码相同 / 非法请求被拒，原密码与标记不变
//   - 未登录 / 无效 / 过期 token 不能改密
//   - 客户端传入他人 id/account/role 不能改他人密码或提权
//   - admin 首登可访问 /api/me 与改密接口，但不能执行受限制业务（首改限制）
//   - admin 改密后 reset_pwd_flag 清除、通过管理员检查、重启后保留
//   - 管理员检查：未登录 401、普通用户 403、未改密 admin 403(改密码)、已改密 200
//   - 修改数据库角色后，已有 token 按当前数据库角色判断
//   - 并发改密只有一个成功，旧 token 行为与文档声明一致
// 不触碰正式数据库与真实密钥。
//
// 运行方式：ctest --test-dir build -R password_api --output-on-failure
// 或直接执行 build/oj_password_api_test。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

#include <atomic>
#include <chrono>
#include <filesystem>
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

const std::string kTestSecret = "it-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";

oj::auth::JwtConfig make_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = kTestSecret;
  cfg.expires_seconds = 3600;
  return cfg;
}

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

// 测试环境：隔离临时库 + 随机端口上的真实 HTTP 服务 + 专用测试密钥。
// enable_test_routes 开启 /api/test/admin-only 测试路由（正式服务恒为 false）。
class Env {
public:
  explicit Env(const std::string &label, const std::string &db_path = "")
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
                                               /*enable_test_routes=*/true);
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

httplib::Result post_json(httplib::Client &cli, const std::string &path,
                          const std::string &body) {
  return cli.Post(path.c_str(), body, "application/json");
}

std::string register_user(httplib::Client &cli, const std::string &nickname,
                          const std::string &password) {
  auto res = post_json(cli, "/api/register",
                       "{\"nickname\":\"" + nickname + "\",\"password\":\"" +
                           password + "\"}");
  if (!res || res->status != 201) {
    return "";
  }
  return json::parse(res->body).value("account", "");
}

json login(httplib::Client &cli, const std::string &account,
           const std::string &password, int &status) {
  auto res = post_json(cli, "/api/login",
                       "{\"account\":\"" + account + "\",\"password\":\"" +
                           password + "\"}");
  status = res ? res->status : -1;
  if (!res || res->body.empty()) {
    return json::object();
  }
  return json::parse(res->body);
}

httplib::Result change_password(httplib::Client &cli, const std::string &token,
                                const std::string &body) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Post("/api/me/password", h, body, "application/json");
}

httplib::Result change_password_raw(httplib::Client &cli,
                                    const std::string &auth,
                                    const std::string &body) {
  httplib::Headers h{{"Authorization", auth}};
  return cli.Post("/api/me/password", h, body, "application/json");
}

httplib::Result get_me(httplib::Client &cli, const std::string &token) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Get("/api/me", h);
}

httplib::Result admin_only(httplib::Client &cli, const std::string &token) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Get("/api/test/admin-only", h);
}

std::string sign_token(std::int64_t sub, int exp_offset = 3600) {
  auto now = std::chrono::system_clock::now();
  return jwt::create()
      .set_issuer("oj")
      .set_audience("oj-api")
      .set_issued_at(now)
      .set_expires_at(now + std::chrono::seconds(exp_offset))
      .set_payload_claim("sub", jwt::claim(std::to_string(sub)))
      .sign(jwt::algorithm::hs256{kTestSecret});
}

void test_user_change_password_success() {
  std::cout << "普通用户改密成功：旧密码失效、新密码可登录\n";
  Env env("pw_ok");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::string account = register_user(cli, "alice", "OldPass123");
  check(!account.empty(), "注册成功");

  int status = 0;
  std::string token = login(cli, account, "OldPass123", status).value("token", "");
  check(status == 200 && !token.empty(), "登录成功取得 token");

  auto res = change_password(cli, token,
                             R"({"old_password":"OldPass123","new_password":"NewPass456"})");
  check(res && res->status == 200, "改密返回 200");
  check(res && json::parse(res->body).value("status", "") == "ok",
        "响应 status 为 ok");

  int s_old = 0, s_new = 0;
  login(cli, account, "OldPass123", s_old);
  login(cli, account, "NewPass456", s_new);
  check(s_old == 401, "旧密码不能再登录");
  check(s_new == 200, "新密码可以登录");
}

void test_change_password_rejections() {
  std::cout << "错误旧密码 / 非法新密码 / 相同新密码 / 非法请求被拒\n";
  Env env("pw_reject");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::string account = register_user(cli, "bob", "RealPass1");
  int status = 0;
  std::string token = login(cli, account, "RealPass1", status).value("token", "");

  // 记录改密前的哈希与标记，验证拒绝路径不改变数据库状态。
  std::string err;
  oj::Statement stmt;
  env.db().prepare(
      "SELECT password_hash, reset_pwd_flag FROM users WHERE account = ?", stmt,
      err);
  stmt.bind(1, account);
  bool has_row = (stmt.step() == SQLITE_ROW);
  std::string before_hash = has_row ? stmt.column_text(0) : "";
  int before_flag = has_row ? stmt.column_int(1) : -1;

  // 错误旧密码 -> 401，哈希与标记不变。
  auto wrong_old = change_password(cli, token,
                                   R"({"old_password":"WrongPass9","new_password":"NewPass456"})");
  check(wrong_old && wrong_old->status == 401, "错误旧密码 401");
  check(json::parse(wrong_old->body).value("error", "") == "旧密码错误",
        "错误提示为旧密码错误");

  // 与旧密码相同 -> 400。
  auto same = change_password(cli, token,
                              R"({"old_password":"RealPass1","new_password":"RealPass1"})");
  check(same && same->status == 400, "新密码与旧密码相同 400");

  // 空新密码 / 超长新密码 -> 400。
  check(change_password(cli, token,
                        R"({"old_password":"RealPass1","new_password":""})")
            ->status == 400,
        "空新密码 400");
  check(change_password(cli, token,
                        "{\"old_password\":\"RealPass1\",\"new_password\":\"" +
                            std::string(129, 'p') + "\"}")
            ->status == 400,
        "超长新密码 400");

  // 缺少字段 / 类型错误 / 非法 JSON -> 400。
  check(change_password(cli, token, "{}")->status == 400, "空对象 400");
  check(change_password(cli, token, R"({"old_password":"RealPass1"})")
            ->status == 400,
        "缺 new_password 400");
  check(change_password(cli, token, R"({"new_password":"NewPass456"})")
            ->status == 400,
        "缺 old_password 400");
  check(change_password(cli, token,
                        R"({"old_password":123,"new_password":"x"})")
            ->status == 400,
        "old_password 非字符串 400");
  check(change_password(cli, token, "not json")->status == 400, "非法 JSON 400");

  // 验证原密码与标记均未改变。
  oj::Statement stmt2;
  env.db().prepare(
      "SELECT password_hash, reset_pwd_flag FROM users WHERE account = ?", stmt2,
      err);
  stmt2.bind(1, account);
  bool has_row2 = (stmt2.step() == SQLITE_ROW);
  check(has_row2 && stmt2.column_text(0) == before_hash, "哈希未改变");
  check(has_row2 && stmt2.column_int(1) == before_flag, "首次改密标记未改变");
  check(before_flag == 0, "普通用户标记原为 0");

  // 原密码仍可登录。
  int s = 0;
  login(cli, account, "RealPass1", s);
  check(s == 200, "拒绝后原密码仍可登录");
}

void test_change_password_auth_required() {
  std::cout << "未登录 / 无效 / 过期 token 不能改密\n";
  Env env("pw_auth");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::string account = register_user(cli, "carol", "Pw123456");
  int status = 0;
  login(cli, account, "Pw123456", status);

  // 无 Authorization。
  check(cli.Post("/api/me/password",
                 R"({"old_password":"Pw123456","new_password":"NewPw1"})",
                 "application/json")
            ->status == 401,
        "无 token 401");
  // 非 Bearer。
  check(change_password_raw(cli, "Basic abc",
                            R"({"old_password":"Pw123456","new_password":"NewPw1"})")
            ->status == 401,
        "非 Bearer 401");
  // 损坏 token。
  check(change_password(cli, "not-a-jwt",
                        R"({"old_password":"Pw123456","new_password":"NewPw1"})")
            ->status == 401,
        "损坏 token 401");
  // 过期 token（引用真实用户但已过期）。
  oj::Statement stmt;
  std::string err;
  env.db().prepare("SELECT id FROM users WHERE account = ?", stmt, err);
  stmt.bind(1, account);
  int64_t uid = (stmt.step() == SQLITE_ROW) ? stmt.column_int64(0) : -1;
  check(change_password(cli, sign_token(uid, /*exp_offset=*/-10),
                        R"({"old_password":"Pw123456","new_password":"NewPw1"})")
            ->status == 401,
        "过期 token 401");

  // 原密码仍未变（所有拒绝均未改密）。
  int s = 0;
  login(cli, account, "Pw123456", s);
  check(s == 200, "原密码仍有效");
}

void test_change_password_ignores_forged_fields() {
  std::cout << "传入他人 id/account/role 不能改他人密码或提权\n";
  Env env("pw_forge");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::string alice = register_user(cli, "alice2", "AlicePw1");
  std::string bob = register_user(cli, "bob2", "BobPw1");
  int status = 0;
  std::string alice_token = login(cli, alice, "AlicePw1", status).value("token", "");

  // 取 bob 的数据库 id。
  std::string err;
  oj::Statement stmt;
  env.db().prepare("SELECT id FROM users WHERE account = ?", stmt, err);
  stmt.bind(1, bob);
  int64_t bob_id = (stmt.step() == SQLITE_ROW) ? stmt.column_int64(0) : -1;

  // alice 改密请求体夹带 bob 的 id/account 与 role=admin，试图改 bob 密码并提权。
  std::string body = "{\"old_password\":\"AlicePw1\",\"new_password\":\"AlicePw2\","
                     "\"id\":" + std::to_string(bob_id) +
                     ",\"account\":\"" + bob + "\",\"role\":\"admin\"}";
  auto res = change_password(cli, alice_token, body);
  check(res && res->status == 200, "alice 改密成功（忽略夹带字段）");

  // bob 的密码不变，仍可用旧密码登录。
  int s_bob = 0;
  login(cli, bob, "BobPw1", s_bob);
  check(s_bob == 200, "bob 密码未被修改");

  // alice 用新密码登录，且角色仍为 user（未提权）。
  int s_alice = 0;
  json alice_login = login(cli, alice, "AlicePw2", s_alice);
  check(s_alice == 200, "alice 新密码登录成功");
  check(alice_login["user"].value("role", "") == "user", "alice 角色仍为 user");

  // alice 仍不能访问管理员路由。
  std::string alice_token2 = alice_login.value("token", "");
  auto admin_res = admin_only(cli, alice_token2);
  check(admin_res && admin_res->status == 403, "alice 仍被管理员路由拒绝");
}

void test_admin_first_login_restriction() {
  std::cout << "admin 首登可访问 /api/me 与改密，但不能访问受限业务\n";
  Env env("pw_admin_first");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  int status = 0;
  json body = login(cli, "admin", kAdminPassword, status);
  check(status == 200, "admin 登录成功");
  std::string token = body.value("token", "");
  check(body["user"].value("reset_pwd_flag", -1) == 1, "登录返回 reset_pwd_flag=1");

  // 可访问本人信息。
  auto me = get_me(cli, token);
  check(me && me->status == 200, "/api/me 可访问（首改前）");
  check(json::parse(me->body).value("reset_pwd_flag", -1) == 1,
        "/api/me 仍标记需改密");

  // 不能访问受限业务（测试专用管理员路由）。
  auto restricted = admin_only(cli, token);
  check(restricted && restricted->status == 403, "受限业务 403");
  json rbody = json::parse(restricted->body);
  check(rbody.value("code", "") == "PASSWORD_CHANGE_REQUIRED",
        "错误标识为 PASSWORD_CHANGE_REQUIRED");

  // 改密成功后解除限制。
  auto ch = change_password(cli, token,
                            R"({"old_password":"AdminSecret123!","new_password":"AdminNewPass1"})");
  check(ch && ch->status == 200, "admin 改密成功");

  auto admin_ok = admin_only(cli, token);
  check(admin_ok && admin_ok->status == 200, "改密后受限业务 200");

  auto me2 = get_me(cli, token);
  check(me2 && json::parse(me2->body).value("reset_pwd_flag", -1) == 0,
        "改密后 /api/me 标记已清除");
}

void test_admin_cannot_bypass_first_change() {
  std::cout << "admin 直接 HTTP 请求不能绕过首次改密限制\n";
  Env env("pw_admin_bypass");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  int status = 0;
  std::string token = login(cli, "admin", kAdminPassword, status).value("token", "");

  // 即使请求里夹带 reset_pwd_flag=0 / role 等字段，仍按数据库标记判定。
  // 改密接口本身不受首改限制，但管理员业务入口在改密前始终拒绝。
  auto restricted = admin_only(cli, token);
  check(restricted && restricted->status == 403, "改密前受限业务 403");
  check(json::parse(restricted->body).value("code", "") == "PASSWORD_CHANGE_REQUIRED",
        "标记为需要改密");

  // 尝试通过改密请求夹带字段把标记清 0（无效：改密必须旧密码正确且不改 role/flag 语义）。
  auto ch_fake = change_password(cli, token,
                                 R"({"old_password":"AdminSecret123!","new_password":"AdminNewPass1","reset_pwd_flag":0,"role":"admin"})");
  check(ch_fake && ch_fake->status == 200, "夹带字段的改密请求仍正常处理");
  // 改密后标记被真实清除（因改密成功），此时受限业务通过。
  auto admin_ok = admin_only(cli, token);
  check(admin_ok && admin_ok->status == 200, "真正改密后才通过受限业务");
}

void test_admin_change_persists_restart() {
  std::cout << "admin 改密后重启：新密码与标记保留\n";
  TempDir dir("pw_admin_restart");
  std::string dbpath = dir.db_path();

  {
    Env env("pw_admin_restart_first", dbpath);
    check(env.ok(), "首次启动成功");
    httplib::Client cli("127.0.0.1", env.port());
    int status = 0;
    std::string token = login(cli, "admin", kAdminPassword, status).value("token", "");
    auto ch = change_password(cli, token,
                              R"({"old_password":"AdminSecret123!","new_password":"AdminNewPass9"})");
    check(ch && ch->status == 200, "改密成功");
    env.stop();
    env.close_db();
  }

  {
    Env env("pw_admin_restart_second", dbpath);
    check(env.ok(), "同密钥重启成功");
    httplib::Client cli("127.0.0.1", env.port());

    int s_old = 0, s_new = 0;
    json nbody = login(cli, "admin", "AdminNewPass9", s_new);
    login(cli, "admin", kAdminPassword, s_old);
    check(s_old == 401, "重启后旧密码失效");
    check(s_new == 200, "重启后新密码可登录");
    check(nbody["user"].value("reset_pwd_flag", -1) == 0, "重启后标记仍为 0");

    // 新 token 可直接通过管理员检查。
    auto admin_ok = admin_only(cli, nbody.value("token", ""));
    check(admin_ok && admin_ok->status == 200, "重启后管理员检查通过");
    env.stop();
    env.close_db();
  }
}

void test_admin_check_matrix() {
  std::cout << "管理员检查矩阵：未登录 / 普通用户 / 未改密 / 已改密\n";
  Env env("pw_admin_matrix");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  // 未登录 -> 401。
  check(cli.Get("/api/test/admin-only")->status == 401, "未登录 401");

  // 普通用户 -> 403（权限不足，无 code）。
  std::string account = register_user(cli, "dave", "DavePw1");
  int status = 0;
  std::string user_token = login(cli, account, "DavePw1", status).value("token", "");
  auto user_res = admin_only(cli, user_token);
  check(user_res && user_res->status == 403, "普通用户 403");
  check(!json::parse(user_res->body).contains("code"), "普通用户无 code 标识");

  // admin 未改密 -> 403 + code。
  std::string admin_token = login(cli, "admin", kAdminPassword, status).value("token", "");
  auto admin_first = admin_only(cli, admin_token);
  check(admin_first && admin_first->status == 403, "未改密 admin 403");
  check(json::parse(admin_first->body).value("code", "") == "PASSWORD_CHANGE_REQUIRED",
        "未改密 admin 带改密标识");

  // admin 改密 -> 200。
  change_password(cli, admin_token,
                  R"({"old_password":"AdminSecret123!","new_password":"AdminNewPass2"})");
  check(admin_only(cli, admin_token)->status == 200, "已改密 admin 200");
}

void test_role_change_reflected_in_permissions() {
  std::cout << "修改数据库角色后，已有 token 按当前数据库角色判断\n";
  Env env("pw_role_change");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::string account = register_user(cli, "erin", "ErinPw1");
  int status = 0;
  std::string token = login(cli, account, "ErinPw1", status).value("token", "");

  // 普通用户：受限业务 403。
  check(admin_only(cli, token)->status == 403, "提权前受限业务 403");

  // 直接把数据库角色改为 admin 且清除首次改密标记。
  std::string err;
  oj::Statement stmt;
  env.db().prepare(
      "UPDATE users SET role = 'admin', reset_pwd_flag = 0 WHERE account = ?",
      stmt, err);
  stmt.bind(1, account);
  check(stmt.step() == SQLITE_DONE, "数据库角色改为 admin");

  // 同一 token 现在通过管理员检查（权限按当前数据库角色判断，而非 token 内容）。
  check(admin_only(cli, token)->status == 200, "提权后同一 token 通过管理员检查");

  // 把角色改回 user，同一 token 立即失效。
  oj::Statement stmt2;
  env.db().prepare("UPDATE users SET role = 'user' WHERE account = ?", stmt2, err);
  stmt2.bind(1, account);
  check(stmt2.step() == SQLITE_DONE, "数据库角色改回 user");
  check(admin_only(cli, token)->status == 403, "降级后同一 token 再次被拒绝");

  // 把角色设为 admin 但 reset_pwd_flag=1，则受首次改密限制。
  oj::Statement stmt3;
  env.db().prepare(
      "UPDATE users SET role = 'admin', reset_pwd_flag = 1 WHERE account = ?",
      stmt3, err);
  stmt3.bind(1, account);
  stmt3.step();
  auto res = admin_only(cli, token);
  check(res && res->status == 403 &&
            json::parse(res->body).value("code", "") == "PASSWORD_CHANGE_REQUIRED",
        "admin 但未改密受首改限制");
}

void test_concurrent_change_password() {
  std::cout << "并发改密：恰好一个成功，失败不部分更新\n";
  Env env("pw_race");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::string account = register_user(cli, "frank", "SharedOld1");
  int status = 0;
  std::string token = login(cli, account, "SharedOld1", status).value("token", "");

  const int kThreads = 6;
  std::atomic<int> ok{0};
  std::atomic<int> bad_old{0};
  std::atomic<int> other{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      httplib::Client c("127.0.0.1", env.port());
      std::string body = "{\"old_password\":\"SharedOld1\",\"new_password\":\"NewPw" +
                         std::to_string(i) + "\"}";
      auto r = change_password(c, token, body);
      if (r && r->status == 200) {
        ++ok;
      } else if (r && r->status == 401) {
        ++bad_old;
      } else {
        ++other;
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  check(ok.load() == 1, "恰好一个改密成功");
  check(bad_old.load() == kThreads - 1, "其余因旧密码失效被拒（401）");
  check(other.load() == 0, "无其它状态");

  // 直接校验数据库最终哈希，避免用登录验证时触发登录限速干扰结果。
  std::string err;
  oj::Statement stmt;
  env.db().prepare(
      "SELECT password_hash, reset_pwd_flag FROM users WHERE account = ?", stmt,
      err);
  stmt.bind(1, account);
  bool has_row = (stmt.step() == SQLITE_ROW);
  check(has_row, "读取最终哈希成功");
  std::string hash = has_row ? stmt.column_text(0) : "";
  int flag = has_row ? stmt.column_int(1) : -1;
  check(flag == 0, "首次改密标记被清除");
  check(!oj::auth::verify_password(hash, "SharedOld1", err), "旧密码不再有效");

  int valid_new = 0;
  for (int i = 0; i < kThreads; ++i) {
    if (oj::auth::verify_password(hash, "NewPw" + std::to_string(i), err)) {
      ++valid_new;
    }
  }
  check(valid_new == 1, "恰好一个新密码生效");
}

void test_change_password_internal_error() {
  std::cout << "内部故障：改密返回 500 且不泄露细节\n";
  Env env("pw_500");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::string account = register_user(cli, "grace", "GracePw1");
  int status = 0;
  std::string token = login(cli, account, "GracePw1", status).value("token", "");

  // 关闭数据库模拟内部故障。
  env.close_db();
  auto res = change_password(cli, token,
                             R"({"old_password":"GracePw1","new_password":"GracePw2"})");
  check(res && res->status == 500, "内部故障返回 500");
  if (res) {
    json body = json::parse(res->body);
    check(body.value("error", "") == "内部错误", "错误文案为通用内部错误");
    std::string raw = res->body;
    check(raw.find("sqlite") == std::string::npos &&
              raw.find("argon") == std::string::npos &&
              raw.find("password_hash") == std::string::npos,
          "不泄露数据库/哈希细节");
  }
}

void test_old_token_behavior_after_change() {
  std::cout << "改密后旧 token 行为与文档声明一致（不撤销，持续有效至过期）\n";
  Env env("pw_old_token");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  int status = 0;
  std::string old_token = login(cli, "admin", kAdminPassword, status).value("token", "");

  // 改密前：受限业务被首改限制拒绝。
  check(admin_only(cli, old_token)->status == 403, "改密前受限业务 403");

  // 改密。
  change_password(cli, old_token,
                  R"({"old_password":"AdminSecret123!","new_password":"AdminNewPass3"})");

  // 旧 token 未被撤销：仍可访问 /api/me。
  auto me = get_me(cli, old_token);
  check(me && me->status == 200, "旧 token 仍可访问 /api/me");

  // 旧 token 的权限按当前数据库状态判断：标记已清，故旧 token 也能通过管理员检查。
  check(admin_only(cli, old_token)->status == 200,
        "旧 token 权限随数据库状态更新而通过");
}

} // namespace

int main() {
  test_user_change_password_success();
  test_change_password_rejections();
  test_change_password_auth_required();
  test_change_password_ignores_forged_fields();
  test_admin_first_login_restriction();
  test_admin_cannot_bypass_first_change();
  test_admin_change_persists_restart();
  test_admin_check_matrix();
  test_role_change_reflected_in_permissions();
  test_concurrent_change_password();
  test_change_password_internal_error();
  test_old_token_behavior_after_change();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部改密与权限集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
