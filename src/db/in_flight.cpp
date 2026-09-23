#include "db/in_flight.h"

#include <sqlite3.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>

#include "db/database.h"

namespace oj {

namespace {

// 从 /dev/urandom 读取随机字节；失败时退回 std::random_device。
std::string random_hex(std::size_t bytes) {
  std::string raw(bytes, '\0');
  bool ok = false;
  {
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom) {
      urandom.read(raw.data(), static_cast<std::streamsize>(bytes));
      ok = urandom.gcount() == static_cast<std::streamsize>(bytes);
    }
  }
  if (!ok) {
    static std::random_device rd;
    for (std::size_t i = 0; i < bytes; i += 4) {
      std::uint32_t value = rd();
      for (std::size_t j = 0; j < 4 && i + j < bytes; ++j) {
        raw[i + j] = static_cast<char>((value >> (8 * j)) & 0xFF);
      }
    }
  }
  static const char *const kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (char c : raw) {
    unsigned char byte = static_cast<unsigned char>(c);
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

void load_in_flight(Statement &stmt, InFlightTask &out) {
  out.id = stmt.column_int64(0);
  out.task_id = stmt.column_text(1);
  out.user_id = stmt.column_int64(2);
  out.problem_id = stmt.column_int64(3);
  out.language = stmt.column_text(4);
  out.source_code = stmt.column_text(5);
  out.submitted_at = stmt.column_text(6);
  out.state = stmt.column_text(7);
  out.reason = stmt.column_text(8);
}

} // namespace

std::string generate_task_id() {
  std::time_t now = std::time(nullptr);
  std::tm tm_utc{};
  gmtime_r(&now, &tm_utc);
  char prefix[32] = {0};
  std::strftime(prefix, sizeof(prefix), "%Y%m%d%H%M%S", &tm_utc);
  return std::string(prefix) + "-" + random_hex(12);
}

bool InFlightStore::insert(InFlightTask &task, std::int64_t &out_id,
                           std::string &error) {
  constexpr int kMaxAttempts = 5;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (task.task_id.empty()) {
      task.task_id = generate_task_id();
    }
    Statement stmt;
    if (!db_.prepare(
            "INSERT INTO in_flight_tasks (task_id, user_id, problem_id, "
            "language, source_code, submitted_at, state) "
            "VALUES (?, ?, ?, ?, ?, ?, 'pending') RETURNING id",
            stmt, error)) {
      return false;
    }
    if (!stmt.bind(1, task.task_id) ||
        !stmt.bind(2, static_cast<sqlite3_int64>(task.user_id)) ||
        !stmt.bind(3, static_cast<sqlite3_int64>(task.problem_id)) ||
        !stmt.bind(4, task.language) || !stmt.bind(5, task.source_code) ||
        !stmt.bind(6, task.submitted_at)) {
      error = stmt.errmsg();
      return false;
    }
    int rc = stmt.step();
    if (rc == SQLITE_ROW) {
      out_id = stmt.column_int64(0);
      return true;
    }
    // 任务标识唯一性冲突：换新标识重试（其它错误直接失败，绝不无限重试）。
    if (db_.extended_errcode() == SQLITE_CONSTRAINT_UNIQUE) {
      task.task_id.clear();
      continue;
    }
    error = stmt.errmsg();
    return false;
  }
  error = "生成在途任务标识失败（唯一性冲突重试耗尽）";
  return false;
}

bool InFlightStore::find_by_task_id(const std::string &task_id, bool &found,
                                    InFlightTask &out, std::string &error) {
  Statement stmt;
  if (!db_.prepare(
          "SELECT id, task_id, user_id, problem_id, language, source_code, "
          "submitted_at, state, reason FROM in_flight_tasks WHERE task_id = ?",
          stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, task_id)) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    load_in_flight(stmt, out);
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

bool InFlightStore::remove_by_task_id(const std::string &task_id,
                                      bool &removed, std::string &error) {
  Statement stmt;
  if (!db_.prepare(
          "DELETE FROM in_flight_tasks WHERE task_id = ? RETURNING id", stmt,
          error)) {
    return false;
  }
  if (!stmt.bind(1, task_id)) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    removed = true;
    return true;
  }
  if (rc == SQLITE_DONE) {
    removed = false;
    return true;
  }
  error = stmt.errmsg();
  return false;
}

bool InFlightStore::mark_interrupted(const std::string &task_id,
                                     const std::string &reason,
                                     std::string &error) {
  Statement stmt;
  if (!db_.prepare("UPDATE in_flight_tasks SET state = 'interrupted', "
                   "reason = ? WHERE task_id = ? AND state != 'interrupted'",
                   stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, reason) || !stmt.bind(2, task_id)) {
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

bool InFlightStore::list_pending(int limit, std::vector<InFlightTask> &out,
                                 std::string &error) {
  out.clear();
  if (limit <= 0) {
    return true;
  }
  Statement stmt;
  if (!db_.prepare(
          "SELECT id, task_id, user_id, problem_id, language, source_code, "
          "submitted_at, state, reason FROM in_flight_tasks "
          "WHERE state = 'pending' ORDER BY id ASC LIMIT ?",
          stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, limit)) {
    error = stmt.errmsg();
    return false;
  }
  while (true) {
    int rc = stmt.step();
    if (rc == SQLITE_ROW) {
      InFlightTask task;
      load_in_flight(stmt, task);
      out.push_back(std::move(task));
      continue;
    }
    if (rc == SQLITE_DONE) {
      return true;
    }
    error = stmt.errmsg();
    return false;
  }
}

bool InFlightStore::claim(std::int64_t id, const std::string &owner,
                          bool &claimed, std::string &error) {
  Statement stmt;
  if (!db_.prepare(
          "UPDATE in_flight_tasks SET state = 'claimed', owner = ?, "
          "claimed_at = datetime('now') WHERE id = ? AND state = 'pending' "
          "RETURNING id",
          stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, owner) || !stmt.bind(2, static_cast<sqlite3_int64>(id))) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    claimed = true;
    return true;
  }
  if (rc == SQLITE_DONE) {
    claimed = false;
    return true;
  }
  error = stmt.errmsg();
  return false;
}

bool InFlightStore::reset_stale_claims(std::string &error) {
  return db_.exec("UPDATE in_flight_tasks SET state = 'pending', owner = '', "
                  "claimed_at = NULL WHERE state = 'claimed'",
                  error);
}

bool InFlightStore::count_for_problem(std::int64_t problem_id,
                                      std::int64_t &out, std::string &error) {
  Statement stmt;
  if (!db_.prepare("SELECT COUNT(*) FROM in_flight_tasks WHERE problem_id = ? "
                   "AND state != 'interrupted'",
                   stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, static_cast<sqlite3_int64>(problem_id))) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    out = stmt.column_int64(0);
    return true;
  }
  error = stmt.errmsg();
  return false;
}

} // namespace oj
