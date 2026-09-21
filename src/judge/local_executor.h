#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "judge/executor.h"

namespace oj {
namespace judge {

// 本机进程执行器（M1.5，仅用于开发环境验证）。
//
// 使用 fork + execvp 通过参数数组启动编译器与程序，不经过 shell，避免注入。
// 具备基础能力：
//   - 编译保护超时与运行超时，超时 SIGKILL 强杀并通过 waitpid 回收；
//   - 标准输入写入、标准输出/标准错误分别采集；
//   - 输出有界采集，超过上限继续排空管道但不无界增长；
//   - 所有路径关闭文件描述符，不遗留子进程。
//
// 注意：本实现**不是完整沙箱**。M1.5 未接入 setrlimit、seccomp、tmpfs 与
// 内存限制，不可用于公开接收不可信代码；这些能力在 M3 完善。
class LocalExecutor : public IExecutor {
public:
  ProcessResult compile(const CompileRequest &request) override;
  ProcessResult run(const RunRequest &request, const std::string &input) override;

private:
  // 以 argv 启动进程：stdin 写入 input，stdout/stderr 有界采集。
  // merge_stderr=true 时诊断信息合并到 stdout_data（供编译使用）。
  ProcessResult spawn(const std::vector<std::string> &argv,
                      const std::string &working_directory,
                      const std::string &input, int time_limit_ms,
                      std::size_t stdout_limit, std::size_t stderr_limit,
                      bool merge_stderr);
};

} // namespace judge
} // namespace oj
