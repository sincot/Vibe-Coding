// M3.1 判题任务调度器单元测试（gtest）。
//
// 只测试 JudgeManager 的调度边界，不触碰 HTTP、数据库与真实进程：使用可控的执行
// 函数（handler）与同步屏障/计数器，避免依赖长时间 sleep 制造偶然通过。
//
// 覆盖：
//   - worker 数 = min(CPU 核数, 8)，CPU 无法获取时至少 1
//   - 多个任务真正并发执行（同步屏障证明），活动任务数不超过 worker 上限
//   - 有界等待队列：容量只计等待任务，达到容量后立即拒绝，并发入队不突破容量
//   - 每个任务结果通道独立、不串用
//   - 单个任务抛异常被转换为内部错误，worker 继续处理后续任务
//   - shutdown 停止接收新任务、排空（drain）已接收任务并回收 worker，无永久等待
//
// 运行方式：ctest --test-dir build -R judge_manager_unit --output-on-failure
// 或直接执行 build/oj_judge_manager_unit。

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "judge/manager.h"
#include "submit/submit.h"

namespace {

using oj::judge::compute_worker_count;
using oj::judge::detect_cpu_count;
using oj::judge::JudgeManager;
using oj::judge::SubmissionTask;
using oj::submit::SubmitService;

SubmitService::Outcome ok_outcome() {
  SubmitService::Outcome outcome;
  outcome.kind = SubmitService::Kind::Ok;
  return outcome;
}

SubmissionTask make_task(std::int64_t id) {
  SubmissionTask task;
  task.user_id = id;
  task.problem_id = id;
  task.language = "cpp17";
  task.source_code = "source-" + std::to_string(id);
  task.submitted_at = "2026-01-01 00:00:00";
  return task;
}

// 同步门：handler 进入后登记并等待放行，用于确定性地制造“正在执行/排队”状态。
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  int entered = 0;
  bool open = false;

  void enter() {
    std::unique_lock<std::mutex> lock(mutex);
    ++entered;
    cv.notify_all();
    cv.wait(lock, [this]() { return open; });
  }

  bool wait_entered(int count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, timeout,
                       [&]() { return entered >= count; });
  }

  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    open = true;
    cv.notify_all();
  }
};

constexpr auto kWait = std::chrono::seconds(5);

// ---------------------------------------------------------------------------
// worker 数量规则
// ---------------------------------------------------------------------------

TEST(JudgeManagerWorkerCount, ComputesMinCpuAndEight) {
  EXPECT_EQ(compute_worker_count(100), 8);
  EXPECT_EQ(compute_worker_count(8), 8);
  EXPECT_EQ(compute_worker_count(3), 3);
  EXPECT_EQ(compute_worker_count(1), 1);
}

TEST(JudgeManagerWorkerCount, ZeroOrUnknownCpuStillHasWorker) {
  EXPECT_EQ(compute_worker_count(0), 1);   // 无法获取 -> 至少 1
  EXPECT_EQ(compute_worker_count(-1) >= 1, true); // 自动探测 -> 至少 1
  const int cpus = detect_cpu_count();
  const int expected =
      cpus <= 0 ? 1 : (cpus > 8 ? 8 : cpus);
  EXPECT_EQ(compute_worker_count(-1), expected);
}

TEST(JudgeManagerWorkerCount, ManagerHonorsComputedCount) {
  {
    JudgeManager manager([](const SubmissionTask &) { return ok_outcome(); },
                         JudgeManager::Options(4, 0));
    EXPECT_EQ(manager.worker_count(), 1);
  }
  {
    JudgeManager manager([](const SubmissionTask &) { return ok_outcome(); },
                         JudgeManager::Options(4, 100));
    EXPECT_EQ(manager.worker_count(), 8);
  }
}

// ---------------------------------------------------------------------------
// 并发执行与活动任务上限
// ---------------------------------------------------------------------------

