// 题目列表查询集成测试（M2.3）。
//
// 使用 /tmp 下的隔离临时库与随机端口上的真实 HTTP 服务，不触碰正式数据库。
// 判题执行器可注入：通过人数与本人状态用 FakeExecutor 产生可控 AC/WA 提交，
// 不依赖真实编译器。
//
// 覆盖：
//   - 无条件查询：默认第 1 页、每页 20 条、total/total_pages 正确
//   - 关键词搜索：中文、ASCII 大小写、引号、LIKE 特殊字符 %/_ 按字面匹配
//   - 难度筛选：easy/medium/hard、空表示不限、非法取值 400
//   - 标签筛选：完整标签匹配，不出现子串误匹配（图 vs 图论）
//   - 组合筛选：q+difficulty+tag 之间 AND；空条件与无匹配结果
//   - 分页：跨页不重复不遗漏、末页、超出末页空列表、非法/越界 page 400
//   - total 与筛选后且有权查看的题目数量一致，不泄露隐藏题目
//   - 通过人数：重复 AC 不增加、不同用户 AC 才增加、仅失败提交不计入
//   - 本人状态：未提交/仅失败/已 AC；AC 后再失败仍为已 AC；游客无该字段
//   - 身份只能来自 token，客户端参数不能指定他人身份
//   - 游客/普通用户/已改密管理员/未改密管理员的可见范围
//   - 管理员 visible 筛选正确，普通用户不能借其读取隐藏题目
//   - 列表响应不含隐藏用例内容或用户源码
//   - 无效 token 401，数据库故障 500 通用文案
//
// 运行方式：ctest --test-dir build -R problems_list_api --output-on-failure
// 或直接执行 build/oj_problem_list_api_test。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "auth/jwt.h"
#include "db/database.h"
#include "db/schema.h"
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

const std::string kTestSecret = "it-list-secret-0123456789abcdef";
const std::string kAdminPassword = "AdminSecret123!";

oj::auth::JwtConfig make_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = kTestSecret;
  cfg.expires_seconds = 3600;
  return cfg;
}

