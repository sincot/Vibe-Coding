// M4.3 题目详情「本人做题状态」读取单元测试（gtest）。
//
// 直接验证 oj::ProblemStore::viewer_solved 的语义：无记录视为未 AC、accepted 视为
// 已 AC、none 视为未 AC、游客/非法用户不查询、用户与题目之间互不影响。使用隔离
// 临时文件数据库（非内存库），不触碰正式数据、不启动 HTTP 或判题进程。
//
// 运行方式：ctest --test-dir build -R problem_owner_status_unit --output-on-failure
// 或直接执行 build/oj_problem_owner_status_unit。

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "db/database.h"
#include "db/problems.h"
#include "db/schema.h"

namespace {

using oj::Database;
using oj::ProblemStore;

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

class ProblemOwnerStatusTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::make_unique<TempDir>("m43unit");
    std::string err;
    db_ = Database::open(dir_->db_path(), err);
    ASSERT_TRUE(db_ != nullptr) << err;
    ASSERT_TRUE(oj::initialize_schema(*db_, kAdminPassword, err)) << err;
    store_ = std::make_unique<ProblemStore>(*db_);

    user_a_ = insert_user("1000000001", "user_a");
    user_b_ = insert_user("1000000002", "user_b");
    ASSERT_GT(user_a_, 0);
    ASSERT_GT(user_b_, 0);
    problem_ = insert_problem("题目 P");
    problem2_ = insert_problem("题目 Q");
    ASSERT_GT(problem_, 0);
    ASSERT_GT(problem2_, 0);
  }

  void TearDown() override {
    store_.reset();
    if (db_) {
      db_->close();
    }
    db_.reset();
    dir_.reset();
  }

  std::int64_t insert_user(const std::string &account,
                           const std::string &nickname) {
    oj::Statement stmt;
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
    oj::Statement stmt;
    std::string err;
    EXPECT_TRUE(db_->prepare("INSERT INTO problems (title, difficulty) VALUES "
                             "(?, 'easy') RETURNING id",
                             stmt, err))
        << err;
    stmt.bind(1, title);
    return stmt.step() == SQLITE_ROW ? stmt.column_int64(0) : -1;
  }

  void set_status(std::int64_t user_id, std::int64_t problem_id,
                  const std::string &status, int submit_count) {
    oj::Statement stmt;
    std::string err;
    EXPECT_TRUE(db_->prepare("INSERT INTO user_problem_status (user_id, "
                             "problem_id, status, submit_count) VALUES (?, ?, "
                             "?, ?)",
                             stmt, err))
        << err;
    stmt.bind(1, static_cast<sqlite3_int64>(user_id));
    stmt.bind(2, static_cast<sqlite3_int64>(problem_id));
    stmt.bind(3, status);
    stmt.bind(4, submit_count);
    EXPECT_EQ(stmt.step(), SQLITE_DONE) << stmt.errmsg();
  }

  // 调用 viewer_solved 并断言查询本身成功，返回 out_solved。
  bool solved(std::int64_t user_id, std::int64_t problem_id) {
    bool out = true;
    std::string err;
    EXPECT_TRUE(store_->viewer_solved(user_id, problem_id, out, err)) << err;
    return out;
  }

  std::unique_ptr<TempDir> dir_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<ProblemStore> store_;
  std::int64_t user_a_ = 0;
  std::int64_t user_b_ = 0;
  std::int64_t problem_ = 0;
  std::int64_t problem2_ = 0;
};

// T-001：无状态记录视为未 AC。
TEST_F(ProblemOwnerStatusTest, NoRecordIsNotSolved) {
  EXPECT_FALSE(solved(user_a_, problem_));
}

// T-002：accepted → 已 AC；none → 未 AC。
TEST_F(ProblemOwnerStatusTest, AcceptedIsSolvedAndNoneIsNot) {
  set_status(user_a_, problem_, "accepted", 2);
  EXPECT_TRUE(solved(user_a_, problem_));

  set_status(user_b_, problem2_, "none", 1);
  EXPECT_FALSE(solved(user_b_, problem2_));
}

// T-003：游客/非法用户恒为未 AC 且不查询；不存在的题目保持未 AC。
TEST_F(ProblemOwnerStatusTest, GuestOrInvalidUserNeverSolved) {
  EXPECT_FALSE(solved(0, problem_));
  EXPECT_FALSE(solved(-5, problem_));
  EXPECT_FALSE(solved(user_a_, 999999));
}

// T-004：不同用户、不同题目之间互不影响。
TEST_F(ProblemOwnerStatusTest, UsersAndProblemsAreIsolated) {
  set_status(user_a_, problem_, "accepted", 1);
  EXPECT_TRUE(solved(user_a_, problem_));
  // 同一题目的另一个用户仍为未 AC。
  EXPECT_FALSE(solved(user_b_, problem_));
  // 同一用户的另一题目仍为未 AC。
  EXPECT_FALSE(solved(user_a_, problem2_));
}

} // namespace
