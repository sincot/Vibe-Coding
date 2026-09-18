#include "db/schema.h"

#include <functional>

#include "auth/password.h"
#include "db/database.h"

namespace oj {

namespace {

// 五张业务表的建表脚本（见 SPEC 3.2）。
//
// 说明：
//  - 使用 IF NOT EXISTS，保证重复初始化不清空、不重建已有数据。
//  - account 不设「10 位数字」CHECK 约束，以免阻止预置 admin 账号创建；
//    普通用户的 10 位数字账号约束在注册阶段（M1.1）于应用层保证。
//  - 外键未设置 ON DELETE CASCADE：破坏性的级联删除规则不在本阶段擅自确定，
//    留待 M2.1 明确删题/删用例的关联数据处理方式后再定。
const char *const kSchemaStatements[] = {
    // 用户
    R"sql(
    CREATE TABLE IF NOT EXISTS users (
      id              INTEGER PRIMARY KEY AUTOINCREMENT,
      account         TEXT NOT NULL UNIQUE,
      nickname        TEXT NOT NULL UNIQUE,
      password_hash   TEXT NOT NULL,
      role            TEXT NOT NULL DEFAULT 'user' CHECK (role IN ('admin', 'user')),
      reset_pwd_flag  INTEGER NOT NULL DEFAULT 0,
      created_at      TEXT NOT NULL DEFAULT (datetime('now'))
    ))sql",

    // 题目
    R"sql(
    CREATE TABLE IF NOT EXISTS problems (
      id              INTEGER PRIMARY KEY AUTOINCREMENT,
      title           TEXT NOT NULL,
      description     TEXT NOT NULL DEFAULT '',
      difficulty      TEXT NOT NULL DEFAULT 'easy' CHECK (difficulty IN ('easy', 'medium', 'hard')),
      tags            TEXT NOT NULL DEFAULT '',
      time_limit_ms   INTEGER NOT NULL DEFAULT 2000,
      memory_limit_kb INTEGER NOT NULL DEFAULT 65536,
      visible         INTEGER NOT NULL DEFAULT 1,
      created_at      TEXT NOT NULL DEFAULT (datetime('now')),
      updated_at      TEXT NOT NULL DEFAULT (datetime('now'))
    ))sql",

    // 测试用例（公开样例与隐藏用例共用，样例/隐藏的区分方式待后续阶段确认）
    R"sql(
    CREATE TABLE IF NOT EXISTS testcases (
      id          INTEGER PRIMARY KEY AUTOINCREMENT,
      problem_id  INTEGER NOT NULL REFERENCES problems(id),
      ord         INTEGER NOT NULL DEFAULT 0,
      input       TEXT NOT NULL DEFAULT '',
      output      TEXT NOT NULL DEFAULT ''
    ))sql",

    // 提交记录
    R"sql(
    CREATE TABLE IF NOT EXISTS submissions (
      id           INTEGER PRIMARY KEY AUTOINCREMENT,
      user_id      INTEGER NOT NULL REFERENCES users(id),
      problem_id   INTEGER NOT NULL REFERENCES problems(id),
      language     TEXT NOT NULL,
      source_code  TEXT NOT NULL,
      status       TEXT NOT NULL CHECK (status IN ('AC', 'WA', 'CE', 'TLE', 'RE', 'MLE', 'SYSERR')),
      per_case     TEXT NOT NULL DEFAULT '',
      compile_msg  TEXT NOT NULL DEFAULT '',
      runtime_ms   INTEGER NOT NULL DEFAULT 0,
      memory_kb    INTEGER NOT NULL DEFAULT 0,
      created_at   TEXT NOT NULL DEFAULT (datetime('now'))
    ))sql",

    // 用户 × 题目 状态（供题目列表标记与排行榜统计）
    R"sql(
    CREATE TABLE IF NOT EXISTS user_problem_status (
      id           INTEGER PRIMARY KEY AUTOINCREMENT,
      user_id      INTEGER NOT NULL REFERENCES users(id),
      problem_id   INTEGER NOT NULL REFERENCES problems(id),
      status       TEXT NOT NULL DEFAULT 'none' CHECK (status IN ('accepted', 'none')),
      first_ac_at  TEXT,
      submit_count INTEGER NOT NULL DEFAULT 0,
      UNIQUE(user_id, problem_id)
    ))sql",
};

