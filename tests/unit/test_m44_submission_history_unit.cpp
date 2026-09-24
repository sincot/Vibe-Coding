// M4.4 提交历史与本人状态读取（数据层）单元测试（gtest）。
//
// 直接验证：
//   - oj::SubmissionStore::list_by_user：分页、最新优先（created_at DESC, id DESC）、
//     按用户隔离、按题目筛选、摘要字段（含题目 LEFT JOIN 标题与 memory_kb=0 保留）、
//     超出末页返回空但 total 不变、无提交为空。
//   - oj::UserProblemStatusStore::list_by_user：按 problem_id 升序、accepted/none、
//     first_ac_at 空/非空、按题目筛选、无记录为空、按用户隔离。
//
// 使用隔离临时文件数据库（非内存库），不启动 HTTP 或判题进程，不触碰正式数据。
//
// 运行方式：ctest --test-dir build -R m44_history_unit --output-on-failure
// 或直接执行 build/oj_m44_history_unit。

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "db/database.h"
#include "db/schema.h"
#include "db/submissions.h"

namespace {

using oj::Database;
using oj::Statement;
using oj::SubmissionRecord;
using oj::SubmissionStore;
using oj::SubmissionSummary;
using oj::UserProblemStatusStore;
using oj::UserStatusItem;

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

class M44HistoryUnitTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::make_unique<TempDir>("m44unit");
    std::string err;
    db_ = Database::open(dir_->db_path(), err);
    ASSERT_TRUE(db_ != nullptr) << err;
    ASSERT_TRUE(oj::initialize_schema(*db_, kAdminPassword, err)) << err;
    submissions_ = std::make_unique<SubmissionStore>(*db_);
    statuses_ = std::make_unique<UserProblemStatusStore>(*db_);

    user_a_ = insert_user("1000000001", "m44_a");
    user_b_ = insert_user("1000000002", "m44_b");
    problem1_ = insert_problem("题目 P1");
    problem2_ = insert_problem("题目 P2");
    ASSERT_GT(user_a_, 0);
    ASSERT_GT(user_b_, 0);
    ASSERT_GT(problem1_, 0);
    ASSERT_GT(problem2_, 0);
  }

  void TearDown() override {
    submissions_.reset();
    statuses_.reset();
    if (db_) db_->close();
    db_.reset();
    dir_.reset();
  }

  std::int64_t insert_user(const std::string &account,
                           const std::string &nickname) {
    Statement stmt;
    std::string err;
    EXPECT_TRUE(db_->prepare("INSERT INTO users (account, nickname, "
                             "password_hash, role) VALUES (?, ?, 'x', 'user') "
                             "RETURNING id",
                             stmt, err))
        << err;
    stmt.bind(1, account);
    stmt.bind(2, nickname);
    return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
  }

  std::int64_t insert_problem(const std::string &title) {
    Statement stmt;
    std::string err;
    EXPECT_TRUE(db_->prepare("INSERT INTO problems (title, difficulty) VALUES "
                             "(?, 'easy') RETURNING id",
                             stmt, err))
        << err;
    stmt.bind(1, title);
    return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
  }

  // 插入一条提交并返回其主键；per_case/created_at 可控。
  std::int64_t insert_submission(std::int64_t user_id, std::int64_t problem_id,
                                 const std::string &status,
                                 const std::string &created_at,
                                 long long memory_kb = 4096,
                                 long long runtime_ms = 1,
                                 const std::string &per_case = "[]") {
    SubmissionRecord record;
    record.user_id = user_id;
    record.problem_id = problem_id;
    record.language = "cpp17";
    record.source_code = "int main(){}";
    record.status = status;
    record.per_case = per_case;
    record.compile_msg = "";
    record.runtime_ms = runtime_ms;
    record.memory_kb = memory_kb;
    record.created_at = created_at;
    std::int64_t id = 0;
    std::string err;
    EXPECT_TRUE(submissions_->insert(record, id, err)) << err;
    return id;
  }

  void set_status(std::int64_t user_id, std::int64_t problem_id,
                  const std::string &status, bool has_first_ac,
                  const std::string &first_ac_at, int submit_count) {
    std::string err;
    EXPECT_TRUE(statuses_->upsert(user_id, problem_id, status == "accepted",
                                  has_first_ac, first_ac_at, submit_count,
                                  err))
        << err;
  }

  std::vector<SubmissionSummary> list(std::int64_t user_id,
                                      std::int64_t problem_filter, int page,
                                      int page_size, long long &total) {
    std::vector<SubmissionSummary> out;
    std::string err;
    EXPECT_TRUE(submissions_->list_by_user(user_id, problem_filter, page,
                                           page_size, out, total, err))
        << err;
    return out;
  }

  std::vector<UserStatusItem> list_statuses(std::int64_t user_id,
                                            std::int64_t problem_filter) {
    std::vector<UserStatusItem> out;
    std::string err;
    EXPECT_TRUE(statuses_->list_by_user(user_id, problem_filter, out, err))
        << err;
    return out;
  }

  std::unique_ptr<TempDir> dir_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<SubmissionStore> submissions_;
  std::unique_ptr<UserProblemStatusStore> statuses_;
  std::int64_t user_a_ = 0;
  std::int64_t user_b_ = 0;
  std::int64_t problem1_ = 0;
  std::int64_t problem2_ = 0;
};