TEST(JudgeManagerConcurrency, RunsUpToWorkerCountSimultaneously) {
  constexpr int kWorkers = 3;
  Gate gate;
  JudgeManager manager(
      [&](const SubmissionTask &) {
        gate.enter();
        return ok_outcome();
      },
      JudgeManager::Options(16, kWorkers));

  std::vector<std::future<SubmitService::Outcome>> futures;
  for (int i = 0; i < kWorkers; ++i) {
    auto result = manager.submit(make_task(i));
    ASSERT_EQ(result.status, JudgeManager::EnqueueStatus::Accepted);
    futures.push_back(std::move(result.future));
  }

  // 屏障：只有 kWorkers 个 handler 同时进入才能满足，证明真并发而非串行。
  EXPECT_TRUE(gate.wait_entered(kWorkers, kWait));
  EXPECT_EQ(manager.active_count(), static_cast<std::size_t>(kWorkers));
  EXPECT_EQ(manager.queued_count(), 0u);

  gate.release();
  for (auto &future : futures) {
    EXPECT_EQ(future.get().kind, SubmitService::Kind::Ok);
  }
  SUCCEED();
}

TEST(JudgeManagerConcurrency, ActiveNeverExceedsWorkersAndQueueHoldsRest) {
  constexpr int kWorkers = 2;
  constexpr int kTasks = 5;
  Gate gate;
  JudgeManager manager(
      [&](const SubmissionTask &) {
        gate.enter();
        return ok_outcome();
      },
      JudgeManager::Options(16, kWorkers));

  std::vector<std::future<SubmitService::Outcome>> futures;
  for (int i = 0; i < kTasks; ++i) {
    auto result = manager.submit(make_task(i));
    ASSERT_EQ(result.status, JudgeManager::EnqueueStatus::Accepted);
    futures.push_back(std::move(result.future));
  }

  EXPECT_TRUE(gate.wait_entered(kWorkers, kWait));
  EXPECT_EQ(manager.active_count(), static_cast<std::size_t>(kWorkers));
  EXPECT_EQ(manager.queued_count(), static_cast<std::size_t>(kTasks - kWorkers));

  gate.release();
  for (auto &future : futures) {
    EXPECT_EQ(future.get().kind, SubmitService::Kind::Ok);
  }
}

// ---------------------------------------------------------------------------
// 有界等待队列与满载拒绝
// ---------------------------------------------------------------------------

TEST(JudgeManagerQueue, RejectsWhenWaitingQueueFull) {
  Gate gate;
  JudgeManager manager(
      [&](const SubmissionTask &) {
        gate.enter();
        return ok_outcome();
      },
      JudgeManager::Options(/*capacity=*/1, /*workers=*/1));

  auto first = manager.submit(make_task(1));
  ASSERT_EQ(first.status, JudgeManager::EnqueueStatus::Accepted);
  ASSERT_TRUE(gate.wait_entered(1, kWait));

  auto second = manager.submit(make_task(2)); // 占用唯一的等待槽位
  EXPECT_EQ(second.status, JudgeManager::EnqueueStatus::Accepted);
  EXPECT_EQ(manager.queued_count(), 1u);

  auto third = manager.submit(make_task(3)); // 队列已满
  EXPECT_EQ(third.status, JudgeManager::EnqueueStatus::QueueFull);
  EXPECT_FALSE(third.future.valid());

  gate.release();
  EXPECT_EQ(first.future.get().kind, SubmitService::Kind::Ok);
  EXPECT_EQ(second.future.get().kind, SubmitService::Kind::Ok);
}

