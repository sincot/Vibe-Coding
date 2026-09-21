// 数据库初始化集成测试。
//
// 使用 /tmp 下的隔离临时数据库，不触碰正式运行数据库（data/oj.db）。
// 覆盖 SPEC M0.3 的验证要求：建表 + 预置 admin、WAL / 外键、唯一性约束、
// 非法外键、admin 密码哈希、重复初始化幂等、持久化、失败路径等。
//
// 运行方式：ctest --test-dir build -R db_integration --output-on-failure
// 或直接执行 build/oj_db_test。

#include <sys/types.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "auth/password.h"
#include "db/database.h"
#include "db/schema.h"

namespace {

int g_failures = 0;

void check(bool cond, const std::string &msg) {
  if (cond) {
    std::cout << "  [PASS] " << msg << "\n";
  } else {
    std::cout << "  [FAIL] " << msg << "\n";
    ++g_failures;
  }
}

// 唯一临时目录，析构时自动删除。
class TempDir {
public:
  explicit TempDir(const std::string &label) {
    std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
      auto candidate =
          base / (label + "_" + std::to_string(::getpid()) + "_" +
                  std::to_string(i));
      std::error_code ec;
      std::filesystem::create_directories(candidate, ec);
      if (!ec) {
        path_ = candidate;
        return;
      }
    }
    path_.clear();
  }

  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }

  std::string db_path() const { return (path_ / "oj.db").string(); }
  std::filesystem::path dir() const { return path_; }

private:
  std::filesystem::path path_;
};

bool table_exists(oj::Database &db, const std::string &name) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare(
          "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", stmt,
          err)) {
    return false;
  }
  stmt.bind(1, name);
  return stmt.step() == SQLITE_ROW;
}

std::string query_text(oj::Database &db, const std::string &sql) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare(sql, stmt, err)) {
    return {};
  }
  if (stmt.step() != SQLITE_ROW) {
    return {};
  }
  return stmt.column_text(0);
}

int insert_user(oj::Database &db, const std::string &account,
                const std::string &nickname, const std::string &role) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare(
          "INSERT INTO users (account, nickname, password_hash, role) "
          "VALUES (?, ?, 'x', ?)",
          stmt, err)) {
    return SQLITE_ERROR;
  }
  stmt.bind(1, account);
  stmt.bind(2, nickname);
  stmt.bind(3, role);
  return stmt.step();
}

void test_first_init() {
  std::cout << "首次初始化：创建五张表并预置 admin\n";
  TempDir dir("first");
  std::string err;
  auto db = oj::Database::open(dir.db_path(), err);
  check(db != nullptr, "打开数据库成功");

  bool ok = oj::initialize_schema(*db, std::string("AdminSecret123!"), err);
  check(ok, "initialize_schema 成功");

  check(table_exists(*db, "users"), "存在 users 表");
  check(table_exists(*db, "problems"), "存在 problems 表");
  check(table_exists(*db, "testcases"), "存在 testcases 表");
  check(table_exists(*db, "submissions"), "存在 submissions 表");
  check(table_exists(*db, "user_problem_status"), "存在 user_problem_status 表");

  oj::Statement stmt;
  db->prepare("SELECT account, nickname, role, reset_pwd_flag FROM users "
              "WHERE account='admin'",
              stmt, err);
  bool has_admin = (stmt.step() == SQLITE_ROW);
  check(has_admin, "预置 admin 存在");
  if (has_admin) {
    check(stmt.column_text(0) == "admin", "admin 账号为 'admin'");
    check(stmt.column_text(1) == "admin", "admin 昵称为 'admin'");
    check(stmt.column_text(2) == "admin", "admin 角色为 'admin'");
    check(stmt.column_int(3) == 1, "admin reset_pwd_flag 为 1");
  }
}

void test_wal_and_fk() {
  std::cout << "WAL 与外键检查已启用\n";
  TempDir dir("wal");
  std::string err;
  auto db = oj::Database::open(dir.db_path(), err);
  check(db != nullptr, "打开数据库成功");
  check(oj::initialize_schema(*db, std::string("Pw!"), err), "初始化成功");

  check(query_text(*db, "PRAGMA journal_mode") == "wal", "journal_mode 为 wal");
  check(query_text(*db, "PRAGMA foreign_keys") == "1", "foreign_keys 为 ON");
}