// T-101：无提交 → 空列表、total=0；超出末页返回空但 total 不变。
TEST_F(M44HistoryUnitTest, EmptyAndBeyondEnd) {
  long long total = -1;
  auto items = list(user_a_, 0, 1, 20, total);
  EXPECT_TRUE(items.empty());
  EXPECT_EQ(total, 0);

  insert_submission(user_a_, problem1_, "AC", "2026-09-21 10:00:00");
  insert_submission(user_a_, problem1_, "WA", "2026-09-21 11:00:00");
  items = list(user_a_, 0, 5, 20, total);
  EXPECT_TRUE(items.empty());
  EXPECT_EQ(total, 2); // total 反映全部匹配数，不因越界页改变。
}

// T-102：最新优先；created_at 相同时按 id DESC 稳定排序。
TEST_F(M44HistoryUnitTest, OrderingNewestFirstAndIdTieBreak) {
  const std::int64_t oldest =
      insert_submission(user_a_, problem1_, "AC", "2026-09-21 10:00:00");
  const std::int64_t middle =
      insert_submission(user_a_, problem1_, "WA", "2026-09-21 11:00:00");
  const std::int64_t newest =
      insert_submission(user_a_, problem1_, "CE", "2026-09-21 12:00:00");

  long long total = 0;
  auto items = list(user_a_, 0, 1, 20, total);
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(items[0].id, newest);
  EXPECT_EQ(items[1].id, middle);
  EXPECT_EQ(items[2].id, oldest);

  // 相同 created_at：后插入（id 更大）排前。
  const std::int64_t first =
      insert_submission(user_a_, problem2_, "AC", "2026-09-22 00:00:00");
  const std::int64_t second =
      insert_submission(user_a_, problem2_, "AC", "2026-09-22 00:00:00");
  items = list(user_a_, problem2_, 1, 20, total);
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0].id, second);
  EXPECT_EQ(items[1].id, first);
}

// T-103：摘要字段（题目 LEFT JOIN 标题、memory_kb=0 保留）与按用户隔离。
TEST_F(M44HistoryUnitTest, SummaryFieldsAndUserIsolation) {
  insert_submission(user_a_, problem1_, "AC", "2026-09-21 10:00:00", 0, 7);
  insert_submission(user_a_, problem2_, "WA", "2026-09-21 11:00:00", 8192, 9);
  insert_submission(user_b_, problem1_, "AC", "2026-09-21 12:00:00");

  long long total = 0;
  auto a_items = list(user_a_, 0, 1, 20, total);
  ASSERT_EQ(a_items.size(), 2u);
  EXPECT_EQ(total, 2);
  for (const auto &item : a_items) {
    EXPECT_EQ(item.language, "cpp17");
    EXPECT_FALSE(item.created_at.empty());
    if (item.problem_id == problem1_) {
      EXPECT_EQ(item.problem_title, "题目 P1");
      EXPECT_EQ(item.runtime_ms, 7);
      EXPECT_EQ(item.memory_kb, 0); // 0 保留为「未采集」，结构体不改成 -1。
    } else {
      EXPECT_EQ(item.problem_id, problem2_);
      EXPECT_EQ(item.problem_title, "题目 P2");
      EXPECT_EQ(item.status, "WA");
      EXPECT_EQ(item.memory_kb, 8192);
    }
  }

  auto b_items = list(user_b_, 0, 1, 20, total);
  ASSERT_EQ(b_items.size(), 1u);
  EXPECT_EQ(total, 1);
}