TEST(JudgeManagerQueue, ConcurrentEnqueueCannotExceedCapacity) {
  Gate gate;
  JudgeManager manager(
      [&](const SubmissionTask &) {
        gate.enter();
        return ok_outcome();
      },
      JudgeManager::Options(/*capacity=*/1, /*workers=*/1));

  // 先占满 worker。
  auto active = manager.submit(make_task(0));
  ASSERT_EQ(active.status, JudgeManager::EnqueueStatus::Accepted);
  ASSERT_TRUE(gate.wait_entered(1, kWait));

  constexpr int kThreads = 16;
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      auto result = manager.submit(make_task(100 + i));
      if (result.status == JudgeManager::EnqueueStatus::Accepted) {
        accepted.fetch_add(1);
      } else if (result.status == JudgeManager::EnqueueStatus::QueueFull) {
        rejected.fetch_add(1);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  // 等待槽位只有 1 个：并发入队也只能有一个成功，其余全部被拒绝。
  EXPECT_EQ(accepted.load(), 1);
  EXPECT_EQ(rejected.load(), kThreads - 1);
  EXPECT_EQ(manager.queued_count(), 1u);

  gate.release();
  EXPECT_EQ(active.future.get().kind, SubmitService::Kind::Ok);
}

// ---------------------------------------------------------------------------
// 结果通道独立
// ---------------------------------------------------------------------------

TEST(JudgeManagerResults, EachTaskGetsItsOwnResult) {
  JudgeManager manager(
      [](const SubmissionTask &task) {
        SubmitService::Outcome outcome = ok_outcome();
        outcome.submission.user_id = task.user_id;
        outcome.submission.source_code = task.source_code;
        return outcome;
      },
      JudgeManager::Options(64, 4));

  constexpr int kTasks = 24;
  std::vector<std::future<SubmitService::Outcome>> futures;
  for (int i = 0; i < kTasks; ++i) {
    auto result = manager.submit(make_task(i));
    ASSERT_EQ(result.status, JudgeManager::EnqueueStatus::Accepted);
    futures.push_back(std::move(result.future));
  }
  for (int i = 0; i < kTasks; ++i) {
    SubmitService::Outcome outcome = futures[i].get();
    EXPECT_EQ(outcome.submission.user_id, i);
    EXPECT_EQ(outcome.submission.source_code, "source-" + std::to_string(i));
  }
}

// ---------------------------------------------------------------------------
// 异常隔离：任务抛异常不拖垮 worker
// ---------------------------------------------------------------------------

TEST(JudgeManagerErrors, TaskExceptionIsDeliveredAndWorkerSurvives) {
  JudgeManager manager(
      [](const SubmissionTask &task) {
        if (task.problem_id == 1) {
          throw std::runtime_error("boom");
        }
        return ok_outcome();
      },
      JudgeManager::Options(8, 1));

  auto bad = manager.submit(make_task(1));
  ASSERT_EQ(bad.status, JudgeManager::EnqueueStatus::Accepted);
  SubmitService::Outcome bad_outcome = bad.future.get();
  EXPECT_EQ(bad_outcome.kind, SubmitService::Kind::InternalError);
  EXPECT_FALSE(bad_outcome.error.empty());

  // worker 未被异常终止，仍能处理后续任务。
  auto good = manager.submit(make_task(2));
  ASSERT_EQ(good.status, JudgeManager::EnqueueStatus::Accepted);
  EXPECT_EQ(good.future.get().kind, SubmitService::Kind::Ok);
}

// ---------------------------------------------------------------------------
// 停止：排空已接收任务并回收 worker
// ---------------------------------------------------------------------------

TEST(JudgeManagerShutdown, DrainsQueuedTasksAndRejectsNew) {
  Gate gate;
  JudgeManager manager(
      [&](const SubmissionTask &) {
        gate.enter();
        return ok_outcome();
      },
      JudgeManager::Options(64, 1));

  constexpr int kTasks = 6;
  std::vector<std::future<SubmitService::Outcome>> futures;
  for (int i = 0; i < kTasks; ++i) {
    auto result = manager.submit(make_task(i));
    ASSERT_EQ(result.status, JudgeManager::EnqueueStatus::Accepted);
    futures.push_back(std::move(result.future));
  }
  ASSERT_TRUE(gate.wait_entered(1, kWait));
  EXPECT_EQ(manager.active_count(), 1u);
  EXPECT_EQ(manager.queued_count(), static_cast<std::size_t>(kTasks - 1));

  // 在另一个线程触发停止：应等待已接收任务全部执行完再返回。
  std::atomic<bool> shutdown_done{false};
  std::thread shutdown_thread([&]() {
    manager.shutdown();
    shutdown_done.store(true);
  });

  // 停止已开始但任务仍被门挡住，shutdown 不应提前返回。
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(shutdown_done.load());

  gate.release();
  shutdown_thread.join();

  EXPECT_TRUE(shutdown_done.load());
  for (auto &future : futures) {
    EXPECT_EQ(future.get().kind, SubmitService::Kind::Ok);
  }

  // 停止后不再接收新任务。
  auto rejected = manager.submit(make_task(999));
  EXPECT_EQ(rejected.status, JudgeManager::EnqueueStatus::Stopped);
  EXPECT_FALSE(rejected.future.valid());
}

TEST(JudgeManagerShutdown, IsIdempotent) {
  JudgeManager manager([](const SubmissionTask &) { return ok_outcome(); },
                       JudgeManager::Options(4, 2));
  auto result = manager.submit(make_task(1));
  ASSERT_EQ(result.status, JudgeManager::EnqueueStatus::Accepted);
  EXPECT_EQ(result.future.get().kind, SubmitService::Kind::Ok);

  manager.shutdown();
  manager.shutdown(); // 重复调用安全
  SUCCEED();
}

TEST(JudgeManagerShutdown, ConcurrentShutdownIsSafeAndDrains) {
  Gate gate;
  JudgeManager manager(
      [&](const SubmissionTask &) {
        gate.enter();
        return ok_outcome();
      },
      JudgeManager::Options(64, 2));

  constexpr int kTasks = 6;
  std::vector<std::future<SubmitService::Outcome>> futures;
  for (int i = 0; i < kTasks; ++i) {
    auto result = manager.submit(make_task(i));
    ASSERT_EQ(result.status, JudgeManager::EnqueueStatus::Accepted);
    futures.push_back(std::move(result.future));
  }
  ASSERT_TRUE(gate.wait_entered(2, kWait));

  // 多个线程同时触发停止：串行化 + 幂等，均能安全返回且不遗漏任务结果。
  std::atomic<int> shutdown_returns{0};
  std::vector<std::thread> shutdown_threads;
  for (int i = 0; i < 3; ++i) {
    shutdown_threads.emplace_back([&]() {
      manager.shutdown();
      shutdown_returns.fetch_add(1);
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  gate.release();
  for (auto &thread : shutdown_threads) {
    thread.join();
  }
  EXPECT_EQ(shutdown_returns.load(), 3);
  for (auto &future : futures) {
    EXPECT_EQ(future.get().kind, SubmitService::Kind::Ok);
  }
  EXPECT_EQ(manager.submit(make_task(1000)).status,
            JudgeManager::EnqueueStatus::Stopped);
}

TEST(JudgeManagerOptions, DefaultAndZeroCapacityAreBounded) {
  {
    JudgeManager manager([](const SubmissionTask &) { return ok_outcome(); });
    EXPECT_EQ(manager.queue_capacity(), oj::judge::kDefaultQueueCapacity);
    EXPECT_GE(manager.worker_count(), 1);
    EXPECT_LE(manager.worker_count(), 8);
  }
  {
    // 容量 0 被视为未配置，收敛为至少 1，避免无法接收任何任务。
    JudgeManager manager([](const SubmissionTask &) { return ok_outcome(); },
                         JudgeManager::Options(0, 1));
    EXPECT_EQ(manager.queue_capacity(), 1u);
  }
}

// ---------------------------------------------------------------------------
// cancel_all：非阻塞地取消正在执行与等待中的任务，并停止接收新任务
// ---------------------------------------------------------------------------

TEST(JudgeManagerCancel, AllCancelsActiveAndQueuedWithoutBlocking) {
  Gate gate;
  std::mutex record_mutex;
  int active_saw_cancel = -1;
  int queued_saw_cancel = -1;

  JudgeManager manager(
      [&](const SubmissionTask &task) {
        gate.enter(); // 正在执行的任务在此被挡住，直到放行
        const bool saw = task.cancel && task.cancel->cancelled();
        std::lock_guard<std::mutex> lock(record_mutex);
        if (task.problem_id == 1) {
          active_saw_cancel = saw ? 1 : 0;
        } else if (task.problem_id == 2) {
          queued_saw_cancel = saw ? 1 : 0;
        }
        return ok_outcome();
      },
      JudgeManager::Options(/*capacity=*/8, /*workers=*/1));

  auto active = manager.submit(make_task(1)); // 占住唯一 worker
  ASSERT_EQ(active.status, JudgeManager::EnqueueStatus::Accepted);
  ASSERT_TRUE(gate.wait_entered(1, kWait));
  auto queued = manager.submit(make_task(2)); // 进入等待队列
  ASSERT_EQ(queued.status, JudgeManager::EnqueueStatus::Accepted);
  EXPECT_EQ(manager.queued_count(), 1u);

  // cancel_all 必须在 active 任务仍被挡住时立即返回（不阻塞、不回收 worker）。
  const auto start = std::chrono::steady_clock::now();
  manager.cancel_all();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  EXPECT_LT(elapsed, 500) << "cancel_all 在任务仍执行时不应阻塞";
  EXPECT_EQ(manager.active_count(), 1u);
  EXPECT_EQ(manager.queued_count(), 1u);
  // 停止接收新任务。
  EXPECT_EQ(manager.submit(make_task(3)).status,
            JudgeManager::EnqueueStatus::Stopped);

  // 放行后：正在执行与等待中的任务都观察到取消令牌，且结果均被投递（不永久挂起）。
  gate.release();
  EXPECT_EQ(active.future.get().kind, SubmitService::Kind::Ok);
  EXPECT_EQ(queued.future.get().kind, SubmitService::Kind::Ok);
  EXPECT_EQ(active_saw_cancel, 1);
  EXPECT_EQ(queued_saw_cancel, 1);

  manager.shutdown();
}

// ---------------------------------------------------------------------------
// M3.7 接收容量预留：reserve / commit / release
// ---------------------------------------------------------------------------

TEST(JudgeManagerReservation, ReserveConsumesCapacityUntilReleased) {
  Gate gate;
  JudgeManager manager(
      [&](const SubmissionTask &) {
        gate.enter();
        return ok_outcome();
      },
      JudgeManager::Options(/*capacity=*/1, /*workers=*/1));

  // 预留占用唯一等待槽位：再预留应满载。
  auto first = manager.reserve();
  ASSERT_TRUE(first.ok);
  EXPECT_EQ(first.status, JudgeManager::EnqueueStatus::Accepted);
  auto blocked = manager.reserve();
  EXPECT_FALSE(blocked.ok);
  EXPECT_EQ(blocked.status, JudgeManager::EnqueueStatus::QueueFull);

  // 释放后可再次预留，且 commit 正常交付结果。
  manager.release(first);
  auto again = manager.reserve();
  ASSERT_TRUE(again.ok);
  auto committed = manager.commit(make_task(7), again);
  ASSERT_EQ(committed.status, JudgeManager::EnqueueStatus::Accepted);
  ASSERT_TRUE(gate.wait_entered(1, kWait));
  gate.release();
  EXPECT_EQ(committed.future.get().kind, SubmitService::Kind::Ok);

  manager.shutdown();
}

TEST(JudgeManagerReservation, CommitWithInvalidReservationReturnsFailure) {
  Gate gate;
  JudgeManager manager(
      [&](const SubmissionTask &) {
        gate.enter();
        return ok_outcome();
      },
      JudgeManager::Options(/*capacity=*/1, /*workers=*/1));

  auto held = manager.reserve();
  ASSERT_TRUE(held.ok);
  auto full = manager.reserve();
  ASSERT_FALSE(full.ok);
  EXPECT_EQ(full.status, JudgeManager::EnqueueStatus::QueueFull);

  // 用无效预留提交：返回对应失败状态、无 future，且不占用容量。
  auto bad = manager.commit(make_task(5), full);
  EXPECT_EQ(bad.status, JudgeManager::EnqueueStatus::QueueFull);
  EXPECT_FALSE(bad.future.valid());

  // release 无效预留为空操作；重复释放已释放的预留也不会下溢容量。
  manager.release(JudgeManager::Reservation{});
  manager.release(held);
  manager.release(held);

  // 容量已恢复：可再预留一次，第二次因容量 1 满载。
  auto a = manager.reserve();
  EXPECT_TRUE(a.ok);
  auto b = manager.reserve();
  EXPECT_FALSE(b.ok);
  EXPECT_EQ(b.status, JudgeManager::EnqueueStatus::QueueFull);
  manager.release(a);
  manager.shutdown();
}

TEST(JudgeManagerReservation, ReleaseDoNotUnderflowAndRestoreCapacity) {
  JudgeManager manager([](const SubmissionTask &) { return ok_outcome(); },
                       JudgeManager::Options(/*capacity=*/2, /*workers=*/1));
  auto r = manager.reserve();
  ASSERT_TRUE(r.ok);
  manager.release(r);
  manager.release(r); // 重复释放应被保护，不下溢

  auto a = manager.reserve();
  auto b = manager.reserve();
  EXPECT_TRUE(a.ok);
  EXPECT_TRUE(b.ok);
  auto c = manager.reserve();
  EXPECT_FALSE(c.ok);
  EXPECT_EQ(c.status, JudgeManager::EnqueueStatus::QueueFull);
  manager.release(a);
  manager.release(b);
  manager.shutdown();
}

TEST(JudgeManagerReservation, ConcurrentReserveCannotExceedCapacity) {
  JudgeManager manager([](const SubmissionTask &) { return ok_outcome(); },
                       JudgeManager::Options(/*capacity=*/1, /*workers=*/1));
  auto held = manager.reserve();
  ASSERT_TRUE(held.ok);

  constexpr int kThreads = 16;
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&]() {
      auto r = manager.reserve();
      if (r.ok) {
        accepted.fetch_add(1);
        manager.release(r);
      } else if (r.status == JudgeManager::EnqueueStatus::QueueFull) {
        rejected.fetch_add(1);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  // 槽位已被 held 占用，所有并发预留都应被拒绝。
  EXPECT_EQ(accepted.load(), 0);
  EXPECT_EQ(rejected.load(), kThreads);
  manager.release(held);
  manager.shutdown();
}

TEST(JudgeManagerReservation, CommitAfterCancelStillDeliversCanceledTask) {
  Gate gate;
  std::atomic<bool> saw_cancel{false};
  JudgeManager manager(
      [&](const SubmissionTask &task) {
        saw_cancel.store(task.cancel && task.cancel->cancelled());
        gate.enter();
        return ok_outcome();
      },
      JudgeManager::Options(/*capacity=*/4, /*workers=*/1));

  auto reservation = manager.reserve();
  ASSERT_TRUE(reservation.ok);

  // 预留期间服务开始停止：commit 仍必须入队并交付结果（带取消令牌），不永久挂起。
  manager.cancel_all();
  auto committed = manager.commit(make_task(42), reservation);
  ASSERT_EQ(committed.status, JudgeManager::EnqueueStatus::Accepted);
  ASSERT_TRUE(gate.wait_entered(1, kWait));
  gate.release();
  EXPECT_EQ(committed.future.get().kind, SubmitService::Kind::Ok);
  EXPECT_TRUE(saw_cancel.load());

  // 停止后新预留立即被拒。
  auto after = manager.reserve();
  EXPECT_FALSE(after.ok);
  EXPECT_EQ(after.status, JudgeManager::EnqueueStatus::Stopped);
  manager.shutdown();
}

} // namespace
