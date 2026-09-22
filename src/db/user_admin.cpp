#include "db/user_admin.h"

#include <mutex>

#include <sqlite3.h>

#include "db/database.h"

namespace oj {

namespace {

void rollback_quiet(Database &db) {
  std::string ignored;
  db.rollback(ignored);
}

constexpr const char *kAdminRole = "admin";

} // namespace

bool UserAdminStore::list_users(int page, int page_size,
                                std::vector<UserSummary> &out, long long &total,
                                std::string &error) {
  out.clear();
  total = 0;

  {
    Statement stmt;
    if (!db_.prepare("SELECT COUNT(*) FROM users", stmt, error)) {
      return false;
    }
    const int rc = stmt.step();
    if (rc != SQLITE_ROW) {
      error = stmt.errmsg();
      return false;
    }
    total = static_cast<long long>(stmt.column_int64(0));
  }

  const long long offset =
      static_cast<long long>(page - 1) * static_cast<long long>(page_size);
  {
    Statement stmt;
    if (!db_.prepare("SELECT id, account, nickname, role, reset_pwd_flag, "
                     "created_at FROM users ORDER BY id ASC LIMIT ? OFFSET ?",
                     stmt, error)) {
      return false;
    }
    if (!stmt.bind(1, page_size) ||
        !stmt.bind(2, static_cast<sqlite3_int64>(offset))) {
      error = stmt.errmsg();
      return false;
    }
    while (true) {
      const int rc = stmt.step();
      if (rc == SQLITE_ROW) {
        UserSummary user;
        user.id = stmt.column_int64(0);
        user.account = stmt.column_text(1);
        user.nickname = stmt.column_text(2);
        user.role = stmt.column_text(3);
        user.reset_pwd_flag = stmt.column_int(4);
        user.created_at = stmt.column_text(5);
        out.push_back(std::move(user));
        continue;
      }
      if (rc == SQLITE_DONE) {
        break;
      }
      error = stmt.errmsg();
      return false;
    }
  }
  return true;
}

UserAdminStore::ResetStatus
UserAdminStore::reset_password(std::int64_t user_id,
                               const std::string &new_hash,
                               std::string &error) {
  // 单条 UPDATE 同时写入 password_hash 与 reset_pwd_flag=1，两字段原子生效；
  // 使用 RETURNING 区分「更新成功」与「目标用户不存在」。密码哈希已在事务外
  // 计算完成，此处不持有额外锁。
  Statement stmt;
  if (!db_.prepare("UPDATE users SET password_hash = ?, reset_pwd_flag = 1 "
                   "WHERE id = ? RETURNING id",
                   stmt, error)) {
    return ResetStatus::Error;
  }
  if (!stmt.bind(1, new_hash) ||
      !stmt.bind(2, static_cast<sqlite3_int64>(user_id))) {
    error = stmt.errmsg();
    return ResetStatus::Error;
  }
  const int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    return ResetStatus::Updated;
  }
  if (rc == SQLITE_DONE) {
    return ResetStatus::NotFound;
  }
  error = stmt.errmsg();
  return ResetStatus::Error;
}

UserAdminStore::RoleStatus
UserAdminStore::change_role(std::int64_t user_id, const std::string &new_role,
                            std::string &error) {
  std::lock_guard<std::mutex> lock(db_.transaction_mutex());
  if (!db_.begin(error)) {
    return RoleStatus::Error;
  }

  bool found = false;
  UserRecord record;
  if (!users_.find_by_id(user_id, found, record, error)) {
    rollback_quiet(db_);
    return RoleStatus::Error;
  }
  if (!found) {
    rollback_quiet(db_);
    return RoleStatus::NotFound;
  }

  // 仅当把管理员降级、且目标当前确为管理员时，才检查是否为最后一名管理员。
  if (record.role == kAdminRole && new_role != kAdminRole) {
    sqlite3_int64 admin_count = 0;
    Statement count_stmt;
    if (!db_.prepare("SELECT COUNT(*) FROM users WHERE role = 'admin'",
                     count_stmt, error)) {
      rollback_quiet(db_);
      return RoleStatus::Error;
    }
    if (count_stmt.step() != SQLITE_ROW) {
      error = count_stmt.errmsg();
      rollback_quiet(db_);
      return RoleStatus::Error;
    }
    admin_count = count_stmt.column_int64(0);
    if (admin_count <= 1) {
      rollback_quiet(db_);
      return RoleStatus::LastAdmin;
    }
  }

  {
    Statement stmt;
    if (!db_.prepare("UPDATE users SET role = ? WHERE id = ?", stmt, error)) {
      rollback_quiet(db_);
      return RoleStatus::Error;
    }
    if (!stmt.bind(1, new_role) ||
        !stmt.bind(2, static_cast<sqlite3_int64>(user_id))) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return RoleStatus::Error;
    }
    if (stmt.step() != SQLITE_DONE) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return RoleStatus::Error;
    }
  }

  if (!db_.commit(error)) {
    rollback_quiet(db_);
    return RoleStatus::Error;
  }
  return RoleStatus::Updated;
}

} // namespace oj