// T-104：按题目筛选；problem_id_filter=0 表示全部。
TEST_F(M44HistoryUnitTest, ProblemFilter) {
  insert_submission(user_a_, problem1_, "AC", "2026-09-21 10:00:00");
  insert_submission(user_a_, problem1_, "WA", "2026-09-21 11:00:00");
  insert_submission(user_a_, problem2_, "AC", "2026-09-21 12:00:00");

  long long total = 0;
  auto p1 = list(user_a_, problem1_, 1, 20, total);
  ASSERT_EQ(p1.size(), 2u);
  EXPECT_EQ(total, 2);
  for (const auto &item : p1) EXPECT_EQ(item.problem_id, problem1_);

  auto all = list(user_a_, 0, 1, 20, total);
  ASSERT_EQ(all.size(), 3u);
  EXPECT_EQ(total, 3);

  auto none = list(user_a_, 999999, 1, 20, total);
  EXPECT_TRUE(none.empty());
  EXPECT_EQ(total, 0);
}

// T-101（分页）：25 条按 created_at 相同时 id DESC，分页取回 20+5。
TEST_F(M44HistoryUnitTest, PaginationTwentyPlusFive) {
  std::vector<std::int64_t> ids;
  for (int i = 0; i < 25; ++i) {
    ids.push_back(insert_submission(user_a_, problem1_, "AC",
                                    "2026-09-22 00:00:00"));
  }

  long long total = 0;
  auto page1 = list(user_a_, 0, 1, 20, total);
  ASSERT_EQ(page1.size(), 20u);
  EXPECT_EQ(total, 25);
  EXPECT_EQ(page1.front().id, ids.back());
  EXPECT_EQ(page1.back().id, ids[5]);

  auto page2 = list(user_a_, 0, 2, 20, total);
  ASSERT_EQ(page2.size(), 5u);
  EXPECT_EQ(total, 25);
  EXPECT_EQ(page2.front().id, ids[4]);
  EXPECT_EQ(page2.back().id, ids[0]);
}

// T-105：状态列表按 problem_id 升序；accepted/none、first_ac_at 空/非空。
TEST_F(M44HistoryUnitTest, StatusListSortedAndFields) {
  set_status(user_a_, problem2_, "none", false, "", 1);
  set_status(user_a_, problem1_, "accepted", true, "2026-09-21 12:00:00", 3);

  auto items = list_statuses(user_a_, 0);
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0].problem_id, problem1_);
  EXPECT_TRUE(items[0].accepted);
  EXPECT_TRUE(items[0].has_first_ac_at);
  EXPECT_EQ(items[0].first_ac_at, "2026-09-21 12:00:00");
  EXPECT_EQ(items[0].submit_count, 3);

  EXPECT_EQ(items[1].problem_id, problem2_);
  EXPECT_FALSE(items[1].accepted);
  EXPECT_FALSE(items[1].has_first_ac_at);
  EXPECT_EQ(items[1].submit_count, 1);
}

// T-105：状态筛选、无记录为空、按用户隔离。
TEST_F(M44HistoryUnitTest, StatusFilterEmptyAndIsolation) {
  set_status(user_a_, problem1_, "accepted", true, "2026-09-21 12:00:00", 2);
  set_status(user_b_, problem2_, "none", false, "", 1);

  auto filtered = list_statuses(user_a_, problem1_);
  ASSERT_EQ(filtered.size(), 1u);
  EXPECT_EQ(filtered[0].problem_id, problem1_);

  auto missing = list_statuses(user_a_, problem2_);
  EXPECT_TRUE(missing.empty());

  auto b_only = list_statuses(user_b_, 0);
  ASSERT_EQ(b_only.size(), 1u);
  EXPECT_EQ(b_only[0].problem_id, problem2_);
}

} // namespace
