// M5.1 账号与认证回归补充集成测试。
//
// 目标：核对注册、账号/昵称唯一性、密码处理、登录限速、账号永久不复用等既有实现，
// 补足既有测试未覆盖的边界与安全场景（不重复已有测试）。使用隔离临时库 + 随机端口
// + 专用测试密钥，不触碰正式数据库与真实密钥。
//
// 覆盖：
//   - 登录限速不信任 X-Forwarded-For / X-Real-IP（伪造转发头不能按不同来源绕过）
//   - 成功登录清除失败计数
//   - 受控并发失败登录不能突破阈值（恰好 5 次放行）
//   - 限速窗口边界（自最早失败起 15 分钟内仍受限，超过后解除）
//   - 昵称规范化后与已有昵称冲突；内部空白保留且视为不同昵称
//   - 密码不裁剪不截断（含纯空白密码）在「注册 → 登录」链路生效
//   - 昵称长度按字节计（30 字节接受、31 字节拒绝，含多字节字符）
//   - 并发不同昵称注册：全部成功、账号唯一、无部分记录
//   - 账号永久不复用：数据库 account 唯一约束 + 重启后账号保留且不复用
//   - 登录失败响应不泄露哈希/明文密码
//
// 运行方式：ctest --test-dir build -R m51_auth_regression --output-on-failure
// 或直接执行 build/oj_m51_auth_regression_test。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
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

const std::string kTestSecret = "m51-secret-0123456789abc";
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
  std::string db_path() const { return dir_.db_path(); }

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

json register_user(httplib::Client &cli, const std::string &nickname,
                   const std::string &password, int *status = nullptr) {
  json body{{"nickname", nickname}, {"password", password}};
  auto res = post_json(cli, "/api/register", body.dump());
  if (status != nullptr) {
    *status = res ? res->status : -1;
  }
  if (!res || res->body.empty()) {
    return json::object();
  }
  return json::parse(res->body);
}

json login(httplib::Client &cli, const std::string &account,
           const std::string &password, int &status) {
  json body{{"account", account}, {"password", password}};
  auto res = post_json(cli, "/api/login", body.dump());
  status = res ? res->status : -1;
  if (!res || res->body.empty()) {
    return json::object();
  }
  return json::parse(res->body);
}

bool is_10_digits(const std::string &s) {
  if (s.size() != 10) {
    return false;
  }
  for (char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

int count_rows(oj::Database &db, const std::string &sql,
               const std::string &value) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare(sql, stmt, err)) {
    return -1;
  }
  if (!stmt.bind(1, value)) {
    return -1;
  }
  if (stmt.step() != SQLITE_ROW) {
    return -1;
  }
  return stmt.column_int(0);
}

// ---------------------------------------------------------------------------
// 登录限速
// ---------------------------------------------------------------------------

void test_rate_limit_ignores_forwarded_headers() {
  std::cout << "登录限速不信任客户端转发头\n";
  Env env("m51_xff");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  for (int i = 0; i < 5; ++i) {
    httplib::Headers h{{"X-Forwarded-For", "10.0.0." + std::to_string(i)},
                       {"X-Real-IP", "10.0.1." + std::to_string(i)}};
    auto res = cli.Post("/api/login", h,
                        R"({"account":"9999999999","password":"bad"})",
                        "application/json");
    check(res && res->status == 401,
          "第 " + std::to_string(i + 1) + " 次（伪造转发头）401");
  }

  httplib::Headers h{{"X-Forwarded-For", "10.0.0.250"}};
  auto blocked = cli.Post("/api/login", h,
                          R"({"account":"9999999999","password":"bad"})",
                          "application/json");
  check(blocked && blocked->status == 429,
        "不同转发头共用同一来源限速：第 6 次 429");

  auto plain = post_json(cli, "/api/login",
                         R"({"account":"9999999999","password":"bad"})");
  check(plain && plain->status == 429,
        "不携带转发头同样按 remote_addr 受限 429");
}

