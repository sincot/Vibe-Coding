#pragma once

#include "auth/context.h"

namespace oj {
namespace auth {

// 首次强制改密检查：reset_pwd_flag 非 0 表示该用户（当前仅预置 admin 可能为 1）
// 尚未完成首次改密，除登录、查询本人信息与修改本人密码外的业务操作应先改密。
// 返回值仅依据已从数据库回查的当前用户上下文，不信任客户端传入的字段或 token。
bool requires_password_change(const AuthUser &user);

// 管理员权限检查结果。基于已验证的当前用户上下文判断：其 role 与 reset_pwd_flag
// 均来自数据库最新值，既不信客户端传入的角色，也不依赖 JWT 中可能过时的角色。
enum class AdminCheck {
  Ok,                     // 已登录 + 已完成必要改密 + 具备 admin 角色
  NotAdmin,               // 不具备 admin 角色（权限不足）
  PasswordChangeRequired, // 具备 admin 角色但尚未完成首次改密
};

// 管理员业务入口的组合检查：满足「已登录 + 已完成必要改密 + 具备 admin 角色」
// 时返回 Ok。调用方应先用 authenticate_request 完成登录校验，再调用本函数。
AdminCheck check_admin(const AuthUser &user);

} // namespace auth
} // namespace oj
