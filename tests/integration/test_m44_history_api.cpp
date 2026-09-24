// M4.4 本人提交历史 / 提交详情 / 本人题目状态 HTTP 接口集成测试。
//
// 使用 /tmp 下的隔离临时库与随机端口上的真实 HTTP 服务，不触碰正式数据库；不启动
// 判题（提交记录直接按数据库契约写入），聚焦 M4.4 的读取接口与权限边界。
//
// 覆盖：
//   - GET /api/submissions?mine：仅本人、最新优先、摘要字段、不含源码/逐点；mine 参数
//     解析（空值/省略/非法）；user_id 不可指定；分页元数据与参数校验；题目筛选。
//   - GET /api/submissions/{id}：本人可读、他人/不存在 404、非法 ID 400、鉴权 401；
//     管理员（已改密）可读他人、未改密管理员按非管理员处理；AC 点不泄露隐藏用例；
//     WA 点保留输入/期望/实际；损坏 per_case 标记不可解析；更新后展示保存结果且
//     created_at 不变；隐藏题本人历史与详情仍可读（题目接口仍 404）。
//   - GET /api/status：本人状态字段、按题目筛选、无记录不返回、鉴权与隔离。
//
// 运行方式：ctest --test-dir build -R m44_history_api --output-on-failure
// 或直接执行 build/oj_m44_history_api_test。

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

const std::string kTestSecret = "it-m44-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";
const std::string kAdminNewPassword = "AdminNewSecret456!";

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
    started_ = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  bool ok() const { return started_; }
  int port() const { return port_; }
  oj::Database &db() { return *db_; }
  void stop() {
    if (server_) server_->stop();
  }

private:
  TempDir dir_;
  std::unique_ptr<oj::Database> db_;
  std::unique_ptr<oj::HttpServer> server_;
  int port_ = 0;
  bool started_ = false;
};

// ---------------------------------------------------------------------------
// 数据库 / HTTP 辅助
// ---------------------------------------------------------------------------

