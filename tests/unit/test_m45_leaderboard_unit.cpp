// M4.5 排行榜统计读取（数据层）单元测试（gtest）。
//
// 直接验证：
//   - oj::LeaderboardStore::query：按可见题目聚合每用户 AC 数/提交次数/首次 AC
//     时间；重复 AC 不重复增加 AC 题目数；失败提交计入总提交次数；AC 后失败不清除；
//     Rejudge 重算（UserProblemStatusStore::recompute）后 AC 数/首次 AC 正确变化且
//     提交次数不变；隐藏题目排除；仅统计有已结算可见提交的用户；管理员同口径参与；
//     排序 AC↓→提交↑→首次AC↑→注册↑→ID↑；null 首次 AC 排在有 AC 之后；分页与越界。
//
// 使用隔离临时文件数据库（非内存库），不启动 HTTP 或判题进程，不触碰正式数据。
// 统计写入路径复用真实的 compute_status_update（提交）与 recompute（Rejudge），
// 不照搬实现：测试按需求约定构造预期。
//
// 运行方式：ctest --test-dir build -R m45_leaderboard_unit --output-on-failure
// 或直接执行 build/oj_m45_leaderboard_unit。

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "db/database.h"
#include "db/leaderboard.h"
#include "db/schema.h"
#include "db/submissions.h"
#include "submit/submit.h"

namespace {

using oj::Database;
using oj::LeaderboardEntry;
using oj::LeaderboardQuery;
using oj::LeaderboardResult;
using oj::LeaderboardStore;
using oj::Statement;
using oj::SubmissionRecord;
using oj::SubmissionStore;
using oj::UserProblemStatusStore;

const std::string kAdminPassword = "AdminSecret123!";

class TempDir {
public:
  explicit TempDir(const std::string &label) {
    std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
      auto candidate = base / (label + "_" + std::to_string(::getpid()) + "_" +
                               std::to_string(i));
      std::error_code ec;
      std::filesystem::create_directories(candidate, ec);
      if (!ec) {
        path_ = candidate;
        return;
      }
    }
    path_.clear();
  }
  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }
  std::string db_path() const { return (path_ / "oj.db").string(); }

private:
  std::filesystem::path path_;
};

