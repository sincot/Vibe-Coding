// M3.4 分类与汇总逻辑单元测试（gtest，纯函数，不启动真实进程）。
//
// 覆盖：单点分类的正常/异常结果、同一测试点多个错误迹象时的确定优先级、
// 缺失指标（未采集内存）不误判、Sanitizer 文本不单独导致失败、真实退出原因
// 仍由集成测试验证；以及总体汇总的严重度与顺序无关性、编译诊断路径清洗。

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "judge/classification.h"
#include "judge/judge.h"

namespace {

using oj::judge::CaseEvidence;
using oj::judge::CaseVerdict;
using oj::judge::classify_case;
using oj::judge::JudgeStatus;
using oj::judge::scrub_compile_diagnostics;
using oj::judge::summarize_cases;
using oj::judge::TestcaseResult;

CaseEvidence normal_completed() {
  CaseEvidence e;
  e.exited = true;
  e.exit_code = 0;
  e.output_matches = true;
  return e;
}

TEST(ClassifyCase, NormalMatchIsAc) {
  const CaseVerdict v = classify_case(normal_completed());
  EXPECT_EQ(v.status, JudgeStatus::AC);
  EXPECT_FALSE(v.global_deadline_hit);
  EXPECT_FALSE(v.abort_remaining);
}

TEST(ClassifyCase, NormalMismatchIsWa) {
  CaseEvidence e = normal_completed();
  e.output_matches = false;
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::WA);
  EXPECT_FALSE(v.abort_remaining);
}

TEST(ClassifyCase, NonZeroExitIsReEvenIfOutputMatches) {
  CaseEvidence e = normal_completed();
  e.exit_code = 7;
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::RE);
  EXPECT_NE(v.message.find("7"), std::string::npos);
}

TEST(ClassifyCase, SignalIsRe) {
  CaseEvidence e = normal_completed();
  e.exited = false;
  e.term_signal = 11; // SIGSEGV
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::RE);
  EXPECT_NE(v.message.find("11"), std::string::npos);
}

TEST(ClassifyCase, SingleTimeoutIsTleAndDoesNotAbort) {
  CaseEvidence e;
  e.timed_out = true;
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::TLE);
  EXPECT_FALSE(v.global_deadline_hit);
  EXPECT_FALSE(v.abort_remaining);
}

TEST(ClassifyCase, GlobalCappedTimeoutIsGlobalDeadline) {
  CaseEvidence e;
  e.timed_out = true;
  e.global_capped = true;
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::TLE);
  EXPECT_TRUE(v.global_deadline_hit);
  EXPECT_TRUE(v.abort_remaining);
}

TEST(ClassifyCase, MemoryEvidenceIsMle) {
  CaseEvidence e = normal_completed();
  e.memory_exceeded = true;
  e.peak_memory_kb = 40000;
  e.memory_limit_kb = 32768;
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::MLE);
}

// SIGKILL（信号 9）但无可靠 RSS 证据：不得据此判 MLE，按 RE 处理。
TEST(ClassifyCase, SignalWithoutMemoryEvidenceIsNotMle) {
  CaseEvidence e = normal_completed();
  e.exited = false;
  e.term_signal = 9;
  e.memory_exceeded = false;
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::RE);
}

// 同时出现超时与内存证据时，按确定性优先级取内存（更具体的资源证据）。
TEST(ClassifyCase, MultipleSignsPreferMemoryEvidence) {
  CaseEvidence e;
  e.timed_out = true;
  e.memory_exceeded = true;
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::MLE);
}

TEST(ClassifyCase, LaunchFailureIsSyserrAndAborts) {
  CaseEvidence e;
  e.launch_error = true;
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::SYSERR);
  EXPECT_TRUE(v.abort_remaining);
}

TEST(ClassifyCase, SandboxFailureMessageMentionsSandbox) {
  CaseEvidence e;
  e.launch_error = true;
  e.sandbox_error = true;
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::SYSERR);
  EXPECT_NE(v.message.find("沙箱"), std::string::npos);
}