void test_unique_constraints() {
  std::cout << "唯一性约束（昵称、账号、用户×题目）\n";
  TempDir dir("unique");
  std::string err;
  auto db = oj::Database::open(dir.db_path(), err);
  check(oj::initialize_schema(*db, std::string("Pw!"), err), "初始化成功");

  check(insert_user(*db, "1000000000", "alice", "user") == SQLITE_DONE,
        "插入用户 alice 成功");

  // 重复昵称 -> 违反 UNIQUE(nickname)
  check(insert_user(*db, "1000000001", "alice", "user") != SQLITE_DONE,
        "重复昵称被拒绝");

  // 重复账号 -> 违反 UNIQUE(account)
  check(insert_user(*db, "1000000000", "bob", "user") != SQLITE_DONE,
        "重复账号被拒绝");

  // 准备一道题目与 alice 的 id，用于构造合法的用户×题目记录。
  oj::Statement pstmt;
  db->prepare("INSERT INTO problems (title) VALUES ('P1')", pstmt, err);
  check(pstmt.step() == SQLITE_DONE, "插入题目 P1 成功");
  std::string user_id = query_text(*db, "SELECT id FROM users WHERE nickname='alice'");
  std::string problem_id = query_text(*db, "SELECT id FROM problems WHERE title='P1'");

  // 用户×题目组合唯一性
  std::string sql =
      "INSERT INTO user_problem_status (user_id, problem_id) VALUES (?, ?)";
  oj::Statement stmt;
  db->prepare(sql, stmt, err);
  stmt.bind(1, user_id);
  stmt.bind(2, problem_id);
  bool first = (stmt.step() == SQLITE_DONE);
  check(first, "插入 user_problem_status 成功");

  oj::Statement stmt2;
  db->prepare(sql, stmt2, err);
  stmt2.bind(1, user_id);
  stmt2.bind(2, problem_id);
  bool dup = (stmt2.step() == SQLITE_DONE);
  check(!dup, "重复 (user_id, problem_id) 被拒绝");
}

void test_foreign_key_rejection() {
  std::cout << "非法外键引用被拒绝\n";
  TempDir dir("fk");
  std::string err;
  auto db = oj::Database::open(dir.db_path(), err);
  check(oj::initialize_schema(*db, std::string("Pw!"), err), "初始化成功");

  // testcases 引用不存在的 problem_id -> 外键拒绝
  oj::Statement stmt;
  db->prepare("INSERT INTO testcases (problem_id, ord, input, output) "
              "VALUES (?, 0, '', '')",
              stmt, err);
  stmt.bind(1, 999);
  check(stmt.step() != SQLITE_DONE, "引用不存在的 problem_id 被拒绝");

  // submissions 引用不存在的 user_id -> 外键拒绝
  oj::Statement stmt2;
  db->prepare("INSERT INTO submissions (user_id, problem_id, language, "
              "source_code, status) VALUES (?, ?, 'cpp17', '', 'AC')",
              stmt2, err);
  stmt2.bind(1, 999);
  stmt2.bind(2, 1);
  check(stmt2.step() != SQLITE_DONE, "引用不存在的 user_id 被拒绝");
}

void test_admin_password_hash() {
  std::cout << "admin 密码哈希可验证\n";
  TempDir dir("pwhash");
  std::string err;
  auto db = oj::Database::open(dir.db_path(), err);
  check(oj::initialize_schema(*db, std::string("CorrectHorse!"), err),
        "初始化成功");

  std::string hash = query_text(
      *db, "SELECT password_hash FROM users WHERE account='admin'");
  check(!hash.empty(), "存在 password_hash");
  check(hash.find("$argon2id$") == 0, "哈希为 argon2id 编码格式");

  std::string verr;
  check(oj::auth::verify_password(hash, "CorrectHorse!", verr),
        "正确密码验证通过");
  check(!oj::auth::verify_password(hash, "WrongPassword", verr),
        "错误密码验证失败");
}

void test_reinit_idempotent() {
  std::cout << "重复初始化不重复创建/覆盖 admin\n";
  TempDir dir("reinit");
  std::string err;
  auto db = oj::Database::open(dir.db_path(), err);
  check(oj::initialize_schema(*db, std::string("FirstPass!"), err),
        "首次初始化成功");

  // 人为把 admin 的密码改成哨兵值并清除首次改密标记，模拟「已存在且被修改」状态。
  db->exec("UPDATE users SET password_hash='sentinel', reset_pwd_flag=0 "
           "WHERE account='admin'",
           err);

  // 用不同密码再次初始化。
  check(oj::initialize_schema(*db, std::string("SecondPass!"), err),
        "重复初始化成功");

  check(query_text(*db, "SELECT COUNT(*) FROM users WHERE account='admin'") ==
            "1",
        "admin 未重复创建");
  check(query_text(*db, "SELECT password_hash FROM users WHERE account='admin'") ==
            "sentinel",
        "密码未被覆盖");
  check(query_text(*db, "SELECT reset_pwd_flag FROM users WHERE account='admin'") ==
            "0",
        "首次改密标记未被重置");
}

