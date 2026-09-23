#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "judge/executor.h"
#include "judge/sandbox.h"

namespace oj {
namespace judge {

// 本机进程执行器。
//
// 通过 fork + execve 以参数数组启动编译器与程序，不经过 shell，避免注入。
// M3.3 起为编译与运行提供进程级沙箱：
//   - 每次执行创建独立随机工作目录（默认 tmpfs），并在用户/挂载/PID/网络/IPC/UTS
//     命名空间中搭建最小根目录，仅暴露系统库、工作目录与必要设备；
//   - setrlimit 限制 CPU/文件/栈/描述符，RSS 采样强制内存上限；
//   - seccomp-bpf 拒绝网络、挂载/逃逸、调试/内核接口，运行阶段额外禁止创建进程；
//   - 子进程继承受控的最小环境，绝不传递服务密钥等敏感变量；
//   - 沙箱初始化失败即拒绝执行（launch_error + sandbox_error），绝不降级。
//
// 仍然具备：编译/运行超时、SIGKILL 终止整个进程组、waitpid 回收、输出有界采集。
class LocalExecutor : public IExecutor {
public:
  ProcessResult compile(const CompileRequest &request) override;
  ProcessResult run(const RunRequest &request, const std::string &input) override;

  // 启动自检：在指定工作目录根下真实运行一次沙箱化进程，验证命名空间、最小根
  // 目录、seccomp 与 setrlimit 均可用。失败时返回 false 并给出明确原因，供服务
  // 在初始化阶段拒绝带病启动。workspace_root 为空时使用系统临时目录。
  static bool sandbox_self_test(const std::string &workspace_root,
                                std::string &error);

private:
  // 以 argv 启动进程：stdin 写入 input，stdout/stderr 有界采集。
  // merge_stderr=true 时诊断信息合并到 stdout_data（供编译使用）。
  // sandbox=true 时启用上述沙箱；phase 决定 seccomp/limits 策略。
  // cancel 非空时轮询取消令牌，服务停止时终止整个进程组。
  ProcessResult spawn(const std::vector<std::string> &argv,
                      const std::string &working_directory,
                      const std::string &input, int time_limit_ms,
                      std::size_t stdout_limit, std::size_t stderr_limit,
                      bool merge_stderr, const CancellationToken *cancel,
                      bool sandbox, SandboxPhase phase,
                      long long memory_limit_kb);
};

} // namespace judge
} // namespace oj
