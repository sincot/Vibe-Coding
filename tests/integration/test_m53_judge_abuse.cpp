// M5.3 判题异常与安全回归——常规（有界、受控）集成测试。
//
// 本文件只包含「有界、受控、可安全纳入常规回归」的场景：不进行无约束 fork 轰炸、
// 不尝试耗尽宿主机内存、不填满共享 tmpfs。死循环/超内存/进程数量增长等更具破坏性
// 的场景放在同一小节的特殊测试 tests/integration/test_m53_special.cpp，默认不注册
// 到 CTest，避免全量常规回归误触发。
//
// 与既有 M1.5 judge_integration、M3.2、M3.3 sandbox_integration、M3.4
// m34_classification 不重复，仅补足 M5.3 审查发现的覆盖缺口：
//   A. 输入输出异常：程序不读取大输入、提前关闭标准输入后继续运行 —— 父进程写入
//      侧必须处理 EPIPE/背压，请求能结束，服务不因 SIGPIPE 退出（第 6 类）。
//   B. 内存分配失败（无可靠 RSS 证据）不得误判 MLE，须与资源超限区分（第 4 类）。
//   C. 进程创建变体 clone/vfork 亦被拒绝，而不仅是 fork（第 8 类）。
//   D. 向宿主进程发送信号（信号 0 权限探测）被用户/PID 命名空间拒绝，不能干扰
//      判题进程或其他任务（第 8 类）。
//   E. 受控异常序列之后重复提交正常程序，服务对两套语言均恢复正常（第 1 类）。
//
// 运行方式：ctest --test-dir build -R m53_judge_abuse --output-on-failure

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

