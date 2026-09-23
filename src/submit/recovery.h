#pragma once

#include <atomic>
#include <cstddef>
#include <string>

#include "judge/manager.h"

namespace oj {

class Database;

namespace submit {

// 启动恢复（M3.7）：数据库迁移与判题环境检查完成后，扫描未结算的在途任务，
// 按有界队列容量分批、逐步重新入队判题。
//
// 恢复策略：
//   - 使用已保存的源码、语言与**当前**题目配置/测试用例重新判题（与 Rejudge
//     同口径，不持久化判题快照）；题目不存在等无法判题时标记为中断并保留任务信息。
//   - 恢复正常任务与普通提交共用同一 JudgeManager 有界队列、worker、编译门限与
//     沙箱，不新开无限制执行路径。
//   - 分批读取（至多 batch_size 条源码驻留内存），队列暂满时等待重试，绝不丢弃。
//   - 同一任务通过 pending -> claimed 原子认领，避免重复扫描重复执行；最终是否
//     落库仍由结算事务的唯一性保证。
class RecoveryService {
public:
  struct Options {
    // 每批从数据库读取的在途任务数（限制源码驻留内存）。
    int batch_size = 16;
    // 队列暂满时的重试间隔。
    int retry_sleep_ms = 50;
    // 启动时是否将上次崩溃遗留的 claimed 记录重置为 pending（解除失效占用）。
    bool reset_stale_claims = true;
  };

  // 调用方显式传入 Options（不使用依赖 NSDMI 的默认实参，避免在类未完成时
  // 默认成员初始化器不可用的限制）。
  RecoveryService(Database &db, judge::JudgeManager &manager,
                  std::string instance_id, Options options);

  // 执行一次启动恢复，返回重新入队的任务数。应在服务开始接收新提交前调用。
  std::size_t run();

  // 请求中止恢复投递（供停止流程使用）。run() 会在任务边界尽快返回。
  void abort();

private:
  Database &db_;
  judge::JudgeManager &manager_;
  std::string instance_id_;
  Options options_;
  std::atomic<bool> aborted_{false};
};

} // namespace submit
} // namespace oj