class M45LeaderboardUnitTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::make_unique<TempDir>("m45unit");
    std::string err;
    db_ = Database::open(dir_->db_path(), err);
    ASSERT_TRUE(db_ != nullptr) << err;
    ASSERT_TRUE(oj::initialize_schema(*db_, kAdminPassword, err)) << err;
    store_ = std::make_unique<LeaderboardStore>(*db_);
    submissions_ = std::make_unique<SubmissionStore>(*db_);
    statuses_ = std::make_unique<UserProblemStatusStore>(*db_);

    p1_ = insert_problem("可见题 P1", 1);
    p2_ = insert_problem("可见题 P2", 1);
    p3_ = insert_problem("可见题 P3", 1);
    p_hidden_ = insert_problem("隐藏题 H", 0);
    ASSERT_GT(p1_, 0);
    ASSERT_GT(p2_, 0);
    ASSERT_GT(p3_, 0);
    ASSERT_GT(p_hidden_, 0);
  }

  void TearDown() override {
    statuses_.reset();
    submissions_.reset();
    store_.reset();
    if (db_) db_->close();
    db_.reset();
    dir_.reset();
  }

  std::int64_t insert_user(const std::string &account,
                           const std::string &nickname, const std::string &role,
                           const std::string &created_at) {
    Statement stmt;
    std::string err;
    EXPECT_TRUE(db_->prepare(
        "INSERT INTO users (account, nickname, password_hash, role, created_at) "
        "VALUES (?, ?, 'x', ?, ?) RETURNING id",
        stmt, err))
        << err;
    stmt.bind(1, account);
    stmt.bind(2, nickname);
    stmt.bind(3, role);
    stmt.bind(4, created_at);
    return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
  }

  std::int64_t insert_problem(const std::string &title, int visible) {
    Statement stmt;
    std::string err;
    EXPECT_TRUE(db_->prepare("INSERT INTO problems (title, difficulty, visible) "
                             "VALUES (?, 'easy', ?) RETURNING id",
                             stmt, err))
        << err;
    stmt.bind(1, title);
    stmt.bind(2, visible);
    return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
  }

  std::int64_t find_user_by_account(const std::string &account) {
    Statement stmt;
    std::string err;
    EXPECT_TRUE(db_->prepare("SELECT id FROM users WHERE account = ?", stmt,
                             err))
        << err;
    stmt.bind(1, account);
    return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
  }

  void upsert_status(std::int64_t user_id, std::int64_t problem_id,
                     bool accepted, bool has_first_ac,
                     const std::string &first_ac_at, int submit_count) {
    std::string err;
    EXPECT_TRUE(statuses_->upsert(user_id, problem_id, accepted, has_first_ac,
                                  first_ac_at, submit_count, err))
        << err;
  }

  std::int64_t insert_submission(std::int64_t user_id, std::int64_t problem_id,
                                 const std::string &status,
                                 const std::string &created_at) {
    SubmissionRecord record;
    record.user_id = user_id;
    record.problem_id = problem_id;
    record.language = "cpp17";
    record.source_code = "int main(){}";
    record.status = status;
    record.per_case = "[]";
    record.created_at = created_at;
    std::int64_t id = 0;
    std::string err;
    EXPECT_TRUE(submissions_->insert(record, id, err)) << err;
    return id;
  }

  void set_submission_status(std::int64_t id, const std::string &status) {
    bool found = false;
    SubmissionRecord record;
    std::string err;
    EXPECT_TRUE(submissions_->find_by_id(id, found, record, err)) << err;
    ASSERT_TRUE(found);
    record.status = status;
    EXPECT_TRUE(submissions_->update(record, err)) << err;
  }

  LeaderboardResult query(int page, int page_size) {
    LeaderboardResult out;
    std::string err;
    LeaderboardQuery q;
    q.page = page;
    q.page_size = page_size;
    EXPECT_TRUE(store_->query(q, out, err)) << err;
    return out;
  }

  // 依据真实提交状态计算函数把 user/problem 记为一次提交后的状态写入。
  // 返回本次提交是否 AC；写入后状态即与提交链路一致。
  void apply_submission(std::int64_t user_id, std::int64_t problem_id,
                        bool accepted, const std::string &time) {
    bool found = false;
    oj::UserProblemStatusRecord current;
    std::string err;
    EXPECT_TRUE(statuses_->find(user_id, problem_id, found, current, err))
        << err;
    oj::submit::StatusState state;
    state.has_record = found;
    state.accepted = found && current.accepted;
    state.first_ac_at = found ? current.first_ac_at : "";
    state.submit_count = found ? current.submit_count : 0;
    const oj::submit::StatusUpdate update =
        oj::submit::compute_status_update(state, accepted, time);
    upsert_status(user_id, problem_id, update.accepted,
                  update.has_first_ac_at, update.first_ac_at,
                  update.submit_count);
  }

  std::unique_ptr<TempDir> dir_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<LeaderboardStore> store_;
  std::unique_ptr<SubmissionStore> submissions_;
  std::unique_ptr<UserProblemStatusStore> statuses_;
  std::int64_t p1_ = 0;
  std::int64_t p2_ = 0;
  std::int64_t p3_ = 0;
  std::int64_t p_hidden_ = 0;
};

// T-001：无已结算提交 → 空列表、total=0。
TEST_F(M45LeaderboardUnitTest, EmptyWhenNoSettledSubmissions) {
  // 有用户但从未提交。
  insert_user("1000000001", "nobody", "user", "2026-01-01 00:00:00");
  auto result = query(1, 20);
  EXPECT_TRUE(result.items.empty());
  EXPECT_EQ(result.total, 0);
}

// T-002：同题重复 AC 不重复增加 AC 数，提交次数按次累加，首次 AC 不被覆盖。
TEST_F(M45LeaderboardUnitTest, DuplicateAcCountsOnce) {
  const std::int64_t u = insert_user("1000000001", "dupac", "user",
                                     "2026-01-01 00:00:00");
  apply_submission(u, p1_, true, "2026-03-01 10:00:00");
  apply_submission(u, p1_, true, "2026-03-01 11:00:00"); // 重复 AC

  auto result = query(1, 20);
  ASSERT_EQ(result.items.size(), 1u);
  EXPECT_EQ(result.items[0].ac_count, 1);
  EXPECT_EQ(result.items[0].submit_count, 2);
  EXPECT_TRUE(result.items[0].has_first_ac_at);
  EXPECT_EQ(result.items[0].first_ac_at, "2026-03-01 10:00:00");
}

