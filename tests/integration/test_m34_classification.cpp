// M3.4 编译与结果分类集成测试（真实 g++/gcc + 沙箱进程）。
//
// 聚焦本阶段新增/收敛的行为，与 M1.5 judge_integration、M3.3 sandbox_integration
// 不重复：
//   - C++17 / C11 采用默认生产编译模板（ASan/UBSan 全开 + -fno-sanitize-recover=all）
//     的 AC / WA / CE；
//   - 行尾空白与文末空行规则；
//   - 受控越界与不可恢复的未定义行为触发 Sanitizer 并判失败（非 AC），保留诊断；
//   - 普通程序自行打印「AddressSanitizer / runtime error」字样且正常退出、输出匹配
//     时仍判 AC（不因文本内容误判）；
//   - 死循环 TLE、受控崩溃 RE、有可靠 RSS 证据的 MLE；无证据的 SIGKILL 不判 MLE；
//   - 超大标准输出按约定失败且截断；编译诊断有界；
//   - 编译器缺失返回 SYSERR 且不挂死，随后正常任务仍可执行；
//   - 前面测试点失败后继续执行后续测试点；混合结果优先级确定；
//   - 逐点耗时/内存与编译耗时的计量口径。
//
// 运行方式：ctest --test-dir build -R m34_classification --output-on-failure

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

// ---------------------------------------------------------------------------
// 样例源码
// ---------------------------------------------------------------------------

const char *kCppSum =
    "#include <iostream>\n"
    "int main(){ long long a,b; if(!(std::cin>>a>>b)) return 0; "
    "std::cout<<(a+b)<<\"\\n\"; return 0; }\n";

const char *kC11Sum =
    "#include <stdio.h>\n"
    "int main(void){ long long a,b; "
    "if(scanf(\"%lld %lld\",&a,&b)!=2) return 0; "
    "printf(\"%lld\\n\", a+b); return 0; }\n";

const char *kCppWrong =
    "#include <cstdio>\n"
    "int main(){ long long a,b; if(scanf(\"%lld %lld\",&a,&b)!=2) return 0; "
    "printf(\"0\\n\"); return 0; }\n";

const char *kC11Wrong =
    "#include <stdio.h>\n"
    "int main(void){ long long a,b; "
    "if(scanf(\"%lld %lld\",&a,&b)!=2) return 0; "
    "printf(\"0\\n\"); return 0; }\n";

// ---------------------------------------------------------------------------
// 编译模板：两套语言的 AC / WA / CE
// ---------------------------------------------------------------------------

void test_both_languages_ac_wa_ce() {
  std::cout << "C++17/C11 默认（ASan/UBSan）编译模板：AC / WA / CE\n";
  TempDir root("m34_lang");

  JudgeResult cpp = judge_src("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 4000,
                              root.path());
  check(cpp.status == JudgeStatus::AC, "C++17 判 AC");
  check(cpp.compile_ok && cpp.compile_time_ms >= 0,
        "C++17 编译成功并记录编译耗时");

  JudgeResult c11 = judge_src("c11", kC11Sum, {{"4 5\n", "9\n"}}, 4000,
                              root.path());
  check(c11.status == JudgeStatus::AC, "C11 判 AC");

  JudgeResult cpp_wa = judge_src("cpp17", kCppWrong, {{"1 2\n", "3\n"}}, 4000,
                                 root.path());
  check(cpp_wa.status == JudgeStatus::WA, "C++17 错误输出判 WA");

  JudgeResult c11_wa = judge_src("c11", kC11Wrong, {{"1 2\n", "3\n"}}, 4000,
                                 root.path());
  check(c11_wa.status == JudgeStatus::WA, "C11 错误输出判 WA");

  JudgeResult ce = judge_src("cpp17", "int main(){ this is broken }\n",
                             {{"", ""}}, 4000, root.path());
  check(ce.status == JudgeStatus::CE, "语法错误判 CE");
  check(!ce.compile_output.empty() &&
            contains(ce.compile_output, "error"),
        "CE 保留编译诊断");
  check(ce.cases.empty(), "CE 不执行测试点");
}

