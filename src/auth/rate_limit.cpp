#include "auth/rate_limit.h"

#include <functional>

namespace oj {
namespace auth {

namespace {
// 每处理若干次请求执行一次全量清理，删除已完全过期的 key，避免状态无限增长。
constexpr std::size_t kSweepInterval = 1024;
} // namespace

RateLimiter::RateLimiter(Config cfg)
    : cfg_(cfg),
      clock_([]() { return std::chrono::steady_clock::now(); }) {}

RateLimiter::Result RateLimiter::allow(const std::string &key) {
  const auto now = clock_();
  const auto window = std::chrono::seconds(cfg_.window_seconds);

  std::lock_guard<std::mutex> lock(mutex_);

  // 周期性全量清理：删除窗口已完全过期的 key。
  if (++sweep_counter_ >= kSweepInterval) {
    sweep_counter_ = 0;
    for (auto it = failures_.begin(); it != failures_.end();) {
      auto &dq = it->second;
      while (!dq.empty() && now - dq.front() > window) {
        dq.pop_front();
      }
      if (dq.empty()) {
        it = failures_.erase(it);
      } else {
        ++it;
      }
    }
  }

  auto it = failures_.find(key);
  if (it != failures_.end()) {
    auto &dq = it->second;
    while (!dq.empty() && now - dq.front() > window) {
      dq.pop_front();
    }
    if (dq.empty()) {
      failures_.erase(it);
    } else if (static_cast<int>(dq.size()) >= cfg_.max_failures) {
      // 已到达阈值：解除还需等待窗口剩余时间（自最早失败时刻起算）。
      const auto oldest = dq.front();
      const auto remaining = window - (now - oldest);
      Result r;
      r.status = Status::Blocked;
      r.retry_after_seconds = static_cast<int>(
          (remaining + std::chrono::seconds(1) -
           std::chrono::steady_clock::duration(1)) /
          std::chrono::seconds(1));
      if (r.retry_after_seconds < 1) {
        r.retry_after_seconds = 1;
      }
      return r;
    }
  }

  // 占用一个失败名额：登录失败后无需再记录；登录成功则调用 clear 清除。
  failures_[key].push_back(now);
  return Result{Status::Allowed, 0};
}

void RateLimiter::clear(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);
  failures_.erase(key);
}

std::size_t RateLimiter::tracked_keys() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return failures_.size();
}

void RateLimiter::set_clock(Clock clock) {
  std::lock_guard<std::mutex> lock(mutex_);
  clock_ = std::move(clock);
}

} // namespace auth
} // namespace oj