void test_persistence() {
  std::cout << "关闭后重新打开数据仍然存在\n";
  TempDir dir("persist");
  std::string err;

  {
    auto db = oj::Database::open(dir.db_path(), err);
    check(db != nullptr, "首次打开成功");
    check(oj::initialize_schema(*db, std::string("Pw!"), err), "初始化成功");

    oj::Statement stmt;
    db->prepare("INSERT INTO problems (title, description) VALUES (?, ?)", stmt,
                err);
    stmt.bind(1, "A+B Problem");
    stmt.bind(2, "计算两数之和");
    check(stmt.step() == SQLITE_DONE, "写入一道题目");
    db->close();
  }

  {
    auto db = oj::Database::open(dir.db_path(), err);
    check(db != nullptr, "重新打开成功");
    check(query_text(*db, "SELECT title FROM problems WHERE title='A+B Problem'") ==
              "A+B Problem",
          "题目数据仍存在");
    check(query_text(*db, "SELECT COUNT(*) FROM users WHERE account='admin'") ==
              "1",
          "admin 仍存在");
  }
}

void test_schema_migration_old_db() {
  std::cout << "旧库迁移：补充 is_sample / seed_key 列且保留已有数据\n";
  TempDir dir("migrate");
  std::string err;
  auto db = oj::Database::open(dir.db_path(), err);
  check(db != nullptr, "打开数据库成功");

  // 模拟本阶段之前创建的旧库：problems/testcases 无 is_sample、seed_key 列。
  check(db->exec("CREATE TABLE problems ("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, "
                 "description TEXT NOT NULL DEFAULT '', difficulty TEXT NOT NULL "
                 "DEFAULT 'easy', tags TEXT NOT NULL DEFAULT '', time_limit_ms "
                 "INTEGER NOT NULL DEFAULT 2000, memory_limit_kb INTEGER NOT NULL "
                 "DEFAULT 65536, visible INTEGER NOT NULL DEFAULT 1, created_at "
                 "TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT "
                 "NULL DEFAULT (datetime('now')));",
                 err),
        "创建旧版 problems 表");
  check(db->exec("CREATE TABLE testcases ("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT, problem_id INTEGER NOT "
                 "NULL REFERENCES problems(id), ord INTEGER NOT NULL DEFAULT 0, "
                 "input TEXT NOT NULL DEFAULT '', output TEXT NOT NULL DEFAULT "
                 "'');",
                 err),
        "创建旧版 testcases 表");
  check(db->exec("INSERT INTO problems (title, description) VALUES "
                 "('Legacy', '旧题面');",
                 err),
        "写入旧数据");
  check(db->exec("INSERT INTO testcases (problem_id, ord, input, output) "
                 "VALUES (1, 0, 'old-in', 'old-out');",
                 err),
        "写入旧用例");

  check(oj::initialize_schema(*db, std::string("Pw!"), err), "初始化（迁移）成功");

  auto column_exists = [&](const std::string &table,
                           const std::string &column) -> bool {
    std::string e;
    oj::Statement stmt;
    db->prepare("PRAGMA table_info(" + table + ")", stmt, e);
    while (stmt.step() == SQLITE_ROW) {
      if (stmt.column_text(1) == column) {
        return true;
      }
    }
    return false;
  };
  check(column_exists("testcases", "is_sample"), "testcases 已补充 is_sample 列");
  check(column_exists("problems", "seed_key"), "problems 已补充 seed_key 列");

  check(query_text(*db, "SELECT title FROM problems WHERE title='Legacy'") ==
            "Legacy",
        "旧题目数据保留");
  check(query_text(*db, "SELECT input FROM testcases WHERE ord=0") == "old-in",
        "旧用例数据保留");
  check(query_text(*db, "SELECT is_sample FROM testcases WHERE ord=0") == "0",
        "旧用例 is_sample 默认为 0（隐藏）");
}

void test_missing_admin_password_fails() {
  std::cout << "无 admin 且缺少初始密码时初始化失败\n";
  TempDir dir("nopw");
  std::string err;
  auto db = oj::Database::open(dir.db_path(), err);
  check(db != nullptr, "打开数据库成功");

  bool ok = oj::initialize_schema(*db, std::nullopt, err);
  check(!ok, "未提供密码时初始化失败");
  check(err.find("OJ_ADMIN_PASSWORD") != std::string::npos,
        "错误信息提示 OJ_ADMIN_PASSWORD");
  check(!table_exists(*db, "users"), "失败后未留下部分初始化（无 users 表）");
}

void test_unwritable_path_fails() {
  std::cout << "数据库路径不可写时打开失败\n";
  TempDir dir("unwritable");
  std::error_code ec;
  // 用一个普通文件充当「目录」，使其父目录无法创建。
  std::filesystem::path file = dir.dir() / "plain_file";
  std::ofstream(file.string()).close();

  std::string err;
  auto db = oj::Database::open((file / "sub" / "oj.db").string(), err);
  check(db == nullptr, "不可写路径打开失败");
  check(!err.empty(), "失败时给出错误信息");
}

} // namespace

int main() {
  test_first_init();
  test_wal_and_fk();
  test_unique_constraints();
  test_foreign_key_rejection();
  test_admin_password_hash();
  test_reinit_idempotent();
  test_persistence();
  test_schema_migration_old_db();
  test_missing_admin_password_fails();
  test_unwritable_path_fails();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
