#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "judge/executor.h"
#include "judge/status.h"

namespace oj {
namespace judge {

// 一个测试点：标准输入与期望输出均为文本。
struct Testcase {
  std::string input;
  std::string output;
};

// 判题配置。workspace_root 为空时使用系统临时目录。
struct JudgeOptions {
  std::string workspace_root;
  std::string cpp_compiler = "g++";
  std::string c_compiler = "gcc";
  int compile_time_limit_ms = 10000; // 编译保护超时
  int max_time_limit_ms = 60000;     // 单测试点硬上限（SPEC JUDGE-10）
  // 单次判题全局硬上限：从本次判题开始计时，覆盖编译与全部测试点执行，不包含排队
  // 等待。题目时限为无限或极大时仍然生效，且进入新测试点时不会重置。默认 60s
  // （SPEC JUDGE-10）。
  int global_time_limit_ms = 60000;
  std::size_t stdout_limit_bytes = 64 * 1024;
  std::size_t stderr_limit_bytes = 16 * 1024;
  std::size_t compile_output_limit_bytes = 64 * 1024;
};

// 判题任务。判题核心不依赖 HTTP，也不负责任何提交记录入库。
struct JudgeTask {
  std::string language; // "cpp17"/"c++17"/"cpp" 或 "c11"/"c"
  std::string source_code;
  std::vector<Testcase> testcases; // 按顺序执行
  int time_limit_ms = 2000;
};

// 单个测试点的结果。
struct TestcaseResult {
  int index = 0; // 执行顺序（0 起）
  JudgeStatus status = JudgeStatus::AC;
  long long time_ms = 0;
  bool timed_out = false;
  bool output_truncated = false; // 标准输出超过上限
  bool global_deadline_hit = false; // 该点因全局硬上限（而非单点时限）被终止
  int exit_code = 0;
  int term_signal = 0;
  // 非 AC 时保留实际输出，供 WA 反馈与诊断；有界，最多 stdout_limit_bytes。
  std::string actual_output;
  std::string stderr_output; // 有界采集的标准错误
  std::string message;
};

// 汇总结果。
struct JudgeResult {
  JudgeStatus status = JudgeStatus::SYSERR;
  bool compile_ok = false;
  std::string compile_output; // 有界编译诊断
  std::vector<TestcaseResult> cases;
  int total = 0;
  int passed = 0;
  std::string message;
  // 是否因单次判题全局硬上限而终止（区别于单个测试点超时）。为 true 时未执行的
  // 测试点不会出现在 cases 中，已有结果保留。
  bool global_deadline_hit = false;
  // 是否因服务停止被主动取消（内部错误约定，不归咎于用户程序）。
  bool cancelled = false;
};

// 汇总逐点结果为总体状态：
//   - 没有任何测试点 -> SYSERR（绝不因未执行任何点而返回 AC）；
//   - 存在 SYSERR 点 -> SYSERR；否则 TLE > MLE > RE > WA > AC 取最严重者；
//   - 全部 AC -> AC。
JudgeStatus summarize_cases(const std::vector<TestcaseResult> &cases);

// 判题核心：验证输入 -> 创建工作目录 -> 编译一次 -> 顺序执行测试点 -> 归一化
// 比对 -> 汇总。依赖 IExecutor 抽象，便于单元测试与后续替换沙箱实现。
//
// cancel 非空时支持协作式取消：服务停止后不再启动新进程，正在运行/编译的进程组
// 会在执行器中被终止，未执行的测试点不伪造为已运行或通过。
class JudgeEngine {
public:
  JudgeEngine(IExecutor &executor, JudgeOptions options = {});

  JudgeResult judge(const JudgeTask &task,
                    const CancellationToken *cancel = nullptr);

private:
  IExecutor &executor_;
  JudgeOptions options_;
};

} // namespace judge
} // namespace oj
