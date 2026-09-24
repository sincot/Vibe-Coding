// M4.5 排行榜 HTTP 接口集成测试。
//
// 使用 /tmp 下的隔离临时库与随机端口上的真实 HTTP 服务，不触碰正式数据库；不启动
// 判题（统计状态按数据库契约写入），聚焦 GET /api/leaderboard 的公开访问、统计聚合、
// 排序、分页/参数校验、字段范围与 Rejudge 重算联动。
//
// 覆盖：
//   - 游客（无 token / 无效 token）可直接访问，空库返回空榜；
//   - 排序 AC 数↓→提交次数↑→首次AC时间↑→注册时间↑→ID↑ 与全局名次 rank；
//   - 分页 20/页、total/total_pages、越界页、page 参数校验（0/非数字/超上限）；
//   - 名次不按页重置（第 2 页从 21 开始）；
//   - 隐藏题目排除、仅统计有已结算提交的用户、管理员同口径参与；
//   - 不返回账号/密码哈希/token/源码/逐点结果；
//   - 模拟 Rejudge 重算（UserProblemStatusStore::recompute）后统计与排序联动。
//
// 运行方式：ctest --test-dir build -R m45_leaderboard_api --output-on-failure
// 或直接执行 build/oj_m45_leaderboard_api_test。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
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
#include "db/submissions.h"
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

const std::string kTestSecret = "it-m45-secret-0123456789abcdef";
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

httplib::Client make_client(int port) {
  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(10, 0);
  cli.set_read_timeout(60, 0);
  cli.set_write_timeout(60, 0);
  return cli;
}

