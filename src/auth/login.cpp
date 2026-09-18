#include "auth/login.h"

#include <string>

#include "auth/jwt.h"
#include "auth/password.h"
#include "db/database.h"
#include "log.h"

namespace oj {
namespace auth {

LoginService::LoginService(Database &db, JwtService &jwt)
    : db_(db), jwt_(jwt), store_(db) {}

LoginOutcome LoginService::login(const std::string &account,
                                 const std::string &password) {
  LoginOutcome outcome;

  bool found = false;
  UserRecord user;
  std::string err;
  if (!store_.find_by_account(account, found, user, err)) {
    log(LogLevel::Error, "登录：按账号查询失败: " + err);
    outcome.kind = LoginOutcome::Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  // 账号不存在与密码错误使用同一失败提示，避免泄露账号是否存在。
  if (!found) {
    outcome.kind = LoginOutcome::Kind::InvalidCredentials;
    outcome.error = "账号或密码错误";
    return outcome;
  }

  if (!verify_password(user.password_hash, password, err)) {
    if (err.empty()) {
      // 密码不匹配（非校验异常）。
      outcome.kind = LoginOutcome::Kind::InvalidCredentials;
      outcome.error = "账号或密码错误";
      return outcome;
    }
    // 哈希校验过程出错（如存储的哈希格式非法）视为内部故障，不伪装成密码错误。
    log(LogLevel::Error, "登录：密码校验失败: " + err);
    outcome.kind = LoginOutcome::Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  if (!jwt_.sign(user.id, outcome.token, err)) {
    log(LogLevel::Error, "登录：JWT 签发失败: " + err);
    outcome.kind = LoginOutcome::Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  outcome.kind = LoginOutcome::Kind::Success;
  outcome.expires_in_seconds = jwt_.expires_seconds();
  outcome.user = user;
  return outcome;
}

} // namespace auth
} // namespace oj
