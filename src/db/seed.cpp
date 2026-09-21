#include "db/seed.h"

#include <sqlite3.h>

#include <mutex>
#include <vector>

#include "db/database.h"

namespace oj {

namespace {

// 一条种子用例。is_sample=true 为公开样例，false 为隐藏用例。
struct SeedTestcase {
  const char *input;
  const char *output;
  bool is_sample;
};

// 一道种子题的定义。seed_key 为稳定标识，用于幂等导入。
struct SeedProblem {
  const char *seed_key;
  const char *title;
  const char *description;
  const char *difficulty;
  const char *tags;
  int time_limit_ms;
  int memory_limit_kb;
  int visible;
  std::vector<SeedTestcase> cases;
};

// 内置种子题（标准 ACM 输入输出模式）。样例内容与隐藏内容刻意不同，便于验证
// 隐藏用例不会随题面下发。
std::vector<SeedProblem> builtin_seeds() {
  std::vector<SeedProblem> seeds;

  seeds.push_back(SeedProblem{
      "a-plus-b",
      "A+B Problem",
      "给定两个整数 a 和 b，输出它们的和。\n"
      "\n"
      "输入格式：\n"
      "一行两个整数 a 与 b，以空格分隔。\n"
      "\n"
      "输出格式：\n"
      "输出一个整数，表示 a + b 的值。\n",
      "easy",
      "入门,数学",
      2000,
      65536,
      1,
      {
          {"1 2\n", "3\n", true},
          {"100 -50\n", "50\n", true},
          {"111111111 222222222\n", "333333333\n", false},
          {"-444444444 -555555555\n", "-999999999\n", false},
          {"123456789 987654321\n", "1111111110\n", false},
      }});

  seeds.push_back(SeedProblem{
      "sum-of-integers",
      "整数求和",
      "给定 n 个整数，求它们的和。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 1000）。\n"
      "第二行 n 个整数，以空格分隔。\n"
      "\n"
      "输出格式：\n"
      "输出一个整数，表示这 n 个整数的和。\n",
      "easy",
      "数组,入门",
      2000,
      65536,
      1,
      {
          {"5\n1 2 3 4 5\n", "15\n", true},
          {"3\n-1 -2 -3\n", "-6\n", true},
          {"1\n987654321\n", "987654321\n", false},
          {"4\n111111111 222222222 333333333 444444444\n", "1111111110\n",
           false},
          {"6\n-111111111 -222222222 333333333 444444444 -555555555 "
           "666666666\n",
           "555555555\n", false},
      }});

  seeds.push_back(SeedProblem{
      "max-value",
      "求最大值",
      "给定 n 个整数，输出其中的最大值。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 1000）。\n"
      "第二行 n 个整数，以空格分隔。\n"
      "\n"
      "输出格式：\n"
      "输出一个整数，表示这 n 个整数的最大值。\n",
      "easy",
      "数组,入门",
      2000,
      65536,
      1,
      {
          {"5\n3 1 4 1 5\n", "5\n", true},
          {"3\n-7 -2 -9\n", "-2\n", true},
          {"1\n876543210\n", "876543210\n", false},
          {"5\n-987654321 -123456789 -456789123 -321654987 -147258369\n",
           "-123456789\n", false},
          {"4\n7654321 7654321 7654321 7654321\n", "7654321\n", false},
      }});

  return seeds;
}

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
