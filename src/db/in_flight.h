#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oj {

class Database;

// 一条在途任务记录（M3.7）：提交被接收后、最终结算前保存在独立表
// in_flight_tasks 中的可恢复任务。包含用户、题目、语言、完整源码、原提交时间
// 与持久化任务标识，使崩溃不丢源码与任务归属。
//
// 在途记录在结算前不计入 submit_count、AC 状态、通过人数与排行榜统计。
struct InFlightTask {
  std::int64_t id = 0;
  // 持久化任务标识：跨重启仍能定位同一任务；唯一性由 task_id UNIQUE 约束保证。
  std::string task_id;
  std::int64_t user_id = 0;
  std::int64_t problem_id = 0;
  std::string language;
  std::string source_code;
  // 原始提交时间（UTC "YYYY-MM-DD HH:MM:SS"），恢复时不得被重启时间覆盖。
  std::string submitted_at;
  // pending | claimed | interrupted
  std::string state;
  // interrupted 的明确原因（用于排查，不记录源码或敏感信息）。
  std::string reason;
};

// 生成持久化任务标识：时间前缀 + 随机十六进制。碰撞概率极低，最终由
// in_flight_tasks.task_id 的 UNIQUE 约束兜底。
std::string generate_task_id();

// in_flight_tasks 表的写入与读取。所有语句使用参数绑定，不拼接外部输入。
//
// insert / remove 不自行开启事务：调用方（提交服务结算、接收持久化）必须在
// 同一个短事务内完成在途记录的写入或删除，并与提交记录/做题状态更新保持原子。
class InFlightStore {
public:
  explicit InFlightStore(Database &db) : db_(db) {}

  // 插入一条 pending 在途记录，成功时 out_id 为新记录主键，并回写 task.task_id
  // （为空时自动生成；与既有记录冲突时换新标识重试）。失败返回 false 并置 error。
  bool insert(InFlightTask &task, std::int64_t &out_id, std::string &error);

  // 按持久化任务标识查询。返回 true 表示查询过程正常，found 指示是否存在。
  bool find_by_task_id(const std::string &task_id, bool &found, InFlightTask &out,
                       std::string &error);

  // 删除一条在途记录（最终结算）。removed 指示是否确实删除，用于配合唯一性约束
  // 保证同一任务只最终落库一次。须在结算事务内调用。
  bool remove_by_task_id(const std::string &task_id, bool &removed,
                         std::string &error);

  // 标记为中断（保留任务信息，不参与统计，不再被恢复）。仅对非 interrupted 生效。
  bool mark_interrupted(const std::string &task_id, const std::string &reason,
                        std::string &error);

  // 按 id 升序读取至多 limit 条 pending 记录（分批读取，避免一次将全部源码载入
  // 内存）。恢复调度据此逐步投递。
  bool list_pending(int limit, std::vector<InFlightTask> &out, std::string &error);

  // 原子认领：仅在仍为 pending 时置为 claimed 并记录 owner。claimed 指示是否
  // 认领成功（并发/重复扫描时只有一个调用者成功）。
  bool claim(std::int64_t id, const std::string &owner, bool &claimed,
             std::string &error);

  // 启动时将上次崩溃遗留的 claimed 记录恢复为 pending（解除失效占用）。
  bool reset_stale_claims(std::string &error);

  // 统计引用某题的「未结算」在途记录数（pending/claimed，不含 interrupted）。
  // 删题保护据此避免外键失败或已接收任务无法保存终态；标记为中断的残留记录
  // 随删题一并清理，不作为删除障碍。
  bool count_for_problem(std::int64_t problem_id, std::int64_t &out,
                         std::string &error);

private:
  Database &db_;
};

} // namespace oj
