#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace oj {
namespace auth {

// 基础登录限速：按「来源（客户端 IP）」维度统计同一时间窗口内的连续失败次数，
// 超过阈值即拒绝（配合 HTTP 层返回 429 + Retry-After）。
//
// 实现为进程内状态（互斥锁保护的滑动窗口），不做跨重启持久化——SPEC 未要求，
// 且教学班规模下进程内实现足够；重启后计数自然清零。
class RateLimiter {
public:
  struct Config {
    int max_failures = 5;       // 窗口内允许的最大失败次数
    int window_seconds = 900;   // 滑动窗口时长（15 分钟）
  };

  // 可注入时钟（返回单调时钟时间点），便于单元测试用可控时钟验证窗口/解除行为。
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  enum class Status {
    Allowed,  // 允许本次尝试（已原子占用一个失败名额，失败后无需再记录）
    Blocked,  // 已达阈值，拒绝本次尝试
  };

  struct Result {
    Status status = Status::Allowed;
    int retry_after_seconds = 0; // Blocked 时有效：还需等待的秒数（向上取整）
  };

  explicit RateLimiter(Config cfg);

  // 原子地占用一个失败名额并返回是否允许。线程安全，并发尝试无法绕过限速；
  // 每次调用都会清理过期记录，避免状态无限增长。
  Result allow(const std::string &key);

  // 清除某 key 的全部失败记录（登录成功时调用）。
  void clear(const std::string &key);

  // 仅测试用：当前被跟踪的 key 数量。
  std::size_t tracked_keys() const;

  // 仅测试用：替换时钟。
  void set_clock(Clock clock);

private:
  Config cfg_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>>
      failures_;
  Clock clock_;
  std::size_t sweep_counter_ = 0;
};

} // namespace auth
} // namespace oj
