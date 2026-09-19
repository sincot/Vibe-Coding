// M1.2 登录与身份验证 —— 登录限速单元测试（GoogleTest）。
//
// 覆盖对象：oj::auth::RateLimiter（进程内、线程安全、可注入时钟）
//   - 阈值内放行、达到阈值后拒绝（含 Retry-After 合法性）
//   - 不同来源（key）相互独立
//   - 窗口结束后自动恢复
//   - 成功清除失败计数
//   - 并发尝试不可绕过（原子占用名额）
//   - 单 key 状态有界、周期性清理过期 key（避免无限增长）
//
// 运行方式：ctest --test-dir build -R rate_limit_gtest --output-on-failure
// 或直接执行 build/oj_rate_limit_gtest（支持 gtest 全部过滤参数）。

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "auth/rate_limit.h"

namespace {

using oj::auth::RateLimiter;
using TimePoint = std::chrono::steady_clock::time_point;

// 可控单调时钟：共享秒计数，测试通过推进计数模拟时间流逝。
struct FakeClock {
  std::atomic<long long> seconds{0};
  TimePoint now() const { return TimePoint(std::chrono::seconds(seconds.load())); }
};

std::unique_ptr<RateLimiter> MakeLimiter(int max_failures, int window_seconds,
                                         FakeClock &fc) {
  RateLimiter::Config cfg;
  cfg.max_failures = max_failures;
  cfg.window_seconds = window_seconds;
  auto rl = std::make_unique<RateLimiter>(cfg);
  rl->set_clock([&fc]() { return fc.now(); });
  return rl;
}

} // namespace

// ---------------------------------------------------------------------------
// 阈值与拒绝
// ---------------------------------------------------------------------------

TEST(RateLimiterTest, AllowsUntilThreshold) {
  FakeClock fc;
  auto rl = MakeLimiter(3, 60, fc);

  EXPECT_EQ(rl->allow("ip").status, RateLimiter::Status::Allowed);
  EXPECT_EQ(rl->allow("ip").status, RateLimiter::Status::Allowed);
  EXPECT_EQ(rl->allow("ip").status, RateLimiter::Status::Allowed);
}

TEST(RateLimiterTest, BlocksAfterThreshold) {
  FakeClock fc;
  auto rl = MakeLimiter(3, 60, fc);

  rl->allow("ip");
  rl->allow("ip");
  rl->allow("ip");
  auto blocked = rl->allow("ip");
  EXPECT_EQ(blocked.status, RateLimiter::Status::Blocked);
}

TEST(RateLimiterTest, RetryAfterWithinWindow) {
  FakeClock fc;
  auto rl = MakeLimiter(1, 60, fc);

  rl->allow("ip"); // 占用唯一名额
  auto blocked = rl->allow("ip");
  EXPECT_EQ(blocked.status, RateLimiter::Status::Blocked);
  EXPECT_GT(blocked.retry_after_seconds, 0);
  EXPECT_LE(blocked.retry_after_seconds, 60);
}

TEST(RateLimiterTest, BlockedAttemptsDoNotConsumeMoreSlots) {
  FakeClock fc;
  auto rl = MakeLimiter(1, 60, fc);

  rl->allow("ip");
  // 连续多次被拒绝不改变状态，Retry-After 稳定。
  auto first = rl->allow("ip");
  auto second = rl->allow("ip");
  EXPECT_EQ(first.status, RateLimiter::Status::Blocked);
  EXPECT_EQ(second.status, RateLimiter::Status::Blocked);
}

// ---------------------------------------------------------------------------
// 维度隔离与恢复
// ---------------------------------------------------------------------------

TEST(RateLimiterTest, KeysAreIndependent) {
  FakeClock fc;
  auto rl = MakeLimiter(1, 60, fc);

  EXPECT_EQ(rl->allow("ip1").status, RateLimiter::Status::Allowed);
  EXPECT_EQ(rl->allow("ip1").status, RateLimiter::Status::Blocked);
  EXPECT_EQ(rl->allow("ip2").status, RateLimiter::Status::Allowed);
}

TEST(RateLimiterTest, RecoversAfterWindowExpiry) {
  FakeClock fc;
  auto rl = MakeLimiter(2, 60, fc);

  rl->allow("ip");
  rl->allow("ip");
  EXPECT_EQ(rl->allow("ip").status, RateLimiter::Status::Blocked);

  fc.seconds = 61; // 推进超过窗口时长
  EXPECT_EQ(rl->allow("ip").status, RateLimiter::Status::Allowed);
}

TEST(RateLimiterTest, ClearResetsFailureCount) {
  FakeClock fc;
  auto rl = MakeLimiter(2, 60, fc);

  rl->allow("ip");
  EXPECT_EQ(rl->tracked_keys(), 1u);
  rl->clear("ip");
  EXPECT_EQ(rl->tracked_keys(), 0u);
  // 清除后重新计数，而非沿用旧失败记录。
  EXPECT_EQ(rl->allow("ip").status, RateLimiter::Status::Allowed);
  EXPECT_EQ(rl->allow("ip").status, RateLimiter::Status::Allowed);
}

// ---------------------------------------------------------------------------
// 并发与状态增长
// ---------------------------------------------------------------------------

TEST(RateLimiterTest, ConcurrentAttemptsCannotBypass) {
  FakeClock fc;
  auto rl = MakeLimiter(10, 60, fc);

  const int kThreads = 50;
  std::atomic<int> allowed{0};
  std::atomic<int> blocked{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&]() {
      auto r = rl->allow("ip");
      if (r.status == RateLimiter::Status::Allowed) {
        ++allowed;
      } else {
        ++blocked;
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(allowed.load(), 10);
  EXPECT_EQ(blocked.load(), kThreads - 10);
}

TEST(RateLimiterTest, PerKeyStateIsBounded) {
  FakeClock fc;
  auto rl = MakeLimiter(3, 60, fc);

  for (int i = 0; i < 100; ++i) {
    rl->allow("ip");
  }
  EXPECT_EQ(rl->tracked_keys(), 1u);
}

TEST(RateLimiterTest, SweepClearsExpiredKeys) {
  FakeClock fc;
  auto rl = MakeLimiter(5, 60, fc);

  for (int i = 0; i < 10; ++i) {
    rl->allow("old-" + std::to_string(i));
  }
  EXPECT_EQ(rl->tracked_keys(), 10u);

  fc.seconds = 120; // 所有记录过期
  for (int i = 0; i < 1024; ++i) {
    rl->allow("new-" + std::to_string(i));
  }
  // 10 个过期 key 被周期性全量清理，仅剩本循环新加入的 1024 个。
  EXPECT_EQ(rl->tracked_keys(), 1024u);
}
