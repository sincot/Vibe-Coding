#include "judge/manager.h"

#include <exception>
#include <unistd.h>
#include <utility>

#include "log.h"

namespace oj {
namespace judge {

namespace {

submit::SubmitService::Outcome make_internal_error(const std::string &message) {
  submit::SubmitService::Outcome outcome;
  outcome.kind = submit::SubmitService::Kind::InternalError;
  outcome.error = message;
  return outcome;
}

const char *outcome_kind_name(submit::SubmitService::Kind kind) {
  switch (kind) {
  case submit::SubmitService::Kind::Ok:
    return "Ok";
  case submit::SubmitService::Kind::ProblemNotFound:
    return "ProblemNotFound";
  case submit::SubmitService::Kind::AlreadySettled:
    return "AlreadySettled";
  case submit::SubmitService::Kind::InternalError:
    return "InternalError";
  }
  return "Unknown";
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
  // 分配（或沿用调用方显式提供的）任务标识：所有后续日志都带该标识，便于把同一
  // 任务的接收、执行、完成、取消与清理问题串起来定位。
  if (task.task_id == 0) {
    task.task_id = next_task_id_.fetch_add(1);
  }
  const std::int64_t task_id = task.task_id;
  const std::int64_t task_user_id = task.user_id;
  const std::int64_t task_problem_id = task.problem_id;
  auto item = std::make_shared<Item>(std::move(task));

  // 入队判断与入队操作在同一把锁内完成：并发提交也无法突破容量上限。
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      result.status = EnqueueStatus::Stopped;
      return result;
    }
    if (queue_.size() + reserved_ >= queue_capacity_) {
      result.status = EnqueueStatus::QueueFull;
      return result;
    }

    // 必须在任务入队（可能被 worker 立即取走并完成）之前取得 future，避免错过结果。
    result.future = item->promise.get_future();
    queue_.push_back(std::move(item));
    not_empty_.notify_one();
    result.status = EnqueueStatus::Accepted;
  }

  accepted_.fetch_add(1);
  log(LogLevel::Info, "判题任务 #" + std::to_string(task_id) + " 已接收（用户 " +
                          std::to_string(task_user_id) + "，题目 " +
                          std::to_string(task_problem_id) + "）");
  return result;
}

JudgeManager::Reservation JudgeManager::reserve() {
  Reservation reservation;
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopped_) {
    reservation.status = EnqueueStatus::Stopped;
    return reservation;
  }
  if (queue_.size() + reserved_ >= queue_capacity_) {
    reservation.status = EnqueueStatus::QueueFull;
    return reservation;
  }
  ++reserved_;
  reservation.ok = true;
  reservation.status = EnqueueStatus::Accepted;
  return reservation;
}

JudgeManager::SubmitResult
JudgeManager::commit(SubmissionTask task, const Reservation &reservation) {
  SubmitResult result;
  if (!reservation.ok) {
    result.status = reservation.status;
    return result;
  }
  if (task.task_id == 0) {
    task.task_id = next_task_id_.fetch_add(1);
  }
  const std::int64_t task_id = task.task_id;
  const std::int64_t task_user_id = task.user_id;
  const std::int64_t task_problem_id = task.problem_id;
  auto item = std::make_shared<Item>(std::move(task));

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reserved_ > 0) {
      --reserved_;
    }
    // 防御：若 worker 已被回收（shutdown 之后才提交预留），不能入队后永久等待，
    // 直接以内部错误交付结果；在途记录由下次启动恢复处理。正常停止流程保证
    // shutdown 前所有 HTTP 请求（含 commit）已结束，此分支不应触发。
    if (workers_stop_) {
      item->task.cancel->cancel();
      result.future = item->promise.get_future();
      item->promise.set_value(
          make_internal_error("判题调度器已回收，任务未执行"));
      result.status = EnqueueStatus::Accepted;
      return result;
    }
    // 预留期间若调度器被停止，任务仍入队，但立即带取消令牌结算（SYSERR），
    // 不会留下永不完成的结果通道，也不会被误当作「未接收」。
    if (stopped_) {
      item->task.cancel->cancel();
    }
    result.future = item->promise.get_future();
    queue_.push_back(std::move(item));
    not_empty_.notify_one();
    result.status = EnqueueStatus::Accepted;
  }

  accepted_.fetch_add(1);
  log(LogLevel::Info, "判题任务 #" + std::to_string(task_id) + " 已接收（用户 " +
                          std::to_string(task_user_id) + "，题目 " +
                          std::to_string(task_problem_id) + "）");
  return result;
}

