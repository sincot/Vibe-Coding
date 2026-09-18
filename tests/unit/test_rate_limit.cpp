// 登录限速单元测试（M1.2）。
//
// 使用可注入的单调时钟，验证：阈值内放行、达到阈值后拒绝、窗口结束恢复、
// 成功清除、并发尝试不可绕过、过期记录自动清理。
//
// 运行方式：ctest --test-dir build -R rate_limit_unit --output-on-failure
// 或直接执行 build/oj_rate_limit_test。

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "auth/rate_limit.h"

namespace {

int g_failures = 0;

void check(bool cond, const std::string &msg) {
  if (cond) {
    std::cout << "  [PASS] " << msg << "\n";
  } else {
    std::cout << "  [FAIL] " << msg << "\n";
    ++g_failures;
  }
}

using Clock = oj::auth::RateLimiter::Clock;
using TimePoint = std::chrono::steady_clock::time_point;

// 可控时钟：now 保存为共享状态，测试通过 set() 推进时间。
struct FakeClock {
  std::atomic<long long> seconds{0};
  TimePoint now() const {
    return TimePoint(std::chrono::seconds(seconds.load()));
  }
};

std::unique_ptr<oj::auth::RateLimiter> make_limiter(int max_failures,
                                                    int window_seconds,
                                                    FakeClock &fc) {
  oj::auth::RateLimiter::Config cfg;
  cfg.max_failures = max_failures;
  cfg.window_seconds = window_seconds;
  auto rl = std::make_unique<oj::auth::RateLimiter>(cfg);
  rl->set_clock([&fc]() { return fc.now(); });
  return rl;
}

void test_threshold_and_block() {
  std::cout << "阈值与拒绝\n";
  FakeClock fc;
  auto rl = make_limiter(3, 60, fc);

  check(rl->allow("ip1").status == oj::auth::RateLimiter::Status::Allowed,
        "第 1 次允许");
  check(rl->allow("ip1").status == oj::auth::RateLimiter::Status::Allowed,
        "第 2 次允许");
  check(rl->allow("ip1").status == oj::auth::RateLimiter::Status::Allowed,
        "第 3 次允许");
  auto blocked = rl->allow("ip1");
  check(blocked.status == oj::auth::RateLimiter::Status::Blocked,
        "第 4 次被拒绝");
  check(blocked.retry_after_seconds > 0, "Retry-After 为正数");
  check(blocked.retry_after_seconds <= 60, "Retry-After 不超过窗口时长");
}

void test_independent_keys() {
  std::cout << "不同来源相互独立\n";
  FakeClock fc;
  auto rl = make_limiter(1, 60, fc);

  check(rl->allow("ip1").status == oj::auth::RateLimiter::Status::Allowed,
        "ip1 首次允许");
  check(rl->allow("ip1").status == oj::auth::RateLimiter::Status::Blocked,
        "ip1 第二次拒绝");
  check(rl->allow("ip2").status == oj::auth::RateLimiter::Status::Allowed,
        "ip2 不受 ip1 影响");
}

void test_window_expiry_recovers() {
  std::cout << "窗口结束恢复\n";
  FakeClock fc;
  auto rl = make_limiter(2, 60, fc);

  rl->allow("ip");
  rl->allow("ip");
  check(rl->allow("ip").status == oj::auth::RateLimiter::Status::Blocked,
        "窗口内达到阈值被拒绝");

  // 推进到窗口结束后，过期记录被清理，恢复可用。
  fc.seconds = 61;
  check(rl->allow("ip").status == oj::auth::RateLimiter::Status::Allowed,
        "窗口结束后恢复允许");
}

void test_clear_on_success() {
  std::cout << "成功清除失败计数\n";
  FakeClock fc;
  auto rl = make_limiter(2, 60, fc);

  rl->allow("ip");
  check(rl->tracked_keys() == 1, "失败记录被跟踪");
  rl->clear("ip");
  check(rl->tracked_keys() == 0, "清除后无跟踪记录");
  check(rl->allow("ip").status == oj::auth::RateLimiter::Status::Allowed,
        "清除后重新计数");
}

void test_concurrent_cannot_bypass() {
  std::cout << "并发尝试不可绕过\n";
  FakeClock fc;
  auto rl = make_limiter(10, 60, fc);

  const int kThreads = 50;
  std::atomic<int> allowed{0};
  std::atomic<int> blocked{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&]() {
      auto r = rl->allow("ip");
      if (r.status == oj::auth::RateLimiter::Status::Allowed) {
        ++allowed;
      } else {
        ++blocked;
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  check(allowed.load() == 10, "并发下恰好 10 次被允许");
  check(blocked.load() == kThreads - 10, "其余被拒绝");
}

void test_bounded_per_key() {
  std::cout << "单 key 状态有界\n";
  FakeClock fc;
  auto rl = make_limiter(3, 60, fc);

  for (int i = 0; i < 100; ++i) {
    rl->allow("ip");
  }
  check(rl->tracked_keys() == 1, "反复失败不新增 key");
}

void test_sweep_clears_expired_keys() {
  std::cout << "周期性清理过期 key，避免无限增长\n";
  FakeClock fc;
  auto rl = make_limiter(5, 60, fc);

  for (int i = 0; i < 10; ++i) {
    rl->allow("old-" + std::to_string(i));
  }
  check(rl->tracked_keys() == 10, "窗口内跟踪 10 个 key");

  // 推进到所有记录过期，并触发周期性全量清理（每 1024 次 allow 一次）。
  fc.seconds = 120;
  for (int i = 0; i < 1024; ++i) {
    rl->allow("new-" + std::to_string(i));
  }
  // 10 个过期 key 被清理，仅剩本次循环新加的 1024 个 key。
  check(rl->tracked_keys() == 1024, "过期 key 已被清理，不无限增长");
}

} // namespace

int main() {
  test_threshold_and_block();
  test_independent_keys();
  test_window_expiry_recovers();
  test_clear_on_success();
  test_concurrent_cannot_bypass();
  test_bounded_per_key();
  test_sweep_clears_expired_keys();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部限速单元测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
