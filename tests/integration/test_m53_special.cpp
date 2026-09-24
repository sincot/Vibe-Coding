// M5.3 判题异常与安全回归——特殊（破坏性类别）集成测试。
//
// 与 test_m53_judge_abuse.cpp 的常规有界测试分开：本文件包含死循环、持续输出、
// 受控超内存与进程数量增长等「危险类别」场景。为遵守测试规范：
//   - 本目标**不注册到 CTest**，常规 `ctest` 全量回归不会意外触发；
//   - 每个场景仍采用受控样例（有限时限、有限分配、有限次数尝试），并自带
//     宿主机内存自检：可用内存不足时标记 [SKIP] 未验证，绝不耗尽宿主机资源；
//   - 需要真正确认宿主机内存压力的场景由 OJ_M53_ALLOW_HOST_PRESSURE=1 显式开启，
//     并再次校验可用内存余量，默认跳过；
//   - 不使用无约束 fork 轰炸（seccomp 已禁止运行阶段创建进程，本测试用有限次
//     尝试证明零后代即可，不以此伪装成完整恶意进程隔离验证）。
//
// 显式执行：cmake --build build --target run_m53_special
// 需要宿主机压力场景：OJ_M53_ALLOW_HOST_PRESSURE=1 ./build/oj_m53_special

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "judge/judge.h"
#include "judge/local_executor.h"
#include "judge/status.h"

namespace {

using oj::judge::JudgeEngine;
using oj::judge::JudgeOptions;
using oj::judge::JudgeResult;
using oj::judge::JudgeStatus;
using oj::judge::JudgeTask;
using oj::judge::LocalExecutor;
using oj::judge::Testcase;
using oj::judge::TestcaseResult;

int g_failures = 0;
int g_skipped = 0;

void check(bool condition, const std::string &message) {
  if (condition) {
    std::cout << "  [PASS] " << message << "\n";
  } else {
    std::cout << "  [FAIL] " << message << "\n";
    ++g_failures;
  }
}

void skip(const std::string &message) {
  std::cout << "  [SKIP] " << message << " （未验证）\n";
  ++g_skipped;
}

class TempDir {
public:
  explicit TempDir(const std::string &label) {
    namespace fs = std::filesystem;
    fs::path base = fs::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
      fs::path candidate = base / (label + "_" + std::to_string(::getpid()) +
                                   "_" + std::to_string(i));
      std::error_code ec;
      fs::create_directories(candidate, ec);
      if (!ec) {
        path_ = candidate.string();
        return;
      }
    }
  }
  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }
  const std::string &path() const { return path_; }
  bool empty() const {
    std::error_code ec;
    return std::filesystem::is_empty(path_, ec);
  }

private:
  std::string path_;
};

bool no_leftover_children() {
  int status = 0;
  pid_t reaped = ::waitpid(-1, &status, WNOHANG);
  return reaped == -1 && errno == ECHILD;
}

// 读取 /proc/meminfo 的 MemAvailable（MiB）；无法读取返回 -1。
long long available_memory_mb() {
  std::ifstream in("/proc/meminfo");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("MemAvailable:", 0) == 0) {
      std::istringstream ss(line.substr(13));
      long long kb = 0;
      ss >> kb;
      return kb / 1024;
    }
  }
  return -1;
}

// 读取本进程所处 cgroup v2 的 memory.max（MiB）；无硬上限或无法读取返回 -1。
// 用于在「已被外层硬性 cgroup 内存上限保护」的专用环境中安全执行压力场景。
long long cgroup_memory_max_mb() {
  std::ifstream cg("/proc/self/cgroup");
  std::string line;
  std::string path;
  while (std::getline(cg, line)) {
    if (line.rfind("0::", 0) == 0) {
      path = line.substr(3);
      break;
    }
  }
  if (path.empty()) return -1;
  std::ifstream mx("/sys/fs/cgroup" + path + "/memory.max");
  std::string value;
  if (!std::getline(mx, value) || value == "max" || value.empty()) return -1;
  try {
    return std::stoll(value) / (1024 * 1024);
  } catch (...) {
    return -1;
  }
}