void check(bool condition, const std::string &message) {
  if (condition) {
    std::cout << "  [PASS] " << message << "\n";
  } else {
    std::cout << "  [FAIL] " << message << "\n";
    ++g_failures;
  }
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

JudgeResult judge_src(const std::string &language, const std::string &source,
                      std::vector<Testcase> cases, int time_limit_ms,
                      const std::string &workspace_root,
                      long long memory_limit_kb = 262144,
                      JudgeOptions options = JudgeOptions{}) {
  LocalExecutor executor;
  options.workspace_root = workspace_root;
  options.default_memory_limit_kb = memory_limit_kb;
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

bool contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
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

const char *kC11Sum =
    "#include <stdio.h>\n"
    "int main(void){ long long a,b; "
    "if(scanf(\"%lld %lld\",&a,&b)!=2) return 0; "
    "printf(\"%lld\\n\", a+b); return 0; }\n";

// ---------------------------------------------------------------------------
// A. 输入输出异常（第 6 类）
// ---------------------------------------------------------------------------

// 程序完全不读取 stdin，同时父进程要写入 1 MiB 输入。父进程写入侧不能在子进程
// 不读取时无限阻塞；子进程结束（不读输入）后以 EPIPE 收尾，请求正常返回 AC。
void test_program_ignores_large_input() {
  std::cout << "程序不读取输入：1 MiB 输入写入不挂死\n";
  TempDir root("m53_ignore_stdin");
  const char *ignores =
      "#define _DEFAULT_SOURCE\n"
      "#include <cstdio>\n"
      "#include <unistd.h>\n"
      "int main(){\n"
      "  volatile long s = 0;\n"
      "  for (long i = 0; i < 60000000; ++i) s += i;\n"
      "  printf(\"OK\\n\");\n"
      "  return (s == 1) ? 1 : 0;\n"
      "}\n";
  std::string big_input(1 << 20, 'a'); // 1 MiB，程序从不读取
  auto start = std::chrono::steady_clock::now();
  JudgeResult result = judge_src("cpp17", ignores, {{big_input, "OK\n"}}, 4000,
                                 root.path());
  check(result.status == JudgeStatus::AC,
        "不读取输入但输出匹配判为 AC");
  check(elapsed_ms(start) < 8000, "大输入写入未挂死（耗时 < 8s）");
  check(no_leftover_children(), "无遗留子进程");
  check(root.empty(), "工作目录已清理");
}

// 程序提前关闭标准输入后继续运行：父进程写入已关闭的管道只能得到 EPIPE，绝不能
// 因 SIGPIPE 终止服务进程；请求仍正常结束并判 AC。
void test_program_closes_stdin_continues() {
  std::cout << "程序提前关闭标准输入后继续运行：EPIPE 不致命\n";
  TempDir root("m53_close_stdin");
  const char *closer =
      "#define _DEFAULT_SOURCE\n"
      "#include <stdio.h>\n"
      "#include <unistd.h>\n"
      "int main(){\n"
      "  close(STDIN_FILENO);\n"
      "  volatile long s = 0;\n"
      "  for (long i = 0; i < 20000000; ++i) s += i;\n"
      "  printf(\"DONE\\n\");\n"
      "  return (s == 1) ? 1 : 0;\n"
      "}\n";
  std::string big_input(1 << 20, 'b');
  auto start = std::chrono::steady_clock::now();
  JudgeResult result = judge_src("c11", closer, {{big_input, "DONE\n"}}, 4000,
                                 root.path());
  check(result.status == JudgeStatus::AC,
        "关闭 stdin 后继续运行、输出匹配判为 AC");
  if (result.status != JudgeStatus::AC) {
    const TestcaseResult *cs = first_case(result);
    std::cout << "      状态=" << oj::judge::judge_status_name(result.status)
              << " 消息=" << result.message
              << " 实际输出=" << (cs ? cs->actual_output : std::string("<none>"))
              << " 终止原因="
              << (cs ? oj::judge::termination_reason_name(cs->termination)
                     : std::string("<none>"))
              << "\n";
  }
  check(elapsed_ms(start) < 8000, "关闭 stdin 后未挂死（耗时 < 8s）");
  check(no_leftover_children(), "无遗留子进程");
}

// ---------------------------------------------------------------------------
// B. 内存分配失败不是 MLE（第 4 类）
// ---------------------------------------------------------------------------

// 申请远超物理内存的地址（不是持续占用、没有 RSS 增长）：无论分配返回 NULL 还是
// ASan 分配器中止，都不得判为 MLE —— MLE 必须有可靠 RSS 采样证据。
void test_allocation_failure_not_mle() {
  std::cout << "分配失败/超大申请不得误判 MLE\n";
  TempDir root("m53_alloc_fail");
  const char *huge =
      "#include <cstddef>\n"
      "#include <cstdio>\n"
      "#include <cstdlib>\n"
      "int main(){\n"
      "  void* p = std::malloc(static_cast<std::size_t>(1) << 46);\n"
      "  if (p == nullptr) { std::printf(\"ALLOC_NULL\\n\"); return 0; }\n"
      "  std::printf(\"ALLOC_OK\\n\");\n"
      "  return 0;\n"
      "}\n";
  auto start = std::chrono::steady_clock::now();
  JudgeResult result = judge_src("cpp17", huge, {{"", "ALLOC_NULL\n"}}, 3000,
                                 root.path(), /*memory_limit_kb=*/65536);
  const TestcaseResult *cs = first_case(result);
  check(result.status != JudgeStatus::MLE,
        "分配失败不是 MLE（无可靠 RSS 证据）");
  check(cs != nullptr && !cs->memory_exceeded,
        "未标记 memory_exceeded");
  check(elapsed_ms(start) < 6000, "分配失败路径不挂死（耗时 < 6s）");
  check(no_leftover_children(), "无遗留子进程");
}

// ---------------------------------------------------------------------------
// C. 进程创建变体被拒绝（第 8 类）
// ---------------------------------------------------------------------------

// 除 fork 外，clone/vfork 同样必须被 seccomp 拒绝，不能借此创建后代进程。
void test_clone_and_vfork_blocked() {
  std::cout << "clone/vfork 进程创建被拒绝\n";
  TempDir root("m53_clone");
  const char *source =
      "#define _GNU_SOURCE\n"
      "#include <sched.h>\n"
      "#include <stdio.h>\n"
      "#include <stdlib.h>\n"
      "#include <unistd.h>\n"
      "static int child_fn(void*) { _exit(0); }\n"
      "int main(){\n"
      "  void* stack = malloc(1 << 16);\n"
      "  if (!stack) return 1;\n"
      "  long c = (long)clone(child_fn, (char*)stack + (1 << 16), 0, NULL);\n"
      "  pid_t v = vfork();\n"
      "  if (c == -1 && v == -1) { printf(\"CLONE_VFORK_DENIED\\n\");"
      " return 0; }\n"
      "  printf(\"CREATED c=%ld v=%d\\n\", c, (int)v);\n"
      "  return 0;\n"
      "}\n";
  JudgeResult result =
      judge_src("c11", source, {{"", "CLONE_VFORK_DENIED\n"}}, 3000, root.path());
  check(result.status == JudgeStatus::AC, "clone/vfork 均被拒绝");
  check(no_leftover_children(), "无遗留子进程");
}

// ---------------------------------------------------------------------------
// D. 不能向宿主进程发信号 / 干扰其他任务（第 8、13 类）
// ---------------------------------------------------------------------------

// 用信号 0（仅做权限检查，不实际投递信号）探测沙箱进程能否向宿主判题进程发信号。
// 用户/PID 命名空间隔离应使其得到 EPERM，从而不能干扰判题进程或其他任务。
void test_signal_to_host_denied() {
  std::cout << "沙箱进程不能向宿主进程发信号（命名空间隔离）\n";
  TempDir root("m53_signal");
  const std::string host_pid = std::to_string(::getpid());
  const std::string source =
      "#include <errno.h>\n"
      "#include <signal.h>\n"
      "#include <stdio.h>\n"
      "#include <stdlib.h>\n"
      "int main(){\n"
      "  errno = 0;\n"
      "  int r = kill(" + host_pid + ", 0);\n"
      "  if (r == 0) { printf(\"SIGNAL_ALLOWED\\n\"); return 0; }\n"
      "  if (errno == EPERM || errno == ESRCH) {"
      " printf(\"SIGNAL_DENIED\\n\"); return 0; }\n"
      "  printf(\"SIGNAL_UNEXPECTED errno=%d\\n\", errno); return 0;\n"
      "}\n";
  JudgeResult result = judge_src("c11", source, {{"", "SIGNAL_DENIED\n"}}, 3000,
                                 root.path());
  check(result.status == JudgeStatus::AC,
        "信号 0 权限探测被拒绝（EPERM/ESRCH），不能干扰宿主任务");
  if (result.status != JudgeStatus::AC) {
    const TestcaseResult *cs = first_case(result);
    std::cout << "      实际输出: "
              << (cs ? cs->actual_output : std::string("<none>")) << "\n";
  }
  check(no_leftover_children(), "无遗留子进程");
}

// ---------------------------------------------------------------------------
// E. 异常序列后恢复正常（第 1 类）
// ---------------------------------------------------------------------------

// 在多个受控异常（信号崩溃、非零退出、越界、分配失败）之后，两套语言重复提交
// 正常程序仍判 AC，确认服务与判题核心未受影响。
void test_recovery_after_anomalies() {
  std::cout << "受控异常序列之后恢复正常判题（两套语言）\n";
  TempDir root("m53_recover");

  judge_src("cpp17", "#include <cstdlib>\nint main(){ std::abort(); }\n",
            {{"", ""}}, 3000, root.path());
  judge_src("cpp17", "int main(){ return 3; }\n", {{"", ""}}, 3000,
            root.path());
  judge_src("cpp17",
            "#include <cstdio>\n#include <cstdlib>\n"
            "int main(){ int* p=(int*)malloc(4*sizeof(int)); p[8]=1;"
            " printf(\"x\\n\"); return 0; }\n",
            {{"", "x\n"}}, 3000, root.path());
  judge_src("c11",
            "#include <stdlib.h>\n"
            "int main(){ void* p = malloc((size_t)1<<46);"
            " return p ? 0 : 9; }\n",
            {{"", ""}}, 3000, root.path());

  for (int round = 0; round < 2; ++round) {
    JudgeResult cpp = judge_src("cpp17", kCppSum, {{"2 3\n", "5\n"}}, 3000,
                                root.path());
    check(cpp.status == JudgeStatus::AC, "异常后 C++17 正常程序仍 AC");
    JudgeResult c11 = judge_src("c11", kC11Sum, {{"7 8\n", "15\n"}}, 3000,
                                root.path());
    check(c11.status == JudgeStatus::AC, "异常后 C11 正常程序仍 AC");
  }
  check(root.empty(), "异常与恢复路径均未遗留工作目录");
  check(no_leftover_children(), "无遗留子进程");
}

} // namespace

int main() {
  test_program_ignores_large_input();
  test_program_closes_stdin_continues();
  test_allocation_failure_not_mle();
  test_clone_and_vfork_blocked();
  test_signal_to_host_denied();
  test_recovery_after_anomalies();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部 M5.3 判题异常常规集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
