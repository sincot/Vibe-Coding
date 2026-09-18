#include "db/database.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "auth/password.hpp"

namespace oj {
namespace {

// 建表脚本。CREATE TABLE IF NOT EXISTS 仅用于“缺表补建”，不充当自动迁移；
// 已存在表的结构兼容性由 VerifySchemaColumns() 单独检查。
const char* const kSchemaSql = R"SQL(
-- users：账号（普通用户 10 位数字，admin 为例外，故不在库层约束格式）
CREATE TABLE IF NOT EXISTS users (
  id             INTEGER PRIMARY KEY,
  account        TEXT    NOT NULL UNIQUE,
  nickname       TEXT    NOT NULL UNIQUE,
  password_hash  TEXT    NOT NULL,
  role           TEXT    NOT NULL DEFAULT 'user' CHECK (role IN ('admin', 'user')),
  reset_pwd_flag INTEGER NOT NULL DEFAULT 0   CHECK (reset_pwd_flag IN (0, 1)),
  created_at     TEXT    NOT NULL DEFAULT (datetime('now'))
);

-- problems：题目元数据；时限/内存默认 2s / 64MB（PRB-02）
CREATE TABLE IF NOT EXISTS problems (
  id              INTEGER PRIMARY KEY,
  title           TEXT    NOT NULL,
  description     TEXT    NOT NULL DEFAULT '',
  difficulty      TEXT    NOT NULL DEFAULT 'easy' CHECK (difficulty IN ('easy', 'medium', 'hard')),
  tags            TEXT    NOT NULL DEFAULT '',
  time_limit_ms   INTEGER NOT NULL DEFAULT 2000  CHECK (time_limit_ms > 0),
  memory_limit_kb INTEGER NOT NULL DEFAULT 65536 CHECK (memory_limit_kb > 0),
  visible         INTEGER NOT NULL DEFAULT 1 CHECK (visible IN (0, 1)),
  created_at      TEXT    NOT NULL DEFAULT (datetime('now')),
  updated_at      TEXT    NOT NULL DEFAULT (datetime('now'))
);

-- testcases：多组测试用例，ord 为题内序号，UNIQUE(problem_id, ord) 防同题重复序号
CREATE TABLE IF NOT EXISTS testcases (
  id         INTEGER PRIMARY KEY,
  problem_id INTEGER NOT NULL REFERENCES problems (id),
  ord        INTEGER NOT NULL DEFAULT 0,
  input      TEXT    NOT NULL DEFAULT '',
  output     TEXT    NOT NULL DEFAULT '',
  UNIQUE (problem_id, ord)
);
CREATE INDEX IF NOT EXISTS idx_testcases_problem_id ON testcases (problem_id);

-- submissions：完整提交记录，全部持久化（PERS-01）；不做级联删除，保留历史
CREATE TABLE IF NOT EXISTS submissions (
  id          INTEGER PRIMARY KEY,
  user_id     INTEGER NOT NULL REFERENCES users (id),
  problem_id  INTEGER NOT NULL REFERENCES problems (id),
  language    TEXT    NOT NULL CHECK (language IN ('cpp17', 'c11')),
  source_code TEXT    NOT NULL,
  status      TEXT    NOT NULL CHECK (status IN ('AC', 'WA', 'CE', 'TLE', 'RE', 'MLE', 'SYSERR')),
  per_case    TEXT,
  compile_msg TEXT,
  runtime_ms  INTEGER,
  memory_kb   INTEGER,
  created_at  TEXT    NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_submissions_user_id ON submissions (user_id);
CREATE INDEX IF NOT EXISTS idx_submissions_problem_id ON submissions (problem_id);

-- user_problem_status：用户×题的做题状态与统计（PERS-02）
CREATE TABLE IF NOT EXISTS user_problem_status (
  id           INTEGER PRIMARY KEY,
  user_id      INTEGER NOT NULL REFERENCES users (id),
  problem_id   INTEGER NOT NULL REFERENCES problems (id),
  status       TEXT    NOT NULL DEFAULT 'none' CHECK (status IN ('accepted', 'none')),
  first_ac_at  DATETIME,
  submit_count INTEGER NOT NULL DEFAULT 0,
  UNIQUE (user_id, problem_id)
);
CREATE INDEX IF NOT EXISTS idx_user_problem_status_user_id ON user_problem_status (user_id);
)SQL";

// 各表必须存在的列（结构兼容性检查）。缺列即视为旧数据库不兼容，明确报错。
const std::vector<std::pair<const char*, std::vector<const char*>>> kRequiredColumns = {
    {"users",
     {"id", "account", "nickname", "password_hash", "role", "reset_pwd_flag", "created_at"}},
    {"problems",
     {"id", "title", "description", "difficulty", "tags", "time_limit_ms",
      "memory_limit_kb", "visible", "created_at", "updated_at"}},
    {"testcases", {"id", "problem_id", "ord", "input", "output"}},
    {"submissions",
     {"id", "user_id", "problem_id", "language", "source_code", "status", "per_case",
      "compile_msg", "runtime_ms", "memory_kb", "created_at"}},
    {"user_problem_status",
     {"id", "user_id", "problem_id", "status", "first_ac_at", "submit_count"}},
};

}  // namespace

Database::~Database() {
  Close();
}

bool Database::Open(const std::string& path, int busy_timeout_ms, std::string* err) {
  Close();

  const std::filesystem::path fs_path(path);
  const std::filesystem::path parent = fs_path.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      if (err) {
        *err = "cannot create database directory '" + parent.string() + "': " + ec.message();
      }
      return false;
    }
  }

  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK) {
    const std::string msg = db_ ? sqlite3_errmsg(db_) : "unknown error";
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    if (err) {
      *err = "cannot open database file '" + path + "': " + msg;
    }
    return false;
  }
  path_ = path;

  std::string journal_mode;
  rc = sqlite3_exec(
      db_, "PRAGMA journal_mode=WAL;",
      [](void* out, int, char** argv, char**) -> int {
        if (argv && argv[0]) {
          *static_cast<std::string*>(out) = argv[0];
        }
        return 0;
      },
      &journal_mode, nullptr);
  if (rc != SQLITE_OK) {
    if (err) {
      *err = "cannot enable WAL on '" + path + "': " + sqlite3_errmsg(db_);
    }
    Close();
    return false;
  }
  if (journal_mode != "wal") {
    if (err) {
      *err = "WAL not enabled on '" + path + "' (journal_mode='" + journal_mode +
             "'); check write permission for the database file and its directory";
    }
    Close();
    return false;
  }

  char* fk_msg = nullptr;
  rc = sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, &fk_msg);
  if (rc != SQLITE_OK) {
    const std::string msg = fk_msg ? fk_msg : sqlite3_errmsg(db_);
    sqlite3_free(fk_msg);
    if (err) {
      *err = "cannot enable foreign key checks on '" + path + "': " + msg;
    }
    Close();
    return false;
  }

  sqlite3_busy_timeout(db_, busy_timeout_ms);
  sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
  return true;
}

