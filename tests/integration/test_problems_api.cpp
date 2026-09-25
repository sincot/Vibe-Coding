// 题目数据与查询集成测试（M1.4）。
//
// 使用 /tmp 下的隔离临时库与随机端口上的真实 HTTP 服务，不触碰正式数据库。
// 覆盖：
//   - 内置种子题导入：题面/公开样例/隐藏用例保存正确，用例顺序明确
//   - 重复导入幂等：不重复创建、不覆盖已修改的题目
//   - 游客无需登录即可查看可见题目列表与详情
//   - 普通用户只能看可见题目，直接请求隐藏题 ID 也返回 404
//   - 已完成首次改密的管理员可看隐藏题目；未改密管理员不能借此获取隐藏内容
//   - 列表与详情均不返回隐藏用例输入/输出（检查实际 HTTP 响应体）
//   - 空题库返回空列表；不存在/非法 ID 返回正确状态
//   - 无公开样例的可见题详情返回空 samples
//   - 数据库故障（含认证回查）返回 500 且不泄露 SQL/路径
//   - 种子导入中途失败时整题回滚，故障消除后可重新导入
//   - 重启后题目与用例（含隐藏）仍在
//
// 运行方式：ctest --test-dir build -R problems_api --output-on-failure
// 或直接执行 build/oj_problems_api_test。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "auth/jwt.h"
#include "db/database.h"
#include "db/problems.h"
#include "db/schema.h"
#include "db/seed.h"
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
constexpr std::size_t kSeedProblemCount = 41;

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

// 测试环境：隔离临时库 + 随机端口真实 HTTP 服务 + 专用测试密钥。
// seed 为 true 时在启动前导入内置种子题。
class Env {
public:
  explicit Env(const std::string &label, bool seed = false,
               const std::string &db_path = "")
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
    if (seed) {
      int created = 0;
      if (!oj::import_seed_problems(*db_, created, err)) {
        return;
      }
    }
    port_ = find_free_port();
    server_ = std::make_unique<oj::HttpServer>("127.0.0.1", port_, *db_,
                                               make_config(),
                                               /*enable_test_routes=*/false);
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
// 测试辅助
// ---------------------------------------------------------------------------

std::int64_t seed_problem_id(oj::Database &db, const std::string &seed_key) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT id FROM problems WHERE seed_key = ?", stmt, err)) {
    return -1;
  }
  stmt.bind(1, seed_key);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t count_rows(oj::Database &db, const std::string &table) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT COUNT(*) FROM " + table, stmt, err)) {
    return -1;
  }
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

// 直接写入一道自定义题目（用于构造隐藏题），返回题目 ID。
std::int64_t insert_problem(oj::Database &db, const std::string &title,
                            const std::string &description, int visible) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("INSERT INTO problems (title, description, difficulty, tags, "
                  "visible) VALUES (?, ?, 'easy', '测试', ?) RETURNING id",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, title);
  stmt.bind(2, description);
  stmt.bind(3, visible);
  if (stmt.step() != SQLITE_ROW) {
    return -1;
  }
  return stmt.column_int64(0);
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

// 读取某题隐藏用例中的「足够长」的输入/输出串，用于验证响应体不泄露它们。
// 过滤短串以免与 id、时限等正常字段产生误判。
std::vector<std::string> hidden_tokens(oj::Database &db,
                                       std::int64_t problem_id) {
  std::vector<std::string> tokens;
  std::string err;
  oj::Statement stmt;
  db.prepare("SELECT input, output FROM testcases WHERE problem_id = ? AND "
             "is_sample = 0",
             stmt, err);
  stmt.bind(1, static_cast<sqlite3_int64>(problem_id));
  while (stmt.step() == SQLITE_ROW) {
    for (int col = 0; col < 2; ++col) {
      std::string value = stmt.column_text(col);
      // 去掉空白后取整体作为标记（保留换行可能影响匹配，统一用去掉换行后的串）。
      std::string compact;
      for (char c : value) {
        if (c != '\n' && c != '\r') {
          compact.push_back(c);
        }
      }
      if (compact.size() >= 6) {
        tokens.push_back(compact);
      }
    }
  }
  return tokens;
}

