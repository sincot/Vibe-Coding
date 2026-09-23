// M3.3 运行隔离与资源限制集成测试（真实 Linux 进程）。
//
// 使用真实的 g++/gcc、命名空间（user/mount/pid/net/ipc/uts）、chroot 最小根目录、
// setrlimit 与 seccomp-bpf，在隔离临时工作目录中执行，不触碰正式数据库，不读取
// 真实秘密。所有恶意样例均为受控样例：有限的内存分配与单次 fork 尝试，绝不进行
// fork 轰炸、耗尽宿主机内存或填满共享 tmpfs。
//
// 覆盖（对应 SPEC M3.3 与工程要求）：
//   - 沙箱自检：命名空间/最小根目录/seccomp/setrlimit 可用；
//   - 正常 C++17、C11 程序在沙箱中编译、运行并得到预期结果；
//   - 目录访问隔离：不能读取沙箱外的哨兵文件、/etc/passwd，不能越权写文件，
//     但可读取自己工作目录内的文件（隔离而非全禁）；
//   - 网络系统调用、fork/clone 进程创建被拒绝；
//   - /proc 被 PID 命名空间隔离，仅可见本任务进程；
//   - CPU 超时、内存超限被实际强制（非常数值采集），且之后仍可正常判题；
//   - 标准输出 64KB 上限：接近/等于上限仍 AC，超过上限不判 AC；
//   - 大量标准错误有界采集且不挂死；
//   - ASan/UBSan 兼容：正常样例运行，越界样例产生 AddressSanitizer 诊断；
//   - 沙箱/工作目录失败时明确 SYSERR，绝不无保护执行；
//   - 环境不泄露服务密钥；各路径无遗留子进程与临时目录。
//
// 运行方式：ctest --test-dir build -R sandbox_integration --output-on-failure
// 或直接执行 build/oj_sandbox_integration。

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "judge/judge.h"
#include "judge/local_executor.h"
#include "judge/sandbox.h"
#include "judge/status.h"