bool Database::InitSchema(std::string* err) {
  if (!IsOpen()) {
    if (err) {
      *err = "database is not open";
    }
    return false;
  }

  // 先做只读结构检查：已存在的表若缺必要列，直接明确报错，不做任何 DDL、
  // 不删除重建。
  if (!VerifySchemaColumns(err)) {
    return false;
  }

  std::string rollback_err;
  if (Exec("BEGIN IMMEDIATE;", err) != SQLITE_OK) {
    return false;
  }
  // 多步建表放入单个事务，任一步失败整体回滚。
  if (Exec(kSchemaSql, err) != SQLITE_OK || Exec("PRAGMA user_version = 1;", err) != SQLITE_OK) {
    Exec("ROLLBACK;", &rollback_err);
    return false;
  }
  // DDL 后再次校验：所有表与必要列此时都应齐备。
  if (!VerifySchemaColumns(err)) {
    Exec("ROLLBACK;", &rollback_err);
    return false;
  }
  if (Exec("COMMIT;", err) != SQLITE_OK) {
    Exec("ROLLBACK;", &rollback_err);
    return false;
  }
  return true;
}

int Database::EnsureAdmin(const std::string& account, const std::string& nickname,
                          const std::string& plain_password, std::string* err) {
  if (!IsOpen()) {
    if (err) {
      *err = "database is not open";
    }
    return -1;
  }

  {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM users WHERE account = ?1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      if (err) {
        *err = std::string("cannot query admin account: ") + sqlite3_errmsg(db_);
      }
      return -1;
    }
    sqlite3_bind_text(stmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    int exists = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      exists = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (exists > 0) {
      // 已存在：不重置密码、不改角色、不改首次改密标记。
      return 0;
    }
  }

  if (plain_password.empty()) {
    if (err) {
      *err = "admin account '" + account + "' does not exist and no initial password was "
             "provided. Set OJ_ADMIN_PASSWORD (env) or pass --admin-password to seed it on "
             "first start; nothing existing was overwritten.";
    }
    return -1;
  }

  const std::string hash = HashPassword(plain_password);
  if (hash.empty()) {
    if (err) {
      *err = "failed to hash the admin password (argon2id)";
    }
    return -1;
  }

  const char* sql =
      "INSERT INTO users (account, nickname, password_hash, role, reset_pwd_flag) "
      "VALUES (?1, ?2, ?3, 'admin', 1);";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (err) {
      *err = std::string("cannot prepare admin insert: ") + sqlite3_errmsg(db_);
    }
    return -1;
  }
  sqlite3_bind_text(stmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, nickname.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, hash.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  const std::string db_msg = sqlite3_errmsg(db_);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    if (err) {
      *err = "cannot create admin '" + account + "': " + db_msg +
             ". The account or nickname may already belong to another user; "
             "no existing data was overwritten.";
    }
    return -1;
  }
  return 1;
}

