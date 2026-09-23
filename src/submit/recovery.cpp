#include "submit/recovery.h"

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "db/database.h"
#include "db/in_flight.h"
#include "log.h"

namespace oj {
namespace submit {

RecoveryService::RecoveryService(Database &db, judge::JudgeManager &manager,
                                 std::string instance_id, Options options)
    : db_(db), manager_(manager), instance_id_(std::move(instance_id)),
      options_(options) {}

void RecoveryService::abort() { aborted_ = true; }

std::size_t RecoveryService::run() {
  InFlightStore store(db_);
  std::string err;

  // 启动时解除上次崩溃遗留的 claimed 占用：单机单实例由 InstanceLock 保证不会
  // 与仍在运行的实例冲突，进程崩溃后锁自动释放，故可安全重置。
  if (options_.reset_stale_claims) {
    if (!store.reset_stale_claims(err)) {
      log(LogLevel::Error, "启动恢复：重置失效认领失败: " + err);
      return 0;
    }
  }

  const int batch_size = options_.batch_size > 0 ? options_.batch_size : 1;
  std::size_t enqueued = 0;

  while (!aborted_.load()) {
    std::vector<InFlightTask> batch;
    if (!store.list_pending(batch_size, batch, err)) {
      log(LogLevel::Error, "启动恢复：读取在途任务失败: " + err);
      break;
    }
    if (batch.empty()) {
      break;
    }

    for (InFlightTask &task : batch) {
      if (aborted_.load()) {
        break;
      }

      // 等待容量：队列暂满时按间隔重试，绝不丢弃或判为不可恢复。已停止则退出。
      judge::JudgeManager::Reservation reservation;
      for (;;) {
        reservation = manager_.reserve();
        if (reservation.ok) {
          break;
        }
        if (reservation.status == judge::JudgeManager::EnqueueStatus::Stopped) {
          log(LogLevel::Warn,
              "启动恢复：判题调度器已停止，剩余任务留待下次启动恢复");
          return enqueued;
        }
        if (aborted_.load()) {
          break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(options_.retry_sleep_ms));
      }
      if (!reservation.ok) {
        break;
      }

      // 原子认领：并发/重复扫描时只有一个执行路径成功；未被认领（已被他处认领或
      // 状态已变化）则释放预留并跳过。
      bool claimed = false;
      if (!store.claim(task.id, instance_id_, claimed, err)) {
        // 临时数据库故障不无限重试：释放预留后停止本次恢复，剩余任务留待下次启动。
        log(LogLevel::Error,
            "启动恢复：认领任务 " + task.task_id + " 失败，停止本次恢复: " + err);
        manager_.release(reservation);
        return enqueued;
      }
      if (!claimed) {
        manager_.release(reservation);
        continue;
      }

      judge::SubmissionTask submit_task;
      submit_task.user_id = task.user_id;
      submit_task.problem_id = task.problem_id;
      submit_task.language = task.language;
      submit_task.source_code = task.source_code;
      submit_task.viewer_is_admin = true; // 接收时已完成可见性判定，不再受可见性限制
      submit_task.submitted_at = task.submitted_at; // 保留原始提交时间
      submit_task.in_flight_task_id = task.task_id;

      manager_.commit(std::move(submit_task), reservation);
      ++enqueued;
      log(LogLevel::Info, "启动恢复：任务 " + task.task_id +
                              " 已认领并重新入队（用户 " +
                              std::to_string(task.user_id) + "，题目 " +
                              std::to_string(task.problem_id) + "）");
    }
  }

  if (enqueued > 0) {
    log(LogLevel::Info, "启动恢复完成：重新入队 " + std::to_string(enqueued) +
                            " 个未结算任务");
  }
  return enqueued;
}

} // namespace submit
} // namespace oj
