#include "db/problems.h"

#include <cctype>
#include <set>

#include <sqlite3.h>

#include "db/database.h"
#include "problem/list_query.h"

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

// 读取带统计的列表查询列序：
// id, title, difficulty, tags, visible, pass_count, viewer_status。
// viewer_status 为空（SQL NULL）表示当前用户对该题没有状态记录（视为未 AC）。
void load_summary_with_stats(Statement &stmt, ProblemSummary &out) {
  out.id = stmt.column_int64(0);
  out.title = stmt.column_text(1);
  out.difficulty = stmt.column_text(2);
  out.tags = split_tags(stmt.column_text(3));
  out.visible = stmt.column_int(4) != 0;
  out.pass_count = static_cast<long long>(stmt.column_int64(5));
  out.solved = !stmt.column_is_null(6) && stmt.column_text(6) == "accepted";
}

// 列表/计数共用的筛选条件。sql 为 " WHERE ..."（无任何条件时为空串），
// params 为按顺序绑定的文本参数。可见性、关键词、难度、标签互相之间为 AND。
struct BuiltWhere {
  std::string sql;
  std::vector<std::string> params;
};

BuiltWhere build_where(const ProblemListQuery &query) {
  BuiltWhere out;
  std::vector<std::string> conditions;
  switch (query.visibility) {
    case ProblemVisibility::VisibleOnly:
      conditions.push_back("p.visible = 1");
      break;
    case ProblemVisibility::All:
      break;
    case ProblemVisibility::HiddenOnly:
      conditions.push_back("p.visible = 0");
      break;
  }
  if (!query.keyword.empty()) {
    // 子串匹配，%/_ 已由 title_like_pattern 转义为普通字符。
    conditions.push_back("p.title LIKE ? ESCAPE '\\'");
    out.params.push_back(problem::title_like_pattern(query.keyword));
  }
  if (!query.difficulty.empty()) {
    conditions.push_back("p.difficulty = ?");
    out.params.push_back(query.difficulty);
  }
  if (!query.tag.empty()) {
    // 以逗号补齐首尾后做 instr 精确查找，避免子串误匹配（如「图」误中「图论」）。
    conditions.push_back("instr(',' || p.tags || ',', ?) > 0");
    out.params.push_back(problem::tag_match_needle(query.tag));
  }
  for (std::size_t i = 0; i < conditions.size(); ++i) {
    out.sql += (i == 0 ? " WHERE " : " AND ");
    out.sql += conditions[i];
  }
  return out;
}

// 按顺序绑定文本参数，起始占位符下标为 start。返回 false 时 stmt.errmsg() 有值。
bool bind_text_params(Statement &stmt, int start,
                      const std::vector<std::string> &params) {
  int index = start;
  for (const std::string &param : params) {
    if (!stmt.bind(index++, param)) {
      return false;
    }
  }
  return true;
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

bool ProblemStore::query(const ProblemListQuery &query,
                         ProblemListResult &out, std::string &error) {
  if (query.page < 1 || query.page_size < 1) {
    error = "非法的分页参数";
    return false;
  }
  const BuiltWhere where = build_where(query);

  // 1. 计数：与列表完全相同的筛选与可见性条件（不含分页/本人状态）。
  {
    Statement stmt;
    if (!db_.prepare("SELECT COUNT(*) FROM problems p" + where.sql, stmt,
                     error)) {
      return false;
    }
    if (!bind_text_params(stmt, 1, where.params)) {
      error = stmt.errmsg();
      return false;
    }
    int rc = stmt.step();
    if (rc != SQLITE_ROW) {
      error = stmt.errmsg();
      return false;
    }
    out.total = static_cast<long long>(stmt.column_int64(0));
  }

  // 2. 当前页数据。pass_count 用相关子查询统计「已 AC 的不同用户数」；
  //    viewer_status 用当前登录用户（游客为 0，不匹配任何记录）查询本人状态。
  //    两个子查询对每个问题最多返回一行，不会放大结果行数或打乱分页。
  const std::string select =
      "SELECT p.id, p.title, p.difficulty, p.tags, p.visible, "
      "(SELECT COUNT(DISTINCT s.user_id) FROM user_problem_status s "
      " WHERE s.problem_id = p.id AND s.status = 'accepted'), "
      "(SELECT s.status FROM user_problem_status s "
      " WHERE s.problem_id = p.id AND s.user_id = ?) "
      "FROM problems p" +
      where.sql + " ORDER BY p.id ASC LIMIT ? OFFSET ?";

  Statement stmt;
  if (!db_.prepare(select, stmt, error)) {
    return false;
  }
  int index = 1;
  if (!stmt.bind(index++, static_cast<sqlite3_int64>(query.viewer_user_id))) {
    error = stmt.errmsg();
    return false;
  }
  if (!bind_text_params(stmt, index, where.params)) {
    error = stmt.errmsg();
    return false;
  }
  index += static_cast<int>(where.params.size());
  const sqlite3_int64 offset =
      static_cast<sqlite3_int64>(query.page - 1) * query.page_size;
  if (!stmt.bind(index++, query.page_size) || !stmt.bind(index++, offset)) {
    error = stmt.errmsg();
    return false;
  }

  while (true) {
    int rc = stmt.step();
    if (rc == SQLITE_ROW) {
      ProblemSummary summary;
      load_summary_with_stats(stmt, summary);
      out.items.push_back(std::move(summary));
      continue;
    }
    if (rc == SQLITE_DONE) {
      return true;
    }
    error = stmt.errmsg();
    return false;
  }
}

bool ProblemStore::list_tags(bool include_hidden,
                             std::vector<std::string> &out,
                             std::string &error) {
  const char *sql = include_hidden
                        ? "SELECT tags FROM problems"
                        : "SELECT tags FROM problems WHERE visible = 1";
  Statement stmt;
  if (!db_.prepare(sql, stmt, error)) {
    return false;
  }
  std::set<std::string> unique;
  while (true) {
    int rc = stmt.step();
    if (rc == SQLITE_ROW) {
      for (const std::string &tag : split_tags(stmt.column_text(0))) {
        unique.insert(tag);
      }
      continue;
    }
    if (rc == SQLITE_DONE) {
      break;
    }
    error = stmt.errmsg();
    return false;
  }
  out.assign(unique.begin(), unique.end());
  return true;
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

bool ProblemStore::viewer_solved(std::int64_t user_id,
                                 std::int64_t problem_id, bool &out_solved,
                                 std::string &error) {
  out_solved = false;
  // 游客（user_id<=0）不查询，保持未 AC；本人状态只依据已验证身份。
  if (user_id <= 0) {
    return true;
  }
  Statement stmt;
  if (!db_.prepare("SELECT status FROM user_problem_status WHERE user_id = ? "
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
    out_solved = stmt.column_text(0) == "accepted";
    return true;
  }
  if (rc == SQLITE_DONE) {
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
