#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/users.h"

namespace oj {

class Database;

// 管理后台用户列表条目：仅包含管理所需字段，结构上不含密码哈希。
// 绝不用于对外响应之外的任何序列化，避免误把 password_hash 下发。
struct UserSummary {
  std::int64_t id = 0;
  std::string account;
  std::string nickname;
  std::string role;
  int reset_pwd_flag = 0;
  std::string created_at;
};

// 管理员用户写操作的数据访问封装（M2.4）。
//
// 所有语句使用参数绑定；涉及「读角色 → 最后管理员检查 → 写角色」的多步操作在
// 单个短事务内以 BEGIN IMMEDIATE 完成，并复用 Database::transaction_mutex()
// 串行化，保证最后一名管理员的保护在并发更新下仍然有效（不会出现两次无保护的
// 计数查询都判定「还有两人」而把管理员清零）。
//
// 本类不下发密码哈希；调用方只能拿到不含哈希的 UserSummary 或状态码。
class UserAdminStore {
public:
  explicit UserAdminStore(Database &db) : db_(db), users_(db) {}

  // 分页列出全部用户，按 id 升序稳定排序（唯一主键，跨页不重不漏）。
  // page 从 1 开始，page_size 由调用方给定；total 返回满足条件的用户总数
  // （与列表同一数据源，当前为全部用户，不做任何筛选）。
  // 返回 false 表示数据库错误，error 非空。
  bool list_users(int page, int page_size, std::vector<UserSummary> &out,
                  long long &total, std::string &error);

  enum class ResetStatus {
    Updated,  // 密码哈希已替换且 reset_pwd_flag 置 1
    NotFound, // 目标用户不存在
    Error,    // 数据库错误，error 非空
  };

  // 管理员重置目标用户密码：单条 UPDATE 原子写入新哈希并置 reset_pwd_flag=1，
  // 与 M1.3 的首次改密机制一致（目标用户须自行改密后才能继续受限业务）。
  // 不需要、也不校验目标用户旧密码。password_hash 由调用方在事务外哈希后传入。
  ResetStatus reset_password(std::int64_t user_id, const std::string &new_hash,
                             std::string &error);

  enum class RoleStatus {
    Updated,   // 角色已更新
    NotFound,  // 目标用户不存在
    LastAdmin, // 会把最后一名管理员降级为普通用户，拒绝
    Error,     // 数据库错误，error 非空
  };

  // 修改目标用户角色。允许把管理员降级为普通用户（含自我降级），但若目标当前是
  // 管理员且新角色不是 admin，则在事务内统计管理员数量，仅当管理员多于一名时才
  // 允许，从而禁止取消最后一名管理员的权限。检查与更新在同一 BEGIN IMMEDIATE
  // 事务内完成，并发更新下保护仍有效。
  RoleStatus change_role(std::int64_t user_id, const std::string &new_role,
                         std::string &error);

private:
  Database &db_;
  UserStore users_;
};

} // namespace oj
