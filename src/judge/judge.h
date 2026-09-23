#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "judge/executor.h"
#include "judge/status.h"

namespace oj {
namespace judge {

class CompileGate;

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
  // 沙箱与资源限制（M3.3）：
  //   - sandbox_enabled=false 仅用于本机开发/单元测试；正式服务始终保持 true，
  //     沙箱不可用时拒绝执行，绝不降级为无保护运行；
  //   - compile_memory_limit_kb 为编译器 RSS 预算（编译器会派生多个子进程）；
  //   - default_memory_limit_kb 为题目未提供内存上限时的默认值。
  bool sandbox_enabled = true;
  long long compile_memory_limit_kb = 1024 * 1024;
  long long default_memory_limit_kb = 65536;
  // 全局编译并发门限（可选）。非空时，单次判题在编译前获取许可，等待计入本次
  // 判题的全局硬上限并可被取消；编译结束立即释放。为空表示不限制（测试/单任务）。
  std::shared_ptr<CompileGate> compile_gate;
  // 额外的编译选项（追加在语言标准选项之后）。供 M3.4 接入 ASan/UBSan 或测试注入
  // 使用；为空时保持当前编译选项不变。
  std::vector<std::string> extra_compile_flags;
};

// 判题任务。判题核心不依赖 HTTP，也不负责任何提交记录入库。
struct JudgeTask {
  std::string language; // "cpp17"/"c++17"/"cpp" 或 "c11"/"c"
  std::string source_code;
  std::vector<Testcase> testcases; // 按顺序执行
  int time_limit_ms = 2000;
  long long memory_limit_kb = 65536; // 题目内存上限（RSS），超限判 MLE
};

// 单个测试点的结果。
struct TestcaseResult {
  int index = 0; // 执行顺序（0 起）
  JudgeStatus status = JudgeStatus::AC;
  long long time_ms = 0;
  long long memory_kb = 0;       // 观测峰值 RSS（kB），0 表示未采集到
  bool memory_exceeded = false;  // 因 RSS 超限被强制终止
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
