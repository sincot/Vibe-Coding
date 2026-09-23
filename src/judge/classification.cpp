#include "judge/classification.h"

#include <cstddef>
#include <string>

namespace oj {
namespace judge {

CaseVerdict classify_case(const CaseEvidence &e) {
  CaseVerdict verdict;

  // 1. 服务停止主动取消：内部错误约定，绝不归咎于用户程序。
  if (e.cancelled) {
    verdict.status = JudgeStatus::SYSERR;
    verdict.message = "服务停止，判题已取消";
    verdict.abort_remaining = true;
    return verdict;
  }

  // 2. 进程/沙箱启动失败：环境或策略故障，绝不伪装成用户 CE/RE。
  if (e.launch_error) {
    verdict.status = JudgeStatus::SYSERR;
    verdict.message = e.sandbox_error ? "运行沙箱不可用" : "无法启动运行进程";
    verdict.abort_remaining = true;
    return verdict;
  }

  // 3. 内存超限必须基于可靠证据（RSS 采样超限并强制终止）。仅有 SIGKILL、
  //    bad_alloc、分配失败或 Sanitizer 启动失败等迹象时不据此判 MLE。
  if (e.memory_exceeded) {
    verdict.status = JudgeStatus::MLE;
    verdict.message = "超出内存限制（RSS > " +
                      std::to_string(e.memory_limit_kb) + " kB，峰值 " +
                      std::to_string(e.peak_memory_kb) + " kB）";
    return verdict;
  }

  // 4. 可用全局预算不足以完成该点：属全局硬上限终止（内部兜底），不是单点超时。
  if (e.timed_out && e.global_capped) {
    verdict.status = JudgeStatus::TLE;
    verdict.global_deadline_hit = true;
    verdict.abort_remaining = true;
    verdict.message = "触发全局判题时间上限";
    return verdict;
  }

  // 5. 单点超时：普通失败，不阻断后续测试点。
  if (e.timed_out) {
    verdict.status = JudgeStatus::TLE;
    verdict.message =
        "超出时间限制（" + std::to_string(e.time_limit_ms) + " ms）";
    return verdict;
  }

  // 6. 崩溃或异常退出：即使输出碰巧匹配也判 RE，绝不判 AC。
  if (e.term_signal != 0 || (e.exited && e.exit_code != 0) || !e.exited) {
    verdict.status = JudgeStatus::RE;
    if (e.term_signal != 0) {
      verdict.message =
          "运行时被信号 " + std::to_string(e.term_signal) + " 终止";
      if (e.sanitizer_error) {
        verdict.message += "（疑似 Sanitizer 诊断）";
      }
    } else if (e.exited) {
      verdict.message = "非零退出码 " + std::to_string(e.exit_code);
    } else {
      verdict.message = "运行进程未正常结束";
    }
    return verdict;
  }

  // 7. 标准输出超限：结果已被截断，绝不能当作正常输出判为 AC（沿用 RE 约定）。
  if (e.output_truncated) {
    verdict.status = JudgeStatus::RE;
    verdict.message = "标准输出超过上限 " +
                      std::to_string(e.stdout_limit_bytes) + " 字节";
    return verdict;
  }

  // 8. 正常执行、无失败诊断、输出未超限且归一化匹配 -> AC。
  if (e.output_matches) {
    verdict.status = JudgeStatus::AC;
    return verdict;
  }

  // 9. 正常执行但输出不匹配 -> WA。
  verdict.status = JudgeStatus::WA;
  verdict.message = "输出不匹配";
  return verdict;
}

std::string scrub_compile_diagnostics(const std::string &text,
                                      const std::string &workspace_path) {
  std::string out = text;
  auto erase_all = [&out](const std::string &needle) {
    if (needle.empty()) {
      return;
    }
    std::size_t pos = 0;
    while ((pos = out.find(needle, pos)) != std::string::npos) {
      out.erase(pos, needle.size());
    }
  };
  // 先移除较长的工作目录路径（含其下的沙箱目录），再移除哨兵目录名与沙箱
  // 内部挂载前缀，避免回显服务端内部路径；保留文件名（如 main.cpp）便于定位。
  erase_all(workspace_path);
  erase_all("/.oj_sandbox");
  // 将沙箱内部工作目录前缀替换为空，使 /box/main.cpp -> main.cpp。
  erase_all("/box/");
  return out;
}

} // namespace judge
} // namespace oj
