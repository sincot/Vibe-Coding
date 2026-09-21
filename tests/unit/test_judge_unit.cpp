// 判题核心单元测试（M1.5，基于 gtest）。
//
// 覆盖不依赖真实进程与数据库的纯逻辑：
//   - 输出归一化与比对（行尾空白、文末空行、行首/行内空白、中间空行）
//   - 逐点结果的汇总规则（AC/WA/RE/TLE/MLE/SYSERR 与混合结果）
//   - 语言标识解析
//   - 使用 FakeExecutor 驱动 JudgeEngine 的编排逻辑（编译一次、顺序执行、WA 不
//     阻断后续点、CE/SYSERR 分类、非法输入不返回 AC 等）
//
// 真实 g++/gcc 编译与 fork/exec 行为由 judge_integration 集成测试覆盖。
//
// 运行方式：ctest --test-dir build -R judge_unit --output-on-failure
// 或直接执行 build/oj_judge_unit。

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "judge/comparator.h"
#include "judge/executor.h"
#include "judge/judge.h"

namespace {

using oj::judge::CompileRequest;
using oj::judge::IExecutor;
using oj::judge::JudgeEngine;
using oj::judge::JudgeOptions;
using oj::judge::JudgeResult;
using oj::judge::JudgeStatus;
using oj::judge::JudgeTask;
using oj::judge::Language;
using oj::judge::ProcessResult;
using oj::judge::RunRequest;
using oj::judge::Testcase;
using oj::judge::TestcaseResult;

// ---------------------------------------------------------------------------
// 输出归一化与比对
// ---------------------------------------------------------------------------

TEST(NormalizeOutput, IdenticalTextUnchanged) {
  EXPECT_EQ(oj::judge::normalize_output("3\n"), "3");
  EXPECT_EQ(oj::judge::normalize_output("a\nb\n"), "a\nb");
  EXPECT_TRUE(oj::judge::outputs_match("3\n", "3\n"));
}

TEST(NormalizeOutput, TrailingSpacesAndTabsIgnored) {
  EXPECT_EQ(oj::judge::normalize_output("3  \n"), "3");
  EXPECT_EQ(oj::judge::normalize_output("3\t\n"), "3");
  EXPECT_EQ(oj::judge::normalize_output("1  \n2\t\n"), "1\n2");
  EXPECT_TRUE(oj::judge::outputs_match("3\n", "3   \n"));
  EXPECT_TRUE(oj::judge::outputs_match("1\n2\n", "1  \n2\t\n"));
}

TEST(NormalizeOutput, CarriageReturnTreatedAsTrailingWhitespace) {
  EXPECT_EQ(oj::judge::normalize_output("3\r\n"), "3");
  EXPECT_TRUE(oj::judge::outputs_match("3\n", "3\r\n"));
}

TEST(NormalizeOutput, TrailingBlankLinesIgnored) {
  EXPECT_EQ(oj::judge::normalize_output("3\n\n\n"), "3");
  EXPECT_TRUE(oj::judge::outputs_match("3\n", "3\n\n\n"));
  EXPECT_TRUE(oj::judge::outputs_match("3", "3\n"));
}

TEST(NormalizeOutput, LeadingWhitespaceIsSignificant) {
  EXPECT_EQ(oj::judge::normalize_output("  a\n"), "  a");
  EXPECT_FALSE(oj::judge::outputs_match("a\n", "  a\n"));
  EXPECT_FALSE(oj::judge::outputs_match("a\n", "\ta\n"));
}

TEST(NormalizeOutput, InternalWhitespaceIsSignificant) {
  EXPECT_FALSE(oj::judge::outputs_match("a b\n", "a  b\n"));
  EXPECT_FALSE(oj::judge::outputs_match("a b\n", "ab\n"));
  EXPECT_TRUE(oj::judge::outputs_match("a b\n", "a b\n"));
}

TEST(NormalizeOutput, MiddleBlankLinesPreserved) {
  EXPECT_EQ(oj::judge::normalize_output("a\n\nb\n"), "a\n\nb");
  EXPECT_TRUE(oj::judge::outputs_match("a\n\nb\n", "a\n\nb\n\n"));
  EXPECT_FALSE(oj::judge::outputs_match("a\nb\n", "a\n\nb\n"));
}

TEST(NormalizeOutput, EmptyAndWhitespaceOnly) {
  EXPECT_EQ(oj::judge::normalize_output(""), "");
  EXPECT_EQ(oj::judge::normalize_output("\n"), "");
  EXPECT_EQ(oj::judge::normalize_output("\n\n\n"), "");
  EXPECT_EQ(oj::judge::normalize_output("   \n\t\n"), "");
  EXPECT_TRUE(oj::judge::outputs_match("", "\n\n"));
  EXPECT_TRUE(oj::judge::outputs_match("", "   \n"));
}

// ---------------------------------------------------------------------------
// 汇总规则
// ---------------------------------------------------------------------------

TestcaseResult case_with(JudgeStatus status) {
  TestcaseResult result;
  result.status = status;
  return result;
}

TEST(SummarizeCases, EmptyIsSyserr) {
  EXPECT_EQ(oj::judge::summarize_cases({}), JudgeStatus::SYSERR);
}

TEST(SummarizeCases, AllAcIsAc) {
  std::vector<TestcaseResult> cases{case_with(JudgeStatus::AC),
                                    case_with(JudgeStatus::AC)};
  EXPECT_EQ(oj::judge::summarize_cases(cases), JudgeStatus::AC);
}

TEST(SummarizeCases, WaWinsOverAc) {
  std::vector<TestcaseResult> cases{case_with(JudgeStatus::AC),
                                    case_with(JudgeStatus::WA)};
  EXPECT_EQ(oj::judge::summarize_cases(cases), JudgeStatus::WA);
}

TEST(SummarizeCases, SeverityPrecedence) {
  EXPECT_EQ(oj::judge::summarize_cases(
                {case_with(JudgeStatus::WA), case_with(JudgeStatus::RE)}),
            JudgeStatus::RE);
  EXPECT_EQ(oj::judge::summarize_cases(
                {case_with(JudgeStatus::WA), case_with(JudgeStatus::TLE)}),
            JudgeStatus::TLE);
  EXPECT_EQ(oj::judge::summarize_cases(
                {case_with(JudgeStatus::RE), case_with(JudgeStatus::MLE)}),
            JudgeStatus::MLE);
  EXPECT_EQ(oj::judge::summarize_cases(
                {case_with(JudgeStatus::TLE), case_with(JudgeStatus::SYSERR)}),
            JudgeStatus::SYSERR);
  EXPECT_EQ(oj::judge::summarize_cases(
                {case_with(JudgeStatus::AC), case_with(JudgeStatus::SYSERR),
                 case_with(JudgeStatus::RE)}),
            JudgeStatus::SYSERR);
}

// ---------------------------------------------------------------------------
// 语言解析
// ---------------------------------------------------------------------------

TEST(ParseLanguage, RecognizesSupportedLanguages) {
  Language language = Language::C11;
  EXPECT_TRUE(oj::judge::parse_language("cpp17", language));
  EXPECT_EQ(language, Language::Cpp17);
  EXPECT_TRUE(oj::judge::parse_language("C++17", language));
  EXPECT_EQ(language, Language::Cpp17);
  EXPECT_TRUE(oj::judge::parse_language("cpp", language));
  EXPECT_EQ(language, Language::Cpp17);
  EXPECT_TRUE(oj::judge::parse_language("C11", language));
  EXPECT_EQ(language, Language::C11);
  EXPECT_TRUE(oj::judge::parse_language("c", language));
  EXPECT_EQ(language, Language::C11);
}

TEST(ParseLanguage, RejectsUnknown) {
  Language language = Language::Cpp17;
  EXPECT_FALSE(oj::judge::parse_language("java", language));
  EXPECT_FALSE(oj::judge::parse_language("python3", language));
  EXPECT_FALSE(oj::judge::parse_language("", language));
}

// ---------------------------------------------------------------------------
// FakeExecutor 驱动 JudgeEngine
// ---------------------------------------------------------------------------

ProcessResult ok_compile() {
  ProcessResult result;
  result.launched = true;
  result.exited = true;
  result.exit_code = 0;
  return result;
}

ProcessResult ok_run(const std::string &stdout_data) {
  ProcessResult result;
  result.launched = true;
  result.exited = true;
  result.exit_code = 0;
  result.stdout_data = stdout_data;
  return result;
}

class FakeExecutor : public IExecutor {
public:
  ProcessResult compile_result = ok_compile();
  std::vector<ProcessResult> run_results;

