#include "auth/register.h"

#include <string>

#include "auth/account.h"
#include "auth/password.h"
#include "auth/validation.h"
#include "db/database.h"
#include "db/users.h"
#include "log.h"

namespace oj {
namespace auth {

RegisterService::RegisterService(Database &db, AccountGenerator &generator,
                                 int max_account_attempts)
    : db_(db),
      generator_(generator),
      max_account_attempts_(max_account_attempts),
      store_(db) {}

RegisterOutcome RegisterService::register_user(const std::string &nickname,
                                               const std::string &password) {
  RegisterOutcome outcome;

  std::string normalized;
  std::string err;
  if (!validate_nickname(nickname, normalized, err)) {
    outcome.kind = RegisterOutcome::Kind::InvalidNickname;
    outcome.error = err;
    return outcome;
  }
  if (!validate_password(password, err)) {
    outcome.kind = RegisterOutcome::Kind::InvalidPassword;
    outcome.error = err;
    return outcome;
  }

  // 昵称预查询：给出友好提示；并发下的最终保证由数据库唯一性约束兜底。
  bool found = false;
  UserRecord existing;
  if (!store_.find_by_nickname(normalized, found, existing, err)) {
    log(LogLevel::Error, "注册：按昵称查询失败: " + err);
    outcome.kind = RegisterOutcome::Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }
  if (found) {
    outcome.kind = RegisterOutcome::Kind::NicknameTaken;
    outcome.error = "昵称已被使用";
    return outcome;
  }

  std::string hash;
  if (!hash_password(password, hash, err)) {
    log(LogLevel::Error, "注册：密码哈希失败: " + err);
    outcome.kind = RegisterOutcome::Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  for (int attempt = 0; attempt < max_account_attempts_; ++attempt) {
    std::string account = generator_.generate();
    auto status = store_.create(account, normalized, hash, outcome.user, err);
    switch (status) {
      case UserStore::CreateStatus::Success:
        outcome.kind = RegisterOutcome::Kind::Success;
        return outcome;
      case UserStore::CreateStatus::NicknameTaken:
        // 并发下另一请求已占用该昵称。
        outcome.kind = RegisterOutcome::Kind::NicknameTaken;
        outcome.error = "昵称已被使用";
        return outcome;
      case UserStore::CreateStatus::AccountTaken:
        // 账号碰撞：换一个账号重试。
        continue;
      case UserStore::CreateStatus::Error:
        log(LogLevel::Error, "注册：创建用户失败: " + err);
        outcome.kind = RegisterOutcome::Kind::InternalError;
        outcome.error = "内部错误";
        return outcome;
    }
  }

  log(LogLevel::Error, "注册：账号分配重试次数耗尽（" +
                           std::to_string(max_account_attempts_) + " 次）");
  outcome.kind = RegisterOutcome::Kind::InternalError;
  outcome.error = "内部错误";
  return outcome;
}

} // namespace auth
} // namespace oj
