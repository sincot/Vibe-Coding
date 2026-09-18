#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace oj {
namespace auth {

// 登录限速器：按“账号@IP”记录失败次数，超过阈值即在窗口内锁定，防止暴力爆破。
// 内存实现，进程重启后重置；窗口内失败计数自然过期。
class LoginRateLimiter {
 public:
  LoginRateLimiter(int max_failures = 8, std::int64_t window_seconds = 300);

  // 记录一次登录失败，返回是否已锁定（达到阈值）。
  bool AddFailure(const std::string& key);

  // 是否处于锁定状态（窗口内失败次数 >= 阈值）。命中过期项时顺手清理。
  bool IsLocked(const std::string& key);

  // 登录成功后清除该 key 的失败记录。
  void Reset(const std::string& key);

 private:
  struct Bucket {
    std::int64_t last_failure_;
    int count;
  };

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Bucket> buckets_;
  const int max_failures_;
  const std::int64_t window_seconds_;
};

}  // namespace auth
}  // namespace oj