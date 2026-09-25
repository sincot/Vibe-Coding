#include "db/seed.h"

#include <sqlite3.h>

#include <mutex>
#include <vector>

#include "db/database.h"
#include "db/seed_data.h"

namespace oj {

namespace {

// 查询指定 seed_key 的题目是否已存在。
bool seed_exists(Database &db, const std::string &seed_key, bool &exists,
                 std::string &error) {
  Statement stmt;
  if (!db.prepare("SELECT 1 FROM problems WHERE seed_key = ?", stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, seed_key)) {
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

// 在已开启的事务内插入一道种子题及其全部用例。
bool insert_seed(Database &db, const SeedProblem &seed, std::string &error) {
  std::int64_t problem_id = 0;
  {
    Statement stmt;
    if (!db.prepare(
            "INSERT INTO problems (title, description, difficulty, tags, "
            "time_limit_ms, memory_limit_kb, visible, seed_key) VALUES (?, ?, "
            "?, ?, ?, ?, ?, ?) RETURNING id",
            stmt, error)) {
      return false;
    }
    if (!stmt.bind(1, seed.title) || !stmt.bind(2, seed.description) ||
        !stmt.bind(3, seed.difficulty) || !stmt.bind(4, seed.tags) ||
        !stmt.bind(5, seed.time_limit_ms) ||
        !stmt.bind(6, seed.memory_limit_kb) || !stmt.bind(7, seed.visible) ||
        !stmt.bind(8, seed.seed_key)) {
      error = stmt.errmsg();
      return false;
    }
    int rc = stmt.step();
    if (rc != SQLITE_ROW) {
      error = stmt.errmsg();
      return false;
    }
    problem_id = stmt.column_int64(0);
  }

  for (std::size_t i = 0; i < seed.cases.size(); ++i) {
    const SeedTestcase &tc = seed.cases[i];
    Statement stmt;
    if (!db.prepare(
            "INSERT INTO testcases (problem_id, ord, input, output, is_sample) "
            "VALUES (?, ?, ?, ?, ?)",
            stmt, error)) {
      return false;
    }
    if (!stmt.bind(1, static_cast<sqlite3_int64>(problem_id)) ||
        !stmt.bind(2, static_cast<int>(i)) || !stmt.bind(3, tc.input) ||
        !stmt.bind(4, tc.output) || !stmt.bind(5, tc.is_sample ? 1 : 0)) {
      error = stmt.errmsg();
      return false;
    }
    int rc = stmt.step();
    if (rc != SQLITE_DONE) {
      error = stmt.errmsg();
      return false;
    }
  }
  return true;
}

} // namespace

bool import_seed_problems(Database &db, int &created, std::string &error) {
  created = 0;
  for (const SeedProblem &seed : builtin_seeds()) {
    bool exists = false;
    if (!seed_exists(db, seed.seed_key, exists, error)) {
      return false;
    }
    if (exists) {
      continue; // 已导入：整题跳过，保留已有（可能已被修改）数据
    }

    // 单题一个事务：题目与用例要么全部写入，要么全部回滚，不产生半道题。
    std::lock_guard<std::mutex> lock(db.transaction_mutex());
    if (!db.begin(error)) {
      return false;
    }
    if (!insert_seed(db, seed, error)) {
      std::string ignored;
      db.rollback(ignored);
      return false;
    }
    if (!db.commit(error)) {
      std::string ignored;
      db.rollback(ignored);
      return false;
    }
    ++created;
  }
  return true;
}

} // namespace oj