// 按已有查询需求配置的索引（IF NOT EXISTS，重复初始化幂等）。
const char *const kIndexStatements[] = {
    // 题目列表按可见性筛选
    "CREATE INDEX IF NOT EXISTS idx_problems_visible ON problems(visible);",
    // 用例按题目归属读取（leftmost 为 problem_id，配合 ord 排序）
    "CREATE INDEX IF NOT EXISTS idx_testcases_problem_ord ON testcases(problem_id, ord);",
    // 提交历史：按用户 / 按题目查询
    "CREATE INDEX IF NOT EXISTS idx_submissions_user ON submissions(user_id);",
    "CREATE INDEX IF NOT EXISTS idx_submissions_problem ON submissions(problem_id);",
    // 题目「通过人数」统计（按 problem_id 聚合）
    "CREATE INDEX IF NOT EXISTS idx_user_problem_status_problem ON user_problem_status(problem_id);",
};

// 在事务内执行 body；失败自动回滚并保留原始错误。
bool run_in_transaction(Database &db,
                        const std::function<bool(std::string &)> &body,
                        std::string &error) {
  if (!db.begin(error)) {
    return false;
  }
  if (!body(error)) {
    std::string ignored;
    db.rollback(ignored);
    return false;
  }
  if (!db.commit(error)) {
    std::string ignored;
    db.rollback(ignored);
    return false;
  }
  return true;
}

// 判断 admin 是否已存在（account = 'admin'）。
bool admin_exists(Database &db, bool &exists, std::string &error) {
  Statement stmt;
  if (!db.prepare("SELECT 1 FROM users WHERE account = ?", stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, "admin")) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    exists = true;
    return true;
  }
  if (rc == SQLITE_DONE) {
    exists = false;
    return true;
  }
  error = stmt.errmsg();
  return false;
}

// 仅在 admin 不存在时创建 admin（幂等）。密码已由调用方哈希好。
bool insert_admin_if_absent(Database &db, const std::string &hash,
                            std::string &error) {
  Statement stmt;
  if (!db.prepare(
          "INSERT INTO users (account, nickname, password_hash, role, "
          "reset_pwd_flag) "
          "SELECT 'admin', 'admin', ?, 'admin', 1 "
          "WHERE NOT EXISTS (SELECT 1 FROM users WHERE account = 'admin')",
          stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, hash)) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc != SQLITE_DONE) {
    error = stmt.errmsg();
    return false;
  }
  return true;
}

} // namespace

bool initialize_schema(Database &db,
                       const std::optional<std::string> &admin_password,
                       std::string &error) {
  return run_in_transaction(
      db,
      [&](std::string &err) -> bool {
        for (const char *sql : kSchemaStatements) {
          if (!db.exec(sql, err)) {
            return false;
          }
        }
        for (const char *sql : kIndexStatements) {
          if (!db.exec(sql, err)) {
            return false;
          }
        }

        bool exists = false;
        if (!admin_exists(db, exists, err)) {
          return false;
        }

        if (!exists) {
          if (!admin_password.has_value() || admin_password->empty()) {
            err = "数据库首次初始化需要设置初始管理员密码：请通过环境变量 "
                  "OJ_ADMIN_PASSWORD 提供（已有 admin 时无需设置）";
            return false;
          }
          std::string hash;
          if (!auth::hash_password(*admin_password, hash, err)) {
            return false;
          }
          if (!insert_admin_if_absent(db, hash, err)) {
            return false;
          }
        }
        return true;
      },
      error);
}

} // namespace oj