void JudgeManager::release(const Reservation &reservation) {
  if (!reservation.ok) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (reserved_ > 0) {
    --reserved_;
  }
}

std::size_t JudgeManager::queued_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

void JudgeManager::cancel_all() {
  std::vector<std::int64_t> cancelled_ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    // 正在执行与等待执行的任务都置位取消令牌：正在运行的任务据此终止子进程，等待中
    // 的任务在被取出后立即短路（不启动新进程）。所有已接收任务仍会得到一个明确结果。
    // 只在首次置位时记录日志，重复 cancel_all（含并发停止）不会重复刷屏。
    auto cancel_item = [&cancelled_ids](Item *item) {
      if (item->task.cancel && !item->task.cancel->cancelled()) {
        item->task.cancel->cancel();
        cancelled_ids.push_back(item->task.task_id);
      }
    };
    for (const std::shared_ptr<Item> &item : queue_) {
      cancel_item(item.get());
    }
    for (Item *item : active_items_) {
      cancel_item(item);
    }
    not_empty_.notify_all();
  }
  for (std::int64_t id : cancelled_ids) {
    log(LogLevel::Info,
        "判题任务 #" + std::to_string(id) + " 收到取消（停止/取消流程）");
  }
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

  // 置位「允许 worker 退出」并唤醒：worker 会继续排空队列后才退出。cancel_all 只
  // 停止接收，不导致 worker 提前退出，从而保证预留尚未 commit 的任务仍能被处理。
  {
    std::lock_guard<std::mutex> lock(mutex_);
    workers_stop_ = true;
  }
  not_empty_.notify_all();

  for (std::thread &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_joined_ = true;

  // 核对责任归属：正常情况下所有已接收任务都已执行完 handler（持久化在 handler 内
  // 完成），据此留下明确证据，而不是无条件宣称「全部已保存」。
  const std::int64_t accepted = accepted_.load();
  const std::int64_t completed = completed_.load();
  if (accepted == completed) {
    log(LogLevel::Info, "判题调度器已排空：已接收 " + std::to_string(accepted) +
                            " 个任务，全部执行并持久化完成");
  } else {
    log(LogLevel::Error, "判题调度器停止后仍有未完成任务：已接收 " +
                             std::to_string(accepted) + "，已完成 " +
                             std::to_string(completed));
  }
}

void JudgeManager::worker_loop() {
  for (;;) {
    std::shared_ptr<Item> item;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      not_empty_.wait(lock, [this]() { return !queue_.empty() || workers_stop_; });
      if (queue_.empty()) {
        if (workers_stop_) {
          return; // shutdown 已允许退出且队列已排空
        }
        continue;
      }
      item = std::move(queue_.front());
      queue_.pop_front();
      active_items_.insert(item.get());
    }

    active_.fetch_add(1);
    const std::int64_t task_id = item->task.task_id;
    log(LogLevel::Info, "判题任务 #" + std::to_string(task_id) + " 开始执行");
    submit::SubmitService::Outcome outcome;
    try {
      outcome = handler_(item->task);
    } catch (const std::exception &e) {
      // 单个任务抛异常绝不能导致 worker 退出或服务终止：转换为内部错误结果交付。
      outcome = make_internal_error(std::string("判题任务异常: ") + e.what());
    } catch (...) {
      outcome = make_internal_error("判题任务异常");
    }
    completed_.fetch_add(1);

    // handler 返回即代表该任务的「最终处理 + 持久化」已结束（责任归属明确）。在把
    // 结果移入 promise 之前记录任务标识与结果类型，不重复输出源码或逐点详情。
    const std::string verdict =
        (outcome.kind == submit::SubmitService::Kind::Ok)
            ? outcome.submission.status
            : std::string(outcome_kind_name(outcome.kind));
    log(LogLevel::Info, "判题任务 #" + std::to_string(task_id) +
                            " 执行完成：" + verdict);

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