// 每个场景一个隔离库 + 随机端口服务。
class Env {
public:
  explicit Env(const std::string &label) : dir_(label) {
    std::string err;
    db_ = oj::Database::open(dir_.db_path(), err);
    if (!db_) return;
    if (!oj::initialize_schema(*db_, kAdminPassword, err)) return;
    port_ = find_free_port();
    server_ = std::make_unique<oj::HttpServer>("127.0.0.1", port_, *db_,
                                               make_config(),
                                               /*enable_test_routes=*/false);
    if (!server_->start(err)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    started_ = true;
  }
  ~Env() {
    if (server_) server_->stop();
  }
  bool ok() const { return started_; }
  int port() const { return port_; }
  oj::Database &db() { return *db_; }

private:
  TempDir dir_;
  std::unique_ptr<oj::Database> db_;
  std::unique_ptr<oj::HttpServer> server_;
  int port_ = 0;
  bool started_ = false;
};

std::string ten_digit(long long n) {
  std::string s = std::to_string(n);
  while (s.size() < 10) s = "0" + s;
  return s;
}

std::int64_t insert_user(oj::Database &db, const std::string &account,
                         const std::string &nickname, const std::string &role,
                         const std::string &created_at) {
  oj::Statement stmt;
  std::string err;
  if (!db.prepare("INSERT INTO users (account, nickname, password_hash, role, "
                  "created_at) VALUES (?, ?, 'x', ?, ?) RETURNING id",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, account);
  stmt.bind(2, nickname);
  stmt.bind(3, role);
  stmt.bind(4, created_at);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t find_user_by_account(oj::Database &db,
                                  const std::string &account) {
  oj::Statement stmt;
  std::string err;
  if (!db.prepare("SELECT id FROM users WHERE account = ?", stmt, err)) {
    return -1;
  }
  stmt.bind(1, account);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t insert_problem(oj::Database &db, const std::string &title,
                            int visible) {
  oj::Statement stmt;
  std::string err;
  if (!db.prepare("INSERT INTO problems (title, difficulty, visible) VALUES "
                  "(?, 'easy', ?) RETURNING id",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, title);
  stmt.bind(2, visible);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

void set_status(oj::Database &db, std::int64_t user_id, std::int64_t problem_id,
                bool accepted, bool has_first_ac,
                const std::string &first_ac_at, int submit_count) {
  oj::UserProblemStatusStore store(db);
  std::string err;
  store.upsert(user_id, problem_id, accepted, has_first_ac, first_ac_at,
               submit_count, err);
}

std::int64_t insert_submission(oj::Database &db, std::int64_t user_id,
                               std::int64_t problem_id, const std::string &status,
                               const std::string &created_at) {
  oj::SubmissionStore store(db);
  oj::SubmissionRecord record;
  record.user_id = user_id;
  record.problem_id = problem_id;
  record.language = "cpp17";
  record.source_code = "int main(){}";
  record.status = status;
  record.per_case = "[]";
  record.created_at = created_at;
  std::int64_t id = 0;
  std::string err;
  if (!store.insert(record, id, err)) return -1;
  return id;
}

json body_of(const httplib::Result &res) {
  return res && !res->body.empty() ? json::parse(res->body) : json();
}

const json *find_entry(const json &list, std::int64_t user_id) {
  for (const auto &item : list) {
    if (item.value("user_id", static_cast<std::int64_t>(-1)) == user_id) {
      return &item;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// T-011：公开访问 + 空库
// ---------------------------------------------------------------------------

void test_public_and_empty() {
  std::cout << "\n== M45-1 公开访问与空库 ==\n";
  Env env("m45_pub");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  httplib::Client cli = make_client(env.port());

  {
    auto res = cli.Get("/api/leaderboard");
    check(res && res->status == 200, "游客（无 token）访问返回 200");
    json body = body_of(res);
    check(body.contains("leaderboard") && body["leaderboard"].is_array() &&
              body["leaderboard"].empty(),
          "空库 leaderboard 为空数组");
    check(body.value("page", 0) == 1 && body.value("page_size", 0) == 20 &&
              body.value("total", -1) == 0 && body.value("total_pages", -1) == 0,
          "空库分页元数据 page=1/page_size=20/total=0/total_pages=0");
  }
  {
    httplib::Headers h{{"Authorization", "Bearer not-a-valid-token"}};
    auto res = cli.Get("/api/leaderboard", h);
    check(res && res->status == 200,
          "无效 token 仍按公开接口返回 200（不误报 401）");
  }
}

// ---------------------------------------------------------------------------
// T-012 / T-014：排序、全局名次、字段范围
// ---------------------------------------------------------------------------

void test_ordering_and_privacy() {
  std::cout << "\n== M45-2 排序/名次/字段范围 ==\n";
  Env env("m45_order");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  httplib::Client cli = make_client(env.port());

  const std::int64_t p1 = insert_problem(env.db(), "M45 题一", 1);
  const std::int64_t p2 = insert_problem(env.db(), "M45 题二", 1);
  check(p1 > 0 && p2 > 0, "插入可见题");

  const std::int64_t alice = insert_user(env.db(), ten_digit(1000000001),
                                         "alice", "user",
                                         "2026-01-01 00:00:00");
  const std::int64_t bob = insert_user(env.db(), ten_digit(1000000002), "bob",
                                       "user", "2026-01-01 00:00:00");
  const std::int64_t carol = insert_user(env.db(), ten_digit(1000000003),
                                         "carol", "user",
                                         "2026-01-01 00:00:00");
  check(alice > 0 && bob > 0 && carol > 0, "插入三个用户");

  // alice：ac=2，提交 3+2=5，最早首次 AC 2026-02-01。
  set_status(env.db(), alice, p1, true, true, "2026-02-01 00:00:00", 3);
  set_status(env.db(), alice, p2, true, true, "2026-02-03 00:00:00", 2);
  // bob：ac=2，提交 3+2=5，首次 AC 更晚。
  set_status(env.db(), bob, p1, true, true, "2026-02-02 00:00:00", 3);
  set_status(env.db(), bob, p2, true, true, "2026-02-03 00:00:00", 2);
  // carol：无 AC，提交 1。
  set_status(env.db(), carol, p1, false, false, "", 1);

  auto res = cli.Get("/api/leaderboard");
  check(res && res->status == 200, "排行榜 200");
  json body = body_of(res);
  const auto &list = body["leaderboard"];
  check(list.is_array() && list.size() == 3u, "返回 3 名用户");
  check(body.value("total", 0) == 3 && body.value("total_pages", 0) == 1,
        "total=3/total_pages=1");

  if (list.size() == 3) {
    check(list[0].value("user_id", -1) == alice &&
              list[0].value("rank", 0) == 1,
          "第 1 名 alice（首次 AC 更早）");
    check(list[1].value("user_id", -1) == bob &&
              list[1].value("rank", 0) == 2,
          "第 2 名 bob");
    check(list[2].value("user_id", -1) == carol &&
              list[2].value("rank", 0) == 3,
          "第 3 名 carol（无 AC 排最后）");
    check(list[0].value("nickname", "") == "alice" &&
              list[0].value("ac_count", 0) == 2 &&
              list[0].value("submit_count", 0) == 5,
          "第 1 名字段 ac_count=2/submit_count=5/nickname");
    check(list[0].value("first_ac_at", "") == "2026-02-01 00:00:00",
          "首次 AC 时间取最早 accepted");
    check(list[2]["first_ac_at"].is_null(), "无 AC 用户 first_ac_at 为 null");
    check(list[0].contains("created_at") &&
              list[0].value("created_at", "") == "2026-01-01 00:00:00",
          "返回注册时间（created_at）");
  }

  const std::string raw = body.dump();
  check(raw.find("password_hash") == std::string::npos, "不含 password_hash");
  check(raw.find("source_code") == std::string::npos, "不含 source_code");
  check(raw.find("per_case") == std::string::npos, "不含 per_case");
  check(raw.find("\"results\"") == std::string::npos, "不含逐点结果");
  check(raw.find("token") == std::string::npos, "不含 token");
  check(raw.find(ten_digit(1000000001)) == std::string::npos &&
            raw.find(ten_digit(1000000002)) == std::string::npos,
        "不返回登录账号");
}

// ---------------------------------------------------------------------------
// T-013：分页与参数校验
// ---------------------------------------------------------------------------

void test_pagination_and_params() {
  std::cout << "\n== M45-3 分页与参数校验 ==\n";
  Env env("m45_page");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  httplib::Client cli = make_client(env.port());

  const std::int64_t p1 = insert_problem(env.db(), "M45 分页题", 1);
  check(p1 > 0, "插入可见题");

  std::vector<std::int64_t> ids;
  for (int i = 0; i < 25; ++i) {
    // 注册时间递增，保证稳定顺序 u0..u24。
    const std::string created =
        "2026-03-" + (i < 9 ? std::string("0") : "") + std::to_string(i + 1) +
        " 00:00:00";
    const std::int64_t u = insert_user(
        env.db(), ten_digit(1000000000 + i), "page_user_" + std::to_string(i),
        "user", created);
    ids.push_back(u);
    set_status(env.db(), u, p1, false, false, "", 1);
  }

  {
    auto res = cli.Get("/api/leaderboard");
    json body = body_of(res);
    check(res && res->status == 200 && body["leaderboard"].size() == 20u &&
              body.value("total", 0) == 25 && body.value("total_pages", 0) == 2,
          "第 1 页 20 条，total=25，total_pages=2");
    check(body["leaderboard"][0].value("rank", 0) == 1 &&
              body["leaderboard"][19].value("rank", 0) == 20,
          "第 1 页 rank 1..20");
    check(body["leaderboard"][0].value("user_id", -1) == ids[0],
          "第 1 页首条为最早注册用户");
  }
  {
    auto res = cli.Get("/api/leaderboard?page=2");
    json body = body_of(res);
    check(res && res->status == 200 && body["leaderboard"].size() == 5u &&
              body.value("page", 0) == 2,
          "第 2 页 5 条");
    check(body["leaderboard"][0].value("rank", 0) == 21 &&
              body["leaderboard"][4].value("rank", 0) == 25,
          "名次全局连续，不按页重置（21..25）");
    check(body["leaderboard"][0].value("user_id", -1) == ids[20],
          "第 2 页首条为第 21 个用户");
  }
  {
    auto zero = cli.Get("/api/leaderboard?page=0");
    check(zero && zero->status == 400, "page=0 返回 400");
    auto alpha = cli.Get("/api/leaderboard?page=abc");
    check(alpha && alpha->status == 400, "page=abc 返回 400");
    auto neg = cli.Get("/api/leaderboard?page=-1");
    check(neg && neg->status == 400, "page=-1 返回 400");
    auto big = cli.Get("/api/leaderboard?page=1000001");
    check(big && big->status == 400, "page 超上限返回 400");
    auto beyond = cli.Get("/api/leaderboard?page=999");
    json bb = body_of(beyond);
    check(beyond && beyond->status == 200 && bb["leaderboard"].empty() &&
              bb.value("total", 0) == 25,
          "越界页返回空列表且 total 不变");
  }
}

// ---------------------------------------------------------------------------
// T-015：隐藏题排除、已结算用户、管理员，及 Rejudge 重算联动
// ---------------------------------------------------------------------------

void test_hidden_admin_and_rejudge() {
  std::cout << "\n== M45-4 隐藏题/管理员/Rejudge 联动 ==\n";
  Env env("m45_hidrej");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  httplib::Client cli = make_client(env.port());

  const std::int64_t p_vis = insert_problem(env.db(), "M45 可见题", 1);
  const std::int64_t p_hid = insert_problem(env.db(), "M45 隐藏题", 0);
  check(p_vis > 0 && p_hid > 0, "插入可见题与隐藏题");

  const std::int64_t hidden_only = insert_user(
      env.db(), ten_digit(1000000001), "hidden_only", "user",
      "2026-01-01 00:00:00");
  const std::int64_t mixed = insert_user(env.db(), ten_digit(1000000002),
                                         "mixed", "user",
                                         "2026-01-01 00:00:00");
  const std::int64_t never = insert_user(env.db(), ten_digit(1000000003),
                                         "never", "user",
                                         "2026-01-01 00:00:00");
  // 预置 admin 由 initialize_schema 创建，直接复用其 ID（不重复插入）。
  const std::int64_t admin = find_user_by_account(env.db(), "admin");
  (void)never;
  check(admin > 0, "获取预置管理员 ID");

  set_status(env.db(), hidden_only, p_hid, true, true, "2026-04-01 00:00:00", 5);
  set_status(env.db(), mixed, p_vis, true, true, "2026-04-02 00:00:00", 2);
  set_status(env.db(), mixed, p_hid, true, true, "2026-04-03 00:00:00", 9);
  set_status(env.db(), admin, p_vis, true, true, "2026-04-04 00:00:00", 1);

  {
    auto res = cli.Get("/api/leaderboard");
    json body = body_of(res);
    const auto &list = body["leaderboard"];
    check(res && res->status == 200 && body.value("total", 0) == 2,
          "仅统计有已结算可见提交的用户（total=2）");
    check(find_entry(list, hidden_only) == nullptr,
          "仅隐藏题提交的用户不出现");
    check(find_entry(list, never) == nullptr, "从未提交的用户不出现");
    const json *m = find_entry(list, mixed);
    check(m != nullptr && m->value("ac_count", 0) == 1 &&
              m->value("submit_count", 0) == 2,
          "可见+隐藏混合只计可见题（ac=1/submit=2，隐藏 9 次不计）");
    const json *a = find_entry(list, admin);
    check(a != nullptr && a->value("ac_count", 0) == 1,
          "管理员同口径参与排名");
  }

  // Rejudge 联动：唯一 AC 重判为失败 → AC 数清 0、首次 AC 清空、提交次数不变。
  const std::int64_t target = insert_user(env.db(), ten_digit(1000000004),
                                          "rejudge_user", "user",
                                          "2026-01-01 00:00:00");
  const std::int64_t sub = insert_submission(
      env.db(), target, p_vis, "AC", "2026-05-01 10:00:00");
  set_status(env.db(), target, p_vis, true, true, "2026-05-01 10:00:00", 1);

  {
    auto res = cli.Get("/api/leaderboard");
    json body = body_of(res);
    const json *t = find_entry(body["leaderboard"], target);
    check(t != nullptr && t->value("ac_count", 0) == 1 &&
              t->value("first_ac_at", "") == "2026-05-01 10:00:00",
          "重判前 ac=1、首次 AC 正确");
  }
  {
    // 模拟 Rejudge：更新原提交为 WA，并按原记录重算状态（与 rejudge 同一函数）。
    oj::SubmissionStore submissions(env.db());
    bool found = false;
    oj::SubmissionRecord record;
    std::string err;
    check(submissions.find_by_id(sub, found, record, err) && found,
          "读取待重判提交");
    record.status = "WA";
    check(submissions.update(record, err), "更新原提交为 WA");
    oj::UserProblemStatusStore statuses(env.db());
    check(statuses.recompute(target, p_vis, err), "按原记录重算状态");

    auto res = cli.Get("/api/leaderboard");
    json body = body_of(res);
    const json *t = find_entry(body["leaderboard"], target);
    check(t != nullptr && t->value("ac_count", 0) == 0,
          "重判后 ac_count 归零");
    check(t != nullptr && t->value("submit_count", 0) == 1,
          "重判不增加提交次数（仍为 1）");
    check(t != nullptr && (*t)["first_ac_at"].is_null(),
          "重判后首次 AC 清空为 null");
  }
}

} // namespace

int main() {
  std::cout << "M4.5 排行榜接口集成测试\n";
  test_public_and_empty();
  test_ordering_and_privacy();
  test_pagination_and_params();
  test_hidden_admin_and_rejudge();
  std::cout << "\n失败数：" << g_failures << "\n";
  return g_failures == 0 ? 0 : 1;
}