std::int64_t user_id(oj::Database &db, const std::string &account) {
  std::string err;
  oj::Statement stmt;
  db.prepare("SELECT id FROM users WHERE account = ?", stmt, err);
  stmt.bind(1, account);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::int64_t insert_problem(oj::Database &db, const std::string &title,
                            int visible) {
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

std::int64_t insert_submission(oj::Database &db, std::int64_t user_id,
                               std::int64_t problem_id, const std::string &status,
                               const std::string &created_at,
                               const std::string &per_case = "[]",
                               const std::string &source = "int main(){}",
                               const std::string &compile_msg = "",
                               long long runtime_ms = 1,
                               long long memory_kb = 4096) {
  oj::SubmissionStore store(db);
  oj::SubmissionRecord record;
  record.user_id = user_id;
  record.problem_id = problem_id;
  record.language = "cpp17";
  record.source_code = source;
  record.status = status;
  record.per_case = per_case;
  record.compile_msg = compile_msg;
  record.runtime_ms = runtime_ms;
  record.memory_kb = memory_kb;
  record.created_at = created_at;
  std::int64_t id = 0;
  std::string err;
  if (!store.insert(record, id, err)) return -1;
  return id;
}

void set_status(oj::Database &db, std::int64_t user_id, std::int64_t problem_id,
                bool accepted, bool has_first_ac,
                const std::string &first_ac_at, int submit_count) {
  oj::UserProblemStatusStore store(db);
  std::string err;
  store.upsert(user_id, problem_id, accepted, has_first_ac, first_ac_at,
               submit_count, err);
}

std::string register_user(httplib::Client &cli, const std::string &nickname,
                          const std::string &password) {
  json body;
  body["nickname"] = nickname;
  body["password"] = password;
  auto res = cli.Post("/api/register", body.dump(), "application/json");
  if (!res || res->status != 201) return "";
  return json::parse(res->body).value("account", "");
}

std::string login(httplib::Client &cli, const std::string &account,
                  const std::string &password, int &status_out) {
  json body;
  body["account"] = account;
  body["password"] = password;
  auto res = cli.Post("/api/login", body.dump(), "application/json");
  status_out = res ? res->status : -1;
  if (!res || res->status != 200) return "";
  return json::parse(res->body).value("token", "");
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

httplib::Result change_password(httplib::Client &cli, const std::string &token,
                                const std::string &old_password,
                                const std::string &new_password) {
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  json body;
  body["old_password"] = old_password;
  body["new_password"] = new_password;
  return cli.Post("/api/me/password", h, body.dump(), "application/json");
}

std::string make_admin_token(httplib::Client &cli) {
  int status = 0;
  std::string token = login(cli, "admin", kAdminPassword, status);
  if (token.empty()) return "";
  auto changed = change_password(cli, token, kAdminPassword, kAdminNewPassword);
  if (!changed || changed->status != 200) return "";
  return login(cli, "admin", kAdminNewPassword, status);
}

httplib::Result get_with_token(httplib::Client &cli, const std::string &path,
                               const std::string &token) {
  if (token.empty()) return cli.Get(path.c_str());
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Get(path.c_str(), h);
}

json body_of(const httplib::Result &res) {
  return res && !res->body.empty() ? json::parse(res->body) : json();
}

// ---------------------------------------------------------------------------
// T-201 ~ T-206：历史列表
// ---------------------------------------------------------------------------

void test_history() {
  std::cout << "\n== M44-1 本人提交历史（mine/排序/摘要/鉴权/分页/筛选）==\n";
  TempDir dir("m44_hist");
  std::string err;
  auto db = oj::Database::open(dir.db_path(), err);
  check(db != nullptr, "打开隔离数据库");
  if (!db) return;
  check(oj::initialize_schema(*db, kAdminPassword, err), "初始化表结构");
  int port = find_free_port();
  oj::HttpServer server("127.0.0.1", port, *db, make_config(), false);
  check(server.start(err), "服务启动成功");
  if (!err.empty()) std::cout << "  server error: " << err << "\n";
  httplib::Client cli = make_client(port);

  const std::int64_t p_vis = insert_problem(*db, "M44 可见题", 1);
  const std::int64_t p_hid = insert_problem(*db, "M44 隐藏题", 0);
  check(p_vis > 0 && p_hid > 0, "插入题目");

  User alice = make_user(cli, *db, "m44_hist_alice", "UserPass123");
  User bob = make_user(cli, *db, "m44_hist_bob", "UserPass123");
  check(!alice.token.empty() && !bob.token.empty(), "注册并登录两个用户");

  const std::string ac_case =
      "[{\"index\":0,\"status\":\"AC\",\"time_ms\":1,\"memory_kb\":1024}]";
  const std::string wa_case =
      "[{\"index\":0,\"status\":\"WA\",\"time_ms\":2,\"memory_kb\":2048,"
      "\"reason\":\"non_zero_exit\",\"actual_output\":\"0\\n\","
      "\"input\":\"1 2\\n\",\"expected_output\":\"3\\n\"}]";

  const std::int64_t a1 =
      insert_submission(*db, alice.id, p_vis, "AC", "2026-09-21 10:00:00",
                        ac_case, "int main(){}", "", 1, 1024);
  const std::int64_t a2 =
      insert_submission(*db, alice.id, p_vis, "WA", "2026-09-21 11:00:00",
                        wa_case, "int main(){return 1;}", "warn", 2, 2048);
  const std::int64_t a3 =
      insert_submission(*db, alice.id, p_hid, "AC", "2026-09-21 12:00:00",
                        ac_case, "", "", 1, 1024);
  check(a1 > 0 && a2 > 0 && a3 > 0, "写入 alice 三条提交");
  const std::int64_t b1 =
      insert_submission(*db, bob.id, p_vis, "AC", "2026-09-21 13:00:00",
                        ac_case);
  check(b1 > 0, "写入 bob 一条提交");

  // T-201：正常路径、最新优先、摘要且不泄露。
  {
    auto res = get_with_token(cli, "/api/submissions?mine", alice.token);
    check(res && res->status == 200, "历史 200");
    json body = body_of(res);
    check(body.value("mine", false) == true, "mine=true");
    check(body.value("page", 0) == 1 && body.value("page_size", 0) == 20 &&
              body.value("total", 0) == 3 && body.value("total_pages", 0) == 1,
          "分页元数据 page=1/page_size=20/total=3/total_pages=1");
    const auto &list = body["submissions"];
    check(list.is_array() && list.size() == 3, "返回 3 条");
    if (list.size() == 3) {
      check(list[0].value("id", 0) == a3 && list[1].value("id", 0) == a2 &&
                list[2].value("id", 0) == a1,
            "最新优先排序 a3,a2,a1");
      const auto &item = list[1];
      check(item.value("problem_id", 0) == p_vis &&
                item.value("problem_title", "") == "M44 可见题",
            "摘要含题目 ID 与标题");
      check(item.value("language", "") == "cpp17" &&
                item.value("status", "") == "WA",
            "摘要含语言与状态");
      check(item.contains("runtime_ms") && item.value("runtime_ms", -1) == 2 &&
                item.value("memory_kb", -1) == 2048,
            "摘要含耗时与内存");
      check(item.value("created_at", "") == "2026-09-21 11:00:00",
            "摘要含原提交时间");
    }
    const std::string raw = body.dump();
    check(raw.find("source_code") == std::string::npos &&
              raw.find("compile_output") == std::string::npos &&
              raw.find("per_case") == std::string::npos &&
              raw.find("\"results\"") == std::string::npos,
          "列表不含源码/编译信息/逐点结果");
    check(raw.find("int main") == std::string::npos, "列表不含源码内容");
  }

  // T-203：mine 空值与省略均按本人；显式非法取值 400。
  {
    auto empty_val = get_with_token(cli, "/api/submissions?mine", alice.token);
    check(empty_val && empty_val->status == 200 &&
              body_of(empty_val).value("total", 0) == 3,
          "?mine（空值）按本人历史");
    auto omitted = get_with_token(cli, "/api/submissions", alice.token);
    check(omitted && omitted->status == 200 &&
              body_of(omitted).value("total", 0) == 3,
          "省略 mine 默认仍仅返回本人历史");
    auto bad = get_with_token(cli, "/api/submissions?mine=0", alice.token);
    check(bad && bad->status == 400, "mine=0 返回 400");
    auto bad2 = get_with_token(cli, "/api/submissions?mine=no", alice.token);
    check(bad2 && bad2->status == 400, "mine=no 返回 400");
  }

  // T-204：客户端 user_id 不可指定他人。
  {
    auto res = get_with_token(
        cli, "/api/submissions?mine&user_id=" + std::to_string(bob.id),
        alice.token);
    json body = body_of(res);
    check(res && res->status == 200 && body.value("total", 0) == 3,
          "带他人 user_id 仍只返回本人 3 条");
    bool all_alice = true;
    for (const auto &item : body["submissions"]) {
      if (item.value("id", 0) == b1) all_alice = false;
    }
    check(all_alice, "响应不含他人提交");
  }

  // T-202：鉴权。
  {
    auto no_token = cli.Get("/api/submissions?mine");
    check(no_token && no_token->status == 401, "无 token 返回 401");
    auto bad_token = get_with_token(cli, "/api/submissions?mine", "bad-token");
    check(bad_token && bad_token->status == 401, "无效 token 返回 401");
  }

  // T-205：分页（专用用户 25 条）与参数校验。
  {
    User pager = make_user(cli, *db, "m44_hist_pager", "UserPass123");
    check(!pager.token.empty(), "注册分页用户");
    for (int i = 0; i < 25; ++i) {
      insert_submission(*db, pager.id, p_vis, "AC", "2026-09-22 00:00:00",
                        ac_case);
    }
    auto page1 = get_with_token(cli, "/api/submissions?mine", pager.token);
    json b1 = body_of(page1);
    check(page1 && page1->status == 200 && b1["submissions"].size() == 20 &&
              b1.value("total", 0) == 25 && b1.value("total_pages", 0) == 2,
          "第 1 页 20 条，total=25，total_pages=2");
    auto page2 = get_with_token(cli, "/api/submissions?mine&page=2", pager.token);
    json b2 = body_of(page2);
    check(page2 && page2->status == 200 && b2["submissions"].size() == 5 &&
              b2.value("page", 0) == 2,
          "第 2 页 5 条");
    auto beyond = get_with_token(cli, "/api/submissions?mine&page=999",
                                 pager.token);
    json bb = body_of(beyond);
    check(beyond && beyond->status == 200 && bb["submissions"].empty() &&
              bb.value("total", 0) == 25,
          "超出末页返回空列表且 total 不变");
    auto zero = get_with_token(cli, "/api/submissions?mine&page=0", pager.token);
    check(zero && zero->status == 400, "page=0 返回 400");
    auto alpha = get_with_token(cli, "/api/submissions?mine&page=abc",
                                pager.token);
    check(alpha && alpha->status == 400, "page=abc 返回 400");
    auto toobig = get_with_token(cli, "/api/submissions?mine&page=1000001",
                                 pager.token);
    check(toobig && toobig->status == 400, "page 超上限返回 400");
  }

  // T-206：题目筛选；非法 problem_id 400。
  {
    auto filtered = get_with_token(
        cli, "/api/submissions?mine&problem_id=" + std::to_string(p_hid),
        alice.token);
    json body = body_of(filtered);
    check(filtered && filtered->status == 200 &&
              body.value("total", 0) == 1 &&
              !body["submissions"].empty() &&
              body["submissions"][0].value("id", 0) == a3,
          "problem_id 筛选仅返回该题 1 条");
    auto bad = get_with_token(cli, "/api/submissions?mine&problem_id=abc",
                              alice.token);
    check(bad && bad->status == 400, "非法 problem_id 返回 400");
    auto zero = get_with_token(cli, "/api/submissions?mine&problem_id=0",
                               alice.token);
    check(zero && zero->status == 400, "problem_id=0 返回 400");
  }

  server.stop();
}

// ---------------------------------------------------------------------------
// T-210 ~ T-216：提交详情
// ---------------------------------------------------------------------------

void test_detail() {
  std::cout << "\n== M44-2 提交详情（本人/管理员/错误/损坏/来源/可见性）==\n";
  TempDir dir("m44_detail");
  std::string err;
  auto db = oj::Database::open(dir.db_path(), err);
  check(db != nullptr, "打开隔离数据库");
  if (!db) return;
  check(oj::initialize_schema(*db, kAdminPassword, err), "初始化表结构");
  int port = find_free_port();
  oj::HttpServer server("127.0.0.1", port, *db, make_config(), false);
  check(server.start(err), "服务启动成功");
  httplib::Client cli = make_client(port);

  const std::int64_t p_vis = insert_problem(*db, "M44 详情可见题", 1);
  const std::int64_t p_hid = insert_problem(*db, "M44 详情隐藏题", 0);

  User alice = make_user(cli, *db, "m44_det_alice", "UserPass123");
  User bob = make_user(cli, *db, "m44_det_bob", "UserPass123");
  User corrupt = make_user(cli, *db, "m44_det_corrupt", "UserPass123");
  check(!alice.token.empty() && !bob.token.empty() && !corrupt.token.empty(),
        "注册并登录用户");

  const std::string ac_case =
      "[{\"index\":0,\"status\":\"AC\",\"time_ms\":1,\"memory_kb\":1024}]";
  const std::string wa_case =
      "[{\"index\":0,\"status\":\"WA\",\"time_ms\":2,\"memory_kb\":2048,"
      "\"reason\":\"non_zero_exit\",\"actual_output\":\"0\\n\","
      "\"input\":\"1 2\\n\",\"expected_output\":\"3\\n\"}]";

  const std::int64_t ac_id = insert_submission(
      *db, alice.id, p_vis, "AC", "2026-09-21 10:00:00", ac_case,
      "int main(){ /* alice src */ }", "", 1, 1024);
  const std::int64_t wa_id = insert_submission(
      *db, alice.id, p_vis, "WA", "2026-09-21 11:00:00", wa_case,
      "int main(){return 1;}", "warn: unused", 2, 2048);
  const std::int64_t hidden_id =
      insert_submission(*db, alice.id, p_hid, "AC", "2026-09-21 12:00:00",
                        ac_case);
  check(ac_id > 0 && wa_id > 0 && hidden_id > 0, "写入 alice 提交");
  const std::int64_t bad_json_id = insert_submission(
      *db, corrupt.id, p_vis, "WA", "2026-09-21 13:00:00", "not-json{");
  const std::int64_t not_array_id = insert_submission(
      *db, corrupt.id, p_vis, "WA", "2026-09-21 14:00:00", "{\"a\":1}");
  check(bad_json_id > 0 && not_array_id > 0, "写入损坏 per_case 提交");

  // T-210：本人详情、字段完整、AC 点不泄露隐藏输入/答案。
  {
    auto res = get_with_token(cli, "/api/submissions/" + std::to_string(ac_id),
                              alice.token);
    check(res && res->status == 200, "本人详情 200");
    json body = body_of(res);
    check(body.value("id", 0) == ac_id && body.value("problem_id", 0) == p_vis &&
              body.value("problem_title", "") == "M44 详情可见题",
          "返回 ID/题目 ID/标题");
    check(body.value("language", "") == "cpp17" &&
              body.value("status", "") == "AC",
          "返回语言与状态");
    check(body.value("source_code", "").find("alice src") != std::string::npos,
          "返回完整源码");
    check(body.contains("compile_output") && body.value("runtime_ms", -1) == 1 &&
              body.value("memory_kb", -1) == 1024,
          "返回编译信息与指标");
    check(body.value("created_at", "") == "2026-09-21 10:00:00",
          "返回原提交时间");
    check(body.value("per_case_parse_error", true) == false,
          "有效 per_case：parse_error=false");
    check(body["results"].is_array() && body["results"].size() == 1,
          "返回逐点结果");
    if (body["results"].is_array() && !body["results"].empty()) {
      const auto &pt = body["results"][0];
      check(pt.value("status", "") == "AC" && !pt.contains("input") &&
                !pt.contains("expected_output"),
            "AC 点不含隐藏输入/标准答案");
    }
  }

  // T-213：WA 详情保留输入/期望/实际。
  {
    auto res = get_with_token(cli, "/api/submissions/" + std::to_string(wa_id),
                              alice.token);
    json body = body_of(res);
    check(res && res->status == 200 && body.value("status", "") == "WA",
          "WA 详情 200");
    if (body["results"].is_array() && !body["results"].empty()) {
      const auto &pt = body["results"][0];
      check(pt.value("input", "") == "1 2\n" &&
                pt.value("expected_output", "") == "3\n" &&
                pt.value("actual_output", "") == "0\n",
            "WA 点含输入/期望/实际输出");
    } else {
      check(false, "WA 点含输入/期望/实际输出（results 为空）");
    }
  }

  // T-211：鉴权与错误。
  {
    auto other = get_with_token(cli, "/api/submissions/" + std::to_string(ac_id),
                                bob.token);
    check(other && other->status == 404, "他人查看返回 404");
    auto missing =
        get_with_token(cli, "/api/submissions/999999", alice.token);
    check(missing && missing->status == 404, "不存在记录返回 404");
    auto alpha = get_with_token(cli, "/api/submissions/abc", alice.token);
    check(alpha && alpha->status == 400, "非法 ID abc 返回 400");
    auto zero = get_with_token(cli, "/api/submissions/0", alice.token);
    check(zero && zero->status == 400, "ID 0 返回 400");
    auto neg = get_with_token(cli, "/api/submissions/-1", alice.token);
    check(neg && neg->status == 400, "ID -1 返回 400");
    auto anon = cli.Get(("/api/submissions/" + std::to_string(ac_id)).c_str());
    check(anon && anon->status == 401, "无 token 返回 401");
    auto bad = get_with_token(cli, "/api/submissions/" + std::to_string(ac_id),
                              "bad-token");
    check(bad && bad->status == 401, "无效 token 返回 401");
  }

  // T-212：管理员权限与首次改密规则。
  {
    int status = 0;
    std::string pre_change = login(cli, "admin", kAdminPassword, status);
    check(!pre_change.empty(), "admin 首次登录成功（reset_pwd_flag=1）");
    auto before = get_with_token(cli, "/api/submissions/" + std::to_string(ac_id),
                                 pre_change);
    check(before && before->status == 404,
          "未改密管理员查看他人提交按非管理员处理（404）");

    std::string admin_token = make_admin_token(cli);
    check(!admin_token.empty(), "admin 完成首次改密");
    auto after = get_with_token(cli, "/api/submissions/" + std::to_string(ac_id),
                                admin_token);
    check(after && after->status == 200,
          "已改密管理员查看他人提交 200");
    check(body_of(after).value("source_code", "").find("alice src") !=
              std::string::npos,
          "管理员可见源码");
  }

  // T-214：损坏 per_case 明确标记，不伪装。
  {
    auto res = get_with_token(
        cli, "/api/submissions/" + std::to_string(bad_json_id), corrupt.token);
    json body = body_of(res);
    check(res && res->status == 200, "损坏 per_case 详情仍 200");
    check(body.value("per_case_parse_error", false) == true,
          "非 JSON per_case → parse_error=true");
    check(body["results"].is_array() && body["results"].empty(),
          "损坏结果返回空数组而非伪造 AC");
    check(body.value("status", "") == "WA", "保留数据库保存的总体状态");

    auto res2 = get_with_token(
        cli, "/api/submissions/" + std::to_string(not_array_id), corrupt.token);
    check(body_of(res2).value("per_case_parse_error", false) == true,
          "JSON 非数组 → parse_error=true");
  }

  // T-215：详情读取数据库保存结果（Rejudge 后展示当前保存结果，时间不变）。
  {
    oj::SubmissionStore store(*db);
    oj::SubmissionRecord record;
    bool found = false;
    std::string err2;
    check(store.find_by_id(wa_id, found, record, err2) && found,
          "读取待更新提交");
    record.status = "AC";
    record.per_case = ac_case;
    record.compile_msg = "";
    record.memory_kb = 1024;
    check(store.update(record, err2), "更新结果为 AC（模拟 Rejudge 落库）");
    auto res = get_with_token(cli, "/api/submissions/" + std::to_string(wa_id),
                              alice.token);
    json body = body_of(res);
    check(body.value("status", "") == "AC" &&
              body["results"].is_array() && !body["results"].empty() &&
              body["results"][0].value("status", "") == "AC",
          "详情展示更新后的保存结果");
    check(body.value("created_at", "") == "2026-09-21 11:00:00",
          "原提交时间不被改写");
  }

  // T-216：隐藏题的历史/详情仍归本人可见；题目接口仍 404。
  {
    auto own = get_with_token(
        cli, "/api/submissions/" + std::to_string(hidden_id), alice.token);
    check(own && own->status == 200 &&
              body_of(own).value("problem_title", "") == "M44 详情隐藏题",
          "隐藏题本人提交详情可读");
    auto guest = get_with_token(
        cli, "/api/submissions/" + std::to_string(hidden_id), bob.token);
    check(guest && guest->status == 404, "他人仍不可读");
    auto problem = get_with_token(
        cli, "/api/problems/" + std::to_string(p_hid), alice.token);
    check(problem && problem->status == 404, "题目接口对普通用户仍 404");
  }

  server.stop();
}

// ---------------------------------------------------------------------------
// T-220：本人题目状态
// ---------------------------------------------------------------------------

void test_status() {
  std::cout << "\n== M44-3 本人题目状态（字段/筛选/空/鉴权/隔离）==\n";
  TempDir dir("m44_status");
  std::string err;
  auto db = oj::Database::open(dir.db_path(), err);
  check(db != nullptr, "打开隔离数据库");
  if (!db) return;
  check(oj::initialize_schema(*db, kAdminPassword, err), "初始化表结构");
  int port = find_free_port();
  oj::HttpServer server("127.0.0.1", port, *db, make_config(), false);
  check(server.start(err), "服务启动成功");
  httplib::Client cli = make_client(port);

  const std::int64_t p1 = insert_problem(*db, "M44 状态题一", 1);
  const std::int64_t p2 = insert_problem(*db, "M44 状态题二", 1);
  const std::int64_t p3 = insert_problem(*db, "M44 状态题三", 1);

  User alice = make_user(cli, *db, "m44_st_alice", "UserPass123");
  User bob = make_user(cli, *db, "m44_st_bob", "UserPass123");
  set_status(*db, alice.id, p2, false, false, "", 1);
  set_status(*db, alice.id, p1, true, true, "2026-09-21 12:00:00", 3);
  set_status(*db, bob.id, p3, true, true, "2026-09-21 09:00:00", 1);

  {
    auto res = get_with_token(cli, "/api/status", alice.token);
    check(res && res->status == 200, "本人状态 200");
    json body = body_of(res);
    const auto &list = body["statuses"];
    check(list.is_array() && list.size() == 2u, "仅返回本人已有状态 2 条");
    if (list.size() == 2) {
      check(list[0].value("problem_id", 0) == p1 &&
                list[0].value("status", "") == "accepted" &&
                list[0].value("first_ac_at", "") == "2026-09-21 12:00:00" &&
                list[0].value("submit_count", 0) == 3,
            "accepted 记录字段正确且按 problem_id 升序");
      check(list[1].value("problem_id", 0) == p2 &&
                list[1].value("status", "") == "none" &&
                list[1]["first_ac_at"].is_null() &&
                list[1].value("submit_count", 0) == 1,
            "none 记录 first_ac_at 为 null");
    }
    const std::string raw = body.dump();
    check(raw.find("M44 状态题") == std::string::npos,
          "状态接口不返回题目标题等隐藏资料");
  }

  {
    auto filtered = get_with_token(
        cli, "/api/status?problem_id=" + std::to_string(p1), alice.token);
    json body = body_of(filtered);
    check(filtered && filtered->status == 200 &&
              body["statuses"].size() == 1 &&
              body["statuses"][0].value("problem_id", 0) == p1,
          "problem_id 筛选返回单条");
    auto missing = get_with_token(
        cli, "/api/status?problem_id=" + std::to_string(p3), alice.token);
    check(body_of(missing)["statuses"].empty(),
          "无记录题目不出现在状态响应中（非接口失败）");
    auto bad = get_with_token(cli, "/api/status?problem_id=abc", alice.token);
    check(bad && bad->status == 400, "非法 problem_id 返回 400");
    auto anon = cli.Get("/api/status");
    check(anon && anon->status == 401, "无 token 返回 401");
  }

  {
    auto b = get_with_token(cli, "/api/status", bob.token);
    json body = body_of(b);
    check(body["statuses"].size() == 1 &&
              body["statuses"][0].value("problem_id", 0) == p3,
          "用户状态互相隔离");
  }

  server.stop();
}

} // namespace

int main() {
  std::cout << "M4.4 提交历史与详情接口集成测试\n";
  test_history();
  test_detail();
  test_status();
  std::cout << "\n失败数：" << g_failures << "\n";
  return g_failures == 0 ? 0 : 1;
}
