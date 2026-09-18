#pragma once

#include <optional>
#include <string>

namespace oj {

class Database;

// 初始化数据库结构，并在首次启动（尚无 admin）时预置管理员账号。
//
// 整个过程在单个事务中完成：建表（IF NOT EXISTS，重复初始化保留已有数据）、
// 建索引、按需创建 admin。任一步失败即回滚，不会留下部分初始化的状态。
//
// admin_password 仅在「数据库中没有 admin」时被读取：
//   - 无 admin 且未提供密码 -> 返回 false，error 提示设置 OJ_ADMIN_PASSWORD；
//   - 已有 admin 时忽略该参数，不重复创建、不覆盖密码、不重置首次改密标记。
bool initialize_schema(Database &db,
                       const std::optional<std::string> &admin_password,
                       std::string &error);

} // namespace oj
