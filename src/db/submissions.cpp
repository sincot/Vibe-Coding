#include "db/submissions.h"

#include <sqlite3.h>

#include "db/database.h"

namespace oj {

namespace {

// 统一的列读取顺序，与 find_by_id 的 SELECT 列序一致。
void load_submission(Statement &stmt, SubmissionRecord &out) {
  out.id = stmt.column_int64(0);
  out.user_id = stmt.column_int64(1);
  out.problem_id = stmt.column_int64(2);
  out.language = stmt.column_text(3);
  out.source_code = stmt.column_text(4);
  out.status = stmt.column_text(5);
  out.per_case = stmt.column_text(6);
  out.compile_msg = stmt.column_text(7);
  out.runtime_ms = stmt.column_int64(8);
  out.memory_kb = stmt.column_int64(9);
  out.created_at = stmt.column_text(10);
}

} // namespace

bool SubmissionStore::insert(const SubmissionRecord &record,
                             std::int64_t &out_id, std::string &error) {
  Statement stmt;
  if (!db_.prepare(
          "INSERT INTO submissions (user_id, problem_id, language, "
          "source_code, status, per_case, compile_msg, runtime_ms, memory_kb, "
          "created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING id",
          stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, static_cast<sqlite3_int64>(record.user_id)) ||
      !stmt.bind(2, static_cast<sqlite3_int64>(record.problem_id)) ||
      !stmt.bind(3, record.language) || !stmt.bind(4, record.source_code) ||
      !stmt.bind(5, record.status) || !stmt.bind(6, record.per_case) ||
      !stmt.bind(7, record.compile_msg) ||
      !stmt.bind(8, static_cast<sqlite3_int64>(record.runtime_ms)) ||
      !stmt.bind(9, static_cast<sqlite3_int64>(record.memory_kb)) ||
      !stmt.bind(10, record.created_at)) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    out_id = stmt.column_int64(0);
    return true;
  }
  error = stmt.errmsg();
  return false;
}