  int compile_calls = 0;
  int run_calls = 0;
  std::vector<std::string> run_inputs;
  CompileRequest last_compile_request;
  RunRequest last_run_request;

  ProcessResult compile(const CompileRequest &request) override {
    ++compile_calls;
    last_compile_request = request;
    // 模拟编译器在成功时生成可执行文件，供 JudgeEngine 的存在性检查通过。
    if (compile_result.launched && !compile_result.launch_error &&
        !compile_result.timed_out && compile_result.exited &&
        compile_result.exit_code == 0) {
      std::ofstream out(request.output_path, std::ios::binary);
      out << "fake-binary";
    }
    return compile_result;
  }

  ProcessResult run(const RunRequest &request, const std::string &input) override {
    ++run_calls;
    last_run_request = request;
    run_inputs.push_back(input);
    if (run_calls <= static_cast<int>(run_results.size())) {
      return run_results[run_calls - 1];
    }
    return ok_run("");
  }
};

class WorkspaceRoot {
public:
  WorkspaceRoot() {
    base_ = std::filesystem::temp_directory_path() /
            ("oj_judge_unit_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter_++));
    std::error_code ec;
    std::filesystem::create_directories(base_, ec);
  }
  ~WorkspaceRoot() {
    std::error_code ec;
    std::filesystem::remove_all(base_, ec);
  }
  std::string path() const { return base_.string(); }
  bool empty() const {
    std::error_code ec;
    return std::filesystem::is_empty(base_, ec);
  }

private:
  std::filesystem::path base_;
  static int counter_;
};

int WorkspaceRoot::counter_ = 0;

JudgeTask make_task(const std::string &language,
                    std::vector<Testcase> testcases) {
  JudgeTask task;
  task.language = language;
  task.source_code = "int main(){}";
  task.testcases = std::move(testcases);
  return task;
}

TEST(JudgeEngineFake, AllAcRunsEveryCaseInOrder) {
  FakeExecutor executor;
  executor.run_results = {ok_run("3\n"), ok_run("50\n")};
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeTask task = make_task("cpp17", {{"1 2\n", "3\n"}, {"100 -50\n", "50\n"}});
  JudgeResult result = engine.judge(task);

  EXPECT_EQ(result.status, JudgeStatus::AC);
  EXPECT_TRUE(result.compile_ok);
  EXPECT_EQ(executor.compile_calls, 1);
  EXPECT_EQ(executor.run_calls, 2);
  EXPECT_EQ(result.passed, 2);
  ASSERT_EQ(executor.run_inputs.size(), 2u);
  EXPECT_EQ(executor.run_inputs[0], "1 2\n");
  EXPECT_EQ(executor.run_inputs[1], "100 -50\n");
  EXPECT_TRUE(root.empty()) << "判题结束后不应遗留工作目录";
}

TEST(JudgeEngineFake, WaDoesNotStopLaterCases) {
  FakeExecutor executor;
  executor.run_results = {ok_run("3\n"), ok_run("999\n"), ok_run("50\n")};
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeTask task =
      make_task("cpp17", {{"1 2\n", "3\n"}, {"2 3\n", "5\n"}, {"100 -50\n", "50\n"}});
  JudgeResult result = engine.judge(task);

  EXPECT_EQ(result.status, JudgeStatus::WA);
  EXPECT_EQ(executor.run_calls, 3);
  ASSERT_EQ(result.cases.size(), 3u);
  EXPECT_EQ(result.cases[0].status, JudgeStatus::AC);
  EXPECT_EQ(result.cases[1].status, JudgeStatus::WA);
  EXPECT_EQ(result.cases[2].status, JudgeStatus::AC);
  EXPECT_EQ(result.cases[1].actual_output, "999\n");
  EXPECT_EQ(result.passed, 2);
}

TEST(JudgeEngineFake, MixedWaAndTlePrefersTle) {
  FakeExecutor executor;
  ProcessResult tle;
  tle.launched = true;
  tle.timed_out = true;
  executor.run_results = {ok_run("999\n"), tle};
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeTask task = make_task("cpp17", {{"1\n", "1\n"}, {"2\n", "2\n"}});
  JudgeResult result = engine.judge(task);

  EXPECT_EQ(result.status, JudgeStatus::TLE);
  ASSERT_EQ(result.cases.size(), 2u);
  EXPECT_TRUE(result.cases[1].timed_out);
}

TEST(JudgeEngineFake, NonZeroExitIsRe) {
  FakeExecutor executor;
  ProcessResult re;
  re.launched = true;
  re.exited = true;
  re.exit_code = 1;
  executor.run_results = {re};
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task("c11", {{"1\n", "1\n"}}));
  EXPECT_EQ(result.status, JudgeStatus::RE);
}

TEST(JudgeEngineFake, SignalTerminationIsRe) {
  FakeExecutor executor;
  ProcessResult re;
  re.launched = true;
  re.exited = false;
  re.term_signal = 11;
  executor.run_results = {re};
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task("cpp17", {{"", ""}}));
  EXPECT_EQ(result.status, JudgeStatus::RE);
}

