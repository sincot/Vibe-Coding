#include "db/users.h"

#include <sqlite3.h>

#include "db/database.h"

namespace oj {

namespace {

// 统一的列读取顺序，与所有 SELECT/INSERT RETURNING 中的列序一致。
void load_row(Statement &stmt, UserRecord &out) {
  out.id = stmt.column_int64(0);
  out.account = stmt.column_text(1);
  out.nickname = stmt.column_text(2);
  out.password_hash = stmt.column_text(3);
  out.role = stmt.column_text(4);
  out.reset_pwd_flag = stmt.column_int(5);
  out.created_at = stmt.column_text(6);
}

// column 仅由调用方以字面量传入（"account" / "nickname"），不存在注入风险。
bool find_by(Database &db, const char *column, const std::string &value,
             bool &found, UserRecord &out, std::string &error) {
  Statement stmt;
  std::string sql =
      std::string("SELECT id, account, nickname, password_hash, role, "
                  "reset_pwd_flag, created_at FROM users WHERE ") +
      column + " = ?";
  if (!db.prepare(sql, stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, value)) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    load_row(stmt, out);
    found = true;
    return true;
  }
  if (rc == SQLITE_DONE) {
    found = false;
    return true;
  }
  error = stmt.errmsg();
  return false;
}

} // namespace

bool UserStore::find_by_account(const std::string &account, bool &found,
                                UserRecord &out, std::string &error) {
  return find_by(db_, "account", account, found, out, error);
}

bool UserStore::find_by_id(std::int64_t id, bool &found, UserRecord &out,
                           std::string &error) {
  Statement stmt;
  if (!db_.prepare(
          "SELECT id, account, nickname, password_hash, role, reset_pwd_flag, "
          "created_at FROM users WHERE id = ?",
          stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, static_cast<sqlite3_int64>(id))) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    load_row(stmt, out);
    found = true;
    return true;
  }
  if (rc == SQLITE_DONE) {
    found = false;
    return true;
  }
  error = stmt.errmsg();
  return false;
}

bool UserStore::find_by_nickname(const std::string &nickname, bool &found,
                                 UserRecord &out, std::string &error) {
  return find_by(db_, "nickname", nickname, found, out, error);
}

UserStore::CreateStatus UserStore::create(const std::string &account,
                                          const std::string &nickname,
                                          const std::string &password_hash,
                                          UserRecord &out,
                                          std::string &error) {
  // 使用 RETURNING 一次性取回完整记录（含自增 id 与 created_at），避免二次查询。
  Statement stmt;
  if (!db_.prepare(
          "INSERT INTO users (account, nickname, password_hash, role, "
          "reset_pwd_flag) VALUES (?, ?, ?, 'user', 0) "
          "RETURNING id, account, nickname, password_hash, role, "
          "reset_pwd_flag, created_at",
          stmt, error)) {
    return CreateStatus::Error;
  }
  if (!stmt.bind(1, account) || !stmt.bind(2, nickname) ||
      !stmt.bind(3, password_hash)) {
    error = stmt.errmsg();
    return CreateStatus::Error;
  }

  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    load_row(stmt, out);
    return CreateStatus::Success;
  }

  if (rc == SQLITE_CONSTRAINT ||
      db_.extended_errcode() == SQLITE_CONSTRAINT_UNIQUE) {
    // 唯一性约束冲突：精确区分账号碰撞与昵称冲突，二者处理方式不同。
    std::string msg = stmt.errmsg();
    if (msg.find("nickname") != std::string::npos) {
      return CreateStatus::NicknameTaken;
    }
    if (msg.find("account") != std::string::npos) {
      return CreateStatus::AccountTaken;
    }
    error = msg;
    return CreateStatus::Error;
  }

  error = stmt.errmsg();
  return CreateStatus::Error;
}

} // namespace oj
