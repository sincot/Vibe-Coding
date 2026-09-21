#include "db/problems.h"

#include <cctype>

#include <sqlite3.h>

#include "db/database.h"

namespace oj {

namespace {

// 统一读取列表查询的列序：id, title, difficulty, tags, visible。
void load_summary(Statement &stmt, ProblemSummary &out) {
  out.id = stmt.column_int64(0);
  out.title = stmt.column_text(1);
  out.difficulty = stmt.column_text(2);
  out.tags = split_tags(stmt.column_text(3));
  out.visible = stmt.column_int(4) != 0;
}

// 统一读取详情查询的列序：
// id, title, description, difficulty, tags, time_limit_ms, memory_limit_kb,
// visible, created_at, updated_at。
void load_record(Statement &stmt, ProblemRecord &out) {
  out.id = stmt.column_int64(0);
  out.title = stmt.column_text(1);
  out.description = stmt.column_text(2);
  out.difficulty = stmt.column_text(3);
  out.tags = split_tags(stmt.column_text(4));
  out.time_limit_ms = stmt.column_int(5);
  out.memory_limit_kb = stmt.column_int(6);
  out.visible = stmt.column_int(7) != 0;
  out.created_at = stmt.column_text(8);
  out.updated_at = stmt.column_text(9);
}

} // namespace

std::vector<std::string> split_tags(const std::string &tags) {
  std::vector<std::string> result;
  std::size_t pos = 0;
  while (pos <= tags.size()) {
    std::size_t comma = tags.find(',', pos);
    std::size_t end = (comma == std::string::npos) ? tags.size() : comma;
    std::size_t begin = pos;
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(tags[begin]))) {
      ++begin;
    }
    std::size_t last = end;
    while (last > begin &&
           std::isspace(static_cast<unsigned char>(tags[last - 1]))) {
      --last;
    }
    if (last > begin) {
      result.push_back(tags.substr(begin, last - begin));
    }
    if (comma == std::string::npos) {
      break;
    }
    pos = comma + 1;
  }
  return result;
}

bool ProblemStore::list(bool include_hidden, std::vector<ProblemSummary> &out,
                        std::string &error) {
  const char *sql =
      include_hidden
          ? "SELECT id, title, difficulty, tags, visible FROM problems "
            "ORDER BY id ASC"
          : "SELECT id, title, difficulty, tags, visible FROM problems "
            "WHERE visible = 1 ORDER BY id ASC";
  Statement stmt;
  if (!db_.prepare(sql, stmt, error)) {
    return false;
  }
  while (true) {
    int rc = stmt.step();
    if (rc == SQLITE_ROW) {
      ProblemSummary summary;
      load_summary(stmt, summary);
      out.push_back(std::move(summary));
      continue;
    }
    if (rc == SQLITE_DONE) {
      return true;
    }
    error = stmt.errmsg();
    return false;
  }
}

bool ProblemStore::find_by_id(std::int64_t id, bool &found, ProblemRecord &out,
                              std::string &error) {
  Statement stmt;
  if (!db_.prepare(
          "SELECT id, title, description, difficulty, tags, time_limit_ms, "
          "memory_limit_kb, visible, created_at, updated_at FROM problems "
          "WHERE id = ?",
          stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, static_cast<sqlite3_int64>(id))) {
    error = stmt.errmsg();
    return false;
  }
  int rc = stmt.step();
  if (rc == SQLITE_ROW) {
    load_record(stmt, out);
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

bool ProblemStore::list_samples(std::int64_t problem_id,
                                std::vector<SampleCase> &out,
                                std::string &error) {
  Statement stmt;
  if (!db_.prepare(
          "SELECT input, output FROM testcases WHERE problem_id = ? AND "
          "is_sample = 1 ORDER BY ord ASC, id ASC",
          stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, static_cast<sqlite3_int64>(problem_id))) {
    error = stmt.errmsg();
    return false;
  }
  while (true) {
    int rc = stmt.step();
    if (rc == SQLITE_ROW) {
      SampleCase sample;
      sample.input = stmt.column_text(0);
      sample.output = stmt.column_text(1);
      out.push_back(std::move(sample));
      continue;
    }
    if (rc == SQLITE_DONE) {
      return true;
    }
    error = stmt.errmsg();
    return false;
  }
}

bool ProblemStore::list_testcases(std::int64_t problem_id,
                                  std::vector<TestcaseRecord> &out,
                                  std::string &error) {
  Statement stmt;
  if (!db_.prepare(
          "SELECT id, ord, input, output, is_sample FROM testcases WHERE "
          "problem_id = ? ORDER BY ord ASC, id ASC",
          stmt, error)) {
    return false;
  }
  if (!stmt.bind(1, static_cast<sqlite3_int64>(problem_id))) {
    error = stmt.errmsg();
    return false;
  }
  while (true) {
    int rc = stmt.step();
    if (rc == SQLITE_ROW) {
      TestcaseRecord tc;
      tc.id = stmt.column_int64(0);
      tc.ord = stmt.column_int(1);
      tc.input = stmt.column_text(2);
      tc.output = stmt.column_text(3);
      tc.is_sample = stmt.column_int(4) != 0;
      out.push_back(std::move(tc));
      continue;
    }
    if (rc == SQLITE_DONE) {
      return true;
    }
    error = stmt.errmsg();
    return false;
  }
}

} // namespace oj