TEST(JudgeEngineFake, TruncatedOutputNeverAc) {
  FakeExecutor executor;
  ProcessResult truncated = ok_run("3\n");
  truncated.stdout_truncated = true;
  executor.run_results = {truncated};
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task("cpp17", {{"1 2\n", "3\n"}}));
  EXPECT_NE(result.status, JudgeStatus::AC);
  EXPECT_EQ(result.status, JudgeStatus::RE);
}

TEST(JudgeEngineFake, CompileErrorIsCeAndSkipsRunning) {
  FakeExecutor executor;
  ProcessResult ce;
  ce.launched = true;
  ce.exited = true;
  ce.exit_code = 1;
  ce.stdout_data = "main.cpp:1: error: expected ';'";
  executor.compile_result = ce;
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task("cpp17", {{"1 2\n", "3\n"}}));
  EXPECT_EQ(result.status, JudgeStatus::CE);
  EXPECT_FALSE(result.compile_ok);
  EXPECT_EQ(executor.run_calls, 0);
  EXPECT_NE(result.compile_output.find("error"), std::string::npos);
}

TEST(JudgeEngineFake, CompilerLaunchFailureIsSyserrNotCe) {
  FakeExecutor executor;
  ProcessResult launch;
  launch.launch_error = true;
  launch.launch_error_message = "No such file or directory";
  executor.compile_result = launch;
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task("cpp17", {{"1 2\n", "3\n"}}));
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  EXPECT_NE(result.status, JudgeStatus::CE);
  EXPECT_FALSE(result.compile_ok);
  EXPECT_EQ(executor.run_calls, 0);
}

