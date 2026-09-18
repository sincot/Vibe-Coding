// 注册接口与数据库集成测试（M1.1）。
//
// 通过 httplib::Client 访问真实运行的 HTTP 服务（隔离端口 + /tmp 临时库），
// 覆盖：注册成功、密码哈希、重复昵称、并发同昵称、非法输入、越权字段、内部故障
// 不泄露细节、持久化等。不触碰正式数据库 data/oj.db。
//
// 运行方式：ctest --test-dir build -R register_api --output-on-failure
// 或直接执行 build/oj_register_api_test。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
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

class TempDir {
public:
  explicit TempDir(const std::string &label) {
    std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
      auto candidate =
          base / (label + "_" + std::to_string(::getpid()) + "_" +
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

// 测试环境：隔离临时库 + 随机端口上的真实 HTTP 服务。
class Env {
public:
  explicit Env(const std::string &label) : dir_(label) {
    std::string err;
    db_ = oj::Database::open(dir_.db_path(), err);
    if (!db_) {
      return;
    }
    if (!oj::initialize_schema(*db_, std::string("AdminSecret123!"), err)) {
      return;
    }
    port_ = find_free_port();
    server_ = std::make_unique<oj::HttpServer>("127.0.0.1", port_, *db_);
    if (!server_->start(err)) {
      return;
    }
    started_ = true;
    // 等待监听线程进入 accept 循环，避免首个请求竞态。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  bool ok() const { return started_; }
  int port() const { return port_; }
  std::string db_path() const { return dir_.db_path(); }

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
  std::unique_ptr<oj::Database> db_;
  std::unique_ptr<oj::HttpServer> server_;
  int port_ = 0;
  bool started_ = false;
};

httplib::Result post_json(httplib::Client &cli, const std::string &path,
                          const std::string &body) {
  return cli.Post(path.c_str(), body, "application/json");
}

int count_users(oj::Database &db, const std::string &nickname) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT COUNT(*) FROM users WHERE nickname = ?", stmt, err)) {
    return -1;
  }
  stmt.bind(1, nickname);
  if (stmt.step() != SQLITE_ROW) {
    return -1;
  }
  return stmt.column_int(0);
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

void test_register_success() {
  std::cout << "注册成功：201、账号 10 位、哈希可验证、不泄露密码\n";
  Env env("api_success");
  check(env.ok(), "服务启动成功");

  httplib::Client cli("127.0.0.1", env.port());
  auto res = post_json(cli, "/api/register",
                       R"({"nickname":"alice","password":"Secret123"})");
  check(res && res->status == 201, "返回 201");
  if (!res) {
    return;
  }

  json body = json::parse(res->body);
  check(is_10_digits(body.value("account", "")), "账号为 10 位纯数字");
  check(body.value("nickname", "") == "alice", "昵称正确");
  check(body.value("role", "") == "user", "角色为普通用户");
  check(body.contains("id"), "返回用户 id");
  check(!body.contains("password") && !body.contains("password_hash"),
        "响应不含密码或哈希");

  // 数据库记录一致，且哈希可验证。
  std::string err;
  oj::Statement stmt;
  env.db().prepare(
      "SELECT password_hash FROM users WHERE nickname = 'alice'", stmt, err);
  bool has_row = (stmt.step() == SQLITE_ROW);
  check(has_row, "数据库存在 alice 记录");
  if (has_row) {
    std::string hash = stmt.column_text(0);
    check(hash.find("$argon2id$") == 0, "保存的是 argon2id 哈希");
    std::string verr;
    check(oj::auth::verify_password(hash, "Secret123", verr), "正确密码验证通过");
    check(!oj::auth::verify_password(hash, "WrongPass", verr),
          "错误密码验证失败");
  }
}

void test_duplicate_nickname() {
  std::cout << "重复昵称：409，不产生多余记录\n";
  Env env("api_dup");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  auto first = post_json(cli, "/api/register",
                         R"({"nickname":"bob","password":"Pw123"})");
  auto second = post_json(cli, "/api/register",
                          R"({"nickname":"bob","password":"Another1"})");
  check(first && first->status == 201, "首次注册 201");
  check(second && second->status == 409, "重复昵称 409");
  check(!second->body.empty() && json::parse(second->body).contains("error"),
        "错误响应含 error 字段");
  check(count_users(env.db(), "bob") == 1, "数据库仅一条 bob 记录");
}

void test_missing_or_wrong_type() {
  std::cout << "缺少字段 / 类型错误：400\n";
  Env env("api_type");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  check(post_json(cli, "/api/register", "{}")->status == 400, "空对象 400");
  check(post_json(cli, "/api/register", R"({"nickname":"x"})")->status == 400,
        "缺 password 400");
  check(post_json(cli, "/api/register", R"({"password":"x"})")->status == 400,
        "缺 nickname 400");
  check(post_json(cli, "/api/register",
                  R"({"nickname":123,"password":"x"})")
            ->status == 400,
        "nickname 非字符串 400");
  check(post_json(cli, "/api/register",
                  R"({"nickname":"x","password":123})")
            ->status == 400,
        "password 非字符串 400");
  check(post_json(cli, "/api/register",
                  R"({"nickname":null,"password":"x"})")
            ->status == 400,
        "nickname 为 null 400");
}

void test_invalid_json() {
  std::cout << "非法 JSON：400\n";
  Env env("api_badjson");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  check(post_json(cli, "/api/register", "not a json")->status == 400,
        "非法 JSON 400");
  check(post_json(cli, "/api/register", "[1,2,3]")->status == 400,
        "JSON 数组 400");
}

void test_empty_and_too_long() {
  std::cout << "空昵称 / 超长输入：400\n";
  Env env("api_len");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  check(post_json(cli, "/api/register",
                  R"({"nickname":"","password":"Pw1"})")
            ->status == 400,
        "空昵称 400");
  check(post_json(cli, "/api/register",
                  R"({"nickname":"   ","password":"Pw1"})")
            ->status == 400,
        "纯空白昵称 400");
  check(post_json(cli, "/api/register",
                  R"({"nickname":"aaaaa","password":""})")
            ->status == 400,
        "空密码 400");

  std::string long_nick = "\"" + std::string(31, 'a') + "\"";
  check(post_json(cli, "/api/register",
                  "{\"nickname\":" + long_nick + ",\"password\":\"Pw1\"}")
            ->status == 400,
        "超长昵称 400");
  std::string long_pw = "\"" + std::string(129, 'p') + "\"";
  check(post_json(cli, "/api/register",
                  "{\"nickname\":\"ok\",\"password\":" + long_pw + "}")
            ->status == 400,
        "超长密码 400");
}

void test_extra_fields_cannot_elevate() {
  std::cout << "客户端传 role/account 不能提权或指定账号\n";
  Env env("api_extra");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  auto res = post_json(cli, "/api/register",
                       R"({"nickname":"carol","password":"Pw1","role":"admin","account":"0000000001"})");
  check(res && res->status == 201, "携带额外字段仍注册成功");
  if (!res) {
    return;
  }
  json body = json::parse(res->body);
  check(body.value("role", "") == "user", "响应角色仍为 user");
  check(is_10_digits(body.value("account", "")), "账号仍为后端生成");

  std::string err;
  oj::Statement stmt;
  env.db().prepare(
      "SELECT role FROM users WHERE nickname = 'carol'", stmt, err);
  bool has_row = (stmt.step() == SQLITE_ROW);
  check(has_row && stmt.column_text(0) == "user", "数据库角色为 user");
}

void test_internal_error_no_leak() {
  std::cout << "内部故障：500 且不泄露数据库细节\n";
  Env env("api_500");
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  // 关闭数据库后注册，模拟内部故障。
  env.close_db();
  auto res = post_json(cli, "/api/register",
                       R"({"nickname":"dave","password":"Pw1"})");
  check(res && res->status == 500, "返回 500");
  if (!res) {
    return;
  }
  std::string body = res->body;
  check(body.find("error") != std::string::npos, "错误响应含 error 字段");
  std::string lower = body;
  for (char &c : lower) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  check(lower.find("sqlite") == std::string::npos &&
            lower.find("database") == std::string::npos &&
            lower.find("argon") == std::string::npos,
        "响应不泄露数据库/哈希内部细节");
}

void test_concurrent_same_nickname() {
  std::cout << "并发注册同一昵称：最多一个成功\n";
  Env env("api_race");
  check(env.ok(), "服务启动成功");

  const int kThreads = 8;
  std::atomic<int> success{0};
  std::atomic<int> conflict{0};
  std::atomic<int> other{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      httplib::Client cli("127.0.0.1", env.port());
      std::string body =
          "{\"nickname\":\"raceuser\",\"password\":\"Pw" +
          std::to_string(i) + "\"}";
      auto res = post_json(cli, "/api/register", body);
      if (res && res->status == 201) {
        ++success;
      } else if (res && res->status == 409) {
        ++conflict;
      } else {
        ++other;
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  check(success.load() == 1, "恰好一个成功");
  check(conflict.load() == kThreads - 1, "其余均为 409 昵称冲突");
  check(other.load() == 0, "无其它异常状态");
  check(count_users(env.db(), "raceuser") == 1, "数据库仅一条 raceuser 记录");
}

void test_persistence() {
  std::cout << "关闭并重新打开数据库后用户保留\n";
  Env env("api_persist");
  check(env.ok(), "服务启动成功");
  std::string dbpath = env.db_path();

  std::string account;
  httplib::Client cli("127.0.0.1", env.port());
  auto res = post_json(cli, "/api/register",
                       R"({"nickname":"persist","password":"Pw123"})");
  check(res && res->status == 201, "注册成功");
  account = json::parse(res->body).value("account", "");

  // 停止服务并关闭连接（临时目录在 Env 析构前仍存活，文件不会被删除）。
  env.stop();
  env.close_db();

  std::string err;
  auto db = oj::Database::open(dbpath, err);
  check(db != nullptr, "重新打开成功");
  oj::UserStore store(*db);
  bool found = false;
  oj::UserRecord rec;
  check(store.find_by_account(account, found, rec, err) && found &&
            rec.nickname == "persist",
        "用户记录与账号仍保留");
}

} // namespace

int main() {
  test_register_success();
  test_duplicate_nickname();
  test_missing_or_wrong_type();
  test_invalid_json();
  test_empty_and_too_long();
  test_extra_fields_cannot_elevate();
  test_internal_error_no_leak();
  test_concurrent_same_nickname();
  test_persistence();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
