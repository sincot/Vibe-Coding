// M3.7 在途任务持久化与单实例互斥单元测试（gtest）。
//
// 直接验证 src/db/in_flight.{h,cpp} 的 InFlightStore 状态机、持久化任务标识生成，
// 以及 src/db/instance_lock.{h,cpp} 的单实例互斥与崩溃释放语义。使用隔离临时文件
// 数据库（非内存库），不触碰正式数据、不启动 HTTP/判题进程。
//
// 覆盖：
//   - generate_task_id：非空、格式含分隔符、批量无重复
//   - insert 自动生成/回写 task_id；find/remove 命中与未命中
//   - list_pending 仅返回 pending、按 id 升序、遵守 limit
//   - claim 原子唯一；reset_stale_claims 将 claimed 复位为 pending
//   - mark_interrupted 保留信息且被 list_pending/count_for_problem 排除
//   - count_for_problem 只统计未结算（pending/claimed）
//   - InstanceLock：同路径二次获取失败、析构后释放（进程崩溃由 OS 释放的等价语义）
//
// 运行方式：ctest --test-dir build -R m37_in_flight_unit --output-on-failure
// 或直接执行 build/oj_m37_in_flight_unit。

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "db/database.h"
#include "db/in_flight.h"
#include "db/instance_lock.h"
#include "db/schema.h"

namespace {

using oj::Database;
using oj::InFlightStore;
using oj::InFlightTask;
using oj::InstanceLock;

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
  std::string sub(const std::string &name) const {
    return (path_ / name).string();
  }

private:
  std::filesystem::path path_;
};

// 隔离临时库 + 一个用户与一道题目（满足 in_flight_tasks 的外键）。
class M37InFlightTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::make_unique<TempDir>("m37unit");
    std::string err;
    db_ = Database::open(dir_->db_path(), err);
    ASSERT_TRUE(db_ != nullptr) << err;
    ASSERT_TRUE(oj::initialize_schema(*db_, kAdminPassword, err)) << err;

    oj::Statement stmt;
    ASSERT_TRUE(db_->prepare("INSERT INTO users (account, nickname, "
                             "password_hash, role) VALUES ('1000000001', "
                             "'u1', 'x', 'user') RETURNING id",
                             stmt, err))
        << err;
    ASSERT_EQ(stmt.step(), SQLITE_ROW);
    user_id_ = stmt.column_int64(0);

    oj::Statement pstmt;
    ASSERT_TRUE(db_->prepare("INSERT INTO problems (title, difficulty) VALUES "
                             "('t', 'easy') RETURNING id",
                             pstmt, err))
        << err;
    ASSERT_EQ(pstmt.step(), SQLITE_ROW);
    problem_id_ = pstmt.column_int64(0);
  }

  void TearDown() override {
    if (db_) {
      db_->close();
    }
    db_.reset();
    dir_.reset();
  }

  // 构造一条指向题目与用户的在途任务（不落库）。
  InFlightTask make_task(const std::string &submitted_at = "2020-01-02 03:04:05",
                         const std::string &source = "int main(){}") {
    InFlightTask task;
    task.user_id = user_id_;
    task.problem_id = problem_id_;
    task.language = "cpp17";
    task.source_code = source;
    task.submitted_at = submitted_at;
    return task;
  }

  std::int64_t insert_task(const std::string &submitted_at) {
    InFlightStore store(*db_);
    InFlightTask task = make_task(submitted_at);
    std::int64_t id = 0;
    std::string err;
    EXPECT_TRUE(store.insert(task, id, err)) << err;
    return id;
  }

  std::unique_ptr<TempDir> dir_;
  std::unique_ptr<Database> db_;
  std::int64_t user_id_ = 0;
  std::int64_t problem_id_ = 0;
};

// ---------------------------------------------------------------------------
// generate_task_id
// ---------------------------------------------------------------------------