void test_rate_limit_success_clears() {
  std::cout << "成功登录清除该来源的失败计数\n";
  Env env("m51_clear");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  int status = 0;
  json reg = register_user(cli, "clearuser", "ClearPw1", &status);
  std::string account = reg.value("account", "");
  check(status == 201 && !account.empty(), "注册成功");

  // 先累计 4 次失败（未达阈值）。
  for (int i = 0; i < 4; ++i) {
    int s = 0;
    login(cli, account, "WrongPw1", s);
    check(s == 401, "成功前第 " + std::to_string(i + 1) + " 次失败 401");
  }
  // 成功登录：应清空失败计数。
  int ok_status = 0;
  json ok_body = login(cli, account, "ClearPw1", ok_status);
  check(ok_status == 200 && !ok_body.value("token", "").empty(),
        "成功登录 200 并返回 token");

  // 若计数未清空，此时累计已达 5 次，下一次即会被限速；清空后仍可再失败 5 次。
  for (int i = 0; i < 5; ++i) {
    int s = 0;
    login(cli, account, "WrongPw1", s);
    check(s == 401,
          "成功后第 " + std::to_string(i + 1) + " 次失败仍 401（计数已清空）");
  }
  int s6 = 0;
  login(cli, account, "WrongPw1", s6);
  check(s6 == 429, "重新累计满 5 次后第 6 次 429");
}

