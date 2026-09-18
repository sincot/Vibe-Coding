#pragma once

#include <cstdint>
#include <string>

#include "db/users.h"

namespace oj {

class Database;

namespace auth {

class JwtService;

// 登录结果。
struct LoginOutcome {
  enum class Kind {
    Success,             // 成功，user 与 token 有效
    InvalidCredentials,  // 账号或密码错误（与账号是否存在无关，提示一致）
    InternalError,       // 内部故障（数据库/哈希/JWT 签发），对外返回通用文案
  } kind = Kind::InternalError;

  std::string token;         // Success 时有效
  int expires_in_seconds = 0; // Success 时有效
  UserRecord user;           // Success 时有效（password_hash 绝不对外输出）

  // 非 Success 时的用户可读错误信息（InternalError 为通用文案）。
  std::string error;
};

// 登录服务：按账号查询用户、复用 argon2id 密码验证、成功后签发 JWT。
//
// 账号：普通用户为系统分配的 10 位数字账号，预置管理员为 "admin"，统一按
// account 字段查询；密码不做任何裁剪或截断，空白视为有效内容。
//
// 安全约定：错误账号与错误密码返回一致提示；不记录明文密码、密码哈希或完整 token。
class LoginService {
public:
  LoginService(Database &db, JwtService &jwt);

  LoginOutcome login(const std::string &account, const std::string &password);

private:
  Database &db_;
  JwtService &jwt_;
  UserStore store_;
};

} // namespace auth
} // namespace oj