TEST(M37TaskId, NonEmptyFormattedAndUnique) {
  std::set<std::string> seen;
  for (int i = 0; i < 1000; ++i) {
    const std::string id = oj::generate_task_id();
    ASSERT_FALSE(id.empty());
    ASSERT_NE(id.find('-'), std::string::npos) << id;
    ASSERT_TRUE(seen.insert(id).second) << "duplicate task id: " << id;
  }
}

// ---------------------------------------------------------------------------
// InFlightStore 基本读写
// ---------------------------------------------------------------------------

TEST_F(M37InFlightTest, InsertFillsTaskIdAndFindRemove) {
  InFlightStore store(*db_);
  InFlightTask task = make_task();
  EXPECT_TRUE(task.task_id.empty());
  std::int64_t id = 0;
  std::string err;
  ASSERT_TRUE(store.insert(task, id, err)) << err;
  EXPECT_GT(id, 0);
  EXPECT_FALSE(task.task_id.empty());

  bool found = false;
  InFlightTask loaded;
  ASSERT_TRUE(store.find_by_task_id(task.task_id, found, loaded, err)) << err;
  ASSERT_TRUE(found);
  EXPECT_EQ(loaded.user_id, user_id_);
  EXPECT_EQ(loaded.problem_id, problem_id_);
  EXPECT_EQ(loaded.language, "cpp17");
  EXPECT_EQ(loaded.source_code, task.source_code);
  EXPECT_EQ(loaded.submitted_at, "2020-01-02 03:04:05");
  EXPECT_EQ(loaded.state, "pending");

  bool removed = false;
  ASSERT_TRUE(store.remove_by_task_id(task.task_id, removed, err)) << err;
  EXPECT_TRUE(removed);

  // 再次删除：未命中。
  ASSERT_TRUE(store.remove_by_task_id(task.task_id, removed, err)) << err;
  EXPECT_FALSE(removed);
}

TEST_F(M37InFlightTest, CountForProblemCountsUnsettledOnly) {
  InFlightStore store(*db_);
  std::string err;
  // 两条 pending + 一条 interrupted。
  InFlightTask a = make_task();
  InFlightTask b = make_task();
  std::int64_t id = 0;
  ASSERT_TRUE(store.insert(a, id, err)) << err;
  ASSERT_TRUE(store.insert(b, id, err)) << err;
  InFlightTask c = make_task();
  ASSERT_TRUE(store.insert(c, id, err)) << err;
  ASSERT_TRUE(store.mark_interrupted(c.task_id, "无法判题", err)) << err;

  std::int64_t count = -1;
  ASSERT_TRUE(store.count_for_problem(problem_id_, count, err)) << err;
  EXPECT_EQ(count, 2);
  ASSERT_TRUE(store.count_for_problem(problem_id_ + 999, count, err)) << err;
  EXPECT_EQ(count, 0);
}

// ---------------------------------------------------------------------------
// list_pending / claim / reset / mark_interrupted
// ---------------------------------------------------------------------------

TEST_F(M37InFlightTest, ListPendingOrdersByIdAndHonorsLimit) {
  InFlightStore store(*db_);
  std::string err;
  std::vector<std::string> ids;
  for (int i = 0; i < 3; ++i) {
    InFlightTask t = make_task();
    std::int64_t id = 0;
    ASSERT_TRUE(store.insert(t, id, err)) << err;
    ids.push_back(t.task_id);
  }

  std::vector<InFlightTask> batch;
  ASSERT_TRUE(store.list_pending(2, batch, err)) << err;
  ASSERT_EQ(batch.size(), 2u);
  EXPECT_EQ(batch[0].task_id, ids[0]);
  EXPECT_EQ(batch[1].task_id, ids[1]);

  // 认领第一条后，pending 只剩后两条。
  bool claimed = false;
  ASSERT_TRUE(store.claim(batch[0].id, "owner-a", claimed, err)) << err;
  ASSERT_TRUE(claimed);
  std::vector<InFlightTask> rest;
  ASSERT_TRUE(store.list_pending(10, rest, err)) << err;
  ASSERT_EQ(rest.size(), 2u);
  EXPECT_EQ(rest[0].task_id, ids[1]);
  EXPECT_EQ(rest[1].task_id, ids[2]);
}