JudgeResult judge_src(const std::string &language, const std::string &source,
                      std::vector<Testcase> cases, int time_limit_ms,
                      const std::string &workspace_root,
                      long long memory_limit_kb = 65536,
                      int global_time_limit_ms = 60000) {
  LocalExecutor executor;
  JudgeOptions options;
  options.workspace_root = workspace_root;
  options.default_memory_limit_kb = memory_limit_kb;
  options.global_time_limit_ms = global_time_limit_ms;
  JudgeEngine engine(executor, options);
  JudgeTask task;
  task.language = language;
  task.source_code = source;
  task.testcases = std::move(cases);
  task.time_limit_ms = time_limit_ms;
  task.memory_limit_kb = memory_limit_kb;
  return engine.judge(task);
}

const TestcaseResult *first_case(const JudgeResult &result) {
  return result.cases.empty() ? nullptr : &result.cases.front();
}

long long elapsed_ms(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

const char *kCppSum =
    "#include <iostream>\n"
    "int main(){ long long a,b; if(!(std::cin>>a>>b)) return 0; "
    "std::cout<<(a+b)<<\"\\n\"; return 0; }\n";

// ---------------------------------------------------------------------------
// 持续输出：无限写 stdout，watchdog 必须按单点时限终止，采集有界且不无限排空。
// ---------------------------------------------------------------------------
void test_sustained_output_tle() {
  std::cout << "持续输出程序：单点时限终止、有界采集、不挂死\n";
  TempDir root("m53_special_out");
  const char *spam =
      "#include <cstdio>\n"
      "int main(){ for(;;) { putchar('x'); } return 0; }\n";
  auto start = std::chrono::steady_clock::now();
  JudgeResult result = judge_src("cpp17", spam, {{"", ""}}, 500, root.path());
  long long ms = elapsed_ms(start);
  check(result.status == JudgeStatus::TLE, "持续输出按单点时限判 TLE");
  const TestcaseResult *cs = first_case(result);
  check(cs != nullptr && cs->timed_out, "标记单点超时");
  check(cs != nullptr && cs->actual_output.size() <= 64 * 1024,
        "采集到的输出不超过 64KiB");
  check(ms < 8000, "持续输出被及时终止（耗时 < 8s）");
  check(no_leftover_children(), "无遗留子进程");
  check(root.empty(), "工作目录已清理");

  JudgeResult recovered =
      judge_src("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 2000, root.path());
  check(recovered.status == JudgeStatus::AC, "之后正常判题仍 AC");
}

// ---------------------------------------------------------------------------
// 受控超内存：有可靠 RSS 证据判 MLE；之后恢复正常。
// ---------------------------------------------------------------------------
void test_controlled_memory_exceeded() {
  std::cout << "受控超内存：RSS 采样证据判 MLE 并可恢复\n";
  TempDir root("m53_special_mem");
  if (available_memory_mb() < 700) {
    skip("可用内存不足 700 MiB，跳过受控超内存场景");
    return;
  }
  // 有限分配：每轮 4MB，最多 24 轮（96MB）；限制 32MB，采样应超限并终止。
  const char *hog =
      "#include <stdlib.h>\n"
      "#include <string.h>\n"
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
  auto start = std::chrono::steady_clock::now();
  JudgeResult result = judge_src("cpp17", hog, {{"", ""}}, 8000, root.path(),
                                 /*memory_limit_kb=*/32768);
  check(result.status == JudgeStatus::MLE, "有可靠 RSS 证据判 MLE");
  const TestcaseResult *cs = first_case(result);
  check(cs != nullptr && cs->memory_exceeded, "标记 memory_exceeded");
  check(cs != nullptr && cs->memory_kb > 32768,
        "观测峰值 RSS 超过限制（非 0/非伪造）");
  check(elapsed_ms(start) < 15000, "超内存被及时终止（耗时 < 15s）");
  check(no_leftover_children(), "无遗留子进程");

  JudgeResult recovered =
      judge_src("cpp17", kCppSum, {{"4 5\n", "9\n"}}, 2000, root.path());
  check(recovered.status == JudgeStatus::AC, "之后正常判题仍 AC");
}

// ---------------------------------------------------------------------------
// 进程数量增长：有限次 fork 尝试全部被拒、零后代；不做无约束 fork 轰炸。
// ---------------------------------------------------------------------------
void test_fork_attempts_denied_no_growth() {
  std::cout << "有限次进程创建尝试全部被拒、零后代增长\n";
  TempDir root("m53_special_fork");
  const char *source =
      "#include <stdio.h>\n"
      "#include <sys/types.h>\n"
      "#include <unistd.h>\n"
      "int main(){\n"
      "  int failures = 0;\n"
      "  for (int i = 0; i < 2000; ++i) {\n"
      "    pid_t p = fork();\n"
      "    if (p == 0) { _exit(0); }\n"
      "    if (p < 0) { ++failures; }\n"
      "    else { printf(\"FORK_SUCCEEDED %d\\n\", i); return 0; }\n"
      "  }\n"
      "  printf(\"ALL_DENIED %d\\n\", failures);\n"
      "  return 0;\n"
      "}\n";
  JudgeResult result =
      judge_src("c11", source, {{"", "ALL_DENIED 2000\n"}}, 4000, root.path());
  check(result.status == JudgeStatus::AC,
        "2000 次 fork 尝试全部被拒绝、未创建任何后代");
  check(no_leftover_children(), "无遗留子进程");
}

// ---------------------------------------------------------------------------
// 宿主机内存压力（默认跳过）：高内存上限下持续分配，验证 MLE 在耗尽宿主机前
// 生效。仅当显式设置 OJ_M53_ALLOW_HOST_PRESSURE=1 且可用内存余量充足时执行。
// ---------------------------------------------------------------------------
void test_host_memory_pressure_gated() {
  std::cout << "宿主机内存压力场景（默认跳过）\n";
  const char *allow = std::getenv("OJ_M53_ALLOW_HOST_PRESSURE");
  if (allow == nullptr || std::string(allow) != "1") {
    skip("未设置 OJ_M53_ALLOW_HOST_PRESSURE=1，跳过宿主机内存压力场景");
    return;
  }
  const long long limit_mb = 512;
  const long long avail = available_memory_mb();
  const long long cap_mb = cgroup_memory_max_mb();
  // 若外层以硬性 cgroup 内存上限保护（上限不超过判题内存上限 + 512MiB 余量），
  // 则即使 RSS 采样未及，内核也会在该 cgroup 内 OOM，宿主机不会被耗尽；此时允许
  // 执行压力场景。否则仍要求可用内存充足（原有保守门限）。
  const bool hard_capped = cap_mb > 0 && cap_mb <= limit_mb + 512;
  if (!hard_capped && avail < limit_mb + 700) {
    skip("可用内存不足且无硬性 cgroup 上限，跳过宿主机压力场景");
    return;
  }
  if (hard_capped) {
    std::cout << "  检测到硬性 cgroup 内存上限 " << cap_mb
              << " MiB，在受控压力场景中执行\n";
  }
  TempDir root("m53_special_pressure");
  const char *pressure =
      "#include <stdlib.h>\n"
      "#include <string.h>\n"
      "int main(){\n"
      "  const size_t MB = 1024*1024;\n"
      "  for (;;) {\n"
      "    char* p = (char*)malloc(8*MB);\n"
      "    if (!p) break;\n"
      "    memset(p, 1, 8*MB);\n"
      "  }\n"
      "  return 0;\n"
      "}\n";
  JudgeResult result =
      judge_src("cpp17", pressure, {{"", ""}}, 30000, root.path(),
                /*memory_limit_kb=*/limit_mb * 1024, /*global=*/40000);
  check(result.status == JudgeStatus::MLE,
        "高内存上限下仍以 RSS 证据判 MLE（未耗尽宿主机）");
  check(no_leftover_children(), "无遗留子进程");
}

// ---------------------------------------------------------------------------
// 尝试填满文件系统：用户程序写入受限于沙箱（/box 只读、/tmp 为沙箱自有 tmpfs），
// 宿主判题 tmpfs 可用空间不因用户写入显著减少；结束后无残留任务目录。
// ---------------------------------------------------------------------------
void test_tmpfs_fill_confined() {
  std::cout << "尝试填满文件系统：写入受限于沙箱、宿主 tmpfs 不受影响\n";
  namespace fs = std::filesystem;
  const std::string host_tmpfs = "/opt/oj-tmpfs";
  std::error_code ec;
  if (!fs::exists(host_tmpfs, ec)) {
    skip("未找到正式判题 tmpfs /opt/oj-tmpfs，跳过填满场景");
    return;
  }
  fs::space_info before = fs::space(host_tmpfs, ec);
  if (ec) {
    skip("无法读取 /opt/oj-tmpfs 空间信息，跳过填满场景");
    return;
  }
  const char *fill =
      "#include <stdio.h>\n"
      "#include <string.h>\n"
      "int main(){\n"
      "  char buf[65536]; memset(buf, 'A', sizeof buf);\n"
      "  unsigned long long total = 0;\n"
      "  for (int round = 0; round < 16; ++round) {\n"
      "    char name[64]; snprintf(name, sizeof name, \"/tmp/fill_%d.bin\", round);\n"
      "    FILE *f = fopen(name, \"wb\");\n"
      "    if (!f) continue;\n"
      "    for (int k = 0; k < 16; ++k) {\n"
      "      if (fwrite(buf, 1, sizeof buf, f) != sizeof buf) { fclose(f); printf(\"WRITE_FAILED %llu\\n\", total); return 0; }\n"
      "      total += sizeof buf;\n"
      "    }\n"
      "    fclose(f);\n"
      "  }\n"
      "  printf(\"WROTE %llu\\n\", total);\n"
      "  return 0;\n"
      "}\n";
  JudgeResult result = judge_src("c11", fill, {{"", ""}}, 3000, host_tmpfs);
  check(result.status != JudgeStatus::SYSERR,
        "填满尝试正常返回（非内部错误/不假死）");
  fs::space_info after = fs::space(host_tmpfs, ec);
  long long delta = static_cast<long long>(before.available) -
                    static_cast<long long>(after.available);
  check(delta < 2LL * 1024 * 1024,
        "宿主判题 tmpfs 可用空间未因用户写入显著减少（<2MiB）");
  check(no_leftover_children(), "无遗留子进程");
  std::error_code dir_ec;
  long long dirs = 0;
  for (fs::directory_iterator it(host_tmpfs, dir_ec), end; it != end;
       it.increment(dir_ec)) {
    ++dirs;
  }
  check(!dir_ec && dirs == 0, "判题工作目录无残留任务目录");
}

} // namespace

int main() {
  std::cout << "可用内存: " << available_memory_mb() << " MiB\n\n";
  test_sustained_output_tle();
  test_controlled_memory_exceeded();
  test_fork_attempts_denied_no_growth();
  test_tmpfs_fill_confined();
  test_host_memory_pressure_gated();

  std::cout << "\n";
  std::cout << "特殊测试：失败 " << g_failures << " 项，跳过（未验证）" << g_skipped
            << " 项\n";
  if (g_failures == 0) {
    std::cout << "全部已执行的 M5.3 特殊测试通过\n";
    return 0;
  }
  return 1;
}