// 可控执行器：编译阶段读取源码中 "OUT:<值>" 到行尾，作为所有运行点输出。
// 不启动真实进程，也不做沙箱；仅用于产生可控的 AC/WA 提交。
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
    out.close();

    std::ifstream in(request.source_path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string content = buffer.str();
    std::string value;
    const std::string marker = "OUT:";
    std::size_t pos = content.find(marker);
    if (pos != std::string::npos) {
      std::size_t begin = pos + marker.size();
      std::size_t end = content.find('\n', begin);
      value = content.substr(begin, end == std::string::npos
                                        ? std::string::npos
                                        : end - begin);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    outputs_[request.output_path] = value;
    return result;
  }

  oj::judge::ProcessResult run(const oj::judge::RunRequest &request,
                               const std::string &) override {
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    result.time_ms = 1;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = outputs_.find(request.executable_path);
    if (it != outputs_.end()) {
      result.stdout_data = it->second;
    }
    return result;
  }

private:
  std::mutex mutex_;
  std::map<std::string, std::string> outputs_;
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

// 隔离临时库 + 随机端口真实 HTTP + 可选注入执行器。
class Env {
public:
  Env(const std::string &label,
      oj::judge::IExecutor *executor = nullptr)
      : dir_(label), executor_(executor) {
    std::string err;
    db_ = oj::Database::open(dir_.db_path(), err);
    if (!db_) {
      return;
    }
    if (!oj::initialize_schema(*db_, kAdminPassword, err)) {
      return;
    }
    port_ = find_free_port();
    server_ = std::make_unique<oj::HttpServer>("127.0.0.1", port_, *db_,
                                               make_config(),
                                               /*enable_test_routes=*/false,
                                               executor_);
    if (!server_->start(err)) {
      return;
    }
    started_ = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  bool ok() const { return started_; }
  int port() const { return port_; }
  oj::Database &db() { return *db_; }

  void close_db() {
    if (db_) {
      db_->close();
    }
  }

private:
  TempDir dir_;
  oj::judge::IExecutor *executor_;
  std::unique_ptr<oj::Database> db_;
  std::unique_ptr<oj::HttpServer> server_;
  int port_ = 0;
  bool started_ = false;
};

// ---------------------------------------------------------------------------
// 数据写入与 HTTP 辅助
// ---------------------------------------------------------------------------

std::int64_t insert_problem(oj::Database &db, const std::string &title,
                            const std::string &difficulty,
                            const std::string &tags, int visible) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("INSERT INTO problems (title, description, difficulty, tags, "
                  "time_limit_ms, memory_limit_kb, visible) VALUES (?, '', ?, "
                  "?, 2000, 65536, ?) RETURNING id",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, title);
  stmt.bind(2, difficulty);
  stmt.bind(3, tags);
  stmt.bind(4, visible);
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

httplib::Result list_req(httplib::Client &cli, const httplib::Params &params,
                         const std::string &auth = "") {
  httplib::Headers headers;
  if (!auth.empty()) {
    headers.emplace("Authorization", auth);
  }
  return cli.Get("/api/problems", params, headers);
}

std::string register_user(httplib::Client &cli, const std::string &nickname,
                          const std::string &password) {
  json body;
  body["nickname"] = nickname;
  body["password"] = password;
  auto res = cli.Post("/api/register", body.dump(), "application/json");
  if (!res || res->status != 201) {
    return "";
  }
  return json::parse(res->body).value("account", "");
}

std::string login(httplib::Client &cli, const std::string &account,
                  const std::string &password, int &status) {
  json body;
  body["account"] = account;
  body["password"] = password;
  auto res = cli.Post("/api/login", body.dump(), "application/json");
  status = res ? res->status : -1;
  if (!res || res->body.empty()) {
    return "";
  }
  return json::parse(res->body).value("token", "");
}

bool change_password(httplib::Client &cli, const std::string &token,
                     const std::string &old_password,
                     const std::string &new_password) {
  httplib::Headers headers{{"Authorization", "Bearer " + token}};
  json body;
  body["old_password"] = old_password;
  body["new_password"] = new_password;
  auto res = cli.Post("/api/me/password", headers, body.dump(),
                      "application/json");
  return res && res->status == 200;
}

int submit_code(httplib::Client &cli, const std::string &token,
                std::int64_t problem_id, const std::string &code, json &out) {
  httplib::Headers headers{{"Authorization", "Bearer " + token}};
  json body;
  body["language"] = "cpp17";
  body["code"] = code;
  auto res = cli.Post(
      ("/api/problems/" + std::to_string(problem_id) + "/submit").c_str(),
      headers, body.dump(), "application/json");
  out = (res && !res->body.empty()) ? json::parse(res->body) : json();
  return res ? res->status : -1;
}

std::int64_t user_id(oj::Database &db, const std::string &account) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT id FROM users WHERE account = ?", stmt, err)) {
    return -1;
  }
  stmt.bind(1, account);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

std::vector<long long> problem_ids(const json &body) {
  std::vector<long long> ids;
  for (const auto &p : body["problems"]) {
    ids.push_back(p["id"].get<long long>());
  }
  return ids;
}

bool strictly_increasing(const std::vector<long long> &ids) {
  for (std::size_t i = 1; i < ids.size(); ++i) {
    if (ids[i] <= ids[i - 1]) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// 主数据集：45 道公开题（难度/标签可预测）+ 3 道隐藏题
// ---------------------------------------------------------------------------

struct MainCounts {
  int total_visible = 45;
  int easy = 20;
  int medium = 15;
  int hard = 10;
  int tag_rumen = 12;
  int tag_tu = 11;    // 13..18 (6) + 23..27 (5)
  int tag_tulun = 4;  // 19..22
  int hidden = 3;
};

int seed_main_dataset(oj::Database &db, std::vector<long long> &hidden_ids) {
  for (int i = 1; i <= 45; ++i) {
    std::string title = "题目" + std::string(3 - std::to_string(i).size(), '0') +
                        std::to_string(i);
    std::string difficulty = i <= 20 ? "easy" : (i <= 35 ? "medium" : "hard");
    std::string tags;
    if (i <= 12) {
      tags = "入门";
    } else if (i <= 18) {
      tags = "图";
    } else if (i <= 22) {
      tags = "图论";
    } else if (i <= 27) {
      tags = "图,进阶";
    } else {
      tags = "其他";
    }
    if (insert_problem(db, title, difficulty, tags, 1) <= 0) {
      return 1;
    }
  }
  const char *hidden_titles[] = {"隐藏甲", "隐藏乙", "隐藏丙"};
  const char *hidden_diff[] = {"easy", "medium", "hard"};
  for (int i = 0; i < 3; ++i) {
    std::int64_t id =
        insert_problem(db, hidden_titles[i], hidden_diff[i], "隐藏", 0);
    if (id <= 0) {
      return 1;
    }
    std::string token = "HIDDENTOKEN" + std::to_string(i) + "7654321";
    if (!insert_testcase(db, id, 0, token + "\n", "HIDDENOUTPUT" +
                                                      std::to_string(i) + "\n",
                         false)) {
      return 1;
    }
    hidden_ids.push_back(id);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// 主数据集场景
// ---------------------------------------------------------------------------

void test_no_filter_and_pagination() {
  std::cout << "主数据集：无条件查询与分页\n";
  Env env("list_main");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  std::vector<long long> hidden_ids;
  check(seed_main_dataset(env.db(), hidden_ids) == 0, "数据集写入成功");

  httplib::Client cli("127.0.0.1", env.port());
  MainCounts c;

  {
    auto res = list_req(cli, {});
    check(res && res->status == 200, "无条件列表 200");
    json body = json::parse(res->body);
    check(body["problems"].size() == 20, "默认返回第 1 页 20 条");
    check(body.value("page", -1) == 1, "默认 page=1");
    check(body.value("page_size", -1) == 20, "page_size=20");
    check(body.value("total", -1) == c.total_visible, "total=45");
    check(body.value("total_pages", -1) == 3, "total_pages=3");
    auto ids = problem_ids(body);
    check(strictly_increasing(ids), "第 1 页按 id 升序");
  }

  // 逐页收集，验证不重复、不遗漏。
  std::set<long long> seen;
  int expected_page_sizes[] = {20, 20, 5};
  for (int page = 1; page <= 3; ++page) {
    httplib::Params params{{"page", std::to_string(page)}};
    auto res = list_req(cli, params);
    check(res && res->status == 200, "第 " + std::to_string(page) + " 页 200");
    json body = json::parse(res->body);
    auto ids = problem_ids(body);
    check(static_cast<int>(ids.size()) == expected_page_sizes[page - 1],
          "第 " + std::to_string(page) + " 页数量正确");
    check(strictly_increasing(ids), "第 " + std::to_string(page) + " 页有序");
    for (long long id : ids) {
      if (!seen.insert(id).second) {
        check(false, "跨页出现重复题目 id=" + std::to_string(id));
      }
    }
  }
  check(static_cast<int>(seen.size()) == c.total_visible,
        "跨页共 45 道题且无重复");

  // 超出末页：正常空列表，且 total 仍为筛选后总数。
  {
    httplib::Params params{{"page", "100"}};
    auto res = list_req(cli, params);
    check(res && res->status == 200, "超出末页 200");
    json body = json::parse(res->body);
    check(body["problems"].empty(), "超出末页为空列表");
    check(body.value("total", -1) == c.total_visible, "超出末页 total 不变");
    check(body.value("total_pages", -1) == 3, "超出末页 total_pages 不变");
  }

  // 非法/越界 page。
  for (const char *bad : {"0", "-1", "abc", "1.5", "+1", "1000001",
                          "99999999999999999999"}) {
    httplib::Params params{{"page", bad}};
    auto res = list_req(cli, params);
    check(res && res->status == 400,
          std::string("page=") + bad + " 返回 400");
  }

  // 数据库故障：通用 500，不泄露 SQL/表名。
  env.close_db();
  auto fail = list_req(cli, {});
  check(fail && fail->status == 500, "数据库故障列表返回 500");
  if (fail) {
    json body = json::parse(fail->body);
    check(body.value("error", "") == "内部错误", "内部错误文案通用");
    check(fail->body.find("sqlite") == std::string::npos &&
              fail->body.find("SELECT") == std::string::npos &&
              fail->body.find("problems") == std::string::npos,
          "不泄露 SQL/表名");
  }
}

void test_difficulty_and_tag_and_combined() {
  std::cout << "主数据集：难度/标签/组合筛选\n";
  Env env("list_filter");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  std::vector<long long> hidden_ids;
  seed_main_dataset(env.db(), hidden_ids);
  httplib::Client cli("127.0.0.1", env.port());
  MainCounts c;

  struct DifficultyCase {
    const char *value;
    int expected;
  };
  for (const DifficultyCase &d :
       {DifficultyCase{"easy", c.easy}, DifficultyCase{"medium", c.medium},
        DifficultyCase{"hard", c.hard}}) {
    httplib::Params params{{"difficulty", d.value}};
    auto res = list_req(cli, params);
    json body = json::parse(res->body);
    check(res && res->status == 200 && body.value("total", -1) == d.expected,
          std::string("difficulty=") + d.value + " 命中 " +
              std::to_string(d.expected));
    for (const auto &p : body["problems"]) {
      if (p.value("difficulty", "") != d.value) {
        check(false, "难度筛选返回了不匹配的题目");
      }
    }
  }
  {
    httplib::Params params{{"difficulty", ""}};
    auto res = list_req(cli, params);
    check(res && res->status == 200 &&
              json::parse(res->body).value("total", -1) == c.total_visible,
          "difficulty 为空表示不限");
  }
  {
    httplib::Params params{{"difficulty", "Easy"}};
    auto res = list_req(cli, params);
    check(res && res->status == 400, "非法 difficulty 返回 400");
  }

  // 标签完整匹配：图 ≠ 图论，且图,进阶 中的图应命中。
  {
    httplib::Params params{{"tag", "图"}};
    auto res = list_req(cli, params);
    json body = json::parse(res->body);
    check(res && res->status == 200 && body.value("total", -1) == c.tag_tu,
          "tag=图 精确命中 11 道（含 图,进阶，不含 图论）");
    for (const auto &p : body["problems"]) {
      bool has = false;
      for (const auto &t : p["tags"]) {
        if (t.get<std::string>() == "图") has = true;
      }
      if (!has) check(false, "tag=图 返回了不含该标签的题目");
    }
  }
  {
    httplib::Params params{{"tag", "图论"}};
    auto res = list_req(cli, params);
    json body = json::parse(res->body);
    check(res && res->status == 200 && body.value("total", -1) == c.tag_tulun,
          "tag=图论 精确命中 4 道（不被图误配）");
  }
  {
    httplib::Params params{{"tag", "门"}};
    auto res = list_req(cli, params);
    check(res && res->status == 200 &&
              json::parse(res->body).value("total", -1) == 0,
          "tag=门（入门子串）不误匹配任何题");
  }
  {
    httplib::Params params{{"tag", ""}};
    auto res = list_req(cli, params);
    check(res && res->status == 200 &&
              json::parse(res->body).value("total", -1) == c.total_visible,
          "tag 为空表示不限");
  }

  // 关键词：中文子串 + 组合 AND。
  {
    httplib::Params params{{"q", "题目"}};
    auto res = list_req(cli, params);
    check(res && res->status == 200 &&
              json::parse(res->body).value("total", -1) == c.total_visible,
          "q=题目 命中全部 45 道公开题");
  }
  {
    httplib::Params params{{"q", "题目01"}};
    auto res = list_req(cli, params);
    check(res && res->status == 200 &&
              json::parse(res->body).value("total", -1) == 10,
          "q=题目01 命中 10 道");
  }
  {
    httplib::Params params{{"q", "题目"}, {"difficulty", "easy"}, {"tag", "图"}};
    auto res = list_req(cli, params);
    json body = json::parse(res->body);
    check(res && res->status == 200 && body.value("total", -1) == 6,
          "q+difficulty+tag 组合为 AND（6 道）");
  }
  {
    httplib::Params params{{"q", "题目"}, {"difficulty", "hard"}, {"tag", "图"}};
    auto res = list_req(cli, params);
    check(res && res->status == 200 &&
              json::parse(res->body).value("total", -1) == 0,
          "无匹配组合返回空结果");
    json body = json::parse(res->body);
    check(body["problems"].empty() && body.value("total_pages", -1) == 0,
          "空结果 total_pages=0");
  }
}

void test_visibility_roles_and_admin_filter() {
  std::cout << "主数据集：可见范围与管理员可见性筛选\n";
  Env env("list_roles");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  std::vector<long long> hidden_ids;
  seed_main_dataset(env.db(), hidden_ids);
  httplib::Client cli("127.0.0.1", env.port());
  MainCounts c;
  const long long hidden_id = hidden_ids.empty() ? -1 : hidden_ids[0];

  // 游客：只看公开题，无 solved 字段，且看不到隐藏题。
  {
    auto res = list_req(cli, {});
    json body = json::parse(res->body);
    check(res && res->status == 200 && body.value("total", -1) == c.total_visible,
          "游客 total=45");
    check(!body["problems"][0].contains("solved"),
          "游客条目不含本人状态字段");
    bool hidden_seen = false;
    for (long long id : problem_ids(body)) {
      if (id == hidden_id) hidden_seen = true;
    }
    check(!hidden_seen, "游客列表不含隐藏题");
  }

  // 普通用户：只看公开题，含 solved=false。
  std::string user_account = register_user(cli, "lister_user", "ListPw123");
  int status = 0;
  std::string user_token = login(cli, user_account, "ListPw123", status);
  check(status == 200 && !user_token.empty(), "普通用户登录成功");
  {
    auto res = list_req(cli, {}, "Bearer " + user_token);
    json body = json::parse(res->body);
    check(res && res->status == 200 && body.value("total", -1) == c.total_visible,
          "普通用户 total=45");
    check(body["problems"][0].contains("solved") &&
              body["problems"][0]["solved"] == false,
          "普通用户未做题为 solved=false");
    bool hidden_seen = false;
    for (long long id : problem_ids(body)) {
      if (id == hidden_id) hidden_seen = true;
    }
    check(!hidden_seen, "普通用户列表不含隐藏题");
  }
  // 普通用户伪造 visible=0 / 伪造角色参数，也不能读取隐藏题。
  {
    httplib::Params params{{"visible", "0"}, {"role", "admin"}, {"user_id", "1"}};
    auto res = list_req(cli, params, "Bearer " + user_token);
    json body = json::parse(res->body);
    check(res && res->status == 200 && body.value("total", -1) == c.total_visible,
          "普通用户 visible=0 被忽略，仍只看 45 道公开题");
    bool hidden_seen = false;
    for (long long id : problem_ids(body)) {
      if (id == hidden_id) hidden_seen = true;
    }
    check(!hidden_seen, "普通用户无法借 visible/role 参数读取隐藏题");
  }
  // 游客同样被忽略。
  {
    httplib::Params params{{"visible", "0"}};
    auto res = list_req(cli, params);
    check(res && res->status == 200 &&
              json::parse(res->body).value("total", -1) == c.total_visible,
          "游客 visible=0 被忽略");
  }

  // 未改密管理员：按普通用户处理，看不到隐藏题。
  status = 0;
  std::string admin_token = login(cli, "admin", kAdminPassword, status);
  check(status == 200 && !admin_token.empty(), "admin 登录成功");
  {
    httplib::Params params{{"visible", "0"}};
    auto res = list_req(cli, params, "Bearer " + admin_token);
    json body = json::parse(res->body);
    check(res && res->status == 200 && body.value("total", -1) == c.total_visible,
          "未改密管理员仍只看到 45 道公开题");
    bool hidden_seen = false;
    for (long long id : problem_ids(body)) {
      if (id == hidden_id) hidden_seen = true;
    }
    check(!hidden_seen, "未改密管理员列表不含隐藏题");
  }

  // 完成首次改密后：可见全部（48），并可按 visible 筛选。
  check(change_password(cli, admin_token, kAdminPassword, "AdminNewPass1"),
        "admin 完成首次改密");
  {
    auto res = list_req(cli, {}, "Bearer " + admin_token);
    json body = json::parse(res->body);
    check(res && res->status == 200 &&
              body.value("total", -1) == c.total_visible + c.hidden,
          "已改密管理员 total=48（含隐藏）");
  }
  auto count_with = [&](const httplib::Params &params, const std::string &label,
                        int expected) {
    auto res = list_req(cli, params, "Bearer " + admin_token);
    json body = json::parse(res->body);
    check(res && res->status == 200 && body.value("total", -1) == expected,
          label);
  };
  count_with(httplib::Params{{"visible", "1"}}, "管理员 visible=1 → 45",
             c.total_visible);
  count_with(httplib::Params{{"visible", "0"}}, "管理员 visible=0 → 3 隐藏",
             c.hidden);
  count_with(httplib::Params{{"visible", "all"}}, "管理员 visible=all → 48",
             c.total_visible + c.hidden);
  // 隐藏题的 visible 标记为 false，且可被管理员识别。
  {
    httplib::Params params{{"visible", "0"}};
    auto res = list_req(cli, params, "Bearer " + admin_token);
    json body = json::parse(res->body);
    bool flag_ok = true;
    for (const auto &p : body["problems"]) {
      if (p.value("visible", true)) flag_ok = false;
    }
    check(flag_ok, "管理员 visible=0 结果均为隐藏题");
  }
  // 非法 visible 值 400。
  {
    httplib::Params params{{"visible", "maybe"}};
    auto res = list_req(cli, params);
    check(res && res->status == 400, "非法 visible 返回 400");
  }

  // 无效/伪造 token 仍按 401 处理。
  {
    auto res = list_req(cli, {}, "Bearer not-a-jwt");
    check(res && res->status == 401, "损坏 token 列表 401");
    auto res2 = list_req(cli, {}, "Basic abc");
    check(res2 && res2->status == 401, "非 Bearer 认证头 401");
  }
}

void test_total_does_not_leak_and_no_content_leak() {
  std::cout << "主数据集：total 不泄露隐藏题，列表不含隐藏用例\n";
  Env env("list_leak");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  std::vector<long long> hidden_ids;
  seed_main_dataset(env.db(), hidden_ids);
  httplib::Client cli("127.0.0.1", env.port());
  MainCounts c;

  // 游客/普通用户的 total 必须等于公开题数量；关键词过滤下也不得把隐藏题计入。
  {
    httplib::Params params{{"q", "隐藏"}};
    auto res = list_req(cli, params);
    json body = json::parse(res->body);
    check(res && res->status == 200 && body.value("total", -1) == 0,
          "游客搜索隐藏题关键词 total=0（不泄露）");
  }
  {
    httplib::Params params{{"tag", "隐藏"}};
    auto res = list_req(cli, params);
    json body = json::parse(res->body);
    check(res && res->status == 200 && body.value("total", -1) == 0,
          "游客按隐藏标签筛选 total=0（不泄露）");
  }
  {
    httplib::Params params{{"difficulty", "hard"}, {"page", "1"}};
    auto res = list_req(cli, params);
    json body = json::parse(res->body);
    // 公开 hard 10 道，隐藏 hard 1 道不得计入。
    check(res && res->status == 200 && body.value("total", -1) == c.hard,
          "游客 hard total=10（不含隐藏 hard）");
  }

  // 列表响应体不得包含隐藏用例内容。
  auto guest = list_req(cli, httplib::Params{{"page", "1"}});
  std::string body = guest->body;
  bool leaked = false;
  for (int i = 0; i < 3; ++i) {
    if (body.find("HIDDENTOKEN" + std::to_string(i) + "7654321") !=
            std::string::npos ||
        body.find("HIDDENOUTPUT" + std::to_string(i)) != std::string::npos) {
      leaked = true;
    }
  }
  check(!leaked, "游客列表不含隐藏用例输入/输出");
}

// ---------------------------------------------------------------------------
// 关键词搜索：大小写、中文、引号、LIKE 特殊字符
// ---------------------------------------------------------------------------

void test_keyword_search_special_chars() {
  std::cout << "关键词搜索：大小写/中文/引号/LIKE 特殊字符\n";
  Env env("list_search");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  oj::Database &db = env.db();

  insert_problem(db, "A+B Problem", "easy", "入门", 1);
  insert_problem(db, "a+b lowercase", "easy", "入门", 1);
  insert_problem(db, "进度 100% 完成", "medium", "特殊", 1);
  insert_problem(db, "进度 100X 完成", "medium", "特殊", 1);
  insert_problem(db, "下划线 a_b 测试", "hard", "特殊", 1);
  insert_problem(db, "引号 \"测试\" 专用", "hard", "特殊", 1);
  insert_problem(db, "中文关键词搜索题", "easy", "特殊", 1);
  std::int64_t tu = insert_problem(db, "图的最短路", "hard", "图", 1);
  std::int64_t tulun = insert_problem(db, "图论入门", "hard", "图论", 1);
  (void)tu;
  (void)tulun;

  httplib::Client cli("127.0.0.1", env.port());

  auto total_of = [&](const httplib::Params &params) -> long long {
    auto res = list_req(cli, params);
    if (!res || res->status != 200) return -1;
    json body = json::parse(res->body);
    return body.value("total", -1LL);
  };

  // ASCII 大小写不敏感。
  check(total_of(httplib::Params{{"q", "A+B"}}) == 2, "q=A+B 命中 2 条");
  check(total_of(httplib::Params{{"q", "a+b"}}) == 2,
        "q=a+b 与 A+B 结果一致（大小写不敏感）");
  // 中文子串。
  check(total_of(httplib::Params{{"q", "中文关键词"}}) == 1, "中文子串命中 1 条");
  // % 按字面匹配：只有「进度 100% 完成」，不误中 100X。
  check(total_of(httplib::Params{{"q", "100%"}}) == 1,
        "q=100% 按字面匹配（不把 % 当通配符）");
  check(total_of(httplib::Params{{"q", "100X"}}) == 1, "q=100X 命中 1 条");
  // _ 按字面匹配：只有 a_b，不匹配任意单字符。
  check(total_of(httplib::Params{{"q", "a_b"}}) == 1,
        "q=a_b 按字面匹配（不把 _ 当通配符）");
  // 引号不改变 SQL 结构，按字面搜索。
  check(total_of(httplib::Params{{"q", "\"测试\""}}) == 1, "q=含引号命中 1 条");
  // 反斜杠等其它特殊字符不报错。
  check(total_of(httplib::Params{{"q", "a\\b"}}) == 0,
        "q=a\\b 正常处理且无匹配");
  // 空白关键词视为不限。
  check(total_of(httplib::Params{{"q", "   "}}) == 9, "空白关键词表示不限");
  // 无匹配。
  check(total_of(httplib::Params{{"q", "不存在的关键字zzz"}}) == 0,
        "无匹配关键词 total=0");
}

// ---------------------------------------------------------------------------
// 通过人数与本人状态
// ---------------------------------------------------------------------------

void test_pass_count_and_solved_status() {
  std::cout << "通过人数与本人 AC 状态\n";
  FakeExecutor executor;
  Env env("list_stats", &executor);
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  oj::Database &db = env.db();

  std::int64_t pid = insert_problem(db, "统计题", "easy", "统计", 1);
  insert_testcase(db, pid, 0, "1\n", "42\n", false);
  httplib::Client cli("127.0.0.1", env.port());

  auto make_user = [&](const std::string &nick, const std::string &pw,
                       std::string &token) -> std::int64_t {
    std::string account = register_user(cli, nick, pw);
    int status = 0;
    token = login(cli, account, pw, status);
    return status == 200 ? user_id(db, account) : -1;
  };
  std::string alice_token, bob_token, carol_token, dave_token, eve_token;
  check(make_user("stat_alice", "Pw123456", alice_token) > 0, "alice 注册登录");
  check(make_user("stat_bob", "Pw123456", bob_token) > 0, "bob 注册登录");
  check(make_user("stat_carol", "Pw123456", carol_token) > 0, "carol 注册登录");
  check(make_user("stat_dave", "Pw123456", dave_token) > 0, "dave 注册登录");
  check(make_user("stat_eve", "Pw123456", eve_token) > 0, "eve 注册登录");

  const std::string ac_code = "int main() { return 0; }\n// OUT:42\n";
  const std::string wa_code = "int main() { return 0; }\n// OUT:0\n";
  json out;

  auto submit = [&](const std::string &token, const std::string &code,
                    const std::string &status) {
    int code_status = submit_code(cli, token, pid, code, out);
    check(code_status == 200 && out.value("status", "") == status,
          "提交返回 " + status);
  };

  submit(alice_token, ac_code, "AC");          // alice 首次 AC
  submit(alice_token, ac_code, "AC");          // 重复 AC，不增加人数
  submit(alice_token, wa_code, "WA");          // AC 后再失败，仍为已 AC
  submit(bob_token, ac_code, "AC");            // 第二人 AC
  submit(carol_token, wa_code, "WA");          // 仅失败，不计入
  submit(dave_token, wa_code, "WA");           // 仅失败，不计入

  auto list_for = [&](const std::string &auth, bool &ok) -> json {
    httplib::Params params{{"q", "统计题"}};
    auto res = list_req(cli, params, auth);
    ok = res && res->status == 200;
    return ok ? json::parse(res->body) : json();
  };

  // 游客：pass_count=2，且无 solved 字段。
  {
    bool ok = false;
    json body = list_for("", ok);
    check(ok, "游客列表 200");
    check(body["problems"].size() == 1, "命中统计题一条");
    check(body["problems"][0].value("pass_count", -1) == 2,
          "通过人数=2（重复 AC 与仅失败均不计）");
    check(!body["problems"][0].contains("solved"), "游客无本人状态字段");
  }

  auto solved_of = [&](const std::string &token, int expected) {
    bool ok = false;
    json body = list_for("Bearer " + token, ok);
    if (!ok || body["problems"].empty()) {
      check(false, "本人状态查询失败");
      return;
    }
    check(body["problems"][0].value("solved", expected != 0) == (expected != 0),
          std::string("solved=") + (expected ? "true" : "false"));
  };
  solved_of(alice_token, 1);  // 已 AC
  solved_of(bob_token, 1);    // 已 AC
  solved_of(carol_token, 0);  // 仅失败 -> 未 AC
  solved_of(dave_token, 0);   // 仅失败 -> 未 AC
  solved_of(eve_token, 0);    // 无记录 -> 未 AC

  // 客户端参数不能指定他人身份：carol 附带伪造参数仍是未 AC。
  {
    httplib::Params params{{"q", "统计题"},
                           {"user_id", "1"},
                           {"account", "stat_alice"},
                           {"viewer_user_id", "1"}};
    auto res = list_req(cli, params, "Bearer " + carol_token);
    json body = json::parse(res->body);
    check(res && res->status == 200 &&
              body["problems"][0].value("solved", true) == false,
          "伪造用户参数不改变本人状态");
  }

  // 列表响应不包含用户源码。
  {
    bool ok = false;
    json body = list_for("Bearer " + alice_token, ok);
    check(body.dump().find("OUT:42") == std::string::npos,
          "列表不含用户源码内容");
  }
}

// ---------------------------------------------------------------------------
// 分页边界、注入安全、id 空洞、含空格标签（测试审查补充）
// ---------------------------------------------------------------------------

void test_page_boundaries_exact_multiple() {
  std::cout << "分页边界：整页整除、末页/末页+1/最大页\n";
  Env env("list_page40");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  for (int i = 1; i <= 40; ++i) {
    insert_problem(env.db(), "页题" + std::to_string(100 + i), "easy", "分页", 1);
  }
  httplib::Client cli("127.0.0.1", env.port());

  auto fetch = [&](const char *page) -> json {
    httplib::Params params;
    if (page != nullptr) {
      params.emplace("page", page);
    }
    auto res = list_req(cli, params);
    if (!res || res->status != 200) {
      return json();
    }
    return json::parse(res->body);
  };

  json p1 = fetch(nullptr);
  check(!p1.is_null(), "默认页 200");
  if (p1.is_null()) return;
  check(p1["problems"].size() == 20, "默认页 20 条");
  check(p1.value("total", -1) == 40 && p1.value("total_pages", -1) == 2,
        "40 题 total=40 total_pages=2（整除边界）");

  json p2 = fetch("2");
  check(p2["problems"].size() == 20 && p2.value("total_pages", -1) == 2,
        "第 2 页 20 条且为末页");
  std::set<long long> seen;
  for (long long id : problem_ids(p1)) seen.insert(id);
  for (long long id : problem_ids(p2)) {
    if (!seen.insert(id).second) check(false, "跨页重复 id=" + std::to_string(id));
  }
  check(seen.size() == 40, "两页合计 40 道且无重复");

  json p3 = fetch("3");
  check(p3["problems"].empty() && p3.value("total", -1) == 40,
        "末页+1（第 3 页）为空且 total 不变");
  json pmax = fetch("1000000");
  check(pmax["problems"].empty() && pmax.value("total", -1) == 40,
        "最大页码 1000000 返回空列表且无偏移溢出");
  httplib::Params over{{"page", "1000001"}};
  auto res_over = list_req(cli, over);
  check(res_over && res_over->status == 400, "page=1000001 返回 400");
}

void test_injection_safe_and_admin_hidden_filter() {
  std::cout << "SQL 注入安全与管理员组合筛选\n";
  Env env("list_inject");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  std::vector<long long> hidden_ids;
  seed_main_dataset(env.db(), hidden_ids);
  httplib::Client cli("127.0.0.1", env.port());

  auto total_of = [&](const httplib::Params &params, const std::string &auth,
                      int &status) -> long long {
    auto res = list_req(cli, params, auth);
    status = res ? res->status : -1;
    if (!res || res->status != 200) {
      return -1;
    }
    return json::parse(res->body).value("total", -1LL);
  };

  int st = 0;
  check(total_of(httplib::Params{{"q", "' OR '1'='1"}}, "", st) == 0 &&
            st == 200,
        "q 注入载荷按普通字符串处理、无匹配");
  check(total_of(httplib::Params{{"q", "x'; DROP TABLE problems; --"}}, "", st) ==
                0 &&
            st == 200,
        "破坏性 q 载荷不改变 SQL 结构");
  check(total_of(httplib::Params{{"tag", "'),('"}}, "", st) == 0 && st == 200,
        "tag 注入载荷无匹配");
  {
    std::string err;
    oj::Statement cs;
    long long problems = -1;
    if (env.db().prepare("SELECT COUNT(*) FROM problems", cs, err) &&
        cs.step() == SQLITE_ROW) {
      problems = cs.column_int64(0);
    }
    check(problems == 48, "注入后题目表完整（48 行）");
  }

  // 管理员可见性筛选与 q/difficulty/tag 组合为 AND。
  int status = 0;
  std::string admin_token = login(cli, "admin", kAdminPassword, status);
  check(status == 200 && !admin_token.empty(), "admin 登录成功");
  check(change_password(cli, admin_token, kAdminPassword, "AdminNewPass1"),
        "admin 首次改密");
  const std::string auth = "Bearer " + admin_token;
  check(total_of(httplib::Params{{"visible", "0"}, {"q", "隐藏"}}, auth, st) == 3,
        "admin visible=0 + q=隐藏 → 3");
  check(total_of(httplib::Params{{"visible", "0"}, {"q", "题目"}}, auth, st) == 0,
        "admin visible=0 + q=题目 → 0（AND）");
  check(total_of(httplib::Params{{"visible", "0"}, {"difficulty", "easy"}}, auth,
                 st) == 1,
        "admin visible=0 + difficulty=easy → 1");
  check(total_of(httplib::Params{{"visible", "0"}, {"tag", "隐藏"}}, auth, st) == 3,
        "admin visible=0 + tag=隐藏 → 3");
}

void test_non_contiguous_ids_pagination() {
  std::cout << "非连续 id（删除中间题）的分页稳定性\n";
  Env env("list_gap");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  oj::Database &db = env.db();
  std::vector<long long> ids;
  for (int i = 1; i <= 25; ++i) {
    ids.push_back(
        insert_problem(db, "空题" + std::to_string(i), "easy", "空洞", 1));
  }
  long long removed = ids[4];
  {
    std::string err;
    oj::Statement stmt;
    db.prepare("DELETE FROM problems WHERE id = ?", stmt, err);
    stmt.bind(1, static_cast<sqlite3_int64>(removed));
    stmt.step();
  }

  httplib::Client cli("127.0.0.1", env.port());
  std::set<long long> seen;
  std::size_t sizes[3] = {0, 0, 0};
  for (int page = 1; page <= 2; ++page) {
    httplib::Params params{{"page", std::to_string(page)}};
    auto res = list_req(cli, params);
    json body = json::parse(res->body);
    auto page_ids = problem_ids(body);
    sizes[page] = page_ids.size();
    check(strictly_increasing(page_ids),
          "第 " + std::to_string(page) + " 页按 id 有序");
    for (long long id : page_ids) {
      if (!seen.insert(id).second) {
        check(false, "跨页重复 id=" + std::to_string(id));
      }
    }
  }
  check(sizes[1] == 20 && sizes[2] == 4, "24 题分 20+4 两页");
  check(seen.size() == 24, "合计 24 道且无重复");
  check(seen.count(removed) == 0, "已删除的 id 不出现在任何页");
  httplib::Params p1{{"page", "1"}};
  auto r1 = list_req(cli, p1);
  check(json::parse(r1->body).value("total", -1) == 24,
        "id 空洞下 total 与剩余数量一致");
}

void test_tag_internal_space_match() {
  std::cout << "含空格标签的完整匹配\n";
  Env env("list_tagspace");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) return;
  oj::Database &db = env.db();
  long long p1 = insert_problem(db, "两词标签", "easy", "two words", 1);
  long long p2 = insert_problem(db, "单词标签", "easy", "two", 1);
  long long p3 = insert_problem(db, "多词标签", "easy", "three,four", 1);
  long long p4 = insert_problem(db, "尾标签", "easy", "extra,two", 1);
  httplib::Client cli("127.0.0.1", env.port());

  auto body_for = [&](const std::string &tag) {
    httplib::Params params{{"tag", tag}};
    auto res = list_req(cli, params);
    return json::parse(res->body);
  };
  auto ids_for = [&](const std::string &tag) {
    json body = body_for(tag);
    std::vector<long long> values = problem_ids(body);
    std::set<long long> ids(values.begin(), values.end());
    return ids;
  };

  json a = body_for("two words");
  check(a.value("total", -1) == 1 && problem_ids(a)[0] == p1,
        "tag=two words 仅命中两词标签");
  // 完整标签 two 命中独立的 two 与 multi-tag 中的 two，但不匹配两词标签的子串。
  {
    std::set<long long> ids = ids_for("two");
    check(ids.size() == 2 && ids.count(p2) == 1 && ids.count(p4) == 1 &&
              ids.count(p1) == 0,
          "tag=two 命中完整标签 two（含多标签中的 two），不误配 two words");
  }
  json c = body_for("words");
  check(c.value("total", -1) == 0, "tag=words 不匹配 two words 的子串");
  json d = body_for("three");
  check(d.value("total", -1) == 1 && problem_ids(d)[0] == p3,
        "多标签题可按第一个标签命中");
  json e = body_for("extra");
  check(e.value("total", -1) == 1 && problem_ids(e)[0] == p4,
        "多标签题可按第一个标签 extra 命中");
}

} // namespace

int main() {
  test_no_filter_and_pagination();
  test_difficulty_and_tag_and_combined();
  test_visibility_roles_and_admin_filter();
  test_total_does_not_leak_and_no_content_leak();
  test_keyword_search_special_chars();
  test_pass_count_and_solved_status();
  test_page_boundaries_exact_multiple();
  test_injection_safe_and_admin_hidden_filter();
  test_non_contiguous_ids_pagination();
  test_tag_internal_space_match();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部题目列表查询集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
