// 判题编译与进程集成测试（M1.5）。
//
// 使用真实 g++/gcc 与 fork/exec，在 /tmp 下的隔离临时工作目录中执行判题核心，
// 不触碰正式数据库，不运行破坏性沙箱攻击样例（本阶段无完整沙箱）。
//
// 覆盖：
//   - C++17 与 C11 各自的已知 AC / WA 样例
//   - 行尾空白、文末空行被忽略
//   - 语法错误返回 CE 且编译诊断有用
//   - 多测试点顺序执行，前面的 WA 不阻止后续测试点
//   - 每个测试点使用新的程序进程
//   - 受控死循环超时被终止，子进程回收，后续正常判题仍可执行
//   - 非正常退出不会被判为 AC
//   - 超大输出被限制，采集不挂死；程序提前退出不导致输入写入挂死
//   - 编译器不可用等内部故障返回 SYSERR（而非 CE）
//   - 空测试集 / 非法语言 / 无效时间为明确结果而非 AC
//   - 成功与失败路径均不遗留运行进程与临时目录
//
// 运行方式：ctest --test-dir build -R judge_integration --output-on-failure
// 或直接执行 build/oj_judge_integration。

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
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

int g_failures = 0;

void check(bool condition, const std::string &message) {
  if (condition) {
    std::cout << "  [PASS] " << message << "\n";
  } else {
    std::cout << "  [FAIL] " << message << "\n";
    ++g_failures;
  }
}

// 唯一临时目录，析构时删除。
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

// 是否存在未回收的子进程。
bool no_leftover_children() {
  int status = 0;
  pid_t reaped = ::waitpid(-1, &status, WNOHANG);
  return reaped == -1 && errno == ECHILD;
}

JudgeResult judge_once(const std::string &language, const std::string &source,
                       std::vector<Testcase> cases, int time_limit_ms,
                       const std::string &workspace_root,
                       JudgeOptions options = JudgeOptions{}) {
  LocalExecutor executor;
  options.workspace_root = workspace_root;
  JudgeEngine engine(executor, options);
  JudgeTask task;
  task.language = language;
  task.source_code = source;
  task.testcases = std::move(cases);
  task.time_limit_ms = time_limit_ms;
  return engine.judge(task);
}

// ---------------------------------------------------------------------------
// 样例源码
// ---------------------------------------------------------------------------

const char *kCppSum =
    "#include <iostream>\n"
    "int main(){ long long a,b; if(!(std::cin>>a>>b)) return 0; "
    "std::cout<<(a+b)<<\"\\n\"; return 0; }\n";

const char *kCppWrong = "#include <cstdio>\n"
                        "int main(){ long long a,b; "
                        "if(scanf(\"%lld %lld\",&a,&b)!=2) return 0; "
                        "printf(\"0\\n\"); return 0; }\n";

const char *kC11Sum =
    "#include <stdio.h>\n"
    "int main(void){ long long a,b; "
    "if(scanf(\"%lld %lld\",&a,&b)!=2) return 0; "
    "printf(\"%lld\\n\", a+b); return 0; }\n";

const char *kC11Wrong = "#include <stdio.h>\n"
                        "int main(void){ long long a,b; "
                        "if(scanf(\"%lld %lld\",&a,&b)!=2) return 0; "
                        "printf(\"0\\n\"); return 0; }\n";

// ---------------------------------------------------------------------------
// 语言已知样例
// ---------------------------------------------------------------------------