TEST(ClassifyCase, CancelledIsSyserrAndAborts) {
  CaseEvidence e = normal_completed();
  e.cancelled = true;
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::SYSERR);
  EXPECT_TRUE(v.abort_remaining);
}

TEST(ClassifyCase, OutputTruncationIsReEvenIfMatches) {
  CaseEvidence e = normal_completed();
  e.output_truncated = true;
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::RE);
}

// 仅凭用户可打印的 Sanitizer 类似文本（进程正常退出、输出匹配）不得判失败。
TEST(ClassifyCase, SanitizerTextAloneDoesNotFail) {
  CaseEvidence e = normal_completed();
  e.sanitizer_error = true; // 正常退出却带有疑似文本
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::AC);
}

TEST(ClassifyCase, SanitizerWithSignalAddsHint) {
  CaseEvidence e = normal_completed();
  e.exited = false;
  e.term_signal = 6; // SIGABRT
  e.sanitizer_error = true;
  const CaseVerdict v = classify_case(e);
  EXPECT_EQ(v.status, JudgeStatus::RE);
  EXPECT_NE(v.message.find("Sanitizer"), std::string::npos);
}

std::vector<TestcaseResult> make_cases(std::initializer_list<JudgeStatus> s) {
  std::vector<TestcaseResult> cases;
  for (JudgeStatus status : s) {
    TestcaseResult r;
    r.status = status;
    cases.push_back(r);
  }
  return cases;
}

TEST(SummarizeCases, EmptyIsSyserr) {
  EXPECT_EQ(summarize_cases({}), JudgeStatus::SYSERR);
}

TEST(SummarizeCases, AllAcIsAc) {
  EXPECT_EQ(summarize_cases(make_cases({JudgeStatus::AC, JudgeStatus::AC})),
            JudgeStatus::AC);
}

TEST(SummarizeCases, WorstWinsAndIndependentOfOrder) {
  const auto a = make_cases({JudgeStatus::WA, JudgeStatus::TLE,
                             JudgeStatus::RE, JudgeStatus::AC});
  const auto b = make_cases({JudgeStatus::AC, JudgeStatus::RE,
                             JudgeStatus::TLE, JudgeStatus::WA});
  EXPECT_EQ(summarize_cases(a), JudgeStatus::TLE);
  EXPECT_EQ(summarize_cases(b), JudgeStatus::TLE);
}

TEST(SummarizeCases, MleOutranksReAndWa) {
  EXPECT_EQ(summarize_cases(make_cases({JudgeStatus::WA, JudgeStatus::RE,
                                        JudgeStatus::MLE})),
            JudgeStatus::MLE);
}

TEST(SummarizeCases, SyserrOutranksAll) {
  EXPECT_EQ(summarize_cases(make_cases({JudgeStatus::TLE, JudgeStatus::MLE,
                                        JudgeStatus::RE, JudgeStatus::SYSERR})),
            JudgeStatus::SYSERR);
}

TEST(ScrubDiagnostics, RemovesWorkspaceAndSandboxPrefix) {
  const std::string ws = "/tmp/oj_judge_abc123";
  const std::string text = ws + "/.oj_sandbox/box/main.cpp:3:5: error: bad\n"
                           "/box/main.cpp:9:1: note: here";
  const std::string out = scrub_compile_diagnostics(text, ws);
  EXPECT_EQ(out.find(ws), std::string::npos);
  EXPECT_EQ(out.find(".oj_sandbox"), std::string::npos);
  EXPECT_EQ(out.find("/box/"), std::string::npos);
  // 用户代码定位信息保留（文件名与行列、诊断文本）。
  EXPECT_NE(out.find("main.cpp:3:5: error: bad"), std::string::npos);
  EXPECT_NE(out.find("main.cpp:9:1: note: here"), std::string::npos);
}

TEST(ScrubDiagnostics, EmptyWorkspaceIsSafe) {
  const std::string text = "error: something";
  EXPECT_EQ(scrub_compile_diagnostics(text, ""), text);
}

} // namespace
