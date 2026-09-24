#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oj {

class Database;

// 排行榜条目（M4.5）：仅含公开榜单展示所需字段，绝不含 account、密码哈希、
// token、源码或逐点结果。user_id 为稳定的数据库主键，供前端在已登录时可靠地
// 高亮当前用户（不通过昵称猜测身份）；不对外暴露登录账号。
struct LeaderboardEntry {
  std::int64_t user_id = 0;
  std::string nickname;
  // 已通过的不同题目数：按 user_problem_status 中 status='accepted' 的题目统计，
  // 同一题重复 AC 不重复增加。
  long long ac_count = 0;
  // 总提交次数：沿用既有结算规则（user_problem_status.submit_count 合计），
  // 包含 CE/WA/TLE/RE/MLE/SYSERR 等失败提交，不仅统计成功提交。
  long long submit_count = 0;
  // 首次 AC 时间：取当前仍有效（accepted）题目中最早的 first_ac_at。
  // 没有 AC 时为 false，响应以 null 表示，绝不用注册时间/当前时间/0 伪造。
  bool has_first_ac_at = false;
  std::string first_ac_at;
  // 注册时间（users.created_at），作为最终排序条件之一。
  std::string created_at;
};

// 排行榜查询参数：分页在全局排序之后进行。
struct LeaderboardQuery {
  int page = 1;
  int page_size = 20;
};

struct LeaderboardResult {
  std::vector<LeaderboardEntry> items;
  // 满足相同统计与展示条件的用户总数（分页前），与 items 使用同一数据视图。
  long long total = 0;
};

// 公开排行榜统计（M4.5）。
//
// 统计口径（本次确定，详见 README「排行榜接口（M4.5）」与 SPEC M4.5）：
//   - 数据来源为既有持久化状态：`user_problem_status`（AC 状态、首次 AC 时间、
//     提交次数）与 `users.created_at`（注册时间）；不新增用户汇总字段，重启后
//     可由 SQLite 可靠恢复；
//   - 隐藏题目（`problems.visible = 0`）的 AC 与提交均不计入公开榜；
//   - 仅统计至少有 1 次已结算可见题目提交（提交次数合计 > 0）的用户；从未提交
//     的用户不出现在公开榜；
//   - 管理员按普通用户口径参与排名，不特殊排除；
//   - 排序：AC 数降序 → 总提交次数升序 → 首次 AC 时间升序（无 AC 排在有 AC 之后，
//     显式判定 NULL，不依赖数据库默认行为）→ 注册时间升序 → 用户 ID 升序
//     （稳定的最终排序键，避免分页顺序随机变化）。
//
// 所有语句使用参数绑定；每个用户行来自单个 GROUP BY 聚合子查询（先聚合再与
// users 连接），避免多表连接使 AC 数或提交次数成倍放大；读取使用短只读事务，
// 使 total 与当前页来自同一数据视图，且不长期持有覆盖判题执行的事务。
class LeaderboardStore {
public:
  explicit LeaderboardStore(Database &db) : db_(db) {}

  // 分页查询排行榜。page/page_size 由调用方校验（均应 >= 1）。
  // 返回 false 表示数据库错误，error 非空。
  bool query(const LeaderboardQuery &query, LeaderboardResult &out,
             std::string &error);

private:
  Database &db_;
};

} // namespace oj
