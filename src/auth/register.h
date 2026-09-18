#pragma once

#include <string>

#include "db/users.h"

namespace oj {

class Database;

namespace auth {

class AccountGenerator;

// 注册结果。
struct RegisterOutcome {
  enum class Kind {
    Success,          // 成功，user 填充完整记录
    InvalidNickname,  // 昵称非法
    InvalidPassword,  // 密码非法
    NicknameTaken,    // 昵称已被占用（含并发冲突）
    InternalError,    // 内部故障（数据库/哈希/账号分配重试耗尽）
  } kind = Kind::InternalError;

  // 非 Success 时的用户可读错误信息（InternalError 时为通用文案）。
  std::string error;

  // Success 时有效。
  UserRecord user;
};

// 注册服务：组合昵称/密码校验、argon2id 密码哈希、账号分配与用户创建。
//
// 账号分配流程：
//   - 由 generator 生成账号，尝试入库；
//   - 账号碰撞（数据库 account 唯一性冲突）时换号重试，最多 max_account_attempts 次；
//   - 昵称冲突与账号碰撞被精确区分，不会把昵称冲突当作账号碰撞重试；
//   - 昵称唯一性先做预查询以给出友好提示，真正的并发安全由数据库唯一性约束保证。
//
// 安全约定：不在日志中记录明文密码、完整注册请求体或密码哈希。
class RegisterService {
public:
  explicit RegisterService(Database &db, AccountGenerator &generator,
                           int max_account_attempts = 5);

  RegisterOutcome register_user(const std::string &nickname,
                                const std::string &password);

private:
  Database &db_;
  AccountGenerator &generator_;
  int max_account_attempts_;
  UserStore store_;
};

} // namespace auth
} // namespace oj
