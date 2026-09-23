#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "judge/deadline.h"
#include "submit/submit.h"

namespace oj {
namespace judge {

// 等待队列容量的默认值（表示“已接收但尚未开始执行”的任务数，不含正在执行的任务）。
inline constexpr std::size_t kDefaultQueueCapacity = 32;

// 自动探测 CPU 核数；无法获取或返回 0 时按 1 处理（绝不出现没有 worker 的情况）。
int detect_cpu_count();

// 依据 CPU 核数计算 worker 数：min(CPU 核数, 8)，且至少 1。
// cpu_count_override < 0 时自动探测；否则使用给定值（0 视为无法获取）。
int compute_worker_count(int cpu_count_override);

// 一次提交对应的调度任务。任务自带全部数据，绝不引用 HTTP 请求对象、回调局部变量
// 或数据库语句对象，因此可在 worker 线程中安全、独立地执行。
struct SubmissionTask {
  // 判题任务标识：由 JudgeManager 在接收时分配（调用方留 0 即可），用于把
  // 「接收/开始/完成/取消/清理失败」的日志与同一任务关联起来，便于停止排障。
  std::int64_t task_id = 0;
  std::int64_t user_id = 0;
  std::int64_t problem_id = 0;
  std::string language;    // 规范语言名："cpp17" / "c11"
  std::string source_code; // 完整用户源码，不裁剪不修改
  bool viewer_is_admin = false; // 是否允许向隐藏题提交（由鉴权层判定）
  std::string submitted_at; // 原始提交时间（接受入队时采集），用于首次 AC 时间口径
  // 协作式取消令牌：由 JudgeManager 为每个任务创建并在服务停止时置位。判题核心与
  // 执行器据此停止启动新进程并终止正在运行的进程组。空指针表示不支持取消。
  std::shared_ptr<CancellationToken> cancel;
  // 重判标识：非 0 时表示这是对指定 submissions.id 的重判，handler 应走重判路径。
  // 普通提交保持 0。
  std::int64_t rejudge_submission_id = 0;
};

// 判题任务调度器（SPEC JUDGE-09 / M3.1 架构图中的 JudgeManager）。
//
// 职责：统一负责判题任务接收、入队、worker 调度与结果交付；把“任务调度”与
// “单次判题执行 + 持久化”分离。单次判题与持久化仍复用 SubmitService，不复制
// 编译/运行/比对/落库逻辑。
//
// 关键语义：
//   - 有界等待队列：容量只计算“等待执行”的任务数，正在执行的任务数由 worker
//     数量单独限制；入队判断与入队操作在同一把锁内原子完成，并发也无法突破容量。
//   - 固定 worker 数：min(CPU 核数, 8)，至少 1；每个 worker 一次处理一个完整提交，
//     同一提交的测试点不额外并行化。
//   - 每个被接收的任务拥有独立 std::promise/future 结果通道；结果或异常必交付给
//     对应请求，不串用、不重复完成、不永久等待。
//   - handler 抛出的异常被捕获并转换为内部错误结果，worker 继续处理后续任务。
//   - 队列满载时立即拒绝（EnqueueStatus::QueueFull），不阻塞等待、不无限积压。
//   - shutdown：停止接收新任务，唤醒全部等待线程，并将已接收任务全部执行完毕
//     （drain）后再回收 worker，绝不给已接收任务留下永不完成的结果通道。
class JudgeManager {
public:
  // 单次任务执行函数：判题 + 持久化，由 SubmitService 提供。
  using Handler = std::function<submit::SubmitService::Outcome(const SubmissionTask &)>;

  struct Options {
    // 等待队列容量（等待执行的任务数，非正在执行数）。
    std::size_t queue_capacity;
    // worker 数；<0 表示按 min(CPU 核数, 8) 自动计算（0 视为无法获取 CPU，取 1）。
    int worker_count;

    Options()
        : queue_capacity(kDefaultQueueCapacity), worker_count(-1) {}
    Options(std::size_t capacity, int workers)
        : queue_capacity(capacity), worker_count(workers) {}
  };

  enum class EnqueueStatus {
    Accepted,  // 已接收，future 有效
    QueueFull, // 等待队列已满，未接收、未执行
    Stopped,   // 调度器已停止接收
  };

  struct SubmitResult {
    EnqueueStatus status = EnqueueStatus::Stopped;
    // Accepted 时有效；其余状态为无效 future。调用方仅在 Accepted 时 get()。
    std::future<submit::SubmitService::Outcome> future;
  };

  JudgeManager(Handler handler, Options options = {});
  ~JudgeManager();

  JudgeManager(const JudgeManager &) = delete;
  JudgeManager &operator=(const JudgeManager &) = delete;

  // 尝试接收一个任务。入队判断与入队在同一锁内原子完成；队列满或已停止时立即
  // 返回对应状态，绝不阻塞等待。
  SubmitResult submit(SubmissionTask task);

  // 停止接收新任务，唤醒等待线程，执行完所有已接收任务后回收 worker。
  // 可重复调用；已接收任务的结果一定被投递，不会留下永久等待的 future。
  void shutdown();

  // 仅通知取消：停止接收新任务，并取消所有已接收任务（正在执行的置位取消令牌、
  // 等待队列中的任务会在被 worker 取出后立即看到取消）。不阻塞、不回收 worker，
  // 供服务停止流程在等待 HTTP 处理线程结束之前先行调用，避免同步等待判题的 HTTP
  // 线程与停止流程互相等待。可重复调用、幂等。
  void cancel_all();

  int worker_count() const { return worker_count_; }
  std::size_t queue_capacity() const { return queue_capacity_; }

  // 正在执行的任务数（<= worker_count）。
  std::size_t active_count() const { return active_.load(); }
  // 当前等待执行的任务数（<= queue_capacity）。
  std::size_t queued_count() const;

  // 已接收（Accepted）与已完成（handler 返回）的累计任务数。正常情况下两者在
  // shutdown() 返回后相等；若不等则说明存在未收尾的已接收任务，停止流程会据此
  // 留下明确证据，而不是宣称全部已保存。
  std::int64_t accepted_count() const { return accepted_.load(); }
  std::int64_t completed_count() const { return completed_.load(); }

private:
  struct Item {
    explicit Item(SubmissionTask t) : task(std::move(t)) {
      task.cancel = std::make_shared<CancellationToken>();
    }
    SubmissionTask task;
    std::promise<submit::SubmitService::Outcome> promise;
  };

  void worker_loop();

  Handler handler_;
  std::size_t queue_capacity_;
  int worker_count_;

  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::deque<std::shared_ptr<Item>> queue_;
  // 正在执行任务的原始指针集合（Item 由对应 worker 的 shared_ptr 保活，仅在锁内
  // 访问）。服务停止时据此取消正在执行的子进程。
  std::set<Item *> active_items_;
  bool stopped_ = false;

  std::atomic<std::size_t> active_{0};
  // 任务标识分配器与「已接收 / 已完成」计数，用于日志关联与停止收尾核对。
  std::atomic<std::int64_t> next_task_id_{1};
  std::atomic<std::int64_t> accepted_{0};
  std::atomic<std::int64_t> completed_{0};

  std::mutex shutdown_mutex_;
  bool workers_joined_ = false;
  std::vector<std::thread> workers_;
};

} // namespace judge
} // namespace oj
