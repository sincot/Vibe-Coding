#include "db/schema.h"

#include <functional>

#include "auth/password.h"
#include "db/database.h"

namespace oj {

namespace {

// 业务表建表脚本（见 SPEC 3.2）。
//
// M3.7 起在原五张业务表之外新增独立的「在途任务」表 in_flight_tasks：提交被
// 接收后、最终结算前保存可恢复的任务信息；在途记录不参与 submit_count / AC /
// 排行榜统计，也不改变 submissions.status 的 CHECK 约束（见 schema 下方说明）。
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
    //
    // seed_key：内置种子题的稳定标识（M1.4）。普通题目为 NULL；有值时在唯一索引
    // 约束下保证同一道种子题只被导入一次，重复导入时整题跳过，不覆盖已修改的题目。
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
      seed_key        TEXT,
      created_at      TEXT NOT NULL DEFAULT (datetime('now')),
      updated_at      TEXT NOT NULL DEFAULT (datetime('now'))
    ))sql",

    // 测试用例（公开样例与隐藏用例共用一张表，用 is_sample 显式区分）
    //
    // is_sample=1 表示公开样例，随题面下发；is_sample=0 表示隐藏用例，仅判题读取。
    // 不使用「前 N 个默认公开」等隐含规则，避免顺序变动导致样例/隐藏错位。
    R"sql(
    CREATE TABLE IF NOT EXISTS testcases (
      id          INTEGER PRIMARY KEY AUTOINCREMENT,
      problem_id  INTEGER NOT NULL REFERENCES problems(id),
      ord         INTEGER NOT NULL DEFAULT 0,
      input       TEXT NOT NULL DEFAULT '',
      output      TEXT NOT NULL DEFAULT '',
      is_sample   INTEGER NOT NULL DEFAULT 0
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

    // 在途任务（M3.7 崩溃恢复与在途任务持久化）
    //
    // 采用独立表而非扩展 submissions：在途记录在最终结算前不计入 submit_count、
    // AC 状态、通过人数与排行榜，避免崩溃导致虚增计数或假 AC；既有 submissions
    // 读取/统计接口无需过滤在途状态即可保持兼容。submissions.status 的 CHECK
    // 约束也不受影响。
    //
    // 状态机：
    //   pending     —— 已持久化的可恢复在途任务（尚未被恢复认领）
    //   claimed     —— 启动恢复已认领并重新入队；进程再次崩溃时下次启动重置回 pending
    //   interrupted —— 确认无法判题（如题目不存在），保留任务信息但不再恢复
    // 任务完成后在结算事务内删除该行，配合 task_id UNIQUE 保证同一任务只结算一次。
    R"sql(
    CREATE TABLE IF NOT EXISTS in_flight_tasks (
      id           INTEGER PRIMARY KEY AUTOINCREMENT,
      task_id      TEXT NOT NULL UNIQUE,
      user_id      INTEGER NOT NULL REFERENCES users(id),
      problem_id   INTEGER NOT NULL REFERENCES problems(id),
      language     TEXT NOT NULL,
      source_code  TEXT NOT NULL,
      submitted_at TEXT NOT NULL,
      state        TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending', 'claimed', 'interrupted')),
      owner        TEXT NOT NULL DEFAULT '',
      claimed_at   TEXT,
      reason       TEXT NOT NULL DEFAULT ''
    ))sql",
};

// 按已有查询需求配置的索引（IF NOT EXISTS，重复初始化幂等）。
const char *const kIndexStatements[] = {
    // 题目列表按可见性筛选
    "CREATE INDEX IF NOT EXISTS idx_problems_visible ON problems(visible);",
    // 种子题幂等标识：唯一索引允许多个 NULL，故普通题目（seed_key 为 NULL）不受影响
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_problems_seed_key ON problems(seed_key);",
    // 用例按题目归属读取（leftmost 为 problem_id，配合 ord 排序）
    "CREATE INDEX IF NOT EXISTS idx_testcases_problem_ord ON testcases(problem_id, ord);",
    // 提交历史：按用户 / 按题目查询
    "CREATE INDEX IF NOT EXISTS idx_submissions_user ON submissions(user_id);",
    // 提交历史（M4.4）：按用户分页、最新优先（created_at DESC, id DESC）稳定排序
    "CREATE INDEX IF NOT EXISTS idx_submissions_user_created ON submissions(user_id, created_at DESC, id DESC);",
    "CREATE INDEX IF NOT EXISTS idx_submissions_problem ON submissions(problem_id);",
    // 题目「通过人数」统计（按 problem_id 聚合）
    "CREATE INDEX IF NOT EXISTS idx_user_problem_status_problem ON user_problem_status(problem_id);",
    // 在途任务：启动恢复按状态分批扫描；删题保护按题目统计未结算记录
    "CREATE INDEX IF NOT EXISTS idx_in_flight_state ON in_flight_tasks(state);",
    "CREATE INDEX IF NOT EXISTS idx_in_flight_problem ON in_flight_tasks(problem_id);",
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

// 判断表中是否已存在指定列（table/column 仅由本文件以字面量传入）。
bool column_exists(Database &db, const char *table, const char *column,
                   bool &exists, std::string &error) {
  std::string sql = std::string("PRAGMA table_info(") + table + ")";
  Statement stmt;
  if (!db.prepare(sql, stmt, error)) {
    return false;
  }
  exists = false;
  while (true) {
    int rc = stmt.step();
    if (rc == SQLITE_ROW) {
      if (stmt.column_text(1) == column) {
        exists = true;
        return true;
      }
      continue;
    }
    if (rc == SQLITE_DONE) {
      return true;
    }
    error = stmt.errmsg();
    return false;
  }
}

// 若表中缺少指定列则用 ALTER TABLE 补充，用于兼容本阶段之前创建的旧库。
// table/column/definition 仅由本文件以字面量传入，不存在注入风险。
bool ensure_column(Database &db, const char *table, const char *column,
                   const char *definition, std::string &error) {
  bool exists = false;
  if (!column_exists(db, table, column, exists, error)) {
    return false;
  }
  if (exists) {
    return true;
  }
  std::string sql = std::string("ALTER TABLE ") + table + " ADD COLUMN " +
                    column + " " + definition;
  return db.exec(sql, error);
}

// 执行建表、列迁移与建索引（不含事务控制，由调用方包裹在事务中）。
// 重复执行幂等：表/索引使用 IF NOT EXISTS，新增列先检测存在性再补充。
bool apply_schema(Database &db, std::string &error) {
  for (const char *sql : kSchemaStatements) {
    if (!db.exec(sql, error)) {
      return false;
    }
  }
  // 迁移旧库：补充后续阶段新增的列（新库在建表时已包含，此处为空操作）。
  if (!ensure_column(db, "testcases", "is_sample", "INTEGER NOT NULL DEFAULT 0",
                     error)) {
    return false;
  }
  if (!ensure_column(db, "problems", "seed_key", "TEXT", error)) {
    return false;
  }
  for (const char *sql : kIndexStatements) {
    if (!db.exec(sql, error)) {
      return false;
    }
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

bool ensure_schema(Database &db, std::string &error) {
  return run_in_transaction(
      db, [&](std::string &err) -> bool { return apply_schema(db, err); },
      error);
}

bool initialize_schema(Database &db,
                       const std::optional<std::string> &admin_password,
                       std::string &error) {
  return run_in_transaction(
      db,
      [&](std::string &err) -> bool {
        if (!apply_schema(db, err)) {
          return false;
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