namespace {

using oj::judge::CompileRequest;
using oj::judge::JudgeEngine;
using oj::judge::JudgeOptions;
using oj::judge::JudgeResult;
using oj::judge::JudgeStatus;
using oj::judge::JudgeTask;
using oj::judge::LocalExecutor;
using oj::judge::ProcessResult;
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
                      long long memory_limit_kb = 65536,
                      const std::vector<std::string> &extra_flags = {},
                      int global_time_limit_ms = 60000) {
  LocalExecutor executor;
  JudgeOptions options;
  options.workspace_root = workspace_root;
  options.extra_compile_flags = extra_flags;
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

bool contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// 样例源码
// ---------------------------------------------------------------------------

const char *kCppSum =
    "#include <iostream>\n"
    "int main(){ long long a,b; if(!(std::cin>>a>>b)) return 0; "
    "std::cout<<(a+b)<<\"\\n\"; return 0; }\n";

const char *kCSum =
    "#include <stdio.h>\n"
    "int main(){ long long a=0,b=0; if(scanf(\"%lld %lld\",&a,&b)!=2) return 0; "
    "printf(\"%lld\\n\",a+b); return 0; }\n";

// ---------------------------------------------------------------------------
// 测试
// ---------------------------------------------------------------------------

void test_sandbox_self_test() {
  std::cout << "沙箱自检：命名空间/最小根目录/seccomp/setrlimit 可用\n";
  TempDir root("sb_selftest");
  std::string error;
  check(LocalExecutor::sandbox_self_test(root.path(), error),
        "沙箱自检成功: " + error);
}

void test_normal_programs_sandboxed() {
  std::cout << "正常 C++17 / C11 程序在沙箱中编译运行\n";
  TempDir root("sb_normal");
  JudgeResult cpp = judge_src("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 2000,
                              root.path());
  check(cpp.status == JudgeStatus::AC, "C++17 A+B 判为 AC");
  check(cpp.compile_ok, "C++17 编译成功");

  JudgeResult c = judge_src("c11", kCSum, {{"10 20\n", "30\n"}}, 2000,
                            root.path());
  check(c.status == JudgeStatus::AC, "C11 A+B 判为 AC");
  check(root.empty(), "正常判题后工作目录已清理");
  check(no_leftover_children(), "正常判题后无遗留子进程");
}

void test_filesystem_isolation() {
  std::cout << "目录访问隔离：不能读取沙箱外文件，不能越权写入\n";
  TempDir root("sb_fs");
  const std::string sentinel = root.path() + "/sentinel.txt";
  {
    std::ofstream out(sentinel);
    out << "TOP-SECRET-SENTINEL";
  }

  std::string source =
      "#include <stdio.h>\n"
      "#include <fcntl.h>\n"
      "#include <unistd.h>\n"
      "int main(){\n"
      "  char buf[128];\n"
      "  int fd = open(\"" + sentinel + "\", O_RDONLY);\n"
      "  if (fd >= 0) { int n=read(fd,buf,127); if(n>0){buf[n]=0;"
      " printf(\"HOST_LEAK %s\\n\",buf); return 0;} }\n"
      "  fd = open(\"/etc/passwd\", O_RDONLY);\n"
      "  if (fd >= 0) { printf(\"PASSWD_LEAK\\n\"); return 0; }\n"
      "  fd = open(\"/box/../sentinel.txt\", O_RDONLY);\n"
      "  if (fd >= 0) { printf(\"REL_LEAK\\n\"); return 0; }\n"
      "  fd = open(\"/box/program\", O_RDONLY);\n"
      "  if (fd >= 0) { close(fd); printf(\"BLOCKED READ_OK\\n\");"
      " return 0; }\n"
      "  printf(\"BLOCKED READ_FAIL\\n\"); return 0;\n"
      "}\n";
  JudgeResult read_result =
      judge_src("c11", source, {{"", "BLOCKED READ_OK\n"}}, 3000, root.path());
  check(read_result.status == JudgeStatus::AC,
        "沙箱外文件不可读、自身工作目录文件可读");
  if (read_result.status != JudgeStatus::AC) {
    const TestcaseResult *cs = first_case(read_result);
    std::cout << "      实际输出: "
              << (cs ? cs->actual_output : std::string("<none>")) << "\n";
  }

  const std::string write_source =
      "#include <stdio.h>\n"
      "#include <fcntl.h>\n"
      "#include <unistd.h>\n"
      "int main(){\n"
      "  int fd = open(\"" + sentinel + "\", O_WRONLY|O_CREAT|O_TRUNC, 0644);\n"
      "  if (fd >= 0) { write(fd,\"x\",1); close(fd);"
      " printf(\"HOST_WRITE_OK\\n\"); return 0; }\n"
      "  fd = open(\"evil.txt\", O_WRONLY|O_CREAT|O_TRUNC, 0644);\n"
      "  if (fd >= 0) { write(fd,\"x\",1); close(fd);"
      " printf(\"BOX_WRITE_OK\\n\"); return 0; }\n"
      "  fd = open(\"/box/program\", O_WRONLY);\n"
      "  if (fd >= 0) { printf(\"EXE_WRITE_OK\\n\"); return 0; }\n"
      "  printf(\"WRITE_BLOCKED\\n\"); return 0;\n"
      "}\n";
  JudgeResult write_result = judge_src("c11", write_source,
                                       {{"", "WRITE_BLOCKED\n"}}, 3000,
                                       root.path());
  check(write_result.status == JudgeStatus::AC,
        "沙箱外与只读工作目录写入均被拒绝");

  std::ifstream in(sentinel);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  check(content == "TOP-SECRET-SENTINEL", "宿主哨兵文件未被改写");
}

void test_network_blocked() {
  std::cout << "网络系统调用被拒绝\n";
  TempDir root("sb_net");
  const char *source =
      "#include <stdio.h>\n"
      "#include <sys/socket.h>\n"
      "int main(){\n"
      "  if (socket(AF_INET, SOCK_STREAM, 0) >= 0) {"
      " printf(\"SOCKET_OK\\n\"); return 0; }\n"
      "  if (socket(AF_UNIX, SOCK_STREAM, 0) >= 0) {"
      " printf(\"UNIX_OK\\n\"); return 0; }\n"
      "  if (socketpair(AF_UNIX, SOCK_STREAM, 0, (int[]){0,0}) >= 0) {"
      " printf(\"PAIR_OK\\n\"); return 0; }\n"
      "  printf(\"NET_BLOCKED\\n\"); return 0;\n"
      "}\n";
  JudgeResult result =
      judge_src("c11", source, {{"", "NET_BLOCKED\n"}}, 3000, root.path());
  check(result.status == JudgeStatus::AC, "socket/socketpair 返回失败");
}

void test_process_creation_blocked() {
  std::cout << "进程创建（fork）被拒绝\n";
  TempDir root("sb_fork");
  const char *source =
      "#include <stdio.h>\n"
      "#include <sys/types.h>\n"
      "#include <unistd.h>\n"
      "int main(){\n"
      "  pid_t p = fork();\n"
      "  if (p == 0) { printf(\"CHILD_RAN\\n\"); _exit(0); }\n"
      "  if (p > 0) { printf(\"FORK_OK\\n\"); return 0; }\n"
      "  printf(\"FORK_BLOCKED\\n\"); return 0;\n"
      "}\n";
  JudgeResult result =
      judge_src("c11", source, {{"", "FORK_BLOCKED\n"}}, 3000, root.path());
  check(result.status == JudgeStatus::AC, "fork 被 seccomp 拒绝");
  check(no_leftover_children(), "无遗留子进程");
}

void test_proc_isolated() {
  std::cout << "/proc 被 PID 命名空间隔离\n";
  TempDir root("sb_proc");
  const char *source =
      "#include <stdio.h>\n"
      "#include <dirent.h>\n"
      "int main(){\n"
      "  DIR* d = opendir(\"/proc\");\n"
      "  if (!d) { printf(\"NO_PROC\\n\"); return 0; }\n"
      "  int count = 0; struct dirent* e;\n"
      "  while ((e = readdir(d))) {"
      " if (e->d_name[0] >= '0' && e->d_name[0] <= '9') count++; }\n"
      "  closedir(d);\n"
      "  if (count > 0 && count < 50) printf(\"PROC_ISOLATED\\n\");\n"
      "  else printf(\"PROC_UNEXPECTED %d\\n\", count);\n"
      "  return 0;\n"
      "}\n";
  JudgeResult result =
      judge_src("c11", source, {{"", "PROC_ISOLATED\n"}}, 3000, root.path());
  check(result.status == JudgeStatus::AC,
        "/proc 仅暴露本任务进程（隔离而非宿主 /proc）");
}

void test_cpu_timeout_and_recovery() {
  std::cout << "CPU 超时被强制终止，之后仍可正常判题\n";
  TempDir root("sb_cpu");
  const char *spin = "int main(){ volatile long x=0; for(;;) x++; }\n";
  auto start = std::chrono::steady_clock::now();
  JudgeResult timed = judge_src("cpp17", spin, {{"", ""}}, 400, root.path());
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
  check(timed.status == JudgeStatus::TLE, "死循环判为 TLE");
  check(elapsed < 5000, "超时后及时返回（未等待自然结束）");
  check(no_leftover_children(), "超时后无遗留子进程");

  JudgeResult recovered =
      judge_src("cpp17", kCppSum, {{"2 3\n", "5\n"}}, 2000, root.path());
  check(recovered.status == JudgeStatus::AC, "之后正常判题仍为 AC");
}

void test_memory_limit_and_recovery() {
  std::cout << "内存限制被实际强制（RSS 采样并终止）\n";
  TempDir root("sb_mem");
  // 受控分配：每轮 1MB，共 80MB，远高于测试用 32MB 限制；每轮短暂忙等，
  // 保证内存采样能观察到增长。绝不进行无约束分配。
  const char *hog =
      "#include <stdlib.h>\n"
      "#include <string.h>\n"
      "int main(){\n"
      "  const size_t MB = 1024*1024;\n"
      "  for (int i = 0; i < 80; ++i) {\n"
      "    char* p = (char*)malloc(MB);\n"
      "    if (!p) { break; }\n"
      "    memset(p, 1, MB);\n"
      "    volatile long s = 0; for (long k = 0; k < 4000000; ++k) s += k;\n"
      "  }\n"
      "  return 0;\n"
      "}\n";
  JudgeResult result = judge_src("cpp17", hog, {{"", ""}}, 8000, root.path(),
                                 /*memory_limit_kb=*/32768);
  check(result.status == JudgeStatus::MLE, "超内存判为 MLE");
  const TestcaseResult *cs = first_case(result);
  check(cs != nullptr && cs->memory_exceeded,
        "标记为内存超限（非常数值采集）");
  check(cs != nullptr && cs->memory_kb > 32768 || (cs && cs->memory_exceeded),
        "观测峰值超过测试内存上限");
  check(no_leftover_children(), "内存超限后无遗留子进程");

  JudgeResult recovered =
      judge_src("cpp17", kCppSum, {{"7 8\n", "15\n"}}, 2000, root.path());
  check(recovered.status == JudgeStatus::AC, "之后正常判题仍为 AC");
}

std::string gen_stdout(const std::string &language, std::size_t bytes) {
  if (language == "cpp17") {
    return "#include <cstdio>\nint main(){ for(size_t i=0;i<" +
           std::to_string(bytes) + ";++i) putchar('a'); return 0; }\n";
  }
  return "#include <stdio.h>\nint main(){ for(size_t i=0;i<" +
         std::to_string(bytes) + ";++i) putchar('a'); return 0; }\n";
}

void test_output_limit() {
  std::cout << "标准输出 64KB 上限：接近/等于仍 AC，超过不判 AC\n";
  TempDir root("sb_out");
  const std::size_t limit = 64 * 1024;
  std::string expected(limit, 'a');

  JudgeResult below = judge_src("cpp17", gen_stdout("cpp17", limit - 1),
                                {{"", std::string(limit - 1, 'a')}}, 3000,
                                root.path());
  check(below.status == JudgeStatus::AC, "输出 65535 字节判为 AC");

  JudgeResult exact = judge_src("c11", gen_stdout("c11", limit),
                                {{"", expected}}, 3000, root.path());
  check(exact.status == JudgeStatus::AC, "输出恰好 65536 字节判为 AC");

  JudgeResult over = judge_src("cpp17", gen_stdout("cpp17", limit + 1),
                               {{"", expected}}, 3000, root.path());
  check(over.status != JudgeStatus::AC, "输出超过 65536 字节不判 AC");
  const TestcaseResult *cs = first_case(over);
  check(cs != nullptr && cs->output_truncated, "标记输出截断");
}

void test_large_stderr_no_hang() {
  std::cout << "大量标准错误有界采集且不挂死\n";
  TempDir root("sb_stderr");
  const char *source =
      "#include <stdio.h>\n"
      "int main(){\n"
      "  for (int i = 0; i < 200000; ++i) fprintf(stderr, \"E%d\\n\", i);\n"
      "  printf(\"OK\\n\"); return 0;\n"
      "}\n";
  auto start = std::chrono::steady_clock::now();
  JudgeResult result =
      judge_src("c11", source, {{"", "OK\n"}}, 5000, root.path());
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
  check(result.status == JudgeStatus::AC, "标准错误不影响 stdout 判定");
  check(elapsed < 8000, "采集标准错误未挂死");
  const TestcaseResult *cs = first_case(result);
  check(cs != nullptr && cs->stderr_output.size() <= 16 * 1024,
        "标准错误有界采集（≤16KB）");
  check(cs != nullptr && !cs->stderr_output.empty(), "标准错误被采集");
}

void test_sanitizer_compatibility() {
  std::cout << "ASan/UBSan 与沙箱/内存限制兼容\n";
  TempDir root("sb_asan");
  const std::vector<std::string> flags = {"-fsanitize=address,undefined",
                                          "-fno-omit-frame-pointer",
                                          "-fno-sanitize-recover=address"};

  // 正常样例：ASan 预留海量虚拟地址空间，若误设 RLIMIT_AS 会在启动阶段崩溃。
  // 这里给出 256MB 的 RSS 上限（而非地址空间），验证 ASan 正常启动并 AC。
  JudgeResult normal = judge_src("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 4000,
                                 root.path(), /*memory_limit_kb=*/262144,
                                 flags);
  check(normal.status == JudgeStatus::AC,
        "ASan/UBSan 正常样例在沙箱中编译运行并 AC");

  // 受控越界：堆缓冲区越界写，ASan 应报错并以非零退出/信号终止。
  const char *oob =
      "#include <cstdio>\n"
      "#include <cstdlib>\n"
      "int main(){\n"
      "  int* p = (int*)malloc(4 * sizeof(int));\n"
      "  p[8] = 42;\n"
      "  std::printf(\"%d\\n\", p[8]);\n"
      "  return 0;\n"
      "}\n";
  JudgeResult bad = judge_src("cpp17", oob, {{"", "42\n"}}, 4000, root.path(),
                              262144, flags);
  check(bad.status == JudgeStatus::RE, "受控越界样例判为 RE");
  const TestcaseResult *cs = first_case(bad);
  const bool has_asan =
      cs != nullptr &&
      (contains(cs->stderr_output, "AddressSanitizer") ||
       contains(cs->stderr_output, "heap-buffer-overflow") ||
       contains(cs->stderr_output, "runtime error"));
  check(has_asan, "采集到 AddressSanitizer/运行时诊断");

  // UBSan 可恢复诊断：不改变正常输出，但诊断被采集。
  const char *ub =
      "#include <cstdio>\n"
      "int main(){\n"
      "  volatile int x = 2147483647;\n"
      "  volatile int y = x + 1;\n"
      "  (void)y;\n"
      "  std::printf(\"OK\\n\");\n"
      "  return 0;\n"
      "}\n";
  JudgeResult ubsan = judge_src("cpp17", ub, {{"", "OK\n"}}, 4000, root.path(),
                                262144, flags);
  check(ubsan.status == JudgeStatus::AC, "UBSan 可恢复诊断不误判");
  const TestcaseResult *ucs = first_case(ubsan);
  check(ucs != nullptr && contains(ucs->stderr_output, "runtime error"),
        "采集到 UBSan 运行时诊断");
}

void test_environment_not_inherited() {
  std::cout << "子进程环境不继承服务密钥\n";
  TempDir root("sb_env");
  setenv("OJ_JWT_SECRET", "super-secret-value-for-test", 1);
  setenv("OJ_ADMIN_PASSWORD", "admin-secret-value-for-test", 1);

  const char *source =
      "#include <stdio.h>\n"
      "#include <stdlib.h>\n"
      "#include <string.h>\n"
      "int main(){\n"
      "  if (getenv(\"OJ_JWT_SECRET\") || getenv(\"OJ_ADMIN_PASSWORD\")) {"
      " printf(\"SECRET_LEAK\\n\"); return 0; }\n"
      "  const char* home = getenv(\"HOME\");\n"
      "  if (!home || strcmp(home, \"/nonexistent\") != 0) {"
      " printf(\"HOME_UNEXPECTED\\n\"); return 0; }\n"
      "  printf(\"ENV_MINIMAL\\n\"); return 0;\n"
      "}\n";
  JudgeResult result =
      judge_src("c11", source, {{"", "ENV_MINIMAL\n"}}, 3000, root.path());
  unsetenv("OJ_JWT_SECRET");
  unsetenv("OJ_ADMIN_PASSWORD");
  check(result.status == JudgeStatus::AC, "未继承 OJ_JWT_SECRET/OJ_ADMIN_PASSWORD");
}

void test_sandbox_failure_is_syserr() {
  std::cout << "沙箱/工作目录失败时明确 SYSERR，不无保护执行\n";
  TempDir root("sb_syserr");

  // 1) 直接调用编译：工作目录不存在 -> 沙箱挂载失败，标记 sandbox_error。
  LocalExecutor executor;
  CompileRequest request;
  request.language = oj::judge::Language::Cpp17;
  request.compiler = "g++";
  request.source_path = "/nonexistent/main.cpp";
  request.output_path = "/nonexistent/program";
  request.working_directory = "/proc/oj-no-such-dir";
  request.time_limit_ms = 2000;
  request.sandbox = true;
  ProcessResult compile_result = executor.compile(request);
  check(compile_result.launch_error, "编译沙箱初始化失败返回启动失败");
  check(compile_result.sandbox_error, "编译标记为沙箱故障（非用户 CE）");
  check(!compile_result.launch_error_message.empty(), "给出明确失败原因");
  check(no_leftover_children(), "编译沙箱初始化失败无遗留子进程");

  // 1b) 直接调用运行：工作目录不存在同样应标记沙箱故障，绝不无保护执行。
  oj::judge::RunRequest run_request;
  run_request.executable_path = "/bin/true";
  run_request.working_directory = "/proc/oj-no-such-dir";
  run_request.time_limit_ms = 2000;
  run_request.sandbox = true;
  ProcessResult run_result = executor.run(run_request, "");
  check(run_result.launch_error, "运行沙箱初始化失败返回启动失败");
  check(run_result.sandbox_error, "运行标记为沙箱故障");
  check(no_leftover_children(), "运行沙箱初始化失败无遗留子进程");

  // 2) 经判题核心：不可创建的工作目录 -> SYSERR。
  JudgeResult result =
      judge_src("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 2000,
                "/proc/oj-no-such-workspace");
  check(result.status == JudgeStatus::SYSERR, "工作目录不可用判为 SYSERR");
  check(!result.compile_ok, "未编译成功");

  // 3) 工作目录根为普通文件（非目录）-> SYSERR。
  const std::string file_root = root.path() + "/regular_file";
  {
    std::ofstream out(file_root);
    out << "not a directory";
  }
  JudgeResult file_result =
      judge_src("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 2000, file_root);
  check(file_result.status == JudgeStatus::SYSERR,
        "工作目录根为普通文件判为 SYSERR");
  check(!file_result.compile_ok, "普通文件工作目录未编译成功");
}

void test_memory_below_limit_is_ac() {
  std::cout << "内存低于上限不误判（受控分配）\n";
  TempDir root("sb_membelow");
  // 32 MiB 上限，仅分配并触碰 8 MiB，再忙等以保证被采样；应判 AC 且不标记超限。
  const char *fit =
      "#include <stdlib.h>\n"
      "#include <string.h>\n"
      "int main(){\n"
      "  const size_t MB = 1024*1024;\n"
      "  char* p = (char*)malloc(8*MB);\n"
      "  if (!p) return 1;\n"
      "  memset(p, 1, 8*MB);\n"
      "  volatile long s = 0;\n"
      "  for (long k = 0; k < 30000000; ++k) s += k;\n"
      "  return (s == 0) ? 1 : 0;\n"
      "}\n";
  JudgeResult result = judge_src("c11", fit, {{"", ""}}, 5000, root.path(),
                                 /*memory_limit_kb=*/32768);
  check(result.status == JudgeStatus::AC, "低于内存上限的运行判为 AC");
  const TestcaseResult *cs = first_case(result);
  check(cs != nullptr && !cs->memory_exceeded, "未误标记内存超限");
  check(cs != nullptr && cs->memory_kb <= 32768,
        "观测峰值不超过测试上限");
  check(no_leftover_children(), "低于上限运行后无遗留子进程");
}

void test_compile_output_bounded() {
  std::cout << "编译诊断 64KB 上限（受控模拟编译器）\n";
  TempDir root("sb_ceout");
  const std::string ws = root.path();

  // 构建一个“模拟编译器”：向 stderr 输出约 200KB 后以非零码退出。
  {
    std::ofstream out(ws + "/fake.c");
    out << "#include <stdio.h>\n"
           "int main(){ for (int i = 0; i < 200000; ++i) fputc('E', stderr);"
           " return 1; }\n";
  }
  LocalExecutor executor;
  CompileRequest build;
  build.language = oj::judge::Language::C11;
  build.compiler = "gcc";
  build.source_path = ws + "/fake.c";
  build.output_path = ws + "/fake";
  build.working_directory = ws;
  build.time_limit_ms = 10000;
  build.sandbox = true;
  build.memory_limit_kb = 512 * 1024;
  ProcessResult built = executor.compile(build);
  check(built.exited && built.exit_code == 0, "构建模拟编译器成功");

  {
    std::ofstream out(ws + "/main.cpp");
    out << "int main(){ return 0; }\n";
  }
  CompileRequest request;
  request.language = oj::judge::Language::Cpp17;
  request.compiler = ws + "/fake";
  request.source_path = ws + "/main.cpp";
  request.output_path = ws + "/program";
  request.working_directory = ws;
  request.time_limit_ms = 5000;
  request.output_limit_bytes = 64 * 1024;
  request.sandbox = true;
  request.memory_limit_kb = 512 * 1024;
  ProcessResult result = executor.compile(request);

  check(result.launched, "模拟编译器已启动");
  check(result.exited && result.exit_code == 1, "模拟编译器以非零码退出");
  check(result.stdout_truncated, "编译诊断被标记截断");
  check(result.stdout_data.size() <= 64 * 1024, "编译诊断不超过 64KB");
  check(!result.timed_out, "编译诊断采集未挂死/超时");
  check(no_leftover_children(), "编译诊断采集后无遗留子进程");
}

void test_tmpfs_workspace_if_available() {
  std::cout << "tmpfs 工作目录（/dev/shm）判题\n";
  std::string tmpfs_error;
  if (!oj::judge::path_is_tmpfs("/dev/shm", tmpfs_error)) {
    std::cout << "  [SKIP] /dev/shm 不是 tmpfs: " << tmpfs_error << "\n";
    return;
  }
  const std::string root =
      "/dev/shm/oj_sb_" + std::to_string(::getpid());
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  JudgeResult result =
      judge_src("cpp17", kCppSum, {{"4 5\n", "9\n"}}, 2000, root);
  check(result.status == JudgeStatus::AC, "tmpfs 工作目录上正常判题 AC");
  const bool cleaned = !std::filesystem::exists(root, ec) ||
                       std::filesystem::is_empty(root, ec);
  check(cleaned, "tmpfs 工作目录无遗留任务目录");
  check(no_leftover_children(), "tmpfs 判题后无遗留子进程");
  std::filesystem::remove_all(root, ec);
}

void test_cleanup_no_leftovers() {
  std::cout << "运行隔离与失败路径均无遗留进程/目录\n";
  TempDir root("sb_cleanup");
  const std::string workspace = root.path() + "/judge";
  // 依次执行 AC、超时、内存超限，确认每次都清理干净。
  judge_src("cpp17", kCppSum, {{"1 1\n", "2\n"}}, 2000, workspace);
  judge_src("cpp17", "int main(){ for(;;); }\n", {{"", ""}}, 300, workspace);
  judge_src("cpp17", kCppSum, {{"3 4\n", "7\n"}}, 2000, workspace);

  std::error_code ec;
  const bool judge_empty =
      !std::filesystem::exists(workspace, ec) ||
      std::filesystem::is_empty(workspace, ec);
  check(judge_empty, "判题工作目录内无遗留任务目录");
  check(no_leftover_children(), "无遗留子进程");
}

} // namespace

int main() {
  test_sandbox_self_test();
  test_normal_programs_sandboxed();
  test_filesystem_isolation();
  test_network_blocked();
  test_process_creation_blocked();
  test_proc_isolated();
  test_cpu_timeout_and_recovery();
  test_memory_limit_and_recovery();
  test_memory_below_limit_is_ac();
  test_output_limit();
  test_compile_output_bounded();
  test_large_stderr_no_hang();
  test_sanitizer_compatibility();
  test_environment_not_inherited();
  test_sandbox_failure_is_syserr();
  test_tmpfs_workspace_if_available();
  test_cleanup_no_leftovers();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部沙箱隔离与资源限制集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