// T-003：失败提交计入总提交次数；AC 后再失败不清除 AC 与首次 AC 时间。
TEST_F(M45LeaderboardUnitTest, FailingAndAcThenFail) {
  const std::int64_t u1 = insert_user("1000000001", "onlywa", "user",
                                      "2026-01-01 00:00:00");
  apply_submission(u1, p1_, false, "2026-03-01 10:00:00");
  apply_submission(u1, p1_, false, "2026-03-01 11:00:00");

  const std::int64_t u2 = insert_user("1000000002", "acfail", "user",
                                      "2026-01-01 00:00:00");
  apply_submission(u2, p2_, true, "2026-03-02 10:00:00");
  apply_submission(u2, p2_, false, "2026-03-02 11:00:00"); // AC 后失败

  auto result = query(1, 20);
  ASSERT_EQ(result.items.size(), 2u);
  // 有 AC 用户排在前：ac=1，首次 AC 保持，提交次数累计。
  const auto &af = result.items[0];
  EXPECT_EQ(af.user_id, u2);
  EXPECT_EQ(af.ac_count, 1);
  EXPECT_EQ(af.submit_count, 2);
  EXPECT_TRUE(af.has_first_ac_at);
  EXPECT_EQ(af.first_ac_at, "2026-03-02 10:00:00");
  // 无 AC 用户：ac=0，仍列出（有已结算提交），首次 AC 为 null。
  const auto &wa = result.items[1];
  EXPECT_EQ(wa.user_id, u1);
  EXPECT_EQ(wa.ac_count, 0);
  EXPECT_EQ(wa.submit_count, 2);
  EXPECT_FALSE(wa.has_first_ac_at);
}

// T-004：唯一 AC 被 Rejudge 为失败后，AC 数与首次 AC 清空，提交次数不变。
TEST_F(M45LeaderboardUnitTest, RejudgeUniqueAcToFail) {
  const std::int64_t u = insert_user("1000000001", "rej1", "user",
                                     "2026-01-01 00:00:00");
  const std::int64_t s = insert_submission(u, p1_, "AC", "2026-03-01 10:00:00");
  // 提交链路状态：accepted，首次 AC = 原提交时间，提交一次。
  apply_submission(u, p1_, true, "2026-03-01 10:00:00");

  auto before = query(1, 20);
  ASSERT_EQ(before.items.size(), 1u);
  EXPECT_EQ(before.items[0].ac_count, 1);

  // 模拟 Rejudge：更新原提交为 WA，并按原记录重算状态（rejudge 使用的函数）。
  set_submission_status(s, "WA");
  std::string err;
  ASSERT_TRUE(statuses_->recompute(u, p1_, err)) << err;

  auto after = query(1, 20);
  ASSERT_EQ(after.items.size(), 1u);
  EXPECT_EQ(after.items[0].ac_count, 0);
  EXPECT_EQ(after.items[0].submit_count, 1); // Rejudge 不增加提交次数
  EXPECT_FALSE(after.items[0].has_first_ac_at);
}

// T-005：仍有其他 AC 时，Rejudge 后首次 AC 取剩余有效记录中最早的原提交时间。
TEST_F(M45LeaderboardUnitTest, RejudgeKeepsEarliestRemainingAc) {
  const std::int64_t u = insert_user("1000000001", "rej2", "user",
                                     "2026-01-01 00:00:00");
  const std::int64_t early =
      insert_submission(u, p1_, "AC", "2026-03-01 10:00:00");
  insert_submission(u, p1_, "AC", "2026-03-01 12:00:00");
  apply_submission(u, p1_, true, "2026-03-01 10:00:00");
  apply_submission(u, p1_, true, "2026-03-01 12:00:00");

  auto before = query(1, 20);
  ASSERT_EQ(before.items.size(), 1u);
  EXPECT_EQ(before.items[0].ac_count, 1);
  EXPECT_EQ(before.items[0].submit_count, 2);
  EXPECT_EQ(before.items[0].first_ac_at, "2026-03-01 10:00:00");

  // 最早的 AC 被重判为失败，剩余 AC 的最早原提交时间成为新的首次 AC。
  set_submission_status(early, "WA");
  std::string err;
  ASSERT_TRUE(statuses_->recompute(u, p1_, err)) << err;

  auto after = query(1, 20);
  ASSERT_EQ(after.items.size(), 1u);
  EXPECT_EQ(after.items[0].ac_count, 1);
  EXPECT_EQ(after.items[0].submit_count, 2);
  EXPECT_TRUE(after.items[0].has_first_ac_at);
  EXPECT_EQ(after.items[0].first_ac_at, "2026-03-01 12:00:00");
}

