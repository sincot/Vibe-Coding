#pragma once

#include <atomic>
#include <chrono>

namespace oj {
namespace judge {

// 单调时钟截止时间。全部基于 std::chrono::steady_clock，不受系统时间调整影响，
// 即使程序没有任何输出也能可靠判断超时（不依赖阻塞式读写或等待自然退出）。
class Deadline {
public:
  Deadline() : deadline_(std::chrono::steady_clock::now()) {}
  explicit Deadline(std::chrono::steady_clock::time_point tp) : deadline_(tp) {}

  // 从当前时刻起 after_ms 毫秒后的截止时间。
  static Deadline after_ms(int after_ms) {
    return Deadline(std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(after_ms));
  }

  std::chrono::steady_clock::time_point time_point() const { return deadline_; }

  bool expired() const {
    return std::chrono::steady_clock::now() >= deadline_;
  }

  // 距截止时间的剩余毫秒数，**向上取整**：只要尚未到期就至少返回 1ms，避免因向下
  // 取整把「恰好 60s 的预算」误算成 59999ms。已到期返回 0。
  int remaining_ms() const;

private:
  std::chrono::steady_clock::time_point deadline_;
};

// 有效单点运行时限（毫秒）：题目时限（已按硬上限裁剪）与剩余全局预算的较小者。
// 返回 0 表示剩余全局预算已耗尽，不应再启动新的测试点。
int effective_time_limit_ms(int problem_limit_ms, int remaining_global_ms);

// 协作式取消令牌：服务停止时由 JudgeManager 置位；判题核心与执行器观察它并尽快
// 终止正在运行的子进程。线程安全，可被多个线程同时读取。
class CancellationToken {
public:
  void cancel() noexcept { cancelled_.store(true, std::memory_order_release); }
  bool cancelled() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
  }

private:
  std::atomic<bool> cancelled_{false};
};

} // namespace judge
} // namespace oj
