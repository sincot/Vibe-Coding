#include "db/users.hpp"

#include <sqlite3.h>
#include <sys/random.h>

#include <cerrno>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <utility>

#include "auth/password.hpp"

namespace oj {
namespace db {
namespace {

// 从 /dev/urandom 取熵；失败回退到 std::random_device。
bool FillRandom(unsigned char* buf, const size_t len) {
  size_t done = 0;
  while (done < len) {
    const ssize_t n = ::getrandom(buf + done, len - done, 0);
    if (n > 0) {
      done += static_cast<size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

std::mt19937_64 MakeRng() {
  unsigned char seed[8] = {};
  if (FillRandom(seed, sizeof(seed))) {
    std::uint64_t v = 0;
    for (size_t i = 0; i < sizeof(v); ++i) {
      v = (v << 8) | seed[i];
    }
    return std::mt19937_64(v);
  }
  return std::mt19937_64(std::random_device{}());
}

// 生成 10 位纯数字账号（首位 1-9，其余 0-9）。
std::string RandomAccount(std::mt19937_64& rng) {
  std::uniform_int_distribution<int> first(1, 9);
  std::uniform_int_distribution<int> rest(0, 9);
  std::string out;
  out.reserve(10);
  out.push_back(static_cast<char>('0' + first(rng)));
  for (int i = 0; i < 9; ++i) {
    out.push_back(static_cast<char>('0' + rest(rng)));
  }
  return out;
}

bool AccountExists(sqlite3* db, const std::string& account) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT 1 FROM users WHERE account = ?1 LIMIT 1;";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return true;  // 查询失败时宁可多取一次账号，也不引入重复
  }
  sqlite3_bind_text(stmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return exists;
}

constexpr const char* kSelectUserCols =
    "id, account, nickname, role, reset_pwd_flag, created_at";

void FillUserCore(sqlite3_stmt* stmt, UserInfo* out) {
  out->id = sqlite3_column_int64(stmt, 0);
  if (const char* s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))) {
    out->account = s;
  }
  if (const char* s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))) {
    out->nickname = s;
  }
  if (const char* s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))) {
    out->role = s;
  }
  out->reset_pwd_flag = sqlite3_column_int(stmt, 4) != 0;
  if (const char* s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))) {
    out->created_at = s;
  }
}

// 6 列版（不含密码哈希），列序与 kSelectUserCols 一致。
void FillUser(sqlite3_stmt* stmt, UserInfo* out) { FillUserCore(stmt, out); }

// 7 列版（多 password_hash），列序与 VerifyLogin/ChangePassword 的查询一致。
void FillUserWithHash(sqlite3_stmt* stmt, UserInfo* out) {
  FillUserCore(stmt, out);
  if (const char* s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))) {
    out->password_hash = s;
  }
}

std::string SelectPasswordHash(sqlite3* db, std::int64_t user_id) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT password_hash FROM users WHERE id = ?1;";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return {};
  }
  sqlite3_bind_int64(stmt, 1, user_id);
  std::string hash;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    if (const char* s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))) {
      hash = s;
    }
  }
  sqlite3_finalize(stmt);
  return hash;
}

bool SelectUserBy(Database& db, const char* where, const char* value,
                  UserInfo* out) {
  // 调用方已持有 db.Lock()，此处不再获取，避免非递归互斥锁死锁。
  const std::string sql = std::string("SELECT ") + kSelectUserCols +
                          " FROM users WHERE " + where + " = ?1 LIMIT 1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.raw(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
  const bool found = sqlite3_step(stmt) == SQLITE_ROW;
  if (found && out) {
    FillUser(stmt, out);
  }
  sqlite3_finalize(stmt);
  return found;
}

bool SelectUserById(sqlite3* db, std::int64_t id, UserInfo* out) {
  const std::string sql = std::string("SELECT ") + kSelectUserCols +
                          " FROM users WHERE id = ?1 LIMIT 1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(stmt, 1, id);
  const bool found = sqlite3_step(stmt) == SQLITE_ROW;
  if (found && out) {
    FillUser(stmt, out);
  }
  sqlite3_finalize(stmt);
  return found;
}

}  // namespace

