#pragma once

#include <cstdint>
#include <string>

#include "db/problems.h"
#include "problem/testcase_validation.h"

namespace oj {

class Database;

// 管理员隐藏测试用例写操作的数据访问封装（M2.2）。
//
// 设计要点：
//   - 只操作隐藏用例（is_sample=0）。公开样例（is_sample=1）由 M2.1 的题目接口
//     通过 samples 字段维护，本类绝不写入或删除 is_sample=1 的记录，因此新增、
//     修改、排序都不可能把隐藏用例变成公开样例，也不会破坏公开样例。
//   - 所有语句使用参数绑定；写入在短事务内完成，并复用 Database::transaction_mutex()
//     与 BEGIN IMMEDIATE，和题目管理/提交持久化串行化，避免与并发删题交错产生
//     孤立记录或假成功。
//   - 修改、删除均同时以 problem_id 与用例 ID 定位；用例不存在、不属于该题或属于
//     公开样例时统一返回 NotFound（由 HTTP 层映射为 404）。
//   - ord 规则：缺省新增 = 该题全部用例（含公开样例）当前最大 ord + 1，无用例时为 0；
//     允许重复，顺序由 (ord ASC, id ASC) 确定；删除不重排、不回收空号。
class TestcaseAdminStore {
public:
  explicit TestcaseAdminStore(Database &db) : db_(db), problems_(db) {}

  enum class CreateStatus {
    Created,         // 新增成功
    ProblemNotFound, // 题目不存在
    OrdExhausted,    // 缺省 ord 需自动分配，但该题 ord 已达上限
    Error,           // 数据库错误，error 非空
  };

  // 在指定题目下新增一条隐藏用例（is_sample=0）。题目归属只由 problem_id 决定，
  // 不接受客户端指定的其他归属。成功写出新用例 ID 与最终保存的 ord。
  CreateStatus create(std::int64_t problem_id,
                      const problem::TestcaseData &data, std::int64_t &out_id,
                      int &out_ord, std::string &error);

  enum class UpdateStatus {
    Updated,  // 更新成功
    NotFound, // 用例不存在 / 不属于该题 / 属于公开样例
    Error,    // 数据库错误，error 非空
  };

  // 部分更新：仅写入 patch 中出现的字段，未出现的字段保持原值；显式空串可清空。
  // 成功写出更新后的 ord。
  UpdateStatus update(std::int64_t problem_id, std::int64_t testcase_id,
                      const problem::TestcasePatch &patch, int &out_ord,
                      std::string &error);

  enum class DeleteStatus {
    Deleted,  // 删除成功
    NotFound, // 用例不存在 / 不属于该题 / 属于公开样例
    Error,    // 数据库错误，error 非空
  };

  // 删除一条隐藏用例。按 problem_id 与用例 ID 定位，不重排其余用例的 ord。
  DeleteStatus remove(std::int64_t problem_id, std::int64_t testcase_id,
                      std::string &error);

private:
  Database &db_;
  ProblemStore problems_;
};

} // namespace oj