bool SubmissionStore::find_by_id(std::int64_t id, bool &found,
                                 SubmissionRecord &out, std::string &error) {
  Statement stmt;
  if (!db_.prepare(
          "SELECT id, user_id, problem_id, language, source_code, status, "
          "per_case, compile_msg, runtime_ms, memory_kb, created_at FROM "
          "submissions WHERE id = ?",
          stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, static_cast<sqlite3_int64>(id))) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    load_submission(stmt, out);
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

bool SubmissionStore::update(const SubmissionRecord &record,
                             std::string &error) {
  Statement stmt;
  if (!db_.prepare(
          "UPDATE submissions SET status = ?, per_case = ?, compile_msg = ?, "
          "runtime_ms = ?, memory_kb = ? WHERE id = ?",
          stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, record.status) || !stmt.bind(2, record.per_case) ||
      !stmt.bind(3, record.compile_msg) ||
      !stmt.bind(4, static_cast<sqlite3_int64>(record.runtime_ms)) ||
      !stmt.bind(5, static_cast<sqlite3_int64>(record.memory_kb)) ||
      !stmt.bind(6, static_cast<sqlite3_int64>(record.id))) {
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

bool UserProblemStatusStore::find(std::int64_t user_id,
                                  std::int64_t problem_id, bool &found,
                                  UserProblemStatusRecord &out,
                                  std::string &error) {
  Statement stmt;
  if (!db_.prepare(
          "SELECT status, first_ac_at, submit_count FROM user_problem_status "
          "WHERE user_id = ? AND problem_id = ?",
          stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, static_cast<sqlite3_int64>(user_id)) ||
      !stmt.bind(2, static_cast<sqlite3_int64>(problem_id))) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    out.accepted = stmt.column_text(0) == "accepted";
    out.has_first_ac_at = !stmt.column_is_null(1);
    out.first_ac_at = out.has_first_ac_at ? stmt.column_text(1) : "";
    out.submit_count = stmt.column_int(2);
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

bool UserProblemStatusStore::upsert(std::int64_t user_id,
                                    std::int64_t problem_id, bool accepted,
                                    bool has_first_ac_at,
                                    const std::string &first_ac_at,
                                    int submit_count, std::string &error) {
  Statement stmt;
  if (!db_.prepare(
          "INSERT INTO user_problem_status (user_id, problem_id, status, "
          "first_ac_at, submit_count) VALUES (?, ?, ?, ?, ?) "
          "ON CONFLICT(user_id, problem_id) DO UPDATE SET "
          "status = excluded.status, first_ac_at = excluded.first_ac_at, "
          "submit_count = excluded.submit_count",
          stmt, error)) {
    return false;
  }
  bool bound = stmt.bind(1, static_cast<sqlite3_int64>(user_id)) &&
               stmt.bind(2, static_cast<sqlite3_int64>(problem_id)) &&
               stmt.bind(3, accepted ? "accepted" : "none");
  if (bound && has_first_ac_at) {
    bound = stmt.bind(4, first_ac_at);
  } else if (bound) {
    bound = stmt.bind_null(4);
  }
  bound = bound && stmt.bind(5, submit_count);
  if (!bound) {
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

bool UserProblemStatusStore::recompute(std::int64_t user_id,
                                       std::int64_t problem_id,
                                       std::string &error) {
  // 保持 submit_count 不变；若此前无记录，则按实际提交次数初始化。
  int submit_count = 0;
  bool has_record = false;
  {
    Statement stmt;
    if (!db_.prepare(
            "SELECT submit_count FROM user_problem_status WHERE user_id = ? "
            "AND problem_id = ?",
            stmt, error)) {
      return false;
    }
    if (!stmt.bind(1, static_cast<sqlite3_int64>(user_id)) ||
        !stmt.bind(2, static_cast<sqlite3_int64>(problem_id))) {
      error = stmt.errmsg();
      return false;
    }
    int rc = stmt.step();
    if (rc == SQLITE_ROW) {
      submit_count = stmt.column_int(0);
      has_record = true;
    } else if (rc == SQLITE_DONE) {
      submit_count = 0;
    } else {
      error = stmt.errmsg();
      return false;
    }
  }

  if (!has_record) {
    Statement stmt;
    if (!db_.prepare(
            "SELECT COUNT(*) FROM submissions WHERE user_id = ? AND "
            "problem_id = ?",
            stmt, error)) {
      return false;
    }
    if (!stmt.bind(1, static_cast<sqlite3_int64>(user_id)) ||
        !stmt.bind(2, static_cast<sqlite3_int64>(problem_id))) {
      error = stmt.errmsg();
      return false;
    }
    int rc = stmt.step();
    if (rc == SQLITE_ROW) {
      submit_count = stmt.column_int(0);
    } else if (rc != SQLITE_DONE) {
      error = stmt.errmsg();
      return false;
    }
  }

  // 查找当前仍 AC 的最早原提交时间。
  std::string first_ac_at;
  bool has_ac = false;
  {
    Statement stmt;
    if (!db_.prepare(
            "SELECT MIN(created_at) FROM submissions WHERE user_id = ? AND "
            "problem_id = ? AND status = 'AC'",
            stmt, error)) {
      return false;
    }
    if (!stmt.bind(1, static_cast<sqlite3_int64>(user_id)) ||
        !stmt.bind(2, static_cast<sqlite3_int64>(problem_id))) {
      error = stmt.errmsg();
      return false;
    }
    int rc = stmt.step();
    if (rc == SQLITE_ROW) {
      if (!stmt.column_is_null(0)) {
        first_ac_at = stmt.column_text(0);
        has_ac = true;
      }
    } else if (rc != SQLITE_DONE) {
      error = stmt.errmsg();
      return false;
    }
  }

  return upsert(user_id, problem_id, has_ac, has_ac, first_ac_at,
                submit_count, error);
}

} // namespace oj
