#include "auth/rate_limit.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace oj {
namespace auth {
namespace {

std::int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

LoginRateLimiter::LoginRateLimiter(int max_failures, std::int64_t window_seconds)
    : max_failures_(max_failures > 0 ? max_failures : 1),
      window_seconds_(window_seconds > 0 ? window_seconds : 60) {}

bool LoginRateLimiter::AddFailure(const std::string& key) {
  const std::int64_t now = NowSeconds();
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = buckets_.find(key);
  if (it == buckets_.end()) {
    buckets_[key] = {now, 1};
    return false;
  }
  Bucket& b = it->second;
  if (now - b.last_failure_ >= window_seconds_) {
    b = {now, 1};  // 上一窗口已过期，重新计数
    return false;
  }
  b.count += 1;
  return b.count >= max_failures_;
}

bool LoginRateLimiter::IsLocked(const std::string& key) {
  const std::int64_t now = NowSeconds();
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = buckets_.find(key);
  if (it == buckets_.end()) {
    return false;
  }
  const Bucket& b = it->second;
  if (now - b.last_failure_ >= window_seconds_) {
    buckets_.erase(it);  // 过期即清除（可变更 map，故互斥锁已覆盖）
    return false;
  }
  return b.count >= max_failures_;
}

void LoginRateLimiter::Reset(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  buckets_.erase(key);
}

}  // namespace auth
}  // namespace oj