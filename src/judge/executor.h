#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "judge/deadline.h"
#include "judge/status.h"

namespace oj {
namespace judge {

// 支持判题的语言。M1.5 支持 C++17 与 C11 两种。
enum class Language {
  Cpp17,
  C11,
};

// 解析语言标识。接受大小写不敏感的 "c++17"/"cpp17"/"c++"/"cpp"（C++17）与
// "c11"/"c"（C11）。成功写入 out 并返回 true；无法识别返回 false。
bool parse_language(const std::string &text, Language &out);

// 语言的规范名称（"cpp17" / "c11"）。
const char *language_name(Language language);

// 执行层记录的「结构化终止原因」。判题核心据此统一分类，避免各处执行路径各自
// 用布尔组合随意判定。优先级越高（越靠前）越优先，用于同一进程同时出现多个
// 迹象时（如内存超限与超时临界）给出确定结果。
//   Completed      正常退出（退出码 0）
//   MemoryExceeded 依据可靠的 RSS 采样证据超限并强制终止
//   TimedOut       watchdog 单点/全局时间上限强制终止
//   Cancelled      服务停止主动取消（非用户程序超时）
//   NonZeroExit    正常结束但退出码非 0
//   Signaled       被信号终止（崩溃等；非上述主动终止）
//   LaunchFailure  进程/沙箱未能启动（环境或策略故障）
enum class TerminationReason {
  LaunchFailure = 0,
  Cancelled,
  MemoryExceeded,
  TimedOut,
  Signaled,
  NonZeroExit,
  Completed,
};

const char *termination_reason_name(TerminationReason reason);

// 一次子进程执行的结果。进程退出状态、资源限制触发情况与有界采集的输出全部
// 记录在此，由判题核心据此映射为 JudgeStatus。
struct ProcessResult {
  // fork/exec 之后进程是否成功启动。编译器不存在、fork 失败等都属于 launch_error。
  bool launched = false;
  bool launch_error = false;
  std::string launch_error_message;
  // 沙箱初始化（命名空间/挂载/资源限制/seccomp）失败。属于内部/策略故障，
  // 绝不降级为无保护执行；由判题核心映射为 SYSERR 并保留诊断证据。
  bool sandbox_error = false;

  // 执行层记录的结构化终止原因（单一权威来源），供分类层统一处理。
  TerminationReason termination = TerminationReason::LaunchFailure;

  bool timed_out = false; // 是否因超过时间限制被强制终止
  bool cancelled = false; // 是否因服务停止被主动取消（非用户程序超时）
  bool exited = false;    // 是否被 waitpid 正常回收并取得退出状态
  int exit_code = 0;      // exited==true 且非信号终止时的退出码
  int term_signal = 0;    // 被信号终止时的信号编号（0 表示非信号终止）

  // 内存限制：按 RSS 采样并强制终止（不是仅采集数值）。memory_exceeded 为真时
  // 进程因 RSS 超限被终止；memory_kb 为观测到的峰值 RSS（kB）。
  bool memory_exceeded = false;
  long long memory_kb = 0;

  bool stdout_truncated = false; // 标准输出超过上限，已截断
  bool stderr_truncated = false; // 标准错误超过上限，已截断

  // 仅在进程异常终止（信号/非零退出）时，依据诊断内容标注疑似 Sanitizer 报告，
  // 用于补充诊断文案。**不参与状态判定**：不能仅凭用户可自行打印的 stderr 文本
  // 把一次正常执行判为失败，也不能忽略实际退出状态。
  bool sanitizer_error = false;

  std::string stdout_data; // 有界采集的标准输出（编译时可能包含合并后的诊断）
  std::string stderr_data; // 有界采集的标准错误

  long long time_ms = 0; // 从启动到回收的墙钟耗时
};

// 编译请求。executor 负责按语言选择编译器与编译选项，并通过参数数组启动进程，
// 绝不拼接 shell 命令。
struct CompileRequest {
  Language language = Language::Cpp17;
  std::string compiler;         // 编译器可执行名（如 "g++" / "gcc"）
  std::string source_path;      // 源码文件路径
  std::string output_path;      // 生成的可执行文件路径
  std::string working_directory; // 编译进程工作目录
  std::vector<std::string> extra_flags; // 预留：测试/扩展注入的额外编译选项
  // 是否在编译模板中启用 ASan/UBSan（SPEC JUDGE-01）。默认为真；测试可显式关闭
  // 以仅验证基础编译流程。执行器负责选择确切的 sanitizer 选项，调用方不拼接命令。
  bool sanitizers = true;
  int time_limit_ms = 10000;    // 编译保护超时（可能已被剩余全局预算裁剪）
  std::size_t output_limit_bytes = 64 * 1024; // 诊断信息采集上限
  // 编译阶段是否启用沙箱（命名空间最小根 + setrlimit + seccomp 禁网络/危险调用）。
  // 编译需要读取头文件与库、派生 cc1plus/as/ld，故采用比运行阶段宽松但仍受限的策略。
  bool sandbox = true;
  long long memory_limit_kb = 1024 * 1024; // 编译器 RSS 预算
  // 非空时，执行器在编译过程中轮询该令牌，服务停止时尽快终止编译器进程组。
  const CancellationToken *cancel = nullptr;
};

// 单个测试点的运行请求。每个测试点都必须启动新的进程。
struct RunRequest {
  std::string executable_path;
  std::string working_directory;
  int time_limit_ms = 2000;
  std::size_t stdout_limit_bytes = 64 * 1024;
  std::size_t stderr_limit_bytes = 16 * 1024;
  // 运行阶段是否启用沙箱（最小根目录 + setrlimit + seccomp 禁网络/文件逃逸/进程创建）。
  bool sandbox = true;
  long long memory_limit_kb = 65536; // RSS 上限（kB），超限强制终止并标记 MLE
  // 非空时，执行器在运行过程中轮询该令牌，服务停止时尽快终止程序进程组。
  const CancellationToken *cancel = nullptr;
};

// 进程执行抽象接口。将「如何编译/运行子进程」与「如何比对、汇总」解耦：
//   - 判题核心（JudgeEngine）只依赖该接口，可独立单测；
//   - 后续 M3 提供 seccomp/setrlimit/tmpfs 沙箱实现时替换本接口即可。
class IExecutor {
public:
  virtual ~IExecutor() = default;

  // 编译源码。返回 ProcessResult，其中：
  //   - launch_error=true 表示编译器无法启动（环境故障，不得当作 CE）；
  //   - launch_error=false 时可用 exited/exit_code/term_signal 判断编译是否成功。
  virtual ProcessResult compile(const CompileRequest &request) = 0;

  // 以 input 作为标准输入运行一次程序，分别采集标准输出与标准错误。
  virtual ProcessResult run(const RunRequest &request,
                            const std::string &input) = 0;
};

} // namespace judge
} // namespace oj