// T-006：排序 AC 数↓ → 提交次数↑ → 首次 AC 时间↑ → 注册时间↑。
TEST_F(M45LeaderboardUnitTest, SortCascade) {
  // U1/U2/U3：ac=2；U4：ac=2 但提交更多；U5/U6：ac=0。
  const std::int64_t u1 = insert_user("1000000001", "u1", "user",
                                      "2026-01-01 00:00:00");
  const std::int64_t u2 = insert_user("1000000002", "u2", "user",
                                      "2026-01-02 00:00:00");
  const std::int64_t u3 = insert_user("1000000003", "u3", "user",
                                      "2026-01-01 00:00:00");
  const std::int64_t u4 = insert_user("1000000004", "u4", "user",
                                      "2026-01-01 00:00:00");

  // U1：两题 AC，提交 3+2=5，最早首次 AC 2026-02-01。
  upsert_status(u1, p1_, true, true, "2026-02-01 00:00:00", 3);
  upsert_status(u1, p2_, true, true, "2026-02-03 00:00:00", 2);
  // U2：与 U1 同 AC/提交/首次 AC，但注册更晚。
  upsert_status(u2, p1_, true, true, "2026-02-01 00:00:00", 3);
  upsert_status(u2, p2_, true, true, "2026-02-03 00:00:00", 2);
  // U3：首次 AC 更晚。
  upsert_status(u3, p1_, true, true, "2026-02-02 00:00:00", 3);
  upsert_status(u3, p2_, true, true, "2026-02-03 00:00:00", 2);
  // U4：提交次数更多（6），应排在三名 ac=2、提交 5 的用户之后。
  upsert_status(u4, p1_, true, true, "2026-01-01 00:00:00", 3);
  upsert_status(u4, p2_, true, true, "2026-01-02 00:00:00", 3);

  auto result = query(1, 20);
  ASSERT_EQ(result.items.size(), 4u);
  EXPECT_EQ(result.items[0].user_id, u1);
  EXPECT_EQ(result.items[1].user_id, u2);
  EXPECT_EQ(result.items[2].user_id, u3);
  EXPECT_EQ(result.items[3].user_id, u4);
  for (const auto &item : result.items) {
    EXPECT_EQ(item.ac_count, 2);
  }
  EXPECT_EQ(result.items[0].submit_count, 5);
  EXPECT_EQ(result.items[3].submit_count, 6);
}

// T-007：null 首次 AC 排在有 AC 用户之后；无 AC 用户之间按提交次数、注册、ID。
TEST_F(M45LeaderboardUnitTest, NullFirstAcSortsLastWithTieBreaks) {
  const std::int64_t ac =
      insert_user("1000000001", "withac", "user", "2026-01-01 00:00:00");
  // 无 AC：提交 1、注册较晚；提交 1、注册较早；提交 2。
  const std::int64_t none_late = insert_user("1000000002", "none_late",
                                             "user", "2026-01-05 00:00:00");
  const std::int64_t none_early1 = insert_user(
      "1000000003", "none_early1", "user", "2026-01-02 00:00:00");
  const std::int64_t none_early2 = insert_user(
      "1000000004", "none_early2", "user", "2026-01-02 00:00:00");
  const std::int64_t none_more = insert_user("1000000005", "none_more",
                                             "user", "2026-01-01 00:00:00");

  upsert_status(ac, p1_, true, true, "2026-05-01 00:00:00", 1);
  upsert_status(none_late, p1_, false, false, "", 1);
  upsert_status(none_early1, p1_, false, false, "", 1);
  upsert_status(none_early2, p1_, false, false, "", 1);
  upsert_status(none_more, p1_, false, false, "", 2);

  auto result = query(1, 20);
  ASSERT_EQ(result.items.size(), 5u);
  EXPECT_EQ(result.items[0].user_id, ac);
  EXPECT_TRUE(result.items[0].has_first_ac_at);
  // 无 AC 组：提交次数升序（1 在前，2 在后）；提交相同按注册升序，再按 ID 升序。
  EXPECT_EQ(result.items[1].user_id, none_early1);
  EXPECT_EQ(result.items[2].user_id, none_early2);
  EXPECT_EQ(result.items[3].user_id, none_late);
  EXPECT_EQ(result.items[4].user_id, none_more);
  for (std::size_t i = 1; i < result.items.size(); ++i) {
    EXPECT_FALSE(result.items[i].has_first_ac_at);
  }
}

