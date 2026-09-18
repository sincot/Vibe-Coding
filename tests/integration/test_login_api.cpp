// 登录与身份验证集成测试（M1.2）。
//
// 通过 httplib::Client 访问真实运行的 HTTP 服务（隔离端口 + /tmp 临时库 + 专用
// 测试密钥），覆盖：登录成功（普通用户 / 预置 admin）、错误密码与不存在账号一致
// 提示、非法输入、/api/me 身份一致性、各类非法 token 拒绝、数据库信息变更后
// /api/me 返回最新值、登录限速（含可注入时钟验证窗口恢复）、同密钥重启后 token
// 仍有效。不触碰正式数据库与真实密钥。
//
// 运行方式：ctest --test-dir build -R login_api --output-on-failure
// 或直接执行 build/oj_login_api_test。

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
                                               make_config());
    if (!server_->start(err)) {
      return;
    }
    started_ = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  bool ok() const { return started_; }
  int port() const { return port_; }
  oj::Database &db() { return *db_; }
  oj::HttpServer &server() { return *server_; }

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

httplib::Result get_me(httplib::Client &cli, const std::string &token) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Get("/api/me", h);
}

httplib::Result get_me_raw(httplib::Client &cli, const std::string &auth) {
  httplib::Headers h{{"Authorization", auth}};
  return cli.Get("/api/me", h);
}

// ---- 构造各类非法/边缘 token（与测试密钥一致，便于验证拒绝行为）----
std::string sign_token(std::int64_t sub, int exp_offset = 3600,
                       bool with_sub = true, bool with_exp = true,
                       bool with_iat = true) {
  auto now = std::chrono::system_clock::now();
  auto b = jwt::create().set_issuer("oj").set_audience("oj-api");
  if (with_iat) {
    b.set_issued_at(now);
  }
  if (with_exp) {
    b.set_expires_at(now + std::chrono::seconds(exp_offset));
  }
  if (with_sub) {
    b.set_payload_claim("sub", jwt::claim(std::to_string(sub)));
  }
  return b.sign(jwt::algorithm::hs256{kTestSecret});
}

std::string none_token(std::int64_t sub) {
  auto now = std::chrono::system_clock::now();
  return jwt::create()
      .set_issuer("oj")
      .set_audience("oj-api")
      .set_issued_at(now)
      .set_expires_at(now + std::chrono::seconds(3600))
      .set_payload_claim("sub", jwt::claim(std::to_string(sub)))
      .sign(jwt::algorithm::none{});
}

std::string hs384_token(std::int64_t sub) {
  auto now = std::chrono::system_clock::now();
  return jwt::create()
      .set_issuer("oj")
      .set_audience("oj-api")
      .set_issued_at(now)
      .set_expires_at(now + std::chrono::seconds(3600))
      .set_payload_claim("sub", jwt::claim(std::to_string(sub)))
      .sign(jwt::algorithm::hs384{kTestSecret});
}

std::string numeric_sub_token() {
  auto now = std::chrono::system_clock::now();
  return jwt::create()
      .set_issuer("oj")
      .set_audience("oj-api")
      .set_issued_at(now)
      .set_expires_at(now + std::chrono::seconds(3600))
      .set_payload_claim("sub", jwt::claim(static_cast<std::int64_t>(42)))
      .sign(jwt::algorithm::hs256{kTestSecret});
}

void test_login_success() {
  std::cout << "普通用户登录成功：token/类型/有效期/user 信息\n";
  Env env("login_ok");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::string account = register_user(cli, "alice", "Secret123");
  check(!account.empty(), "注册成功");

  int status = 0;
  json body = login(cli, account, "Secret123", status);
  check(status == 200, "返回 200");
  check(!body.value("token", "").empty(), "返回 token");
  check(body.value("token_type", "") == "Bearer", "token_type 为 Bearer");
  check(body.value("expires_in", 0) == 3600, "有效期 3600");
  check(body.contains("user"), "返回 user 信息");
  check(body["user"].value("account", "") == account, "user.account 正确");
  check(body["user"].value("nickname", "") == "alice", "user.nickname 正确");
  check(body["user"].value("role", "") == "user", "user.role 为 user");
  check(body["user"].value("reset_pwd_flag", -1) == 0,
        "普通用户 reset_pwd_flag 为 0");
  check(body.dump().find("password") == std::string::npos,
        "响应不包含密码字段");
}

void test_admin_login() {
  std::cout << "预置 admin 登录成功并保留首次改密标记\n";
  Env env("login_admin");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  int status = 0;
  json body = login(cli, "admin", kAdminPassword, status);
  check(status == 200, "admin 登录返回 200");
  check(body["user"].value("account", "") == "admin", "账号为 admin");
  check(body["user"].value("role", "") == "admin", "角色为 admin");
  check(body["user"].value("reset_pwd_flag", -1) == 1,
        "保留首次改密标记 reset_pwd_flag=1");
}

