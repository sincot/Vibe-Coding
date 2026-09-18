#pragma once

#include <cstdint>
#include <string>

#include "db/users.h"

namespace oj {

class Database;

namespace auth {

class JwtService;

// 当前登录用户上下文（不含密码哈希）。
struct AuthUser {
  std::int64_t id = 0;
  std::string account;
  std::string nickname;
  std::string role;
  int reset_pwd_flag = 0;
};

// 从 Authorization 头解析 Bearer token：
//   - 仅接受 "Bearer <token>"（大小写不敏感的 scheme，前后允许空白）；
//   - token 本身不允许包含空白。
// 成功返回 true 并写入 token；否则返回 false。
bool extract_bearer_token(const std::string &authorization, std::string &token);

// 身份验证结果。
enum class AuthResult {
  Ok,            // 成功，user 填充当前数据库中的用户信息
  Unauthorized,  // 认证失败（缺 token/格式错误/签名/算法/claims/过期/用户不存在）
  InternalError, // 内部故障（数据库错误等），不应伪装为认证失败
};

// 可复用的登录检查与当前用户上下文解析：
//   1. 验证 token 签名、算法、过期时间与必要 claims（sub 为数据库用户 ID）；
//   2. 按 ID 查询数据库确认用户存在，并读取当前角色与首次改密标记，
//      避免仅依赖 token 中可能过时的权限信息。
AuthResult authenticate_request(JwtService &jwt, UserStore &store,
                                const std::string &token, AuthUser &user,
                                std::string &error);

} // namespace auth
} // namespace oj
