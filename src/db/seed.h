#pragma once

#include <string>

namespace oj {

class Database;

// 导入内置种子题目（M1.4）。
//
// 幂等策略：以 problems.seed_key 作为稳定标识，配合唯一索引保证同一道种子题只被
// 导入一次。
//   - 已存在相同 seed_key 的题目 -> 整题跳过：不重复创建，也不覆盖题目或用例的
//     任何已有修改（包括管理员后续的编辑）；
//   - 不存在 -> 在单个事务内创建题目及其全部用例（公开样例 is_sample=1，
//     隐藏用例 is_sample=0，ord 按定义顺序连续编号）。
//
// 每次调用只处理缺失的种子题，不会清空或重写其它业务数据。created 返回本次新建的
// 题目数量。返回 false 表示失败，error 非空且当次题目的写入已回滚。
bool import_seed_problems(Database &db, int &created, std::string &error);

} // namespace oj