CreateUserResult CreateUser(Database& db, const std::string& nickname,
                            const std::string& plain_password, UserInfo* out) {
  if (out == nullptr || plain_password.empty()) {
    return CreateUserResult::kInvalidPassword;
  }

  const std::string hash = HashPassword(plain_password);
  if (hash.empty()) {
    return CreateUserResult::kInternalError;
  }

  std::unique_lock<std::mutex> lock = db.Lock();
  sqlite3* dbref = db.raw();

  // 预检昵称占用，返回明确的 kNicknameTaken（对唯一约束的兜底见下方 SQLITE_CONSTRAINT 分支）。
  if (SelectUserBy(db, "nickname", nickname.c_str(), nullptr)) {
    return CreateUserResult::kNicknameTaken;
  }

  static thread_local std::mt19937_64 rng = MakeRng();

  for (int attempt = 0; attempt < 20; ++attempt) {
    const std::string account = RandomAccount(rng);
    if (AccountExists(dbref, account)) {
      continue;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO users (account, nickname, password_hash, role, reset_pwd_flag, created_at) "
        "VALUES (?1, ?2, ?3, 'user', 0, datetime('now'));";
    if (sqlite3_prepare_v2(dbref, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      return CreateUserResult::kInternalError;
    }
    sqlite3_bind_text(stmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, nickname.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_null(stmt, 4);  // 使用表默认值 datetime('now')
    sqlite3_bind_null(stmt, 5);  // 使用表默认值 0 / 'user'
    const int rc = sqlite3_step(stmt);
    const int extended = sqlite3_extended_errcode(dbref);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
      const std::int64_t user_id = sqlite3_last_insert_rowid(dbref);
      // 从库中回读完整行（含 created_at 默认值），保证返回字段完整一致。
      if (!SelectUserById(dbref, user_id, out)) {
        return CreateUserResult::kInternalError;
      }
      return CreateUserResult::kOk;
    }
    if (rc == SQLITE_CONSTRAINT &&
        (extended == SQLITE_CONSTRAINT_UNIQUE || extended == SQLITE_CONSTRAINT_PRIMARYKEY)) {
      continue;  // 昵称或账号撞唯一约束，重取账号再试
    }
    return CreateUserResult::kInternalError;
  }
  return CreateUserResult::kInternalError;
}

bool GetUserByAccount(Database& db, const std::string& account, UserInfo* out) {
  std::unique_lock<std::mutex> lock = db.Lock();
  return SelectUserBy(db, "account", account.c_str(), out);
}

bool GetUserByNickname(Database& db, const std::string& nickname, UserInfo* out) {
  std::unique_lock<std::mutex> lock = db.Lock();
  return SelectUserBy(db, "nickname", nickname.c_str(), out);
}

LoginResult VerifyLogin(Database& db, const std::string& account,
                        const std::string& plain_password, UserInfo* out) {
  const std::string sql = std::string("SELECT ") + kSelectUserCols +
                          ", password_hash FROM users WHERE account = ?1 LIMIT 1;";
  std::unique_lock<std::mutex> lock = db.Lock();
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.raw(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return LoginResult::kBadCredential;
  }
  sqlite3_bind_text(stmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  const bool found = sqlite3_step(stmt) == SQLITE_ROW;
  if (!found) {
    sqlite3_finalize(stmt);
    return LoginResult::kBadCredential;
  }
  if (out) {
    FillUserWithHash(stmt, out);  // 列 0-5: 用户信息，列 6: password_hash
  }
  sqlite3_finalize(stmt);
  if (out == nullptr ||
      !VerifyPassword(plain_password, out->password_hash)) {
    return LoginResult::kBadCredential;
  }
  return LoginResult::kOk;
}

bool GetUserById(Database& db, std::int64_t id, UserInfo* out) {
  std::unique_lock<std::mutex> lock = db.Lock();
  return SelectUserById(db.raw(), id, out);
}

int ChangePassword(Database& db, std::int64_t user_id,
                   const std::string& old_password,
                   const std::string& new_password) {
  std::unique_lock<std::mutex> lock = db.Lock();
  sqlite3* dbref = db.raw();

  const std::string old_hash = SelectPasswordHash(dbref, user_id);
  if (old_hash.empty()) {
    return 2;
  }
  if (!VerifyPassword(old_password, old_hash)) {
    return 1;
  }

  const std::string new_hash = HashPassword(new_password);
  if (new_hash.empty()) {
    return 3;
  }

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "UPDATE users SET password_hash = ?1, reset_pwd_flag = 0 WHERE id = ?2;";
  if (sqlite3_prepare_v2(dbref, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return 3;
  }
  sqlite3_bind_text(stmt, 1, new_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, user_id);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : 3;
}

}  // namespace db
}  // namespace oj