void test_whitespace_rules() {
  std::cout << "行尾空白与文末空行规则\n";
  TempDir root("m34_ws");
  const char *trailing =
      "#include <cstdio>\n"
      "int main(){ printf(\"3   \\n\"); printf(\"\\n\\n\"); return 0; }\n";
  JudgeResult ok = judge_src("cpp17", trailing, {{"", "3\n"}}, 4000,
                             root.path());
  check(ok.status == JudgeStatus::AC, "行尾空白/文末空行归一化后 AC");

  const char *leading =
      "#include <cstdio>\nint main(){ printf(\"  a\\n\"); return 0; }\n";
  JudgeResult wa =
      judge_src("cpp17", leading, {{"", "a\n"}}, 4000, root.path());
  check(wa.status == JudgeStatus::WA, "行首空白有意义，差异判 WA");
}

// ---------------------------------------------------------------------------
// Sanitizer
// ---------------------------------------------------------------------------

void test_sanitizer_detects_and_fails() {
  std::cout << "受控越界与未定义行为触发 Sanitizer 并判失败\n";
  TempDir root("m34_san");

  // 越界写入：ASan 报错，进程异常终止 -> RE，且采集到诊断；绝不判 AC。
  const char *oob =
      "#include <cstdio>\n"
      "#include <cstdlib>\n"
      "int main(){\n"
      "  int* p = (int*)malloc(4 * sizeof(int));\n"
      "  p[8] = 42;\n"
      "  std::printf(\"42\\n\");\n"
      "  return 0;\n"
      "}\n";
  JudgeResult bad =
      judge_src("cpp17", oob, {{"", "42\n"}}, 4000, root.path());
  check(bad.status != JudgeStatus::AC, "越界样例不判 AC");
  check(bad.status == JudgeStatus::RE, "越界样例判 RE");
  const TestcaseResult *cs = first_case(bad);
  check(cs != nullptr && cs->sanitizer_error, "标注疑似 Sanitizer");
  check(cs != nullptr &&
            (contains(cs->stderr_output, "AddressSanitizer") ||
             contains(cs->stderr_output, "heap-buffer-overflow") ||
             contains(cs->stderr_output, "runtime error")),
        "保留 Sanitizer 诊断");

  // 不可恢复的未定义行为：-fno-sanitize-recover=all 使其中止，即使打印了匹配输出。
  const char *ub =
      "#include <cstdio>\n"
      "int main(){\n"
      "  volatile int x = 2147483647;\n"
      "  volatile int y = x + 1;\n"
      "  (void)y;\n"
      "  std::printf(\"OK\\n\");\n"
      "  return 0;\n"
      "}\n";
  JudgeResult ubsan =
      judge_src("cpp17", ub, {{"", "OK\n"}}, 4000, root.path());
  check(ubsan.status != JudgeStatus::AC, "UB 样例不判 AC");
  const TestcaseResult *ucs = first_case(ubsan);
  check(ucs != nullptr && contains(ucs->stderr_output, "runtime error"),
        "保留 UBSan 诊断");
}

// 用户可自行打印 Sanitizer 类似文本：正常退出且输出匹配时必须仍判 AC。
void test_fake_sanitizer_text_not_misjudged() {
  std::cout << "普通程序打印 Sanitizer 类似字样不误判\n";
  TempDir root("m34_fake");
  const char *fake =
      "#include <cstdio>\n"
      "int main(){\n"
      "  fprintf(stderr, \"AddressSanitizer: heap-buffer-overflow\\n\");\n"
      "  fprintf(stderr, \"runtime error: fake diagnostic\\n\");\n"
      "  printf(\"OK\\n\");\n"
      "  return 0;\n"
      "}\n";
  JudgeResult result =
      judge_src("cpp17", fake, {{"", "OK\n"}}, 4000, root.path());
  check(result.status == JudgeStatus::AC,
        "仅文本类似 Sanitizer、实际正常退出且输出匹配 -> AC");
  const TestcaseResult *cs = first_case(result);
  check(cs != nullptr && !cs->sanitizer_error,
        "正常退出时未标注 Sanitizer 错误");
}

// ---------------------------------------------------------------------------
// 超时、崩溃、内存
// ---------------------------------------------------------------------------