void test_cpp17_ac_and_whitespace() {
  std::cout << "C++17：AC 样例与行尾空白/文末空行忽略\n";
  TempDir root("judge_cpp_ac");
  JudgeResult result =
      judge_once("cpp17", kCppSum, {{"1 2\n", "3\n"}, {"100 -50\n", "50\n"}},
                 2000, root.path());
  check(result.status == JudgeStatus::AC, "C++17 A+B 判为 AC");
  check(result.compile_ok, "编译成功");
  check(result.passed == 2 && result.total == 2, "2/2 通过");
  check(root.empty(), "成功路径不遗留工作目录");

  // 程序输出 "3   \n\n\n"，期望 "3\n"，应忽略行尾空白与文末空行。
  const char *kTrailing =
      "#include <cstdio>\n"
      "int main(){ printf(\"3   \\n\"); printf(\"\\n\\n\"); return 0; }\n";
  JudgeResult trailing = judge_once("cpp17", kTrailing, {{"", "3\n"}}, 2000,
                                    root.path());
  check(trailing.status == JudgeStatus::AC, "行尾空白与文末空行不影响 AC");
}

void test_cpp17_wa() {
  std::cout << "C++17：WA 样例\n";
  TempDir root("judge_cpp_wa");
  JudgeResult result =
      judge_once("cpp17", kCppWrong, {{"5 3\n", "8\n"}}, 2000, root.path());
  check(result.status == JudgeStatus::WA, "C++17 错误输出判为 WA");
  check(result.cases.size() == 1 && !result.cases[0].actual_output.empty(),
        "WA 点保留了实际输出");
  check(root.empty(), "失败路径不遗留工作目录");
}

void test_c11_ac_and_wa() {
  std::cout << "C11：AC 与 WA 样例\n";
  TempDir root("judge_c11");
  JudgeResult ac =
      judge_once("c11", kC11Sum, {{"2 3\n", "5\n"}}, 2000, root.path());
  check(ac.status == JudgeStatus::AC, "C11 A+B 判为 AC");

  JudgeResult wa =
      judge_once("c11", kC11Wrong, {{"2 3\n", "5\n"}}, 2000, root.path());
  check(wa.status == JudgeStatus::WA, "C11 错误输出判为 WA");
}

// ---------------------------------------------------------------------------
// 编译错误
// ---------------------------------------------------------------------------

void test_compile_error() {
  std::cout << "语法错误：返回 CE 且包含编译诊断\n";
  TempDir root("judge_ce");
  const char *bad = "int main(){ this is not valid c++ }\n";
  JudgeResult result =
      judge_once("cpp17", bad, {{"1 2\n", "3\n"}}, 2000, root.path());
  check(result.status == JudgeStatus::CE, "语法错误判为 CE");
  check(!result.compile_ok, "compile_ok 为 false");
  check(!result.compile_output.empty(), "采集到编译诊断");
  check(result.compile_output.find("error") != std::string::npos,
        "诊断信息包含编译器错误信息");
  check(result.cases.empty(), "CE 时不执行任何测试点");
  check(root.empty(), "CE 路径不遗留工作目录");
}

// ---------------------------------------------------------------------------
// 顺序执行与进程隔离
// ---------------------------------------------------------------------------

void test_multiple_cases_wa_continues() {
  std::cout << "多测试点顺序执行：前面的 WA 不阻止后续\n";
  TempDir root("judge_multi");
  const char *square = "#include <iostream>\n"
                       "int main(){ long long n; std::cin>>n; "
                       "std::cout<<(n*n)<<\"\\n\"; return 0; }\n";
  JudgeResult result = judge_once(
      "cpp17", square,
      {{"2\n", "5\n"}, {"3\n", "9\n"}, {"4\n", "16\n"}}, 2000, root.path());
  check(result.status == JudgeStatus::WA, "汇总为 WA");
  check(result.cases.size() == 3, "全部 3 个测试点均执行");
  check(result.cases.size() == 3 && result.cases[0].status == JudgeStatus::WA,
        "第 1 个测试点 WA");
  check(result.cases.size() == 3 && result.cases[1].status == JudgeStatus::AC,
        "第 2 个测试点仍执行且 AC");
  check(result.cases.size() == 3 && result.cases[2].status == JudgeStatus::AC,
        "第 3 个测试点仍执行且 AC");
}