TEST(JudgeEngineFake, RunLaunchFailureIsSyserrAndStops) {
  FakeExecutor executor;
  ProcessResult launch;
  launch.launch_error = true;
  launch.launch_error_message = "exec failed";
  executor.run_results = {launch, ok_run("3\n")};
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeResult result =
      engine.judge(make_task("cpp17", {{"1\n", "1\n"}, {"2\n", "2\n"}}));
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  EXPECT_EQ(executor.run_calls, 1) << "内部执行故障应提前终止";
}

TEST(JudgeEngineFake, InvalidLanguageIsSyserrWithoutCompile) {
  FakeExecutor executor;
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task("java", {{"1\n", "1\n"}}));
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  EXPECT_EQ(executor.compile_calls, 0);
  EXPECT_EQ(executor.run_calls, 0);
  EXPECT_FALSE(result.message.empty());
}

TEST(JudgeEngineFake, EmptyTestcasesIsSyserrWithoutCompile) {
  FakeExecutor executor;
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task("cpp17", {}));
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  EXPECT_NE(result.status, JudgeStatus::AC);
  EXPECT_EQ(executor.compile_calls, 0);
}

TEST(JudgeEngineFake, InvalidTimeLimitIsSyserr) {
  FakeExecutor executor;
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeTask task = make_task("cpp17", {{"1\n", "1\n"}});
  task.time_limit_ms = 0;
  JudgeResult result = engine.judge(task);
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  EXPECT_EQ(executor.compile_calls, 0);
}

