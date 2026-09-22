#include "db/problem_admin.h"

#include <mutex>

#include <sqlite3.h>

#include "db/database.h"

namespace oj {

namespace {

void rollback_quiet(Database &db) {
  std::string ignored;
  db.rollback(ignored);
}

// 在题目下写入一组公开样例（is_sample=1），ord 从 0 连续编号。
bool insert_samples(Database &db, std::int64_t problem_id,
                    const std::vector<problem::SampleInput> &samples,
                    std::string &error) {
  for (std::size_t i = 0; i < samples.size(); ++i) {
    Statement stmt;
    if (!db.prepare(
            "INSERT INTO testcases (problem_id, ord, input, output, is_sample) "
            "VALUES (?, ?, ?, ?, 1)",
            stmt, error)) {
      return false;
    }
    if (!stmt.bind(1, static_cast<sqlite3_int64>(problem_id)) ||
        !stmt.bind(2, static_cast<int>(i)) || !stmt.bind(3, samples[i].input) ||
        !stmt.bind(4, samples[i].output)) {
      error = stmt.errmsg();
      return false;
    }
    if (stmt.step() != SQLITE_DONE) {
      error = stmt.errmsg();
      return false;
    }
  }
  return true;
}

} // namespace

bool ProblemAdminStore::create(const problem::ProblemData &data,
                               std::int64_t &out_id, std::string &error) {
  std::lock_guard<std::mutex> lock(db_.transaction_mutex());
  if (!db_.begin(error)) {
    return false;
  }

  std::int64_t problem_id = 0;
  {
    Statement stmt;
    if (!db_.prepare(
            "INSERT INTO problems (title, description, difficulty, tags, "
            "time_limit_ms, memory_limit_kb, visible, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, datetime('now'), datetime('now')) "
            "RETURNING id",
            stmt, error)) {
      rollback_quiet(db_);
      return false;
    }
    if (!stmt.bind(1, data.title) || !stmt.bind(2, data.description) ||
        !stmt.bind(3, data.difficulty) ||
        !stmt.bind(4, problem::join_tags(data.tags)) ||
        !stmt.bind(5, data.time_limit_ms) ||
        !stmt.bind(6, data.memory_limit_kb) ||
        !stmt.bind(7, data.visible ? 1 : 0)) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return false;
    }
    if (stmt.step() != SQLITE_ROW) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return false;
    }
    problem_id = stmt.column_int64(0);
  }

  if (!insert_samples(db_, problem_id, data.samples, error)) {
    rollback_quiet(db_);
    return false;
  }

  if (!db_.commit(error)) {
    rollback_quiet(db_);
    return false;
  }
  out_id = problem_id;
  return true;
}