void test_rate_limit_concurrency_cannot_bypass() {
  std::cout << "受控并发失败登录不能突破阈值\n";
  Env env("m51_conc");
  check(env.ok(), "服务启动成功");

  constexpr int kThreads = 20;
  std::atomic<int> c401{0};
  std::atomic<int> c429{0};
  std::atomic<int> other{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&]() {
      httplib::Client cli("127.0.0.1", env.port());
      auto res = post_json(cli, "/api/login",
                           R"({"account":"9999999999","password":"bad"})");
      int s = res ? res->status : -1;
      if (s == 401) {
        ++c401;
      } else if (s == 429) {
        ++c429;
      } else {
        ++other;
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  check(c401.load() == 5, "恰好 5 次失败被放行 401（实际 " +
                              std::to_string(c401.load()) + "）");
  check(c429.load() == kThreads - 5,
        "其余均被限速 429（实际 " + std::to_string(c429.load()) + "）");
  check(other.load() == 0, "无其它异常状态");
}

void test_rate_limit_window_boundary() {
  std::cout << "限速窗口边界：15 分钟内受限，超过后解除\n";
  Env env("m51_window");
  check(env.ok(), "服务启动成功");

  struct FakeClock {
    std::atomic<long long> seconds{0};
  } fc;
  env.server().rate_limiter().set_clock([&fc]() {
    return std::chrono::steady_clock::time_point(
        std::chrono::seconds(fc.seconds.load()));
  });

  httplib::Client cli("127.0.0.1", env.port());
  for (int i = 0; i < 5; ++i) {
    int s = 0;
    login(cli, "9999999999", "bad", s);
    check(s == 401, "窗口内第 " + std::to_string(i + 1) + " 次失败 401");
  }

  fc.seconds = 900; // 恰好到达窗口边界（自最早失败起 15 分钟）
  int at_boundary = 0;
  login(cli, "9999999999", "bad", at_boundary);
  check(at_boundary == 429, "恰好 15 分钟（900s）时仍受限 429");

  fc.seconds = 901; // 超过窗口
  int after = 0;
  login(cli, "9999999999", "bad", after);
  check(after == 401, "超过窗口后解除限制并重新计数 401");
}

// ---------------------------------------------------------------------------
// 注册输入边界：昵称规范化 / 密码不裁剪 / 长度单位
// ---------------------------------------------------------------------------

void test_nickname_normalization_uniqueness() {
  std::cout << "昵称规范化唯一性与内部空白\n";
  Env env("m51_norm");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  int s1 = 0;
  json first = register_user(cli, "  alice  ", "Pw1", &s1);
  check(s1 == 201 && first.value("nickname", "") == "alice",
        "首尾空白被去除后注册成功且返回规范化昵称");

  int s2 = 0;
  register_user(cli, "alice", "Pw2", &s2);
  check(s2 == 409, "规范化后与已有昵称冲突返回 409");
  check(count_rows(env.db(),
                   "SELECT COUNT(*) FROM users WHERE nickname = ?", "alice") ==
            1,
        "数据库仅一条规范化昵称记录");

  int si1 = 0, si2 = 0;
  register_user(cli, "a b", "Pw3", &si1);
  register_user(cli, "a  b", "Pw4", &si2);
  check(si1 == 201 && si2 == 201,
        "内部单/双空白保留，视为不同昵称均可注册");
}

void test_password_not_trimmed_end_to_end() {
  std::cout << "密码不裁剪不截断（注册→登录链路）\n";
  Env env("m51_pw");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  int s = 0;
  json reg = register_user(cli, "spaced", "  spaced pw  ", &s);
  std::string account = reg.value("account", "");
  check(s == 201 && !account.empty(), "含首尾空白的密码注册成功");

  int ok = 0;
  login(cli, account, "  spaced pw  ", ok);
  check(ok == 200, "原样密码登录成功（未被裁剪）");

  int trimmed = 0;
  login(cli, account, "spaced pw", trimmed);
  check(trimmed == 401, "被裁剪后的密码登录失败（内容未被静默修改）");

  int sp = 0;
  json space_reg = register_user(cli, "onlyspace", "   ", &sp);
  std::string space_account = space_reg.value("account", "");
  check(sp == 201 && !space_account.empty(), "纯空白密码允许注册");
  int sp_ok = 0;
  login(cli, space_account, "   ", sp_ok);
  check(sp_ok == 200, "原样纯空白密码登录成功");
  int sp_bad = 0;
  login(cli, space_account, "  ", sp_bad);
  check(sp_bad == 401, "少一个空格的密码登录失败");
}

void test_nickname_length_bytes() {
  std::cout << "昵称长度按字节计（30 接受 / 31 拒绝）\n";
  Env env("m51_len");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  int s30 = 0;
  register_user(cli, std::string(30, 'a'), "Pw1", &s30);
  check(s30 == 201, "30 字节昵称接受");

  int s31 = 0;
  register_user(cli, std::string(31, 'a'), "Pw1", &s31);
  check(s31 == 400, "31 字节昵称拒绝");

  // UTF-8 下 "中" 为 3 字节：10 个 = 30 字节（接受），11 个 = 33 字节（拒绝）。
  std::string cn10;
  for (int i = 0; i < 10; ++i) {
    cn10 += "\xe4\xb8\xad";
  }
  std::string cn11 = cn10 + "\xE4\xB8\xAD";
  check(cn10.size() == 30 && cn11.size() == 33, "多字节样例长度前置校验");
  int c10 = 0, c11 = 0;
  register_user(cli, cn10, "Pw1", &c10);
  register_user(cli, cn11, "Pw1", &c11);
  check(c10 == 201, "10 个多字节字符（30 字节）接受");
  check(c11 == 400, "11 个多字节字符（33 字节）拒绝（按字节计）");
}

// ---------------------------------------------------------------------------
// 账号与昵称唯一性 / 永久不复用
// ---------------------------------------------------------------------------

void test_concurrent_distinct_nickname_registration() {
  std::cout << "并发注册不同昵称：全部成功且账号唯一\n";
  Env env("m51_distinct");
  check(env.ok(), "服务启动成功");

  constexpr int kThreads = 6;
  std::vector<std::string> accounts(kThreads);
  std::atomic<int> success{0};
  std::atomic<int> other{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      httplib::Client cli("127.0.0.1", env.port());
      int s = 0;
      json body = register_user(cli, "conc_" + std::to_string(i),
                                "Pw" + std::to_string(i) + "x", &s);
      if (s == 201) {
        accounts[i] = body.value("account", "");
        ++success;
      } else {
        ++other;
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  check(success.load() == kThreads, "全部注册成功");
  check(other.load() == 0, "无失败/异常状态");

  std::set<std::string> unique;
  bool all_format_ok = true;
  for (const auto &a : accounts) {
    if (!is_10_digits(a)) {
      all_format_ok = false;
    }
    unique.insert(a);
  }
  check(all_format_ok, "每个账号均为 10 位纯数字");
  check(unique.size() == kThreads, "并发分配账号互不相同");

  check(count_rows(env.db(),
                   "SELECT COUNT(*) FROM users WHERE nickname LIKE ?",
                   "conc%") == kThreads,
        "数据库记录数等于成功数，无部分记录");
}

void test_permanent_non_reuse() {
  std::cout << "账号永久不复用：唯一约束 + 重启后保留\n";
  TempDir dir("m51_reuse");
  std::string dbpath = dir.db_path();
  std::string account;
  const std::string password = "UniqPw1";

  {
    Env env("m51_reuse_1", dbpath);
    check(env.ok(), "首次启动成功");
    httplib::Client cli("127.0.0.1", env.port());
    int s = 0;
    json reg = register_user(cli, "uniq", password, &s);
    account = reg.value("account", "");
    check(s == 201 && is_10_digits(account), "注册成功并分配 10 位账号");

    // 直接以相同 account 再插入：必须被数据库 UNIQUE 约束拒绝。
    std::string err;
    oj::Statement stmt;
    check(env.db().prepare(
              "INSERT INTO users (account, nickname, password_hash, role, "
              "reset_pwd_flag) VALUES (?, ?, 'x', 'user', 0)",
              stmt, err),
          "构造重复账号插入语句");
    stmt.bind(1, account);
    stmt.bind(2, "dupe_nick");
    int rc = stmt.step();
    // 连接启用了扩展结果码，唯一约束冲突返回 SQLITE_CONSTRAINT_UNIQUE。
    check(rc == SQLITE_CONSTRAINT_UNIQUE || rc == SQLITE_CONSTRAINT,
          "数据库 account 唯一约束拒绝重复账号（rc=" +
              std::to_string(rc) + "）");

    check(count_rows(env.db(),
                     "SELECT COUNT(*) FROM users WHERE account = ?", account) ==
              1,
          "该账号仅一条记录");
    env.stop();
    env.close_db();
  }

  {
    Env env("m51_reuse_2", dbpath);
    check(env.ok(), "同库重启成功");
    httplib::Client cli("127.0.0.1", env.port());

    int s = 0;
    login(cli, account, password, s);
    check(s == 200, "重启后原账号仍可登录，未被重新分配");

    int s2 = 0;
    json reg2 = register_user(cli, "uniq2", "UniqPw2", &s2);
    std::string account2 = reg2.value("account", "");
    check(s2 == 201 && is_10_digits(account2), "重启后可继续注册新用户");
    check(account2 != account, "新分配账号不等于已存在账号");
    check(count_rows(env.db(),
                     "SELECT COUNT(*) FROM users WHERE account = ?", account) ==
              1,
          "原有账号记录保持唯一");
    env.stop();
    env.close_db();
  }
}

// ---------------------------------------------------------------------------
// 失败响应不泄露敏感信息
// ---------------------------------------------------------------------------

void test_login_failure_no_leak() {
  std::cout << "登录失败响应不泄露哈希/明文\n";
  Env env("m51_leak");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  int s = 0;
  json reg = register_user(cli, "leak", "LeakPw1", &s);
  std::string account = reg.value("account", "");

  int bad = 0;
  json wrong = login(cli, account, "WrongPw1", bad);
  std::string dump = wrong.dump();
  check(bad == 401, "错误密码 401");
  check(dump.find("$argon2") == std::string::npos &&
            dump.find("argon2") == std::string::npos &&
            dump.find("LeakPw1") == std::string::npos &&
            dump.find("WrongPw1") == std::string::npos,
        "响应不含哈希/明文密码");

  int missing = 0;
  json no_user = login(cli, "9999999999", "Whatever1", missing);
  check(missing == 401, "不存在账号 401");
  check(no_user.value("error", "") == wrong.value("error", ""),
        "不存在账号与错误密码提示一致");
}

} // namespace

int main() {
  test_rate_limit_ignores_forwarded_headers();
  test_rate_limit_success_clears();
  test_rate_limit_concurrency_cannot_bypass();
  test_rate_limit_window_boundary();
  test_nickname_normalization_uniqueness();
  test_password_not_trimmed_end_to_end();
  test_nickname_length_bytes();
  test_concurrent_distinct_nickname_registration();
  test_permanent_non_reuse();
  test_login_failure_no_leak();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部 M5.1 账号与认证回归测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
