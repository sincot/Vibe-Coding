#include "db/leaderboard.h"

#include <mutex>
#include <utility>

#include <sqlite3.h>

#include "db/database.h"

namespace oj {

namespace {

// 每个用户一行的聚合统计。仅计入可见题目（`p.visible = 1`），故隐藏题目的 AC
// 与提交都不进入公开榜。使用 GROUP BY 先在 user_problem_status 上聚合成每用户
// 一行，再与 users 一对一连结，避免连接放大 AC 数与提交次数。
//
//   - ac_count：accepted 状态的不同题目数。UNIQUE(user_id, problem_id) 保证每题
//     至多一行，重复 AC 不会重复增加；
//   - submit_count：该用户在可见题目上的提交次数合计（含失败提交）；
//   - first_ac_at：accepted 题目中最早的 first_ac_at（无 AC 为 NULL）。
constexpr const char *kUserStatsSelect =
    "SELECT ups.user_id AS user_id, "
    "COUNT(CASE WHEN ups.status = 'accepted' THEN 1 END) AS ac_count, "
    "SUM(ups.submit_count) AS submit_count, "
    "MIN(CASE WHEN ups.status = 'accepted' THEN ups.first_ac_at END) AS "
    "first_ac_at "
    "FROM user_problem_status ups "
    "JOIN problems p ON p.id = ups.problem_id AND p.visible = 1 "
    "GROUP BY ups.user_id";

} // namespace

bool LeaderboardStore::query(const LeaderboardQuery &query,
                             LeaderboardResult &out, std::string &error) {
  out.items.clear();
  out.total = 0;
  if (query.page < 1 || query.page_size < 1) {
    error = "非法的分页参数";
    return false;
  }
  const long long offset =
      static_cast<long long>(query.page - 1) * query.page_size;

  // 短只读事务：total 与当前页来自同一数据视图，避免同一响应中的总人数与逐条
  // 统计来自互相矛盾的读取阶段。仅读取，不覆盖判题执行；连接为单连接，故与写
  // 事务共用连接级事务互斥锁串行化，事务本身很短。
  std::lock_guard<std::mutex> lock(db_.transaction_mutex());
  if (!db_.exec("BEGIN", error)) {
    return false;
  }

  bool ok = true;
  {
    // 总人数：与列表使用完全相同的聚合与过滤条件（仅计数，不加载全部用户）。
    Statement stmt;
    std::string sql = std::string("SELECT COUNT(*) FROM (") + kUserStatsSelect +
                      ") stats WHERE stats.submit_count > 0";
    if (!db_.prepare(sql, stmt, error)) {
      ok = false;
    } else {
      const int rc = stmt.step();
      if (rc == SQLITE_ROW) {
        out.total = static_cast<long long>(stmt.column_int64(0));
      } else {
        error = stmt.errmsg();
        ok = false;
      }
    }
  }

  if (ok) {
    Statement stmt;
    // 分页在全局排序之后进行，名次不随分页重置。首次 AC 时间无值者排在有值者
    // 之后（NULL 显式判定），无 AC 用户之间继续比较提交次数、注册时间与 ID。
    std::string sql =
        std::string(
            "SELECT u.id, u.nickname, stats.ac_count, stats.submit_count, "
            "stats.first_ac_at, u.created_at FROM (") +
        kUserStatsSelect +
        ") stats JOIN users u ON u.id = stats.user_id "
        "WHERE stats.submit_count > 0 "
        "ORDER BY stats.ac_count DESC, stats.submit_count ASC, "
        "(stats.first_ac_at IS NULL) ASC, stats.first_ac_at ASC, "
        "u.created_at ASC, u.id ASC LIMIT ? OFFSET ?";
    if (!db_.prepare(sql, stmt, error)) {
      ok = false;
    } else if (!stmt.bind(1, query.page_size) ||
               !stmt.bind(2, static_cast<sqlite3_int64>(offset))) {
      error = stmt.errmsg();
      ok = false;
    } else {
      while (true) {
        const int rc = stmt.step();
        if (rc == SQLITE_ROW) {
          LeaderboardEntry entry;
          entry.user_id = stmt.column_int64(0);
          entry.nickname = stmt.column_text(1);
          entry.ac_count = static_cast<long long>(stmt.column_int64(2));
          entry.submit_count = static_cast<long long>(stmt.column_int64(3));
          entry.has_first_ac_at = !stmt.column_is_null(4);
          entry.first_ac_at = entry.has_first_ac_at ? stmt.column_text(4) : "";
          entry.created_at = stmt.column_text(5);
          out.items.push_back(std::move(entry));
          continue;
        }
        if (rc == SQLITE_DONE) {
          break;
        }
        error = stmt.errmsg();
        ok = false;
        break;
      }
    }
  }

  if (!ok) {
    std::string ignored;
    db_.exec("ROLLBACK", ignored);
    return false;
  }
  if (!db_.exec("COMMIT", error)) {
    std::string ignored;
    db_.exec("ROLLBACK", ignored);
    return false;
  }
  return true;
}

} // namespace oj