ProblemAdminStore::UpdateStatus
ProblemAdminStore::update(std::int64_t id, const problem::ProblemPatch &patch,
                          std::string &error) {
  std::lock_guard<std::mutex> lock(db_.transaction_mutex());
  if (!db_.begin(error)) {
    return UpdateStatus::Error;
  }

  // 读当前值用于合并部分更新；事务已取写锁，读改写在本事务内串行安全。
  bool found = false;
  ProblemRecord record;
  if (!problems_.find_by_id(id, found, record, error)) {
    rollback_quiet(db_);
    return UpdateStatus::Error;
  }
  if (!found) {
    rollback_quiet(db_);
    return UpdateStatus::NotFound;
  }

  const std::string title = patch.title.value_or(record.title);
  const std::string description = patch.description.value_or(record.description);
  const std::string difficulty = patch.difficulty.value_or(record.difficulty);
  const std::string tags = patch.tags.has_value()
                               ? problem::join_tags(*patch.tags)
                               : problem::join_tags(record.tags);
  const int time_limit_ms =
      patch.time_limit_ms.value_or(record.time_limit_ms);
  const int memory_limit_kb =
      patch.memory_limit_kb.value_or(record.memory_limit_kb);
  const bool visible = patch.visible.value_or(record.visible);

  {
    Statement stmt;
    if (!db_.prepare(
            "UPDATE problems SET title = ?, description = ?, difficulty = ?, "
            "tags = ?, time_limit_ms = ?, memory_limit_kb = ?, visible = ?, "
            "updated_at = datetime('now') WHERE id = ?",
            stmt, error)) {
      rollback_quiet(db_);
      return UpdateStatus::Error;
    }
    if (!stmt.bind(1, title) || !stmt.bind(2, description) ||
        !stmt.bind(3, difficulty) || !stmt.bind(4, tags) ||
        !stmt.bind(5, time_limit_ms) || !stmt.bind(6, memory_limit_kb) ||
        !stmt.bind(7, visible ? 1 : 0) ||
        !stmt.bind(8, static_cast<sqlite3_int64>(id))) {
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

  // 仅当请求显式提供 samples 时整体替换公开样例；隐藏用例（is_sample=0）不动。
  if (patch.samples.has_value()) {
    {
      Statement stmt;
      if (!db_.prepare("DELETE FROM testcases WHERE problem_id = ? AND "
                       "is_sample = 1",
                       stmt, error)) {
        rollback_quiet(db_);
        return UpdateStatus::Error;
      }
      if (!stmt.bind(1, static_cast<sqlite3_int64>(id))) {
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
    if (!insert_samples(db_, id, *patch.samples, error)) {
      rollback_quiet(db_);
      return UpdateStatus::Error;
    }
  }

  if (!db_.commit(error)) {
    rollback_quiet(db_);
    return UpdateStatus::Error;
  }
  return UpdateStatus::Updated;
}

ProblemAdminStore::DeleteStatus ProblemAdminStore::remove(std::int64_t id,
                                                          std::string &error) {
  std::lock_guard<std::mutex> lock(db_.transaction_mutex());
  if (!db_.begin(error)) {
    return DeleteStatus::Error;
  }

  bool found = false;
  ProblemRecord record;
  if (!problems_.find_by_id(id, found, record, error)) {
    rollback_quiet(db_);
    return DeleteStatus::Error;
  }
  if (!found) {
    rollback_quiet(db_);
    return DeleteStatus::NotFound;
  }

  // 删除策略：题目已有提交记录时拒绝删除，保留提交历史（不在接口层级联清空）。
  // 检查与删除在同一事务内完成，且提交保存复用同一事务互斥锁，故不会被并发
  // 提交插入绕过。
  sqlite3_int64 submission_count = 0;
  {
    Statement stmt;
    if (!db_.prepare("SELECT COUNT(*) FROM submissions WHERE problem_id = ?",
                     stmt, error)) {
      rollback_quiet(db_);
      return DeleteStatus::Error;
    }
    if (!stmt.bind(1, static_cast<sqlite3_int64>(id))) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return DeleteStatus::Error;
    }
    if (stmt.step() != SQLITE_ROW) {
      error = stmt.errmsg();
      rollback_quiet(db_);
      return DeleteStatus::Error;
    }
    submission_count = stmt.column_int64(0);
  }
  if (submission_count > 0) {
    rollback_quiet(db_);
    return DeleteStatus::HasSubmissions;
  }

  // 无提交：删除该题全部用例（公开样例与隐藏用例）、做题状态记录与题目本身。
  const char *statements[] = {
      "DELETE FROM testcases WHERE problem_id = ?",
      "DELETE FROM user_problem_status WHERE problem_id = ?",
      "DELETE FROM problems WHERE id = ?",
  };
  for (const char *sql : statements) {
    Statement stmt;
    if (!db_.prepare(sql, stmt, error)) {
      rollback_quiet(db_);
      return DeleteStatus::Error;
    }
    if (!stmt.bind(1, static_cast<sqlite3_int64>(id))) {
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
