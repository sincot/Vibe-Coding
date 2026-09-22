// M3.2 截止时间与超时控制单元测试（gtest）。
//
// 覆盖不依赖真实进程/HTTP/数据库的纯逻辑与编排：
//   - 单调时钟剩余时间计算（向上取整、到期为 0）
//   - 有效单点时限 = min(题目时限, 剩余全局预算)，为 0 表示全局耗尽
//   - 全局硬上限：跨多个测试点累计，耗尽后终止剩余点并保留已有结果
//   - 编译保护超时（CE）与全局硬上限裁剪导致的编译超时（SYSERR）区分
//   - 协作式取消：预取消不启动任何进程；执行器返回取消时按内部错误处理
//
// 真实进程、进程组清理与取消信号由 judge_integration / submit_scheduling_api 覆盖。
//
// 运行方式：ctest --test-dir build -R judge_deadline --output-on-failure
// 或直接执行 build/oj_judge_deadline_unit。

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "judge/deadline.h"
#include "judge/executor.h"
#include "judge/judge.h"

namespace {

using oj::judge::CancellationToken;
using oj::judge::CompileRequest;
using oj::judge::Deadline;
using oj::judge::effective_time_limit_ms;
using oj::judge::IExecutor;
using oj::judge::JudgeEngine;
using oj::judge::JudgeOptions;
using oj::judge::JudgeResult;
using oj::judge::JudgeStatus;
using oj::judge::JudgeTask;
using oj::judge::ProcessResult;
using oj::judge::RunRequest;
using oj::judge::Testcase;

class WorkspaceRoot {
public:
  WorkspaceRoot() {
    base_ = std::filesystem::temp_directory_path() /
            ("oj_judge_deadline_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter_++));
    std::error_code ec;
    std::filesystem::create_directories(base_, ec);
  }
  ~WorkspaceRoot() {
    std::error_code ec;
    std::filesystem::remove_all(base_, ec);
  }
  std::string path() const { return base_.string(); }

private:
  std::filesystem::path base_;
  static int counter_;
};
int WorkspaceRoot::counter_ = 0;

// 可控执行器：编译可模拟保护超时/全局裁剪；运行按“每点睡眠”模拟耗时，
// 并在睡眠达到有效时限时返回超时（模拟被 watchdog 杀死）。
class SimExecutor : public IExecutor {
public:
  int per_run_sleep_ms = 0;
  int compile_sleep_ms = 0;
  bool compile_timed_out = false;
  bool cancel_on_run = false;
  bool cancel_on_compile = false;
  int compile_calls = 0;
  int run_calls = 0;
  std::vector<int> run_limits_ms;

  ProcessResult compile(const CompileRequest &request) override {
    ++compile_calls;
    ProcessResult result;
    result.launched = true;
    if (cancel_on_compile) {
      result.cancelled = true;
      result.exited = false;
      return result;
    }
    const int limit = request.time_limit_ms;
    const bool too_slow = compile_sleep_ms >= limit;
    const int actual =
        (compile_sleep_ms > limit) ? limit : compile_sleep_ms;
    if (actual > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(actual));
    }
    if (compile_timed_out || too_slow) {
      result.timed_out = true;
      result.exited = false;
      return result;
    }
    result.exited = true;
    result.exit_code = 0;
    std::ofstream out(request.output_path, std::ios::binary);
    out << "fake-program";
    return result;
  }

  ProcessResult run(const RunRequest &request, const std::string &) override {
    ++run_calls;
    run_limits_ms.push_back(request.time_limit_ms);
    ProcessResult result;
    result.launched = true;
    if (cancel_on_run) {
      result.cancelled = true;
      result.exited = false;
      return result;
    }
    const int limit = request.time_limit_ms;
    const bool too_slow = per_run_sleep_ms >= limit;
    const int actual =
        (per_run_sleep_ms > limit) ? limit : per_run_sleep_ms;
    if (actual > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(actual));
    }
    if (too_slow) {
      result.timed_out = true;
      result.exited = false;
      return result;
    }
    result.exited = true;
    result.exit_code = 0;
    result.stdout_data = "OK\n";
    return result;
  }
};

JudgeTask make_task(int cases, int time_limit_ms = 2000) {
  JudgeTask task;
  task.language = "cpp17";
  task.source_code = "int main(){}";
  task.time_limit_ms = time_limit_ms;
  for (int i = 0; i < cases; ++i) {
    task.testcases.push_back(Testcase{"", "OK\n"});
  }
  return task;
}

// ---------------------------------------------------------------------------
// 截止时间与有效时限
// ---------------------------------------------------------------------------

TEST(JudgeDeadline, RemainingIsRoundedUpAndZeroWhenExpired) {
  const Deadline future = Deadline::after_ms(1000);
  const int remaining = future.remaining_ms();
  EXPECT_GT(remaining, 0);
  EXPECT_LE(remaining, 1000);
  EXPECT_FALSE(future.expired());

  const Deadline past = Deadline::after_ms(0);
  EXPECT_TRUE(past.expired());
  EXPECT_EQ(past.remaining_ms(), 0);

  const Deadline negative = Deadline::after_ms(-100);
  EXPECT_TRUE(negative.expired());
  EXPECT_EQ(negative.remaining_ms(), 0);
}

TEST(JudgeDeadline, EffectiveLimitTakesSmallerBound) {
  EXPECT_EQ(effective_time_limit_ms(2000, 60000), 2000);
  EXPECT_EQ(effective_time_limit_ms(2000, 1500), 1500);
  EXPECT_EQ(effective_time_limit_ms(2000, 2000), 2000);
  // 全局预算或题目时限非正时不启动测试点。
  EXPECT_EQ(effective_time_limit_ms(2000, 0), 0);
  EXPECT_EQ(effective_time_limit_ms(0, 5000), 0);
  EXPECT_EQ(effective_time_limit_ms(2000, -10), 0);
}

