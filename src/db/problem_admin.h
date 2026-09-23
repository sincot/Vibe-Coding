#pragma once

#include <cstdint>
#include <string>

#include "db/problems.h"
#include "problem/validation.h"

namespace oj {

class Database;

// 管理员题目写操作的数据访问封装。
//
// 所有语句使用参数绑定；涉及题目与公开样例的多条写入均在同一个短事务内完成，
// 任一失败整体回滚，不产生部分更新。事务以 BEGIN IMMEDIATE 取得写锁，并配合
// Database::transaction_mutex() 串行化，与提交服务使用同一把锁，从而保证
// 「删题检查 + 删题」与「提交保存」不会交错，避免检查通过后产生孤立记录。
//
// 本类只处理题目元数据与公开样例（is_sample=1）。隐藏测试用例的增删改属 M2.2，
// 不在本类范围内；因此更新/删除必须显式区分 is_sample，绝不误删隐藏用例。
class ProblemAdminStore {
public:
  explicit ProblemAdminStore(Database &db) : db_(db), problems_(db) {}

  // 创建题目及其公开样例（单事务）。服务端设置 created_at/updated_at 与 seed_key
  // （普通题为 NULL）；客户端无法指定 id 等由服务端管理的字段。
  // 成功返回 true 并写出新题目 ID。
  bool create(const problem::ProblemData &data, std::int64_t &out_id,
              std::string &error);

  enum class UpdateStatus {
    Updated,  // 更新成功
    NotFound, // 题目不存在
    Error,    // 数据库错误，error 非空
  };

  // 部分更新：仅写入 patch 中出现的字段，未出现的字段保持原值；created_at 保留，
  // updated_at 更新为当前时间。patch.samples 出现时整体替换该题的公开样例
  // （删除 is_sample=1 后重新写入），隐藏用例不受影响。单事务完成。
  UpdateStatus update(std::int64_t id, const problem::ProblemPatch &patch,
                      std::string &error);

  enum class DeleteStatus {
    Deleted,        // 删除成功
    NotFound,       // 题目不存在
    HasSubmissions, // 题目已有提交记录或未结算在途任务，按既定删除策略拒绝删除
    Error,          // 数据库错误，error 非空
  };

  // 删除题目：已有提交记录**或未结算在途任务**时拒绝（保留提交历史、保证已接收
  // 任务能保存终态）；均无时在同一事务内删除该题的测试用例（公开样例与隐藏用例）、
  // 用户做题状态、残留中断在途记录与题目本身，不留孤立记录。
  DeleteStatus remove(std::int64_t id, std::string &error);

private:
  Database &db_;
  ProblemStore problems_;
};

} // namespace oj