void test_new_process_per_case() {
  std::cout << "每个测试点使用新的程序进程\n";
  TempDir root("judge_newproc");
  // 进程内静态计数器：若复用进程，第二次运行会输出 2。
  const char *counter = "#include <iostream>\n"
                        "int main(){ static int c = 0; "
                        "std::cout<<(++c)<<\"\\n\"; return 0; }\n";
  JudgeResult result =
      judge_once("cpp17", counter, {{"", "1\n"}, {"", "1\n"}}, 2000, root.path());
  check(result.status == JudgeStatus::AC,
        "两次运行均输出 1，说明每个测试点都是新进程");
  check(result.cases.size() == 2, "执行了两个测试点");
}

// ---------------------------------------------------------------------------
// 超时与异常退出
// ---------------------------------------------------------------------------

void test_timeout_kills_and_recovers() {
  std::cout << "死循环：超时终止、子进程回收、后续判题正常\n";
  TempDir root("judge_tle");
  const char *loop = "int main(){ volatile unsigned long long c=0; "
                     "while(true){ c++; } return 0; }\n";
  JudgeResult result = judge_once("cpp17", loop, {{"", ""}}, 300, root.path());
  check(result.status == JudgeStatus::TLE, "死循环判为 TLE");
  check(result.cases.size() == 1 && result.cases[0].timed_out,
        "测试点标记为超时");
  check(no_leftover_children(), "超时后子进程已被回收（无僵尸）");
  check(root.empty(), "超时路径不遗留工作目录");

  // 服务主体恢复：后续正常判题仍可执行。
  JudgeResult recovered =
      judge_once("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 2000, root.path());
  check(recovered.status == JudgeStatus::AC, "超时后仍可正常判题");
}

void test_abnormal_exit_not_ac() {
  std::cout << "非正常退出不会被判为 AC\n";
  TempDir root("judge_re");
  const char *nonzero = "int main(){ return 1; }\n";
  JudgeResult r1 = judge_once("cpp17", nonzero, {{"", ""}}, 2000, root.path());
  check(r1.status == JudgeStatus::RE, "非零退出码判为 RE");

  const char *crash = "#include <cstdlib>\nint main(){ std::abort(); }\n";
  JudgeResult r2 = judge_once("cpp17", crash, {{"", ""}}, 2000, root.path());
  check(r2.status == JudgeStatus::RE, "崩溃（SIGABRT）判为 RE");
  check(r1.status != JudgeStatus::AC && r2.status != JudgeStatus::AC,
        "非正常退出均未误判为 AC");
}

// ---------------------------------------------------------------------------
// 输出上限与写入健壮性
// ---------------------------------------------------------------------------

void test_huge_output_bounded() {
  std::cout << "超大输出：有界采集、不挂死、不误判 AC\n";
  TempDir root("judge_bigout");
  const char *spam = "#include <cstdio>\n"
                     "int main(){ for(int i=0;i<200000;i++) "
                     "printf(\"%d\\n\", i); return 0; }\n";
  auto start = std::chrono::steady_clock::now();
  JudgeResult result = judge_once("cpp17", spam, {{"", "0\n"}}, 2000, root.path());
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
  check(result.status != JudgeStatus::AC, "超大输出未误判为 AC");
  check(result.cases.size() == 1 && result.cases[0].output_truncated,
        "标记了输出截断");
  check(elapsed < 10000, "采集过程未挂死（耗时 < 10s）");
  check(root.empty(), "超大输出路径不遗留工作目录");
}

void test_early_exit_no_hang() {
  std::cout << "程序提前退出：输入写入不挂死\n";
  TempDir root("judge_earlyexit");
  const char *fast = "#include <cstdio>\nint main(){ printf(\"OK\\n\"); return 0; }\n";
  std::string big_input(1 << 20, 'a'); // 1 MiB，程序完全不读取
  auto start = std::chrono::steady_clock::now();
  JudgeResult result =
      judge_once("cpp17", fast, {{big_input, "OK\n"}}, 3000, root.path());
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
  check(result.status == JudgeStatus::AC, "提前退出且输出匹配判为 AC");
  check(elapsed < 5000, "大输入写入未挂死（耗时 < 5s）");
}

// ---------------------------------------------------------------------------
// 内部故障与非法输入
// ---------------------------------------------------------------------------

void test_compiler_unavailable_is_syserr() {
  std::cout << "编译器不可用：返回 SYSERR 而非 CE\n";
  TempDir root("judge_nocc");
  JudgeOptions options;
  options.cpp_compiler = "/nonexistent/g++-does-not-exist";
  JudgeResult result =
      judge_once("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 2000, root.path(), options);
  check(result.status == JudgeStatus::SYSERR, "编译器不存在判为 SYSERR");
  check(result.status != JudgeStatus::CE, "未伪装成 CE");
  check(!result.message.empty(), "给出明确的故障信息");
  check(result.cases.empty(), "未执行测试点");
}

void test_invalid_inputs() {
  std::cout << "空测试集 / 非法语言 / 无效时间：明确结果而非 AC\n";
  TempDir root("judge_invalid");

  JudgeResult empty = judge_once("cpp17", kCppSum, {}, 2000, root.path());
  check(empty.status == JudgeStatus::SYSERR && empty.status != JudgeStatus::AC,
        "空测试集返回 SYSERR 而非 AC");

  JudgeResult bad_lang = judge_once("rust", kCppSum, {{"1\n", "1\n"}}, 2000,
                                    root.path());
  check(bad_lang.status == JudgeStatus::SYSERR && !bad_lang.message.empty(),
        "非法语言返回明确 SYSERR");

  JudgeResult bad_time =
      judge_once("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 0, root.path());
  check(bad_time.status == JudgeStatus::SYSERR && bad_time.status != JudgeStatus::AC,
        "无效时间返回 SYSERR 而非 AC");
}

void test_stderr_separate_and_bounded() {
  std::cout << "标准错误：独立采集、不影响 stdout 判定、有界\n";
  TempDir root("judge_stderr");
  const char *with_stderr =
      "#include <cstdio>\n"
      "int main(){ fprintf(stderr, \"debug-line\\n\"); "
      "printf(\"3\\n\"); return 0; }\n";
  JudgeResult result = judge_once("cpp17", with_stderr, {{"1 2\n", "3\n"}},
                                  2000, root.path());
  check(result.status == JudgeStatus::AC, "stderr 内容不影响 stdout 比对");
  check(result.cases.size() == 1 &&
            result.cases[0].stderr_output.find("debug-line") != std::string::npos,
        "标准错误被独立采集");

  const char *flood =
      "#include <cstdio>\n"
      "int main(){ for(int i=0;i<100000;i++) "
      "fprintf(stderr, \"xxxxxxxxxx\\n\"); printf(\"3\\n\"); return 0; }\n";
  auto start = std::chrono::steady_clock::now();
  JudgeResult flood_result =
      judge_once("cpp17", flood, {{"1 2\n", "3\n"}}, 2000, root.path());
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
  check(flood_result.status == JudgeStatus::AC, "刷标准错误仍按 stdout 判 AC");
  check(flood_result.cases.size() == 1 &&
            flood_result.cases[0].stderr_output.size() <= 16 * 1024,
        "标准错误有界采集（≤16KB）");
  check(elapsed < 10000, "采集标准错误未挂死（耗时 < 10s）");
}

void test_whitespace_significance_end_to_end() {
  std::cout << "空白语义（真实运行）：行首/行内/中间空行差异判 WA\n";
  TempDir root("judge_ws");

  const char *leading =
      "#include <cstdio>\nint main(){ printf(\"  a\\n\"); return 0; }\n";
  JudgeResult ac = judge_once("cpp17", leading, {{"", "  a\n"}}, 2000, root.path());
  check(ac.status == JudgeStatus::AC, "行首空白一致时 AC");
  JudgeResult wa = judge_once("cpp17", leading, {{"", "a\n"}}, 2000, root.path());
  check(wa.status == JudgeStatus::WA, "行首空白差异判 WA");

  const char *internal =
      "#include <cstdio>\nint main(){ printf(\"a  b\\n\"); return 0; }\n";
  JudgeResult iw =
      judge_once("cpp17", internal, {{"", "a b\n"}}, 2000, root.path());
  check(iw.status == JudgeStatus::WA, "行内空白差异判 WA");

  const char *midblank =
      "#include <cstdio>\nint main(){ printf(\"a\\n\\nb\\n\"); return 0; }\n";
  JudgeResult mb =
      judge_once("cpp17", midblank, {{"", "a\nb\n"}}, 2000, root.path());
  check(mb.status == JudgeStatus::WA, "中间空行差异判 WA");
}

void test_empty_output_ac() {
  std::cout << "空输出：与空/纯空行期望均判 AC\n";
  TempDir root("judge_emptyout");
  const char *silent = "#include <cstdio>\nint main(){ return 0; }\n";
  JudgeResult result = judge_once("cpp17", silent, {{"", ""}, {"", "\n\n"}},
                                  2000, root.path());
  check(result.status == JudgeStatus::AC, "空输出与纯空行期望均判 AC");
  check(result.passed == 2, "两个空输出测试点均通过");
}

void test_mixed_results_real() {
  std::cout << "真实混合结果：AC + WA + TLE 按严重度汇总\n";
  TempDir root("judge_mixed");
  // 输入 2 -> 正常输出 0（匹配 AC）；输入 3 -> 输出 0（期望 5，WA）；
  // 输入 1 -> 死循环（TLE）。
  const char *mixed =
      "#include <cstdio>\n"
      "int main(){ long long n; if(scanf(\"%lld\",&n)!=1) return 0;"
      " if(n==1){ volatile unsigned long long c=0; while(true){ c++; } }"
      " printf(\"0\\n\"); return 0; }\n";
  JudgeResult result =
      judge_once("cpp17", mixed,
                 {{"2\n", "0\n"}, {"3\n", "5\n"}, {"1\n", "0\n"}}, 500, root.path());
  check(result.status == JudgeStatus::TLE, "混合结果按严重度汇总为 TLE");
  check(result.cases.size() == 3, "TLE 之前的所有测试点均已执行");
  check(result.cases.size() == 3 && result.cases[0].status == JudgeStatus::AC,
        "第 1 点 AC");
  check(result.cases.size() == 3 && result.cases[1].status == JudgeStatus::WA,
        "第 2 点 WA 未被阻断");
  check(result.cases.size() == 3 && result.cases[2].status == JudgeStatus::TLE,
        "第 3 点 TLE");
  check(no_leftover_children(), "混合执行后无遗留子进程");
  check(root.empty(), "混合执行后无遗留工作目录");
}

void test_cleanup_and_no_children() {
  std::cout << "综合路径：无遗留子进程与临时目录\n";
  TempDir root("judge_cleanup");
  judge_once("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 2000, root.path());
  judge_once("cpp17", "int main(){ return 2; }\n", {{"", ""}}, 2000, root.path());
  judge_once("cpp17", "int main(){ this is broken }\n", {{"", ""}}, 2000,
             root.path());
  check(root.empty(), "所有路径均未遗留工作目录");
  check(no_leftover_children(), "所有路径均未遗留子进程");
}

} // namespace

int main() {
  test_cpp17_ac_and_whitespace();
  test_cpp17_wa();
  test_c11_ac_and_wa();
  test_compile_error();
  test_multiple_cases_wa_continues();
  test_new_process_per_case();
  test_timeout_kills_and_recovers();
  test_abnormal_exit_not_ac();
  test_huge_output_bounded();
  test_early_exit_no_hang();
  test_compiler_unavailable_is_syserr();
  test_invalid_inputs();
  test_stderr_separate_and_bounded();
  test_whitespace_significance_end_to_end();
  test_empty_output_ac();
  test_mixed_results_real();
  test_cleanup_and_no_children();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部判题编译与进程集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
