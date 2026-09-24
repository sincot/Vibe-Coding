// M5.3 判题异常与安全回归——服务级（HTTP + JudgeManager + SubmitService + SQLite）
// 补充集成测试。
//
// 背景：M5.3 既有新增测试 `test_m53_judge_abuse.cpp`/`test_m53_special.cpp` 在
// JudgeEngine + LocalExecutor 层验证异常与隔离；`test_m43_problem_page_api.cpp`
// 用 FakeExecutor 验证输出截断响应字段。本文件补足「经真实提交接口、真实 g++/gcc
// 与真实沙箱、并落库」的服务级覆盖缺口：
//   - 网络/文件/进程创建等隔离尝试经 HTTP 提交 -> 被阻止、据实判定、结果持久化；
//   - 真实超内存经 HTTP -> MLE（reason=memory_exceeded、峰值 RSS 证据）并持久化；
//   - 真实超大输出经 HTTP -> 非 AC、output_truncated 持久化；
//   - 一整串恶意提交之后正常提交仍 AC、计数与状态一致、健康检查可用、无遗留子进程。
//
// 隔离手段沿用 M1.6 submit_api：/tmp 下隔离临时库 + 随机端口真实 HTTP 服务，
// 默认 LocalExecutor（真实编译器 + 沙箱）。受控样例：有限分配、有限输出、单次
// fork 尝试，绝不进行无约束 fork 轰炸或耗尽宿主机内存。
//
// 运行方式：ctest --test-dir build -R m53_service_abuse --output-on-failure

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "auth/jwt.h"
#include "db/database.h"
#include "db/schema.h"
#include "db/submissions.h"
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

const std::string kTestSecret = "it-m53-secret-0123456789abcdef";
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
  std::filesystem::path path() const { return path_; }

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
                                               /*executor=*/nullptr);
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

private:
  TempDir dir_;
  std::unique_ptr<oj::Database> db_;
  std::unique_ptr<oj::HttpServer> server_;
  int port_ = 0;
  bool started_ = false;
};

// ---------------------------------------------------------------------------
// DB / HTTP 辅助
// ---------------------------------------------------------------------------

std::int64_t insert_problem(oj::Database &db, const std::string &title, int visible,
                            int time_limit_ms, int memory_limit_kb) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("INSERT INTO problems (title, description, difficulty, tags, "
                  "visible, time_limit_ms, memory_limit_kb) VALUES "
                  "(?, '题面', 'easy', '测试', ?, ?, ?) RETURNING id",
                  stmt, err)) {
    return -1;
  }
  stmt.bind(1, title);
  stmt.bind(2, visible);
  stmt.bind(3, time_limit_ms);
  stmt.bind(4, memory_limit_kb);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