TEST_F(M37InFlightTest, ClaimIsAtomicSingleWinner) {
  InFlightStore store(*db_);
  std::string err;
  InFlightTask t = make_task();
  std::int64_t id = 0;
  ASSERT_TRUE(store.insert(t, id, err)) << err;

  bool first = false;
  bool second = false;
  ASSERT_TRUE(store.claim(id, "owner-1", first, err)) << err;
  ASSERT_TRUE(store.claim(id, "owner-2", second, err)) << err;
  EXPECT_TRUE(first);
  EXPECT_FALSE(second);

  // 不存在的行：claim 返回正常但未命中。
  bool missing = true;
  ASSERT_TRUE(store.claim(id + 999, "owner", missing, err)) << err;
  EXPECT_FALSE(missing);
}

TEST_F(M37InFlightTest, ResetStaleClaimsReturnsToPending) {
  InFlightStore store(*db_);
  std::string err;
  InFlightTask t = make_task();
  std::int64_t id = 0;
  ASSERT_TRUE(store.insert(t, id, err)) << err;
  bool claimed = false;
  ASSERT_TRUE(store.claim(id, "dead-instance", claimed, err)) << err;
  ASSERT_TRUE(claimed);

  ASSERT_TRUE(store.reset_stale_claims(err)) << err;

  std::vector<InFlightTask> pending;
  ASSERT_TRUE(store.list_pending(10, pending, err)) << err;
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].task_id, t.task_id);
  EXPECT_EQ(pending[0].state, "pending");

  // 复位后可再次被认领（模拟下一次启动恢复）。
  ASSERT_TRUE(store.claim(id, "new-instance", claimed, err)) << err;
  EXPECT_TRUE(claimed);
}

TEST_F(M37InFlightTest, MarkInterruptedExcludedFromPendingAndCount) {
  InFlightStore store(*db_);
  std::string err;
  InFlightTask t = make_task();
  std::int64_t id = 0;
  ASSERT_TRUE(store.insert(t, id, err)) << err;

  ASSERT_TRUE(store.mark_interrupted(t.task_id, "题目不存在，无法判题", err))
      << err;

  std::vector<InFlightTask> pending;
  ASSERT_TRUE(store.list_pending(10, pending, err)) << err;
  EXPECT_TRUE(pending.empty());

  bool found = false;
  InFlightTask loaded;
  ASSERT_TRUE(store.find_by_task_id(t.task_id, found, loaded, err)) << err;
  ASSERT_TRUE(found);
  EXPECT_EQ(loaded.state, "interrupted");
  EXPECT_EQ(loaded.reason, "题目不存在，无法判题");

  std::int64_t count = -1;
  ASSERT_TRUE(store.count_for_problem(problem_id_, count, err)) << err;
  EXPECT_EQ(count, 0);
}

// ---------------------------------------------------------------------------
// InstanceLock
// ---------------------------------------------------------------------------

TEST(M37InstanceLock, SecondAcquireFailsAndDestructionReleases) {
  TempDir dir("m37lock");
  std::string err;
  auto first = InstanceLock::acquire(dir.db_path(), err);
  ASSERT_TRUE(first != nullptr) << err;

  std::string err2;
  auto second = InstanceLock::acquire(dir.db_path(), err2);
  EXPECT_FALSE(second);
  EXPECT_FALSE(err2.empty());

  // 释放（模拟进程退出/崩溃由 OS 释放）后可重新获取。
  first.reset();
  std::string err3;
  auto third = InstanceLock::acquire(dir.db_path(), err3);
  EXPECT_TRUE(third != nullptr) << err3;
}

} // namespace