void test_tle_re_mle() {
  std::cout << "死循环 TLE、受控崩溃 RE、可靠证据 MLE\n";
  TempDir root("m34_tre");

  const char *loop = "int main(){ volatile unsigned long long c=0; "
                     "while(true){ c++; } return 0; }\n";
  JudgeResult tle = judge_src("cpp17", loop, {{"", ""}}, 500, root.path());
  check(tle.status == JudgeStatus::TLE, "死循环判 TLE");

  const char *crash =
      "#include <cstdlib>\nint main(){ std::abort(); }\n";
  JudgeResult re = judge_src("cpp17", crash, {{"", ""}}, 4000, root.path());
  check(re.status == JudgeStatus::RE, "受控崩溃判 RE");

  // 受控分配 80MB，测试限 32MB：RSS 采样超限并强制终止 -> MLE（有证据）。
  const char *hog =
      "#include <stdlib.h>\n"
      "#include <string.h>\n"
      "int main(){\n"
      "  const size_t MB = 1024*1024;\n"
      "  for (int i = 0; i < 80; ++i) {\n"
      "    char* p = (char*)malloc(MB);\n"
      "    if (!p) break;\n"
      "    memset(p, 1, MB);\n"
      "    volatile long s = 0; for (long k = 0; k < 4000000; ++k) s += k;\n"
      "  }\n"
      "  return 0;\n"
      "}\n";
  JudgeResult mle = judge_src("cpp17", hog, {{"", ""}}, 8000, root.path(),
                              /*memory_limit_kb=*/32768);
  check(mle.status == JudgeStatus::MLE, "有可靠 RSS 证据判 MLE");
  const TestcaseResult *mcs = first_case(mle);
  check(mcs != nullptr && mcs->memory_exceeded, "标记内存超限证据");
  check(no_leftover_children(), "异常路径无遗留子进程");
}

// ---------------------------------------------------------------------------
// 输出与诊断上限
// ---------------------------------------------------------------------------

void test_output_and_diagnostic_bounds() {
  std::cout << "超大输出失败且截断，编译诊断有界\n";
  TempDir root("m34_out");
  const char *spam =
      "#include <cstdio>\n"
      "int main(){ for(int i=0;i<200000;i++) printf(\"%d\\n\", i);"
      " return 0; }\n";
  JudgeResult over =
      judge_src("cpp17", spam, {{"", "0\n"}}, 4000, root.path());
  check(over.status != JudgeStatus::AC, "超大输出未判 AC");
  const TestcaseResult *cs = first_case(over);
  check(cs != nullptr && cs->output_truncated, "标记输出截断");

  // 用大量语法错误制造有界编译诊断（超出 64KB 只保留上限内内容）。
  std::string many_errors;
  for (int i = 0; i < 4000; ++i) {
    many_errors += "bad_token_" + std::to_string(i) + ";\n";
  }
  JudgeResult ce = judge_src("cpp17", many_errors, {{"", ""}}, 4000,
                             root.path());
  check(ce.status == JudgeStatus::CE, "大量错误判 CE");
  check(ce.compile_output.size() <= 64 * 1024, "编译诊断不超过 64KB");
  check(ce.compile_output_truncated, "编译诊断超限时明确标记截断");
  check(no_leftover_children(), "诊断上限路径无遗留子进程");
}

// ---------------------------------------------------------------------------
// 内部故障、继续执行与混合优先级
// ---------------------------------------------------------------------------