TEST(JudgeEngineFake, TimeLimitClampedToHardCap) {
  // SPEC JUDGE-10：题目时限再大也受全局硬上限约束。
  FakeExecutor executor;
  executor.run_results = {ok_run("1\n")};
  JudgeOptions options;
  options.max_time_limit_ms = 5000;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeTask task = make_task("cpp17", {{"", "1\n"}});
  task.time_limit_ms = 120000;
  JudgeResult result = engine.judge(task);

  EXPECT_EQ(result.status, JudgeStatus::AC);
  EXPECT_EQ(executor.last_run_request.time_limit_ms, 5000);

  // 默认硬上限为 60s。
  FakeExecutor executor2;
  executor2.run_results = {ok_run("1\n")};
  JudgeOptions options2;
  WorkspaceRoot root2;
  options2.workspace_root = root2.path();
  JudgeEngine engine2(executor2, options2);
  JudgeTask task2 = make_task("cpp17", {{"", "1\n"}});
  task2.time_limit_ms = 120000;
  engine2.judge(task2);
  EXPECT_EQ(executor2.last_run_request.time_limit_ms, 60000);
}

TEST(JudgeEngineFake, LanguageSelectsCompilerSourceAndCompileLimit) {
  FakeExecutor executor;
  executor.run_results = {ok_run("1\n"), ok_run("1\n")};
  JudgeOptions options;
  options.cpp_compiler = "cpp-compiler-x";
  options.c_compiler = "c-compiler-x";
  options.compile_time_limit_ms = 7000;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  engine.judge(make_task("cpp17", {{"", "1\n"}}));
  EXPECT_EQ(executor.last_compile_request.compiler, "cpp-compiler-x");
  EXPECT_EQ(executor.last_compile_request.language, Language::Cpp17);
  EXPECT_NE(executor.last_compile_request.source_path.find("main.cpp"),
            std::string::npos);
  EXPECT_EQ(executor.last_compile_request.time_limit_ms, 7000);

  engine.judge(make_task("c11", {{"", "1\n"}}));
  EXPECT_EQ(executor.last_compile_request.compiler, "c-compiler-x");
  EXPECT_EQ(executor.last_compile_request.language, Language::C11);
  EXPECT_NE(executor.last_compile_request.source_path.find("main.c"),
            std::string::npos);
}

TEST(JudgeEngineFake, AcCaseDoesNotRetainActualOutput) {
  FakeExecutor executor;
  executor.run_results = {ok_run("3\n")};
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task("cpp17", {{"1 2\n", "3\n"}}));
  ASSERT_EQ(result.cases.size(), 1u);
  EXPECT_EQ(result.cases[0].status, JudgeStatus::AC);
  EXPECT_TRUE(result.cases[0].actual_output.empty());
}

} // namespace
