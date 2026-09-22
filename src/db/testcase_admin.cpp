#include "db/testcase_admin.h"

#include <mutex>

#include <sqlite3.h>

#include "db/database.h"

namespace oj {

namespace {

void rollback_quiet(Database &db) {
  std::string ignored;
  db.rollback(ignored);
}

// 读取一条隐藏用例当前值。found=false 表示不存在、不属于该题或属于公开样例。
bool find_hidden(Database &db, std::int64_t problem_id,
                 std::int64_t testcase_id, bool &found, int &ord,
                 std::string &input, std::string &output, std::string &error) {
  Statement stmt;
  if (!db.prepare("SELECT ord, input, output FROM testcases WHERE problem_id = ? "
                  "AND id = ? AND is_sample = 0",
                  stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, static_cast<sqlite3_int64>(problem_id)) ||
      !stmt.bind(2, static_cast<sqlite3_int64>(testcase_id))) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    ord = stmt.column_int(0);
    input = stmt.column_text(1);
    output = stmt.column_text(2);
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

TestcaseAdminStore::CreateStatus
TestcaseAdminStore::create(std::int64_t problem_id,
                           const problem::TestcaseData &data,
                           std::int64_t &out_id, int &out_ord,
                           std::string &error) {
  std::lock_guard<std::mutex> lock(db_.transaction_mutex());
  if (!db_.begin(error)) {
    return CreateStatus::Error;
  }

  // 题目存在性在事务内校验（与题目删除共用同一事务互斥锁，避免并发删题绕过）。
  bool found = false;
  ProblemRecord problem;
  if (!problems_.find_by_id(problem_id, found, problem, error)) {
    rollback_quiet(db_);
    return CreateStatus::Error;
  }
  if (!found) {
    rollback_quiet(db_);
    return CreateStatus::ProblemNotFound;
  }

  // 未显式指定 ord 时追加到末尾：max(ord) + 1（含公开样例），无用例时为 0。
  int ord = 0;
  if (data.ord.has_value()) {
    ord = *data.ord;
  } else {
    Statement stmt;
    if (!db_.prepare("SELECT COALESCE(MAX(ord), -1) FROM testcases WHERE "
                     "problem_id = ?",
                     stmt, error)) {
      rollback_quiet(db_);
      return CreateStatus::Error;
    }
    if (!stmt.bind(1, static_cast<sqlite3_int64>(problem_id))) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return CreateStatus::Error;
    }
    if (stmt.step() != SQLITE_ROW) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return CreateStatus::Error;
    }
    ord = stmt.column_int(0) + 1;
    // 缺省追加必须仍落在声明的 ord 范围内；达到上限时拒绝，绝不写入越界值。
    if (ord > problem::kMaxTestcaseOrd) {
      error = "自动分配 ord 已达上限";
      rollback_quiet(db_);
      return CreateStatus::OrdExhausted;
    }
  }

  std::int64_t new_id = 0;
  {
    Statement stmt;
    if (!db_.prepare(
            "INSERT INTO testcases (problem_id, ord, input, output, is_sample) "
            "VALUES (?, ?, ?, ?, 0) RETURNING id",
            stmt, error)) {
      rollback_quiet(db_);
      return CreateStatus::Error;
    }
    if (!stmt.bind(1, static_cast<sqlite3_int64>(problem_id)) ||
        !stmt.bind(2, ord) || !stmt.bind(3, data.input) ||
        !stmt.bind(4, data.output)) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return CreateStatus::Error;
    }
    if (stmt.step() != SQLITE_ROW) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return CreateStatus::Error;
    }
    new_id = stmt.column_int64(0);
  }

  if (!db_.commit(error)) {
    rollback_quiet(db_);
    return CreateStatus::Error;
  }
  out_id = new_id;
  out_ord = ord;
  return CreateStatus::Created;
}

TestcaseAdminStore::UpdateStatus
TestcaseAdminStore::update(std::int64_t problem_id, std::int64_t testcase_id,
                           const problem::TestcasePatch &patch, int &out_ord,
                           std::string &error) {
  std::lock_guard<std::mutex> lock(db_.transaction_mutex());
  if (!db_.begin(error)) {
    return UpdateStatus::Error;
  }

  bool found = false;
  int ord = 0;
  std::string input;
  std::string output;
  if (!find_hidden(db_, problem_id, testcase_id, found, ord, input, output,
                   error)) {
    rollback_quiet(db_);
    return UpdateStatus::Error;
  }
  if (!found) {
    rollback_quiet(db_);
    return UpdateStatus::NotFound;
  }

  const std::string new_input = patch.input.value_or(input);
  const std::string new_output = patch.output.value_or(output);
  const int new_ord = patch.ord.value_or(ord);

  {
    Statement stmt;
    if (!db_.prepare("UPDATE testcases SET input = ?, output = ?, ord = ? WHERE "
                     "problem_id = ? AND id = ? AND is_sample = 0",
                     stmt, error)) {
      rollback_quiet(db_);
      return UpdateStatus::Error;
    }
    if (!stmt.bind(1, new_input) || !stmt.bind(2, new_output) ||
        !stmt.bind(3, new_ord) ||
        !stmt.bind(4, static_cast<sqlite3_int64>(problem_id)) ||
        !stmt.bind(5, static_cast<sqlite3_int64>(testcase_id))) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return UpdateStatus::Error;
    }
    if (stmt.step() != SQLITE_DONE) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return UpdateStatus::Error;
    }
  }

  if (!db_.commit(error)) {
    rollback_quiet(db_);
    return UpdateStatus::Error;
  }
  out_ord = new_ord;
  return UpdateStatus::Updated;
}

TestcaseAdminStore::DeleteStatus
TestcaseAdminStore::remove(std::int64_t problem_id, std::int64_t testcase_id,
                           std::string &error) {
  std::lock_guard<std::mutex> lock(db_.transaction_mutex());
  if (!db_.begin(error)) {
    return DeleteStatus::Error;
  }

  bool found = false;
  int ord = 0;
  std::string input;
  std::string output;
  if (!find_hidden(db_, problem_id, testcase_id, found, ord, input, output,
                   error)) {
    rollback_quiet(db_);
    return DeleteStatus::Error;
  }
  if (!found) {
    rollback_quiet(db_);
    return DeleteStatus::NotFound;
  }

  {
    Statement stmt;
    if (!db_.prepare("DELETE FROM testcases WHERE problem_id = ? AND id = ? AND "
                     "is_sample = 0",
                     stmt, error)) {
      rollback_quiet(db_);
      return DeleteStatus::Error;
    }
    if (!stmt.bind(1, static_cast<sqlite3_int64>(problem_id)) ||
        !stmt.bind(2, static_cast<sqlite3_int64>(testcase_id))) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return DeleteStatus::Error;
    }
    if (stmt.step() != SQLITE_DONE) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return DeleteStatus::Error;
    }
  }

  if (!db_.commit(error)) {
    rollback_quiet(db_);
    return DeleteStatus::Error;
  }
  return DeleteStatus::Deleted;
}

} // namespace oj