httplib::Result get_problems(httplib::Client &cli,
                             const std::string &auth = "") {
  if (auth.empty()) {
    return cli.Get("/api/problems");
  }
  httplib::Headers h{{"Authorization", auth}};
  return cli.Get("/api/problems", h);
}

httplib::Result get_problem(httplib::Client &cli, const std::string &id,
                            const std::string &auth = "") {
  if (auth.empty()) {
    return cli.Get(("/api/problems/" + id).c_str());
  }
  httplib::Headers h{{"Authorization", auth}};
  return cli.Get(("/api/problems/" + id).c_str(), h);
}

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
                                const std::string &body) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Post("/api/me/password", h, body, "application/json");
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

std::int64_t user_id(oj::Database &db, const std::string &account) {
  std::string err;
  oj::Statement stmt;
  db.prepare("SELECT id FROM users WHERE account = ?", stmt, err);
  stmt.bind(1, account);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

// ---------------------------------------------------------------------------
// 种子数据内容
// ---------------------------------------------------------------------------

void test_seed_import_content() {
  std::cout << "种子导入：题面/样例/隐藏用例与顺序正确\n";
  Env env("prb_seed", /*seed=*/true);
  check(env.ok(), "服务启动并导入种子成功");

  oj::ProblemStore store(env.db());
  std::vector<oj::ProblemSummary> list;
  std::string err;
  check(store.list(/*include_hidden=*/true, list, err), "列表查询成功");
  check(list.size() == kSeedProblemCount, "导入 41 道种子题");
  check(list.size() == kSeedProblemCount && list[0].id < list[1].id &&
            list[1].id < list[2].id,
        "列表按 id 升序稳定排序");

  std::int64_t ab = seed_problem_id(env.db(), "a-plus-b");
  check(ab > 0, "存在 A+B Problem");

  bool found = false;
  oj::ProblemRecord record;
  check(store.find_by_id(ab, found, record, err), "详情查询成功");
  check(found, "A+B 题目存在");
  check(record.title == "A+B Problem", "标题正确");
  check(record.difficulty == "easy", "难度正确");
  check(record.tags.size() == 2 && record.tags[0] == "入门" &&
            record.tags[1] == "数学",
        "标签解析正确");
  check(record.time_limit_ms == 2000, "时限正确");
  check(record.memory_limit_kb == 65536, "内存限制正确");
  check(record.visible, "种子题默认可见");
  check(record.description.find("输入格式") != std::string::npos,
        "题面为纯文本且含输入说明");

  std::vector<oj::SampleCase> samples;
  check(store.list_samples(ab, samples, err), "样例查询成功");
  check(samples.size() == 2, "公开样例 2 组");
  check(samples.size() == 2 && samples[0].input == "1 2\n" &&
            samples[0].output == "3\n",
        "第 1 组样例正确且顺序明确");
  check(samples.size() == 2 && samples[1].input == "100 -50\n" &&
            samples[1].output == "50\n",
        "第 2 组样例正确");

  std::vector<oj::TestcaseRecord> cases;
  check(store.list_testcases(ab, cases, err), "全部用例查询成功");
  check(cases.size() == 5, "共 5 个用例（2 样例 + 3 隐藏）");
  bool order_ok = cases.size() == 5;
  for (std::size_t i = 0; order_ok && i < cases.size(); ++i) {
    if (cases[i].ord != static_cast<int>(i)) {
      order_ok = false;
    }
  }
  check(order_ok, "用例 ord 按定义顺序连续编号");
  check(cases.size() == 5 && cases[0].is_sample && cases[1].is_sample &&
            !cases[2].is_sample && !cases[3].is_sample && !cases[4].is_sample,
        "样例/隐藏标记正确");
  check(cases.size() == 5 && cases[2].input == "111111111 222222222\n" &&
            cases[2].output == "333333333\n",
        "隐藏用例内容与顺序正确");
}

void test_seed_idempotent() {
  std::cout << "重复导入：不重复创建、不覆盖已修改题目\n";
  Env env("prb_seed_idem", /*seed=*/true);
  check(env.ok(), "服务启动并导入种子成功");

  std::int64_t before = count_rows(env.db(), "problems");
  std::int64_t tc_before = count_rows(env.db(), "testcases");
  check(before == static_cast<std::int64_t>(kSeedProblemCount), "初始 41 道题");

  std::int64_t ab = seed_problem_id(env.db(), "a-plus-b");
  std::string err;
  {
    oj::Statement stmt;
    env.db().prepare("UPDATE problems SET title = 'A+B（已修改）', description "
                     "= 'SENTINEL-DESC' WHERE id = ?",
                     stmt, err);
    stmt.bind(1, static_cast<sqlite3_int64>(ab));
    stmt.step();
  }
  {
    oj::Statement stmt;
    env.db().prepare("UPDATE testcases SET output = 'SENTINEL-OUT' WHERE "
                     "problem_id = ? AND ord = 2",
                     stmt, err);
    stmt.bind(1, static_cast<sqlite3_int64>(ab));
    stmt.step();
  }

  int created = -1;
  check(oj::import_seed_problems(env.db(), created, err), "再次导入成功");
  check(created == 0, "再次导入未新建题目");
  check(count_rows(env.db(), "problems") == before, "题目数量不变");
  check(count_rows(env.db(), "testcases") == tc_before, "用例数量不变");

  oj::ProblemStore store(env.db());
  bool found = false;
  oj::ProblemRecord record;
  store.find_by_id(ab, found, record, err);
  check(found && record.title == "A+B（已修改）", "已修改标题未被覆盖");
  check(found && record.description == "SENTINEL-DESC", "已修改题面未被覆盖");

  oj::Statement stmt;
  env.db().prepare("SELECT output FROM testcases WHERE problem_id = ? AND ord "
                   "= 2",
                   stmt, err);
  stmt.bind(1, static_cast<sqlite3_int64>(ab));
  bool row = stmt.step() == SQLITE_ROW;
  check(row && stmt.column_text(0) == "SENTINEL-OUT", "已修改用例未被覆盖");
}

void test_seed_atomic_rollback() {
  std::cout << "种子导入失败时整题回滚，恢复后可再次导入\n";
  Env env("prb_seed_rollback", /*seed=*/false);
  check(env.ok(), "服务启动成功（未导入）");

  // 用触发器强制用例写入失败，验证「题目 + 用例」在同一事务内整体回滚。
  std::string err;
  check(env.db().exec("CREATE TRIGGER fail_tc BEFORE INSERT ON testcases "
                      "BEGIN SELECT RAISE(FAIL, 'forced'); END;",
                      err),
        "创建强制失败触发器");

  int created = -1;
  bool ok = oj::import_seed_problems(env.db(), created, err);
  check(!ok, "用例写入失败时导入返回失败");
  check(created == 0, "失败时未计入任何新建题目");
  check(count_rows(env.db(), "problems") == 0, "题目已回滚，未留下半道题");
  check(count_rows(env.db(), "testcases") == 0, "用例已回滚");

  // 故障消除后可正常导入（幂等导入不含一次性状态残留）。
  check(env.db().exec("DROP TRIGGER fail_tc;", err), "移除触发器");
  created = -1;
  check(oj::import_seed_problems(env.db(), created, err), "故障消除后导入成功");
  check(created == static_cast<int>(kSeedProblemCount), "恢复后新建 41 道题");
  check(count_rows(env.db(), "problems") ==
            static_cast<std::int64_t>(kSeedProblemCount),
        "最终 41 道题");
}

// ---------------------------------------------------------------------------
// HTTP：游客与普通用户可见性
// ---------------------------------------------------------------------------

void test_guest_list_and_detail() {
  std::cout << "游客无需登录可获取可见题目列表与详情\n";
  Env env("prb_guest", /*seed=*/true);
  check(env.ok(), "服务启动并导入种子成功");
  httplib::Client cli("127.0.0.1", env.port());

  auto list_res = get_problems(cli);
  check(list_res && list_res->status == 200, "游客列表 200");
  json list = json::parse(list_res->body);
  check(list.value("total", -1) == static_cast<int>(kSeedProblemCount),
        "列表 total 为 41");
  check(list["problems"].is_array() && list["problems"].size() == 20,
        "默认分页返回首页 20 条");
  const json &first = list["problems"][0];
  check(first.contains("id") && first.contains("title") &&
            first.contains("difficulty") && first.contains("tags"),
        "列表条目含必要字段");
  check(first["tags"].is_array(), "tags 为数组");
  check(list["problems"][0]["id"].get<long long>() <
            list["problems"][1]["id"].get<long long>(),
        "列表按 id 稳定升序");

  std::int64_t ab = seed_problem_id(env.db(), "a-plus-b");
  auto detail = get_problem(cli, std::to_string(ab));
  check(detail && detail->status == 200, "游客详情 200");
  json d = json::parse(detail->body);
  check(d.value("title", "") == "A+B Problem", "详情标题正确");
  check(d.contains("description") && d["description"].is_string(),
        "详情含纯文本题面");
  check(d.value("time_limit_ms", -1) == 2000, "详情含时限");
  check(d.value("memory_limit_kb", -1) == 65536, "详情含内存限制");
  check(d["samples"].is_array() && d["samples"].size() == 2,
        "详情含 2 组公开样例");
  check(d["samples"][0].value("input", "") == "1 2\n" &&
            d["samples"][0].value("output", "") == "3\n",
        "样例输入/输出正确");
}

void test_problem_without_samples() {
  std::cout << "无公开样例的可见题：详情返回空 samples 且不泄露隐藏用例\n";
  Env env("prb_nosample", /*seed=*/false);
  check(env.ok(), "服务启动成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::int64_t id = insert_problem(env.db(), "只有隐藏用例的题", "题面", 1);
  insert_testcase(env.db(), id, 0, "111111111 222222222\n", "333333333\n", false);
  insert_testcase(env.db(), id, 1, "444444444 555555555\n", "999999999\n", false);

  auto detail = get_problem(cli, std::to_string(id));
  check(detail && detail->status == 200, "可见题详情 200");
  json d = json::parse(detail->body);
  check(d["samples"].is_array() && d["samples"].empty(),
        "无样例时 samples 为空数组");

  bool leaked = false;
  std::string body = detail->body;
  for (const std::string &token : hidden_tokens(env.db(), id)) {
    if (body.find(token) != std::string::npos) {
      leaked = true;
    }
  }
  check(!leaked, "无样例题详情也不泄露隐藏用例");
}

void test_normal_user_only_visible() {
  std::cout << "普通用户只能获取可见题目，直接请求隐藏题 ID 被拒\n";
  Env env("prb_user", /*seed=*/true);
  check(env.ok(), "服务启动并导入种子成功");
  httplib::Client cli("127.0.0.1", env.port());

  // 构造一道隐藏题。
  std::int64_t hidden = insert_problem(env.db(), "隐藏题", "隐藏题面", 0);
  insert_testcase(env.db(), hidden, 0, "111111111\n", "222222222\n", true);
  insert_testcase(env.db(), hidden, 1, "333333333 444444444\n",
                  "777777777\n", false);
  check(hidden > 0, "隐藏题创建成功");

  std::string account = register_user(cli, "alice", "AlicePw1");
  int status = 0;
  std::string token = login(cli, account, "AlicePw1", status);
  check(status == 200 && !token.empty(), "普通用户登录成功");

  auto list_res = get_problems(cli, "Bearer " + token);
  check(list_res && list_res->status == 200, "普通用户列表 200");
  json list = json::parse(list_res->body);
  check(list.value("total", -1) == static_cast<int>(kSeedProblemCount),
        "普通用户列表不含隐藏题（仍为 41）");
  for (const auto &p : list["problems"]) {
    if (p["id"].get<long long>() == hidden) {
      check(false, "隐藏题不应出现在普通用户列表");
    }
  }

  auto hidden_res = get_problem(cli, std::to_string(hidden),
                                "Bearer " + token);
  check(hidden_res && hidden_res->status == 404, "普通用户直接请求隐藏题 404");

  auto guest_hidden = get_problem(cli, std::to_string(hidden));
  check(guest_hidden && guest_hidden->status == 404, "游客请求隐藏题 404");
}

// ---------------------------------------------------------------------------
// HTTP：管理员可见性与首改限制
// ---------------------------------------------------------------------------

void test_admin_visibility_and_first_change() {
  std::cout << "管理员可见性：首改前不可见隐藏题，改密后可看\n";
  Env env("prb_admin", /*seed=*/true);
  check(env.ok(), "服务启动并导入种子成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::int64_t hidden = insert_problem(env.db(), "管理员隐藏题", "隐藏题面", 0);
  insert_testcase(env.db(), hidden, 0, "555555555\n", "666666666\n", true);
  insert_testcase(env.db(), hidden, 1, "777777777 888888888\n",
                  "999999999\n", false);

  int status = 0;
  std::string admin_token = login(cli, "admin", kAdminPassword, status);
  check(status == 200 && !admin_token.empty(), "admin 登录成功");

  // 未改密：不能查看隐藏题（列表不含、详情 404）。
  auto list_first = get_problems(cli, "Bearer " + admin_token);
  check(list_first &&
            json::parse(list_first->body).value("total", -1) ==
                static_cast<int>(kSeedProblemCount),
        "未改密 admin 列表不含隐藏题");
  auto hidden_first = get_problem(cli, std::to_string(hidden),
                                  "Bearer " + admin_token);
  check(hidden_first && hidden_first->status == 404,
        "未改密 admin 请求隐藏题 404（首改限制）");

  // 完成首次改密。
  auto ch = change_password(
      cli, admin_token,
      R"({"old_password":"AdminSecret123!","new_password":"AdminNewPass1"})");
  check(ch && ch->status == 200, "admin 完成首次改密");

  // 改密后可查看隐藏题。
  auto list_after = get_problems(cli, "Bearer " + admin_token);
  check(list_after && list_after->status == 200, "已改密 admin 列表 200");
  json list = json::parse(list_after->body);
  check(list.value("total", -1) == static_cast<int>(kSeedProblemCount) + 1,
        "已改密 admin 列表含隐藏题（42）");
  bool hidden_listed = false;
  bool hidden_visible_flag = true;
  int total_pages = list.value("total_pages", 1);
  httplib::Headers admin_headers{{"Authorization", "Bearer " + admin_token}};
  for (int page = 1; page <= total_pages; ++page) {
    auto page_res =
        cli.Get(("/api/problems?page=" + std::to_string(page)).c_str(),
                admin_headers);
    if (!page_res || page_res->status != 200) {
      continue;
    }
    json page_json = json::parse(page_res->body);
    for (const auto &p : page_json["problems"]) {
      if (p["id"].get<long long>() == hidden) {
        hidden_listed = true;
        hidden_visible_flag = p.value("visible", true);
      }
    }
  }
  check(hidden_listed, "隐藏题出现在 admin 列表");
  check(!hidden_visible_flag, "隐藏题 visible 标记为 false");

  auto hidden_detail = get_problem(cli, std::to_string(hidden),
                                   "Bearer " + admin_token);
  check(hidden_detail && hidden_detail->status == 200, "已改密 admin 详情 200");
  json hd = json::parse(hidden_detail->body);
  check(hd["samples"].is_array() && hd["samples"].size() == 1,
        "隐藏题详情只返回公开样例");
  // 即使是有权查看隐藏题的管理员，详情接口也不得下发隐藏用例内容。
  bool admin_leaked = false;
  for (const std::string &token : hidden_tokens(env.db(), hidden)) {
    if (hidden_detail->body.find(token) != std::string::npos) {
      admin_leaked = true;
    }
  }
  check(!admin_leaked, "已改密 admin 详情也未泄露隐藏用例");

  // 普通用户仍然看不到隐藏题。
  std::string account = register_user(cli, "bob", "BobPw123");
  int s = 0;
  std::string user_token = login(cli, account, "BobPw123", s);
  check(get_problem(cli, std::to_string(hidden), "Bearer " + user_token)
                ->status == 404,
        "普通用户请求隐藏题仍 404");
}

void test_invalid_token_not_admin() {
  std::cout << "无效/伪造/过期 token 不被当作管理员\n";
  Env env("prb_badtoken", /*seed=*/true);
  check(env.ok(), "服务启动并导入种子成功");
  httplib::Client cli("127.0.0.1", env.port());

  std::int64_t hidden = insert_problem(env.db(), "隐藏题2", "隐藏题面", 0);
  insert_testcase(env.db(), hidden, 0, "111222333\n", "444555666\n", false);

  check(get_problems(cli, "Bearer not-a-jwt")->status == 401,
        "损坏 token 列表 401");
  check(get_problems(cli, "Basic abc")->status == 401, "非 Bearer 401");
  check(get_problem(cli, std::to_string(hidden), "Bearer not-a-jwt")->status ==
            401,
        "损坏 token 详情 401");

  std::int64_t admin_id = user_id(env.db(), "admin");
  check(admin_id > 0, "取得 admin id");
  check(get_problem(cli, std::to_string(hidden),
                    "Bearer " + sign_token(admin_id, /*exp_offset=*/-10))
                ->status == 401,
        "过期 token 401");
  check(get_problem(cli, std::to_string(hidden),
                    "Bearer " + sign_token(999999, 3600))
                ->status == 401,
        "引用不存在用户的 token 401");
}

// ---------------------------------------------------------------------------
// 隐藏用例不泄露
// ---------------------------------------------------------------------------

void test_no_hidden_leak() {
  std::cout << "列表与详情不返回隐藏用例输入/输出\n";
  Env env("prb_leak", /*seed=*/true);
  check(env.ok(), "服务启动并导入种子成功");
  httplib::Client cli("127.0.0.1", env.port());

  auto list_res = get_problems(cli);
  check(list_res && list_res->status == 200, "列表 200");
  std::string list_body = list_res->body;

  oj::ProblemStore store(env.db());
  std::vector<oj::ProblemSummary> problems;
  std::string err;
  store.list(true, problems, err);
  for (const auto &problem : problems) {
    for (const std::string &token : hidden_tokens(env.db(), problem.id)) {
      if (list_body.find(token) != std::string::npos) {
        check(false, "列表泄露隐藏用例内容: " + token);
        return;
      }
    }
  }
  check(true, "列表未泄露任何隐藏用例内容");

  for (const auto &problem : problems) {
    auto detail = get_problem(cli, std::to_string(problem.id));
    if (!detail || detail->status != 200) {
      check(false, "详情请求失败: " + std::to_string(problem.id));
      continue;
    }
    bool leaked = false;
    std::string body = detail->body;
    for (const std::string &token : hidden_tokens(env.db(), problem.id)) {
      if (body.find(token) != std::string::npos) {
        leaked = true;
      }
    }
    check(!leaked, "详情未泄露隐藏用例: " + problem.title);
  }

  // 详情应确实包含公开样例。
  std::int64_t ab = seed_problem_id(env.db(), "a-plus-b");
  auto detail = get_problem(cli, std::to_string(ab));
  check(detail->body.find("1 2") != std::string::npos,
        "详情包含公开样例内容");
}

// ---------------------------------------------------------------------------
// 空库与错误响应
// ---------------------------------------------------------------------------

void test_empty_and_error_responses() {
  std::cout << "空题库与不存在/非法 ID 的响应\n";
  Env env("prb_empty", /*seed=*/false);
  check(env.ok(), "服务启动成功（空库）");
  httplib::Client cli("127.0.0.1", env.port());

  auto list_res = get_problems(cli);
  check(list_res && list_res->status == 200, "空库列表 200");
  json list = json::parse(list_res->body);
  check(list.value("total", -1) == 0 && list["problems"].is_array() &&
            list["problems"].empty(),
        "空库返回空列表");

  check(get_problem(cli, "999999")->status == 404, "不存在题目 404");
  check(get_problem(cli, "abc")->status == 400, "非数字 ID 400");
  check(get_problem(cli, "-1")->status == 400, "负数 ID 400");
  check(get_problem(cli, "0")->status == 400, "0 ID 400");
  check(get_problem(cli, "1.5")->status == 400, "小数 ID 400");
  check(get_problem(cli, "99999999999999999999")->status == 400,
        "溢出 ID 400");

  // 数据库故障：关闭连接后请求返回 500，且不泄露内部细节。
  env.close_db();
  auto fail = get_problems(cli);
  check(fail && fail->status == 500, "数据库故障列表 500");
  if (fail) {
    json body = json::parse(fail->body);
    check(body.value("error", "") == "内部错误", "内部错误文案通用");
    std::string raw = fail->body;
    check(raw.find("sqlite") == std::string::npos &&
              raw.find("SELECT") == std::string::npos &&
              raw.find("problems") == std::string::npos,
          "不泄露 SQL/表名细节");
  }
}

void test_auth_and_detail_internal_error() {
  std::cout << "数据库故障时认证回查/详情返回 500 且不泄露细节\n";
  Env env("prb_internal", /*seed=*/true);
  check(env.ok(), "服务启动并导入种子成功");
  httplib::Client cli("127.0.0.1", env.port());

  int status = 0;
  std::string token = login(cli, "admin", kAdminPassword, status);
  check(status == 200 && !token.empty(), "admin 登录取得 token");
  std::int64_t ab = seed_problem_id(env.db(), "a-plus-b");

  env.close_db();

  // 有效 token：JWT 校验通过，但按 sub 回查用户时数据库故障 -> 500，不降级为游客。
  auto list_res = get_problems(cli, "Bearer " + token);
  check(list_res && list_res->status == 500, "认证回查故障：列表 500");

  // 详情查询（无 token 游客路径）：题目查询故障 -> 500。
  auto detail_res = get_problem(cli, std::to_string(ab));
  check(detail_res && detail_res->status == 500, "详情查询故障：500");
  if (detail_res) {
    json body = json::parse(detail_res->body);
    check(body.value("error", "") == "内部错误", "详情故障文案通用");
    std::string raw = detail_res->body;
    check(raw.find("sqlite") == std::string::npos &&
              raw.find("SELECT") == std::string::npos &&
              raw.find("problems") == std::string::npos,
          "详情故障不泄露 SQL/表名");
  }
}

// ---------------------------------------------------------------------------
// 持久化
// ---------------------------------------------------------------------------

void test_persistence_restart() {
  std::cout << "重启后题目与用例仍然保留\n";
  TempDir dir("prb_persist");
  std::string dbpath = dir.db_path();

  {
    Env env("prb_persist_first", /*seed=*/true, dbpath);
    check(env.ok(), "首次启动成功");
    env.stop();
    env.close_db();
  }

  {
    Env env("prb_persist_second", /*seed=*/false, dbpath);
    check(env.ok(), "重启成功");
    httplib::Client cli("127.0.0.1", env.port());
    auto list_res = get_problems(cli);
    check(list_res && list_res->status == 200, "重启后列表 200");
    check(json::parse(list_res->body).value("total", -1) ==
              static_cast<int>(kSeedProblemCount),
          "重启后 41 道题仍在");
    std::int64_t ab = seed_problem_id(env.db(), "a-plus-b");
    auto detail = get_problem(cli, std::to_string(ab));
    check(detail && detail->status == 200, "重启后详情可访问");

    // 用例（含隐藏）在重启后仍完整保留。
    oj::ProblemStore store(env.db());
    std::vector<oj::TestcaseRecord> cases;
    std::string err;
    check(store.list_testcases(ab, cases, err), "重启后用例查询成功");
    check(cases.size() == 5, "重启后 A+B 用例仍在（5 条）");
    int hidden_count = 0;
    for (const oj::TestcaseRecord &tc : cases) {
      if (!tc.is_sample) {
        ++hidden_count;
      }
    }
    check(hidden_count == 3, "重启后隐藏用例标记与数量保留");
    env.stop();
    env.close_db();
  }
}

} // namespace

int main() {
  test_seed_import_content();
  test_seed_idempotent();
  test_seed_atomic_rollback();
  test_guest_list_and_detail();
  test_problem_without_samples();
  test_normal_user_only_visible();
  test_admin_visibility_and_first_change();
  test_invalid_token_not_admin();
  test_no_hidden_leak();
  test_empty_and_error_responses();
  test_auth_and_detail_internal_error();
  test_persistence_restart();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部题目数据与查询集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
