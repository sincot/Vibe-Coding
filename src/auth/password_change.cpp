#include "auth/password_change.h"

#include <sqlite3.h>

#include <mutex>
#include <string>

#include "auth/password.h"
#include "db/database.h"
#include "log.h"

namespace oj {
namespace auth {

ChangePasswordService::ChangePasswordService(Database &db)
    : db_(db), store_(db) {}

ChangePasswordService::Result ChangePasswordService::change_password(
    std::int64_t user_id, const std::string &old_password,
    const std::string &new_password) {
  Result result;

  // 新密码哈希在事务外完成（argon2id 计算较耗时，不应长时间持有写锁）。
  std::string new_hash;
  std::string err;
  if (!hash_password(new_password, new_hash, err)) {
    log(LogLevel::Error, "改密：新密码哈希失败: " + err);
    result.outcome = Outcome::InternalError;
    result.error = "内部错误";
    return result;
  }

  // 串行化多语句事务：连接为 serialized 模式，单条语句原子，但多语句事务需用
  // 连接级互斥锁整体串行化，避免并发改密在同一连接上交错（见 Database 说明）。
  std::lock_guard<std::mutex> lock(db_.transaction_mutex());

  // BEGIN IMMEDIATE：立即取得写锁，后续「读当前哈希 → 校验旧密码 → 更新」
  // 在同一事务内串行完成，避免并发改密时旧密码覆盖新密码。
  if (!db_.begin(err)) {
    log(LogLevel::Error, "改密：开启事务失败: " + err);
    result.outcome = Outcome::InternalError;
    result.error = "内部错误";
    return result;
  }

  bool found = false;
  UserRecord record;
  if (!store_.find_by_id(user_id, found, record, err)) {
    log(LogLevel::Error, "改密：查询用户失败: " + err);
    db_.rollback(err);
    result.outcome = Outcome::InternalError;
    result.error = "内部错误";
    return result;
  }
  if (!found) {
    db_.rollback(err);
    result.outcome = Outcome::UserNotFound;
    return result;
  }

  // 校验旧密码（针对事务内刚读取的当前哈希）。
  std::string verify_err;
  if (!verify_password(record.password_hash, old_password, verify_err)) {
    db_.rollback(err);
    if (verify_err.empty()) {
      // 密码不匹配（非校验异常）。
      result.outcome = Outcome::InvalidOldPassword;
      return result;
    }
    log(LogLevel::Error, "改密：旧密码校验异常: " + verify_err);
    result.outcome = Outcome::InternalError;
    result.error = "内部错误";
    return result;
  }

  // 原子更新密码哈希 + 清除首次改密标记（单条 UPDATE，两字段同时生效）。
  auto status = store_.update_password(user_id, new_hash, err);
  if (status == UserStore::UpdatePasswordStatus::Error) {
    log(LogLevel::Error, "改密：更新密码失败: " + err);
    db_.rollback(err);
    result.outcome = Outcome::InternalError;
    result.error = "内部错误";
    return result;
  }
  if (status == UserStore::UpdatePasswordStatus::NotFound) {
    db_.rollback(err);
    result.outcome = Outcome::UserNotFound;
    return result;
  }

  if (!db_.commit(err)) {
    log(LogLevel::Error, "改密：提交事务失败: " + err);
    db_.rollback(err);
    result.outcome = Outcome::InternalError;
    result.error = "内部错误";
    return result;
  }

  result.outcome = Outcome::Success;
  return result;
}

} // namespace auth
} // namespace oj
