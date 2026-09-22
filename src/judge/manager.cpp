#include "judge/manager.h"

#include <exception>
#include <unistd.h>
#include <utility>

namespace oj {
namespace judge {

namespace {

submit::SubmitService::Outcome make_internal_error(const std::string &message) {
  submit::SubmitService::Outcome outcome;
  outcome.kind = submit::SubmitService::Kind::InternalError;
  outcome.error = message;
  return outcome;
}

} // namespace

int detect_cpu_count() {
  long online = ::sysconf(_SC_NPROCESSORS_ONLN);
  if (online > 0) {
    return static_cast<int>(online);
  }
  unsigned int hardware = std::thread::hardware_concurrency();
  if (hardware > 0) {
    return static_cast<int>(hardware);
  }
  return 0; // 无法获取
}

int compute_worker_count(int cpu_count_override) {
  long cpus = cpu_count_override;
  if (cpu_count_override < 0) {
    cpus = detect_cpu_count();
  }
  if (cpus <= 0) {
    cpus = 1; // 无法获取或返回 0：至少 1 个 worker
  }
  if (cpus > 8) {
    cpus = 8; // SPEC JUDGE-09：min(CPU 核数, 8)
  }
  return static_cast<int>(cpus);
}

JudgeManager::JudgeManager(Handler handler, Options options)
    : handler_(std::move(handler)),
      // 容量至少为 1：0 或未配置时退回默认语义，避免出现无法接收任何任务。
      queue_capacity_(options.queue_capacity == 0 ? 1 : options.queue_capacity),
      worker_count_(compute_worker_count(options.worker_count)) {
  workers_.reserve(static_cast<std::size_t>(worker_count_));
  for (int i = 0; i < worker_count_; ++i) {
    workers_.emplace_back([this]() { worker_loop(); });
  }
}

JudgeManager::~JudgeManager() { shutdown(); }

JudgeManager::SubmitResult JudgeManager::submit(SubmissionTask task) {
  SubmitResult result;
  auto item = std::make_shared<Item>(std::move(task));

  // 入队判断与入队操作在同一把锁内完成：并发提交也无法突破容量上限。
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopped_) {
    result.status = EnqueueStatus::Stopped;
    return result;
  }
  if (queue_.size() >= queue_capacity_) {
    result.status = EnqueueStatus::QueueFull;
    return result;
  }

  // 必须在任务入队（可能被 worker 立即取走并完成）之前取得 future，避免错过结果。
  result.future = item->promise.get_future();
  queue_.push_back(std::move(item));
  not_empty_.notify_one();
  result.status = EnqueueStatus::Accepted;
  return result;
}

std::size_t JudgeManager::queued_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

void JudgeManager::cancel_all() {
  std::lock_guard<std::mutex> lock(mutex_);
  stopped_ = true;
  // 正在执行与等待执行的任务都置位取消令牌：正在运行的任务据此终止子进程，等待中
  // 的任务在被取出后立即短路（不启动新进程）。所有已接收任务仍会得到一个明确结果。
  for (const std::shared_ptr<Item> &item : queue_) {
    if (item->task.cancel) {
      item->task.cancel->cancel();
    }
  }
  for (Item *item : active_items_) {
    if (item->task.cancel) {
      item->task.cancel->cancel();
    }
  }
  not_empty_.notify_all();
}

void JudgeManager::shutdown() {
  // 串行化并发/重复调用；已回收则直接返回。
  std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
  if (workers_joined_) {
    return;
  }

  // 先通知取消（不在 shutdown_mutex_ 之外重复加锁），再回收 worker。等待队列中的
  // 任务会被 worker 依次取出并交付取消结果，已接收任务的结果通道不会永久挂起。
  cancel_all();

  for (std::thread &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_joined_ = true;
}

void JudgeManager::worker_loop() {
  for (;;) {
    std::shared_ptr<Item> item;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      not_empty_.wait(lock, [this]() { return !queue_.empty() || stopped_; });
      if (queue_.empty()) {
        if (stopped_) {
          return; // 已停止且队列已排空
        }
        continue;
      }
      item = std::move(queue_.front());
      queue_.pop_front();
      active_items_.insert(item.get());
    }

    active_.fetch_add(1);
    submit::SubmitService::Outcome outcome;
    try {
      outcome = handler_(item->task);
    } catch (const std::exception &e) {
      // 单个任务抛异常绝不能导致 worker 退出或服务终止：转换为内部错误结果交付。
      outcome = make_internal_error(std::string("判题任务异常: ") + e.what());
    } catch (...) {
      outcome = make_internal_error("判题任务异常");
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_items_.erase(item.get());
    }
    try {
      item->promise.set_value(std::move(outcome));
    } catch (...) {
      // 结果通道只会被设置一次；此处仅作防御，保证 worker 不受影响。
    }
    active_.fetch_sub(1);
  }
}

} // namespace judge
} // namespace oj