void test_consistent_failure() {
  std::cout << "错误密码与不存在账号返回一致提示\n";
  Env env("login_fail");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::string account = register_user(cli, "bob", "RightPass1");
  check(!account.empty(), "注册成功");

  int s1 = 0, s2 = 0, s3 = 0;
  json wrong_pw = login(cli, account, "WrongPass9", s1);
  json no_user = login(cli, "9999999999", "Whatever1", s2);
  json wrong_admin = login(cli, "admin", "badpass", s3);

  check(s1 == 401 && s2 == 401 && s3 == 401, "均为 401");
  check(wrong_pw.value("error", "") == no_user.value("error", "") &&
            wrong_pw.value("error", "") == wrong_admin.value("error", ""),
        "错误提示完全一致");
  check(!wrong_pw.value("error", "").empty(), "错误提示非空");
}

void test_invalid_input() {
  std::cout << "缺少字段/类型错误/非法 JSON：400\n";
  Env env("login_invalid");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  check(post_json(cli, "/api/login", "{}")->status == 400, "空对象 400");
  check(post_json(cli, "/api/login", R"({"account":"x"})")->status == 400,
        "缺 password 400");
  check(post_json(cli, "/api/login", R"({"password":"x"})")->status == 400,
        "缺 account 400");
  check(post_json(cli, "/api/login",
                  R"({"account":123,"password":"x"})")
            ->status == 400,
        "account 非字符串 400");
  check(post_json(cli, "/api/login",
                  R"({"account":"x","password":123})")
            ->status == 400,
        "password 非字符串 400");
  check(post_json(cli, "/api/login", "not json")->status == 400, "非法 JSON 400");
  check(post_json(cli, "/api/login", R"({"account":"","password":"x"})")
            ->status == 400,
        "空 account 400");
}

void test_me_identity() {
  std::cout << "/api/me 返回与登录一致的身份\n";
  Env env("me_id");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::string account = register_user(cli, "carol", "Pw123456");
  int status = 0;
  json login_body = login(cli, account, "Pw123456", status);
  check(status == 200, "登录成功");
  std::string token = login_body.value("token", "");

  auto me = get_me(cli, token);
  check(me && me->status == 200, "/api/me 返回 200");
  json me_body = json::parse(me->body);
  check(me_body.value("account", "") == account, "账号一致");
  check(me_body.value("nickname", "") == "carol", "昵称一致");
  check(me_body.value("role", "") == "user", "角色一致");
  check(me_body.value("id", 0) == login_body["user"].value("id", -1),
        "id 与登录返回一致");
  check(!me_body.contains("password_hash") && !me_body.contains("password"),
        "不返回密码哈希等敏感字段");
}

void test_me_invalid_tokens() {
  std::cout << "/api/me 拒绝各类非法 token\n";
  Env env("me_invalid");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::string account = register_user(cli, "dave", "Pw123456");
  int status = 0;
  std::string valid = login(cli, account, "Pw123456", status).value("token", "");

  // 缺失 token / 错误认证格式。
  auto no_auth = cli.Get("/api/me");
  check(no_auth && no_auth->status == 401, "缺失 Authorization 401");
  check(get_me_raw(cli, "Basic abc") && get_me_raw(cli, "Basic abc")->status == 401,
        "非 Bearer 401");
  check(get_me_raw(cli, "Bearer")->status == 401, "无 token 401");

  // 损坏 token。
  check(get_me(cli, "not-a-jwt")->status == 401, "损坏 token 401");

  // 伪造签名（错误密钥签发）。
  {
    auto now = std::chrono::system_clock::now();
    auto forged = jwt::create()
                      .set_issuer("oj")
                      .set_audience("oj-api")
                      .set_issued_at(now)
                      .set_expires_at(now + std::chrono::seconds(3600))
                      .set_payload_claim("sub", jwt::claim(std::string("42")))
                      .sign(jwt::algorithm::hs256{"wrong-secret-0123456789abcdef"});
    check(get_me(cli, forged)->status == 401, "错误密钥伪造签名 401");
  }

  // 篡改内容（修改有效 token 的 payload）。
  {
    std::string tampered = valid;
    std::size_t first = tampered.find('.');
    char &c = tampered[first + 1];
    c = (c == 'A') ? 'B' : 'A';
    check(get_me(cli, tampered)->status == 401, "篡改内容 401");
  }

  // 过期 token。
  check(get_me(cli, sign_token(1, /*exp_offset=*/-10))->status == 401,
        "过期 token 401");

  // 无签名（alg=none）与算法不匹配（HS384）。
  check(get_me(cli, none_token(1))->status == 401, "alg=none 401");
  check(get_me(cli, hs384_token(1))->status == 401, "HS384 401");

  // 签名有效但缺少必要 claims / 身份字段非法 / 引用不存在用户。
  check(get_me(cli, sign_token(1, 3600, /*with_sub=*/false))->status == 401,
        "缺少 sub 401");
  check(get_me(cli, sign_token(1, 3600, true, /*with_exp=*/false))->status == 401,
        "缺少 exp 401");
  check(get_me(cli, numeric_sub_token())->status == 401, "数字 sub 401");

  auto bad_sub = jwt::create()
                     .set_issuer("oj")
                     .set_audience("oj-api")
                     .set_issued_at(std::chrono::system_clock::now())
                     .set_expires_at(std::chrono::system_clock::now() +
                                     std::chrono::seconds(3600))
                     .set_payload_claim("sub", jwt::claim(std::string("abc")))
                     .sign(jwt::algorithm::hs256{kTestSecret});
  check(get_me(cli, bad_sub)->status == 401, "sub=abc 401");

  // 签名有效、格式合法但引用不存在的用户。
  check(get_me(cli, sign_token(999999999))->status == 401, "引用不存在用户 401");
}

