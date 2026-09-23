#pragma once

#include <cstdint>
#include <string>

namespace oj {

class Database;

// 一条提交流水记录（含完整用户源码与逐点结果 JSON）。
//
// runtime_ms：各测试点程序执行耗时之和（不含排队与编译）。
// memory_kb：各测试点观测峰值 RSS 的最大值；0 表示「未采集」，对外响应以 null
// 表示，绝不用 0 伪装真实测量值。
struct SubmissionRecord {
  std::int64_t id = 0;
  std::int64_t user_id = 0;
  std::int64_t problem_id = 0;
  std::string language;    // 规范语言名："cpp17" / "c11"
  std::string source_code; // 用户提交的完整源码，不裁剪不修改
  std::string status;      // AC|WA|CE|TLE|RE|MLE|SYSERR
  std::string per_case;    // JSON 数组：逐测试点结果
  std::string compile_msg; // 编译诊断（有界）
  long long runtime_ms = 0;
  long long memory_kb = 0;
  std::string created_at; // UTC "YYYY-MM-DD HH:MM:SS"，与提交时间同口径
};

// submissions 表的写入与读取。所有语句使用参数绑定，不拼接外部输入。
//
// 写入方法不自行开启事务：调用方（提交服务）在同一个短事务内完成提交记录写入与
// 用户题目状态更新，任一失败整体回滚。
class SubmissionStore {
public:
  explicit SubmissionStore(Database &db) : db_(db) {}

  // 插入一条提交记录，成功时 out_id 为新记录主键。失败返回 false 并置 error。
  bool insert(const SubmissionRecord &record, std::int64_t &out_id,
              std::string &error);

  // 按 ID 查询提交记录。返回 true 表示查询过程正常，found 指示是否存在。
  bool find_by_id(std::int64_t id, bool &found, SubmissionRecord &out,
                  std::string &error);

  // 更新已有提交记录的结果字段（status/per_case/compile_msg/runtime_ms/memory_kb）。
  // 保留 id、user_id、problem_id、language、source_code、created_at 不变；
  // 不新增提交记录，也不影响提交次数。
  bool update(const SubmissionRecord &record, std::string &error);

private:
  Database &db_;
};

// user × problem 的做题状态（供题目列表状态标记与排行榜统计）。
struct UserProblemStatusRecord {
  bool accepted = false;
  bool has_first_ac_at = false;
  std::string first_ac_at;
  int submit_count = 0;
};

// user_problem_status 表的读取与 upsert。
class UserProblemStatusStore {
public:
  explicit UserProblemStatusStore(Database &db) : db_(db) {}

  // 查询某用户某题目的状态记录。返回 true 表示查询过程正常，found 指示是否存在。
  bool find(std::int64_t user_id, std::int64_t problem_id, bool &found,
            UserProblemStatusRecord &out, std::string &error);

  // 插入或更新状态记录（INSERT ... ON CONFLICT(user_id, problem_id) DO UPDATE）。
  // 值（accepted / first_ac_at / submit_count）由调用方在事务内基于当前状态计算，
  // 从而保证「首次 AC 时间不被覆盖」「重复 AC 不重复设置」「失败不清除 AC」。
  // 唯一性由 UNIQUE(user_id, problem_id) 约束兜底，不会重复创建状态记录。
  bool upsert(std::int64_t user_id, std::int64_t problem_id, bool accepted,
              bool has_first_ac_at, const std::string &first_ac_at,
              int submit_count, std::string &error);

  // 在事务内依据该用户该题的最新提交记录重算做题状态：
  //   - 仍有 AC 提交：status='accepted'，first_ac_at 为最早 AC 提交时间；
  //   - 无 AC 提交：status='none'，first_ac_at 清空；
  //   - submit_count 保持原值不变（若原无记录，则按实际提交次数初始化）。
  bool recompute(std::int64_t user_id, std::int64_t problem_id,
                 std::string &error);

private:
  Database &db_;
};

} // namespace oj