TEST(JudgeDeadline, DefaultGlobalLimitIsSixtySeconds) {
  // 正式配置必须符合 SPEC JUDGE-10 的单次判题全局 60s 硬上限。
  const JudgeOptions options;
  EXPECT_EQ(options.global_time_limit_ms, 60000);
  EXPECT_EQ(options.max_time_limit_ms, 60000);
}

// ---------------------------------------------------------------------------
// 全局硬上限
// ---------------------------------------------------------------------------

TEST(JudgeEngineGlobal, ExhaustsBudgetAndKeepsEarlierResults) {
  SimExecutor executor;
  executor.per_run_sleep_ms = 250;
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  options.max_time_limit_ms = 60000;
  options.global_time_limit_ms = 2000;
  JudgeEngine engine(executor, options);

  // 题目时限 60s：每个测试点单独都远未超时，但总耗时达到全局上限。
  JudgeResult result = engine.judge(make_task(12, 60000));

  EXPECT_TRUE(result.global_deadline_hit);
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  EXPECT_EQ(result.total, 12);
  ASSERT_GE(result.cases.size(), 2u);
  EXPECT_LT(result.cases.size(), 12u) << "全局耗尽后不再启动新测试点";
  // 已执行的测试点结果保留；只有因全局上限终止的那个点不是 AC。
  size_t ac_count = 0;
  for (const auto &item : result.cases) {
    if (item.status == JudgeStatus::AC) {
      ++ac_count;
    }
  }
  EXPECT_EQ(ac_count, result.cases.size() - 1);
  EXPECT_TRUE(result.cases.back().global_deadline_hit);
  EXPECT_EQ(result.cases.back().status, JudgeStatus::TLE);
  EXPECT_EQ(result.passed, static_cast<int>(ac_count));
  // 未执行的测试点没有出现在结果中，不会伪造成已运行或通过。
  EXPECT_LT(static_cast<int>(result.cases.size()), result.total);
}

TEST(JudgeEngineGlobal, CompileProtectionTimeoutIsCeNotGlobal) {
  SimExecutor executor;
  executor.compile_timed_out = true;
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  options.compile_time_limit_ms = 10000;
  options.global_time_limit_ms = 60000;
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task(1, 2000));
  EXPECT_FALSE(result.global_deadline_hit);
  EXPECT_EQ(result.status, JudgeStatus::CE);
  EXPECT_FALSE(result.compile_ok);
}

TEST(JudgeEngineGlobal, CompileTimeoutCausedByGlobalCapIsSyserr) {
  SimExecutor executor;
  executor.compile_timed_out = true;
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  options.compile_time_limit_ms = 10000;
  options.global_time_limit_ms = 100; // 编译预算被全局上限裁剪
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task(1, 2000));
  EXPECT_TRUE(result.global_deadline_hit);
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  EXPECT_NE(result.status, JudgeStatus::CE);
}

TEST(JudgeEngineGlobal, CompileTimeCountsTowardGlobalBudget) {
  // 全局截止时间从判题开始即生效，编译耗时也应计入；编译后剩余预算决定单点有效时限。
  SimExecutor executor;
  executor.compile_sleep_ms = 300;
  executor.per_run_sleep_ms = 5000;
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  options.compile_time_limit_ms = 10000;
  options.global_time_limit_ms = 400;
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task(1, 2000));
  EXPECT_TRUE(result.global_deadline_hit);
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  ASSERT_EQ(executor.run_limits_ms.size(), 1u);
  EXPECT_LT(executor.run_limits_ms[0], 2000)
      << "编译已消耗部分全局预算，单点有效时限应小于题目时限";
  EXPECT_LE(executor.run_limits_ms[0], 250);
}

// ---------------------------------------------------------------------------
// 协作式取消
// ---------------------------------------------------------------------------

TEST(JudgeEngineCancel, PreCancelledStartsNoProcessAndIsSyserr) {
  SimExecutor executor;
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  CancellationToken token;
  token.cancel();
  JudgeResult result = engine.judge(make_task(3), &token);

  EXPECT_TRUE(result.cancelled);
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  EXPECT_NE(result.status, JudgeStatus::AC);
  EXPECT_EQ(executor.compile_calls, 0);
  EXPECT_EQ(executor.run_calls, 0);
  EXPECT_TRUE(result.cases.empty());
}

TEST(JudgeEngineCancel, CancelledRunStopsRemainingCases) {
  SimExecutor executor;
  executor.cancel_on_run = true;
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task(4, 2000));
  EXPECT_TRUE(result.cancelled);
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  EXPECT_EQ(executor.run_calls, 1) << "取消后不再启动后续测试点";
  ASSERT_EQ(result.cases.size(), 1u);
  EXPECT_EQ(result.cases[0].status, JudgeStatus::SYSERR);
}

TEST(JudgeEngineCancel, CancelledCompileIsSyserrWithoutRun) {
  SimExecutor executor;
  executor.cancel_on_compile = true;
  JudgeOptions options;
  WorkspaceRoot root;
  options.workspace_root = root.path();
  JudgeEngine engine(executor, options);

  JudgeResult result = engine.judge(make_task(2, 2000));
  EXPECT_TRUE(result.cancelled);
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  EXPECT_EQ(executor.compile_calls, 1);
  EXPECT_EQ(executor.run_calls, 0);
  EXPECT_TRUE(result.cases.empty());
}

} // namespace