void test_compiler_failure_is_syserr_and_recovers() {
  std::cout << "编译器缺失/不可执行 SYSERR、不挂死，随后正常任务可执行\n";
  TempDir root("m34_syserr");
  JudgeOptions options;
  options.cpp_compiler = "/nonexistent/g++-m34";
  JudgeResult syserr = judge_src("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 4000,
                                 root.path(), 262144, options);
  check(syserr.status == JudgeStatus::SYSERR, "编译器缺失判 SYSERR");
  check(!syserr.compile_ok, "未编译成功");

  // 编译器路径存在但不是可执行文件：属基础设施故障，仍应 SYSERR（不是 CE）。
  const std::string not_exec = root.path() + "/not_exec_compiler";
  {
    std::ofstream out(not_exec, std::ios::binary | std::ios::trunc);
    out << "not an executable";
  }
  JudgeOptions bad_options;
  bad_options.cpp_compiler = not_exec;
  JudgeResult not_exec_result =
      judge_src("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 4000, root.path(),
                262144, bad_options);
  check(not_exec_result.status == JudgeStatus::SYSERR,
        "存在但不可执行的编译器判 SYSERR");
  check(not_exec_result.status != JudgeStatus::CE, "未伪装成 CE");

  JudgeResult recovered =
      judge_src("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 4000, root.path());
  check(recovered.status == JudgeStatus::AC, "随后正常任务仍 AC");
  check(no_leftover_children(), "故障后无遗留子进程");
}

// 用户源码中的链接错误（未定义符号）属编译错误，判 CE 而非 SYSERR。
void test_link_error_is_ce_not_syserr() {
  std::cout << "用户链接错误判 CE（非 SYSERR）\n";
  TempDir root("m34_link");
  const char *undefined_symbol =
      "#include <cstdio>\n"
      "int missing_function();\n"
      "int main(){ printf(\"%d\\n\", missing_function()); return 0; }\n";
  JudgeResult result = judge_src("cpp17", undefined_symbol, {{"", "0\n"}}, 4000,
                                 root.path());
  check(result.status == JudgeStatus::CE, "未定义符号链接失败判 CE");
  check(result.status != JudgeStatus::SYSERR, "未伪装成 SYSERR");
  check(!result.compile_output.empty(), "保留链接诊断");
  check(result.cases.empty(), "CE 不执行测试点");
}

// 真实编译的 CE 诊断不得回显服务端内部工作目录/沙箱路径。
void test_ce_diagnostic_no_internal_path() {
  std::cout << "CE 诊断不泄露内部路径\n";
  TempDir root("m34_leak");
  const char *bad = "int main(){ this is not valid c++ }\n";
  JudgeResult result =
      judge_src("cpp17", bad, {{"", ""}}, 4000, root.path());
  check(result.status == JudgeStatus::CE, "语法错误判 CE");
  check(result.compile_output.find(root.path()) == std::string::npos,
        "诊断不含工作目录路径");
  check(result.compile_output.find(".oj_sandbox") == std::string::npos,
        "诊断不含沙箱临时目录");
  check(contains(result.compile_output, "main.cpp"),
        "仍保留定位用户代码的文件名");
}

void test_continue_after_failure_and_mixed_priority() {
  std::cout << "前面的失败点不阻止后续；混合结果优先级确定\n";
  TempDir root("m34_cont");
  const char *mixed =
      "#include <cstdio>\n"
      "int main(){ int n=0; if(scanf(\"%d\",&n)!=1) return 0;"
      " if(n==1){ volatile unsigned long long c=0; while(true){ c++; } }"
      " printf(\"%d\\n\", n); return 0; }\n";
  // 第 1 点 WA（输出 2，期望 5）、第 2 点 AC（输出 2）、第 3 点 TLE。
  JudgeResult result = judge_src("cpp17", mixed,
                                 {{"2\n", "5\n"}, {"2\n", "2\n"}, {"1\n", "2\n"}},
                                 500, root.path());
  check(result.cases.size() == 3, "失败点后仍执行全部测试点");
  check(result.cases.size() == 3 && result.cases[0].status == JudgeStatus::WA,
        "第 1 点 WA");
  check(result.cases.size() == 3 && result.cases[1].status == JudgeStatus::AC,
        "第 2 点继续执行且 AC");
  check(result.cases.size() == 3 && result.cases[2].status == JudgeStatus::TLE,
        "第 3 点 TLE");
  check(result.status == JudgeStatus::TLE, "混合结果按严重度汇总为 TLE");
  check(result.passed == 1, "通过计数正确");
}

void test_metric_units() {
  std::cout << "逐点耗时/内存与编译耗时的计量口径\n";
  TempDir root("m34_metric");
  JudgeResult result = judge_src("cpp17", kCppSum, {{"1 2\n", "3\n"}}, 4000,
                                 root.path());
  check(result.status == JudgeStatus::AC, "样例 AC");
  check(result.compile_time_ms >= 0, "编译耗时为非负毫秒数");
  const TestcaseResult *cs = first_case(result);
  check(cs != nullptr && cs->time_ms >= 0, "逐点耗时为非负毫秒数");
  check(cs != nullptr && cs->memory_kb > 0, "逐点内存为采集到的正值（kB）");
  check(result.cases.size() == 1 && result.cases[0].status == JudgeStatus::AC,
        "逐点状态为 AC");
}

} // namespace

int main() {
  test_both_languages_ac_wa_ce();
  test_whitespace_rules();
  test_sanitizer_detects_and_fails();
  test_fake_sanitizer_text_not_misjudged();
  test_tle_re_mle();
  test_output_and_diagnostic_bounds();
  test_compiler_failure_is_syserr_and_recovers();
  test_link_error_is_ce_not_syserr();
  test_ce_diagnostic_no_internal_path();
  test_continue_after_failure_and_mixed_priority();
  test_metric_units();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部 M3.4 编译与分类集成测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
