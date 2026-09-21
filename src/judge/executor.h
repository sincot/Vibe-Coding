#pragma once

#include <cstddef>
#include <string>
#include <vector>

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

// 一次子进程执行的结果。进程退出状态、资源限制触发情况与有界采集的输出全部
// 记录在此，由判题核心据此映射为 JudgeStatus。
struct ProcessResult {
  // fork/exec 之后进程是否成功启动。编译器不存在、fork 失败等都属于 launch_error。
  bool launched = false;
  bool launch_error = false;
  std::string launch_error_message;

  bool timed_out = false; // 是否因超过时间限制被强制终止
  bool exited = false;    // 是否被 waitpid 正常回收并取得退出状态
  int exit_code = 0;      // exited==true 且非信号终止时的退出码
  int term_signal = 0;    // 被信号终止时的信号编号（0 表示非信号终止）

  bool stdout_truncated = false; // 标准输出超过上限，已截断
  bool stderr_truncated = false; // 标准错误超过上限，已截断

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
  std::vector<std::string> extra_flags; // 预留：M3.4 接入 ASan/UBSan 等选项
  int time_limit_ms = 10000;    // 编译保护超时
  std::size_t output_limit_bytes = 64 * 1024; // 诊断信息采集上限
};

// 单个测试点的运行请求。每个测试点都必须启动新的进程。
struct RunRequest {
  std::string executable_path;
  std::string working_directory;
  int time_limit_ms = 2000;
  std::size_t stdout_limit_bytes = 64 * 1024;
  std::size_t stderr_limit_bytes = 16 * 1024;
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