void test_me_reflects_db_changes() {
  std::cout << "/api/me 返回数据库最新昵称与角色\n";
  Env env("me_db");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::string account = register_user(cli, "erin", "Pw123456");
  int status = 0;
  std::string token = login(cli, account, "Pw123456", status).value("token", "");

  // 直接修改数据库中的昵称与角色。
  std::string err;
  oj::Statement stmt;
  env.db().prepare(
      "UPDATE users SET nickname = 'erin2', role = 'admin' WHERE account = ?",
      stmt, err);
  stmt.bind(1, account);
  check(stmt.step() == SQLITE_DONE, "数据库更新成功");

  auto me = get_me(cli, token);
  check(me && me->status == 200, "/api/me 返回 200");
  json body = json::parse(me->body);
  check(body.value("nickname", "") == "erin2", "昵称取最新值");
  check(body.value("role", "") == "admin", "角色取最新值");
}

void test_rate_limit_429_and_recovery() {
  std::cout << "登录限速：阈值后 429，窗口结束恢复\n";
  Env env("login_rl");
  check(env.ok(), "服务启动成功");

  // 可控时钟：注入到服务限速器，验证窗口恢复而无需等待。
  struct FakeClock {
    std::atomic<long long> seconds{0};
    std::chrono::steady_clock::time_point now() const {
      return std::chrono::steady_clock::time_point(
          std::chrono::seconds(seconds.load()));
    }
  } fc;
  env.server().rate_limiter().set_clock([&fc]() { return fc.now(); });

  httplib::Client cli("127.0.0.1", env.port());

  // 5 次失败尝试（默认阈值 5），均返回 401。
  for (int i = 0; i < 5; ++i) {
    int s = 0;
    login(cli, "9999999999", "bad", s);
    check(s == 401, "第 " + std::to_string(i + 1) + " 次失败 401");
  }

  // 第 6 次触发限速：429 + Retry-After。
  auto blocked_res = post_json(cli, "/api/login",
                               R"({"account":"9999999999","password":"bad"})");
  check(blocked_res && blocked_res->status == 429, "达到阈值后 429");
  check(blocked_res->has_header("Retry-After"), "响应含 Retry-After 头");

  // 窗口结束后恢复（推进时钟超过 900s 窗口）。
  fc.seconds = 901;

  std::string account = register_user(cli, "frank", "GoodPass1");
  int status = 0;
  json body = login(cli, account, "GoodPass1", status);
  check(status == 200, "窗口结束后成功登录（200 而非 429）");
}

void test_restart_same_key_token_valid() {
  std::cout << "同密钥重启后原 token 仍可用\n";
  TempDir dir("login_restart");
  std::string dbpath = dir.db_path();
  std::string token;

  {
    Env env("login_restart_first", dbpath);
    check(env.ok(), "首次启动成功");
    httplib::Client cli("127.0.0.1", env.port());
    std::string account = register_user(cli, "grace", "Pw123456");
    int status = 0;
    token = login(cli, account, "Pw123456", status).value("token", "");
    check(!token.empty(), "取得 token");
    env.stop();
    env.close_db();
  }

  {
    Env env("login_restart_second", dbpath);
    check(env.ok(), "同密钥重启成功");
    httplib::Client cli("127.0.0.1", env.port());
    auto me = get_me(cli, token);
    check(me && me->status == 200, "重启后原 token 访问 /api/me 成功");
    check(json::parse(me->body).value("nickname", "") == "grace",
          "身份一致");
    env.stop();
    env.close_db();
  }
}

} // namespace

int main() {
  test_login_success();
  test_admin_login();
  test_consistent_failure();
  test_invalid_input();
  test_me_identity();
  test_me_invalid_tokens();
  test_me_reflects_db_changes();
  test_rate_limit_429_and_recovery();
  test_restart_same_key_token_valid();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部登录与身份验证集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
