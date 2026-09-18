#include "auth/authorize.h"

namespace oj {
namespace auth {

namespace {
constexpr const char *kAdminRole = "admin";
} // namespace

bool requires_password_change(const AuthUser &user) {
  return user.reset_pwd_flag != 0;
}

AdminCheck check_admin(const AuthUser &user) {
  // 组合顺序：先判断角色（非管理员直接拒绝），再判断首次改密标记，
  // 保证管理员业务入口统一满足「已登录 + 已完成必要改密 + 具备 admin 角色」。
  if (user.role != kAdminRole) {
    return AdminCheck::NotAdmin;
  }
  if (requires_password_change(user)) {
    return AdminCheck::PasswordChangeRequired;
  }
  return AdminCheck::Ok;
}

} // namespace auth
} // namespace oj