void Database::Close() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
  path_.clear();
}

int Database::Exec(const char* sql, std::string* err) {
  if (!db_) {
    if (err) {
      *err = "database is not open";
    }
    return SQLITE_ERROR;
  }
  char* msg = nullptr;
  const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &msg);
  if (rc != SQLITE_OK) {
    const std::string text = msg ? msg : sqlite3_errmsg(db_);
    sqlite3_free(msg);
    if (err) {
      *err = "sqlite error (" + std::to_string(rc) + "): " + text;
    }
  }
  return rc;
}

bool Database::VerifySchemaColumns(std::string* err) {
  for (const auto& spec : kRequiredColumns) {
    const std::string& table = spec.first;

    // 表尚不存在：跳过（CREATE TABLE IF NOT EXISTS 会补建）。
    sqlite3_stmt* probe = nullptr;
    const char* exists_sql =
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ?1;";
    if (sqlite3_prepare_v2(db_, exists_sql, -1, &probe, nullptr) != SQLITE_OK) {
      if (err) {
        *err = "cannot inspect schema of table '" + table + "': " + sqlite3_errmsg(db_);
      }
      return false;
    }
    sqlite3_bind_text(probe, 1, table.c_str(), -1, SQLITE_TRANSIENT);
    const bool table_exists = sqlite3_step(probe) == SQLITE_ROW;
    sqlite3_finalize(probe);
    if (!table_exists) {
      continue;
    }

    const std::string pragma = "PRAGMA table_info(\"" + table + "\");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, pragma.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      if (err) {
        *err = "cannot inspect schema of table '" + table + "': " + sqlite3_errmsg(db_);
      }
      return false;
    }
    std::vector<std::string> actual;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      if (const char* col = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))) {
        actual.emplace_back(col);
      }
    }
    sqlite3_finalize(stmt);

    for (const char* need : spec.second) {
      if (std::find(actual.cbegin(), actual.cend(), need) == actual.cend()) {
        if (err) {
          *err = "existing table '" + table + "' is missing required column '" +
                 std::string(need) + "'. The database '" + path_ +
                 "' looks incompatible with this version; refusing to auto-migrate. "
                 "Please migrate it manually (keep existing data) or point --db at a "
                 "different path.";
        }
        return false;
      }
    }
  }
  return true;
}

}  // namespace oj