bool insert_testcase(oj::Database &db, std::int64_t problem_id, int ord,
                     const std::string &input, const std::string &output) {
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

std::int64_t user_id(oj::Database &db, const std::string &account) {
  std::string err;
  oj::Statement stmt;
  db.prepare("SELECT id FROM users WHERE account = ?", stmt, err);
  stmt.bind(1, account);
  return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
}

struct User {
  std::string account;
  std::string token;
  std::int64_t id = 0;
};

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

User make_user(httplib::Client &cli, oj::Database &db, const std::string &nick,
               const std::string &pw) {
  User user;
  user.account = register_user(cli, nick, pw);
  auto res = cli.Post("/api/login",
                      "{\"account\":\"" + user.account + "\",\"password\":\"" +
                          pw + "\"}",
                      "application/json");
  if (res && res->status == 200) {
    user.token = json::parse(res->body).value("token", "");
  }
  user.id = user_id(db, user.account);
  return user;
}

httplib::Result submit(httplib::Client &cli, const std::string &token,
                       const std::string &problem_id, const std::string &language,
                       const std::string &code) {
  json body;
  body["language"] = language;
  body["code"] = code;
  httplib::Headers h{{"Authorization", "Bearer " + token}};
  return cli.Post(("/api/problems/" + problem_id + "/submit").c_str(), h,
                  body.dump(), "application/json");
}

bool find_submission(oj::Database &db, std::int64_t id, std::string &status) {
  oj::SubmissionStore store(db);
  bool found = false;
  oj::SubmissionRecord record;
  std::string err;
  store.find_by_id(id, found, record, err);
  if (found) {
    status = record.status;
  }
  return found;
}

bool read_status(oj::Database &db, std::int64_t uid, std::int64_t pid,
                 oj::UserProblemStatusRecord &out) {
  oj::UserProblemStatusStore store(db);
  bool found = false;
  std::string err;
  store.find(uid, pid, found, out, err);
  return found;
}

bool no_leftover_children() {
  int status = 0;
  pid_t reaped = ::waitpid(-1, &status, WNOHANG);
  return reaped == -1 && errno == ECHILD;
}

const char *kCppSum =
    "#include <iostream>\n"
    "int main(){ long long a,b; if(!(std::cin>>a>>b)) return 0; "
    "std::cout<<(a+b)<<\"\\n\"; return 0; }\n";

// ---------------------------------------------------------------------------
// T-511 服务级隔离：网络 / 文件 / 进程创建尝试经 HTTP 提交均被阻止
// ---------------------------------------------------------------------------

void test_sandbox_isolation_via_http() {
  std::cout << "服务级隔离：网络/文件/进程创建尝试经 HTTP 提交被阻止\n";
  Env env("m53_svc_iso");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m53_iso", "IsoPw1234");
  check(!u.token.empty(), "用户登录成功");

  // 宿主哨兵文件：沙箱内不可读、不可写；用于确认未被改写。
  TempDir host("m53_iso_host");
  const std::string sentinel = (host.path() / "sentinel.txt").string();
  {
    std::ofstream out(sentinel);
    out << "HOST-ONLY-SECRET";
  }

  std::int64_t pid = insert_problem(env.db(), "隔离题", 1, 3000, 65536);
  insert_testcase(env.db(), pid, 0, "", "SANDBOX_OK\n");
  const std::string pids = std::to_string(pid);

  // 依次尝试网络 socket、进程创建（fork）、读取宿主哨兵文件；任一成功即打印
  // 对应标记使其输出不匹配，从而暴露隔离缺口。
  std::string source =
      "#include <cstdio>\n"
      "#include <fcntl.h>\n"
      "#include <sys/socket.h>\n"
      "#include <sys/types.h>\n"
      "#include <unistd.h>\n"
      "int main(){\n"
      "  bool ok = true;\n"
      "  if (socket(AF_INET, SOCK_STREAM, 0) >= 0) { printf(\"NET_OPEN\\n\");"
      " ok = false; }\n"
      "  pid_t c = fork();\n"
      "  if (c == 0) { _exit(0); }\n"
      "  if (c >= 0) { printf(\"FORK_OK\\n\"); ok = false; }\n"
      "  int fd = open(\"" + sentinel + "\", O_RDONLY);\n"
      "  if (fd >= 0) { close(fd); printf(\"FILE_READ\\n\"); ok = false; }\n"
      "  if (ok) { printf(\"SANDBOX_OK\\n\"); }\n"
      "  return 0;\n"
      "}\n";

  auto res = submit(cli, u.token, pids, "cpp17", source);
  check(res && res->status == 200, "隔离尝试提交返回 200");
  std::int64_t sub_id = 0;
  if (res && res->status == 200) {
    json body = json::parse(res->body);
    sub_id = body.value("id", 0LL);
    check(body.value("status", "") == "AC",
          "网络/文件/进程创建均被阻止，程序据实判 AC");
    if (body.value("status", "") != "AC") {
      std::cout << "      实际状态=" << body.value("status", "")
                << " results=" << body.dump() << "\n";
    }
  }
  std::string stored;
  check(find_submission(env.db(), sub_id, stored) && stored == "AC",
        "隔离结果已持久化为 AC");

  std::ifstream in(sentinel);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  check(content == "HOST-ONLY-SECRET", "宿主哨兵文件未被改写");
  env.stop();
}

// ---------------------------------------------------------------------------
// T-512 服务级真实 MLE
// ---------------------------------------------------------------------------

void test_real_mle_via_http() {
  std::cout << "服务级超内存：经 HTTP 触发真实 MLE 并持久化\n";
  Env env("m53_svc_mle");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m53_mle", "MlePw1234");
  check(!u.token.empty(), "用户登录成功");

  std::int64_t pid = insert_problem(env.db(), "内存题", 1, 8000, 32768);
  insert_testcase(env.db(), pid, 0, "", "");
  const std::string pids = std::to_string(pid);

  // 受控分配：每轮 4MiB、最多 24 轮（96MiB），每轮忙等保证 RSS 采样能观察到增长。
  const char *hog =
      "#include <cstdlib>\n"
      "#include <cstring>\n"
      "int main(){\n"
      "  const size_t MB = 1024*1024;\n"
      "  for (int i = 0; i < 24; ++i) {\n"
      "    char* p = (char*)malloc(4*MB);\n"
      "    if (!p) break;\n"
      "    memset(p, 1, 4*MB);\n"
      "    volatile long s = 0; for (long k = 0; k < 8000000; ++k) s += k;\n"
      "  }\n"
      "  return 0;\n"
      "}\n";
  auto res = submit(cli, u.token, pids, "cpp17", hog);
  check(res && res->status == 200, "超内存提交返回 200（判题结果非 HTTP 故障）");
  std::int64_t sub_id = 0;
  bool mle_reason = false;
  bool peak_over = false;
  if (res && res->status == 200) {
    json body = json::parse(res->body);
    sub_id = body.value("id", 0LL);
    check(body.value("status", "") == "MLE", "真实超内存判为 MLE");
    if (!body["results"].empty()) {
      const json &c0 = body["results"][0];
      mle_reason = c0.value("reason", "") == "memory_exceeded";
      peak_over = c0.contains("memory_kb") && c0["memory_kb"].is_number() &&
                  c0["memory_kb"].get<long long>() > 32768;
    }
  }
  check(mle_reason, "逐点 reason=memory_exceeded（可靠 RSS 证据）");
  check(peak_over, "观测峰值 RSS 超过题目上限（非 0/非伪造）");
  std::string stored;
  check(find_submission(env.db(), sub_id, stored) && stored == "MLE",
        "MLE 结果已持久化");
  oj::UserProblemStatusRecord st;
  check(read_status(env.db(), u.id, pid, st) && st.submit_count == 1 &&
            !st.accepted,
        "MLE 计入 submit_count 且不置 AC");
  env.stop();
}

// ---------------------------------------------------------------------------
// T-513 服务级真实超大输出
// ---------------------------------------------------------------------------

void test_output_flood_via_http() {
  std::cout << "服务级超大输出：经 HTTP 判非 AC 且标记截断\n";
  Env env("m53_svc_out");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m53_out", "OutPw1234");
  check(!u.token.empty(), "用户登录成功");

  std::int64_t pid = insert_problem(env.db(), "输出题", 1, 4000, 65536);
  insert_testcase(env.db(), pid, 0, "", "0\n");
  const std::string pids = std::to_string(pid);

  const char *spam =
      "#include <cstdio>\n"
      "int main(){ for(int i=0;i<200000;i++) printf(\"%d\\n\", i);"
      " return 0; }\n";
  auto res = submit(cli, u.token, pids, "cpp17", spam);
  check(res && res->status == 200, "超大输出提交返回 200");
  std::int64_t sub_id = 0;
  std::string status;
  bool truncated = false;
  if (res && res->status == 200) {
    json body = json::parse(res->body);
    sub_id = body.value("id", 0LL);
    status = body.value("status", "");
    if (!body["results"].empty()) {
      truncated = body["results"][0].value("output_truncated", false);
    }
  }
  check(status != "AC", "超大输出未判 AC");
  check(status == "RE", "超大输出按约定判 RE");
  check(truncated, "逐点 output_truncated=true 并随结果返回");
  std::string stored;
  check(find_submission(env.db(), sub_id, stored) && stored != "AC",
        "超大输出结果已持久化且非 AC");
  env.stop();
}

// ---------------------------------------------------------------------------
// T-514 恶意序列之后服务恢复
// ---------------------------------------------------------------------------

void test_service_recovers_after_abuse() {
  std::cout << "恶意提交序列后：正常提交仍 AC、统计一致、服务可用\n";
  Env env("m53_svc_recover");
  check(env.ok(), "服务启动成功");
  if (!env.ok()) {
    return;
  }
  httplib::Client cli = make_client(env.port());
  User u = make_user(cli, env.db(), "m53_recover", "RcvPw1234");
  check(!u.token.empty(), "用户登录成功");

  std::int64_t pid = insert_problem(env.db(), "恢复题", 1, 2000, 65536);
  insert_testcase(env.db(), pid, 0, "2 3\n", "5\n");
  const std::string pids = std::to_string(pid);

  // 先跑一串受控恶意/异常提交：死循环、崩溃、越界。
  const char *loop = "int main(){ volatile unsigned long long c=0; while(true){"
                     " c++; } return 0; }\n";
  const char *abort_src = "#include <cstdlib>\nint main(){ std::abort(); }\n";
  const char *oob =
      "#include <cstdio>\n#include <cstdlib>\n"
      "int main(){ int* p=(int*)malloc(4*sizeof(int)); p[8]=1;"
      " printf(\"x\\n\"); return 0; }\n";
  auto r1 = submit(cli, u.token, pids, "cpp17", loop);
  auto r2 = submit(cli, u.token, pids, "cpp17", abort_src);
  auto r3 = submit(cli, u.token, pids, "cpp17", oob);
  check(r1 && json::parse(r1->body).value("status", "") == "TLE", "第 1 次异常判 TLE");
  check(r2 && json::parse(r2->body).value("status", "") == "RE", "第 2 次异常判 RE");
  check(r3 && json::parse(r3->body).value("status", "") != "AC",
        "第 3 次异常未判 AC");

  // 随后正常提交应判 AC，统计与状态一致。
  auto ok = submit(cli, u.token, pids, "cpp17", kCppSum);
  check(ok && ok->status == 200 && json::parse(ok->body).value("status", "") == "AC",
        "恶意序列后正常提交判 AC");
  int health = -1;
  if (auto h = cli.Get("/api/health")) {
    health = h->status;
  }
  check(health == 200, "恶意序列后健康检查仍返回 200");
  oj::UserProblemStatusRecord st;
  check(read_status(env.db(), u.id, pid, st) && st.accepted &&
            st.submit_count == 4,
        "4 次提交全部计数、最终状态为 accepted");
  check(no_leftover_children(), "无遗留判题子进程");
  env.stop();
}

} // namespace

int main() {
  test_sandbox_isolation_via_http();
  test_real_mle_via_http();
  test_output_flood_via_http();
  test_service_recovers_after_abuse();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部 M5.3 服务级判题异常集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
