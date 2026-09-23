#pragma once

#include <string>

#include "judge/status.h"

namespace oj {
namespace judge {

// 单个测试点的客观证据。由执行层（LocalExecutor/ProcessResult）与判题核心填入，
// 分类层（classify_case）据此统一判定，避免不同执行路径各自随意判定状态。
struct CaseEvidence {
  bool launch_error = false;   // 进程/沙箱启动失败（环境或策略故障）
  bool sandbox_error = false;  // 启动失败是否源于沙箱（用于给出更明确的诊断）
  bool cancelled = false;      // 服务停止主动取消
  bool timed_out = false;      // 单点 watchdog 超时
  bool global_capped = false;  // 单点时限被剩余全局预算裁剪（属全局硬上限）
  bool memory_exceeded = false; // 可靠的 RSS 采样证据（否则不得判 MLE）
  bool exited = false;         // 被 waitpid 正常回收
  int exit_code = 0;           // exited==true 且未信号终止时的退出码
  int term_signal = 0;         // 被信号终止时的信号编号
  bool output_truncated = false; // 标准输出超过上限
  bool sanitizer_error = false;  // 异常终止且诊断疑似 Sanitizer（仅补充文案）
  bool output_matches = false;   // 归一化后输出匹配答案
  // 仅用于生成可读诊断（不影响判定）。
  int time_limit_ms = 0;         // 该点有效时限（毫秒）
  long long memory_limit_kb = 0; // 该点内存上限（kB）
  long long peak_memory_kb = 0;  // 观测峰值 RSS（kB），用于诊断
  long long stdout_limit_bytes = 0; // 标准输出上限，用于诊断
};

// 分类结论。
struct CaseVerdict {
  JudgeStatus status = JudgeStatus::SYSERR;
  std::string message;
  // 该点是否因全局判题硬上限（而非单点时限）终止。
  bool global_deadline_hit = false;
  // 系统/策略故障或取消：无法继续可靠判题，调用方应停止后续测试点。
  bool abort_remaining = false;
};

// 单点分类规则（确定性，与遍历顺序无关）：
//   1. 取消             -> SYSERR（abort_remaining）
//   2. 启动/沙箱失败     -> SYSERR（abort_remaining）
//   3. 可靠内存证据      -> MLE
//   4. 全局裁剪导致的超时 -> TLE + global_deadline_hit（abort_remaining）
//   5. 单点超时          -> TLE
//   6. 信号终止/非正常退出 -> RE
//   7. 输出超限          -> RE（沿用 SPEC JUDGE-05 既定处理，不新增 OLE）
//   8. 输出匹配          -> AC
//   9. 其余              -> WA
// 同一进程同时命中多条时按上述优先级取首个判定，保证结果不依赖执行时序的偶然性。
CaseVerdict classify_case(const CaseEvidence &evidence);

// 清洗编译诊断中的内部路径（工作目录、沙箱临时目录、运行目录等），仅保留定位
// 用户代码错误所需的信息，避免向用户回显服务端内部路径或其它任务信息。
// 传入本次任务的工作目录；同时移除沙箱内部挂载前缀与哨兵目录名。
std::string scrub_compile_diagnostics(const std::string &text,
                                      const std::string &workspace_path);

} // namespace judge
} // namespace oj