// T-008/T-009：隐藏题目排除；仅统计有已结算可见提交的用户；管理员同口径参与。
TEST_F(M45LeaderboardUnitTest, HiddenExcludedUnsettledExcludedAdminIncluded) {
  const std::int64_t hidden_only = insert_user("1000000001", "hidden_only",
                                               "user", "2026-01-01 00:00:00");
  const std::int64_t mixed = insert_user("1000000002", "mixed", "user",
                                         "2026-01-01 00:00:00");
  const std::int64_t never = insert_user("1000000003", "never", "user",
                                         "2026-01-01 00:00:00");
  // 预置 admin 由 initialize_schema 创建，直接复用其 ID（不重复插入）。
  const std::int64_t admin = find_user_by_account("admin");
  ASSERT_GT(admin, 0);

  // 仅隐藏题：完全不计入，用户不出现。
  upsert_status(hidden_only, p_hidden_, true, true, "2026-04-01 00:00:00", 5);
  // 可见 + 隐藏混合：只计可见题。
  upsert_status(mixed, p1_, true, true, "2026-04-02 00:00:00", 2);
  upsert_status(mixed, p_hidden_, true, true, "2026-04-03 00:00:00", 9);
  // 从未提交：无状态行，不出现。
  (void)never;
  // 管理员做题：同口径参与。
  upsert_status(admin, p2_, true, true, "2026-04-04 00:00:00", 1);

  auto result = query(1, 20);
  ASSERT_EQ(result.items.size(), 2u);
  // 两者 ac=1，按提交次数升序：admin(1) 在 mixed(2) 之前。
  EXPECT_EQ(result.items[0].user_id, admin);
  EXPECT_EQ(result.items[0].ac_count, 1);
  EXPECT_EQ(result.items[0].submit_count, 1);
  EXPECT_EQ(result.items[1].user_id, mixed); // ac=1
  EXPECT_EQ(result.items[1].ac_count, 1);
  EXPECT_EQ(result.items[1].submit_count, 2); // 隐藏题的 9 次不计入
  EXPECT_EQ(result.items[1].first_ac_at, "2026-04-02 00:00:00");
  bool seen_hidden_only = false;
  for (const auto &item : result.items) {
    if (item.user_id == hidden_only) seen_hidden_only = true;
  }
  EXPECT_FALSE(seen_hidden_only);
}

// T-010：分页在全局排序之后；越界页返回空但 total 不变。
TEST_F(M45LeaderboardUnitTest, PaginationAndBeyondEnd) {
  std::vector<std::int64_t> ids;
  for (int i = 0; i < 5; ++i) {
    const std::int64_t u = insert_user(
        "100000001" + std::to_string(i), "page" + std::to_string(i), "user",
        "2026-01-0" + std::to_string(i + 1) + " 00:00:00");
    ids.push_back(u);
    upsert_status(u, p1_, false, false, "", 1);
  }

  auto page1 = query(1, 2);
  ASSERT_EQ(page1.items.size(), 2u);
  EXPECT_EQ(page1.total, 5);
  EXPECT_EQ(page1.items[0].user_id, ids[0]);
  EXPECT_EQ(page1.items[1].user_id, ids[1]);

  auto page2 = query(2, 2);
  ASSERT_EQ(page2.items.size(), 2u);
  EXPECT_EQ(page2.total, 5);
  EXPECT_EQ(page2.items[0].user_id, ids[2]);
  EXPECT_EQ(page2.items[1].user_id, ids[3]);

  auto page3 = query(3, 2);
  ASSERT_EQ(page3.items.size(), 1u);
  EXPECT_EQ(page3.total, 5);
  EXPECT_EQ(page3.items[0].user_id, ids[4]);

  auto beyond = query(99, 2);
  EXPECT_TRUE(beyond.items.empty());
  EXPECT_EQ(beyond.total, 5);
}

} // namespace
