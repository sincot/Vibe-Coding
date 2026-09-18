#pragma once

#include <cstdint>
#include <string>

#include "db/users.h"

namespace oj {

class Database;

namespace auth {

// 改密服务：在单个数据库事务内校验旧密码，并原子更新密码哈希与首次改密标记。
//
// 目标用户由调用方（HTTP 层）从已验证的当前用户上下文取得（用户 ID 来自 token
// 验证 + 数据库回查），本服务不接受任何客户端传入的用户 ID / 账号，无法借此
// 修改他人密码。新密码已由调用方按注册密码规则校验（validate_password_change），
// 本服务仅负责哈希与事务内更新，不做长度等重复校验。
//
// 并发安全：事务以 BEGIN IMMEDIATE 取得写锁后再读取当前哈希并校验旧密码，
// 因此两个基于同一旧密码的并发请求会被串行化——后执行者读到已更新的哈希，
// 旧密码校验失败，不会让已失效的旧密码再次覆盖新密码。
class ChangePasswordService {
public:
  explicit ChangePasswordService(Database &db);

  enum class Outcome {
    Success,            // 改密成功，密码哈希已更新且 reset_pwd_flag 已清除
    InvalidOldPassword, // 旧密码错误
    UserNotFound,       // 目标用户不存在（通常意味着会话已失效）
    InternalError,      // 内部故障（数据库/哈希），对外返回通用文案
  };

  struct Result {
    Outcome outcome = Outcome::InternalError;
    std::string error; // 仅 InternalError 时为用户可读的通用文案
  };

  Result change_password(std::int64_t user_id, const std::string &old_password,
                         const std::string &new_password);

private:
  Database &db_;
  UserStore store_;
};

} // namespace auth
} // namespace oj
