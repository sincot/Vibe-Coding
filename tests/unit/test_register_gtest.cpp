// M1.1 注册功能单元测试（GoogleTest）。
//
// 覆盖对象：
//   - oj::auth::validate_nickname / validate_password —— 昵称与密码输入规则
//   - oj::auth::RandomAccountGenerator —— 账号格式（10 位纯数字）
//   - oj::UserStore —— 用户查询、创建、账号/昵称唯一性冲突精确区分
//   - oj::auth::RegisterService —— 注册成功、昵称冲突、账号碰撞重试/耗尽、非法输入、持久化
//
// 均使用 /tmp 下的隔离临时数据库，不触碰正式数据库 data/oj.db。
//
// 运行方式：ctest --test-dir build -R register_gtest --output-on-failure
// 或直接执行 build/oj_register_gtest（支持 gtest 全部过滤参数，如 --gtest_filter=*Nickname*）。

#include <gtest/gtest.h>

#include <sys/types.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "auth/account.h"
#include "auth/password.h"
#include "auth/register.h"
#include "auth/validation.h"
#include "db/database.h"
#include "db/schema.h"
#include "db/users.h"

namespace {

using oj::UserRecord;
using oj::UserStore;
using oj::auth::AccountGenerator;
using oj::auth::RegisterOutcome;
using oj::auth::RegisterService;

// ---------------------------------------------------------------------------
// 测试辅助
// ---------------------------------------------------------------------------

// 唯一临时目录，析构时自动删除。
class TempDir {
public:
  explicit TempDir(const std::string &label) {
    std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
      auto candidate =
          base / (label + "_" + std::to_string(::getpid()) + "_" +
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

// 确定性账号生成器：按脚本序列返回账号，耗尽后重复最后一个（用于模拟碰撞/耗尽）。
class FakeAccountGenerator : public AccountGenerator {
public:
  explicit FakeAccountGenerator(std::vector<std::string> seq)
      : seq_(std::move(seq)) {}
  std::string generate() override {
    if (seq_.empty()) {
      return "0000000000";
    }
    std::size_t idx = idx_ < seq_.size() ? idx_ : seq_.size() - 1;
    ++idx_;
    return seq_[idx];
  }

private:
  std::vector<std::string> seq_;
  std::size_t idx_ = 0;
};

// 供数据库相关测试使用的 Fixture：隔离临时库 + 建表 + 预置 admin。
class DbTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::make_unique<TempDir>("gtest_reg");
    std::string err;
    db_ = oj::Database::open(dir_->db_path(), err);
    ASSERT_NE(db_, nullptr) << err;
    ASSERT_TRUE(
        oj::initialize_schema(*db_, std::string("AdminSecret123!"), err))
        << err;
  }

  int CountUsers(const std::string &nickname) {
    std::string err;
    oj::Statement stmt;
    if (!db_->prepare("SELECT COUNT(*) FROM users WHERE nickname = ?", stmt,
                      err)) {
      return -1;
    }
    stmt.bind(1, nickname);
    if (stmt.step() != SQLITE_ROW) {
      return -1;
    }
    return stmt.column_int(0);
  }

  std::unique_ptr<TempDir> dir_;
  std::unique_ptr<oj::Database> db_;
};

// ---------------------------------------------------------------------------
// 账号生成器
// ---------------------------------------------------------------------------

TEST(RandomAccountGeneratorTest, ProducesTenDigits) {
  oj::auth::RandomAccountGenerator gen;
  for (int i = 0; i < 100; ++i) {
    std::string account = gen.generate();
    ASSERT_EQ(account.size(), 10u);
    for (char c : account) {
      EXPECT_TRUE(c >= '0' && c <= '9');
    }
  }
}

// ---------------------------------------------------------------------------
// 昵称校验
// ---------------------------------------------------------------------------

class InvalidNicknameTest : public ::testing::TestWithParam<std::string> {};

TEST_P(InvalidNicknameTest, Rejected) {
  std::string normalized, err;
  EXPECT_FALSE(oj::auth::validate_nickname(GetParam(), normalized, err));
  EXPECT_FALSE(err.empty());
}

INSTANTIATE_TEST_SUITE_P(Nickname, InvalidNicknameTest,
                         ::testing::Values(std::string(""),
                                           std::string("   "),
                                           std::string("\t\n\r "),
                                           std::string(31, 'a')));

TEST(NicknameValidationTest, TrimsSurroundingWhitespace) {
  std::string normalized, err;
  ASSERT_TRUE(oj::auth::validate_nickname("  alice  ", normalized, err));
  EXPECT_EQ(normalized, "alice");
}

TEST(NicknameValidationTest, PreservesInternalWhitespace) {
  std::string normalized, err;
  ASSERT_TRUE(oj::auth::validate_nickname("a b c", normalized, err));
  EXPECT_EQ(normalized, "a b c");
}

TEST(NicknameValidationTest, EnforcesMaxLength) {
  std::string normalized, err;
  EXPECT_TRUE(
      oj::auth::validate_nickname(std::string(30, 'a'), normalized, err));
  EXPECT_FALSE(
      oj::auth::validate_nickname(std::string(31, 'a'), normalized, err));
}

// 长度上限按「字节」而非「字符」计（与 title/tags 等字段一致）。以 UTF-8 多字节
// 字符（每个 3 字节）锁定该口径：10 个 = 30 字节接受，11 个 = 33 字节拒绝。
TEST(NicknameValidationTest, CountsBytesNotCharacters) {
  std::string normalized, err;
  std::string cjk10;
  for (int i = 0; i < 10; ++i) {
    cjk10 += "\xe4\xb8\xad"; // "中"
  }
  ASSERT_EQ(cjk10.size(), 30u);
  EXPECT_TRUE(oj::auth::validate_nickname(cjk10, normalized, err));
  EXPECT_EQ(normalized, cjk10);

  std::string cjk11 = cjk10 + "\xe4\xb8\xad";
  ASSERT_EQ(cjk11.size(), 33u);
  EXPECT_FALSE(oj::auth::validate_nickname(cjk11, normalized, err));
}

// 先去除首尾空白，再判断长度：原始输入可超过 30 字节，只要去空白后不超过。
TEST(NicknameValidationTest, TrimsBeforeCheckingLength) {
  std::string normalized, err;
  std::string padded = "  " + std::string(30, 'a') + "  ";
  EXPECT_TRUE(oj::auth::validate_nickname(padded, normalized, err));
  EXPECT_EQ(normalized, std::string(30, 'a'));

  std::string too_long = "  " + std::string(31, 'a') + "  ";
  EXPECT_FALSE(oj::auth::validate_nickname(too_long, normalized, err));
}

// ---------------------------------------------------------------------------
// 密码校验
// ---------------------------------------------------------------------------

TEST(PasswordValidationTest, RejectsEmpty) {
  std::string err;
  EXPECT_FALSE(oj::auth::validate_password("", err));
}

TEST(PasswordValidationTest, DoesNotTrimWhitespace) {
  std::string err;
  // 纯空白密码允许（不裁剪、不判空）——空白视为有效内容。
  EXPECT_TRUE(oj::auth::validate_password("   ", err));
  EXPECT_TRUE(oj::auth::validate_password(" ab cd ", err));
}

TEST(PasswordValidationTest, EnforcesMaxLength) {
  std::string err;
  EXPECT_TRUE(oj::auth::validate_password(std::string(128, 'p'), err));
  EXPECT_FALSE(oj::auth::validate_password(std::string(129, 'p'), err));
}

// ---------------------------------------------------------------------------
// 用户数据访问层
// ---------------------------------------------------------------------------

TEST_F(DbTest, CreateInsertsOrdinaryUser) {
  UserStore store(*db_);
  UserRecord out;
  std::string err;
  ASSERT_EQ(store.create("1000000000", "alice", "hash1", out, err),
            UserStore::CreateStatus::Success);
  EXPECT_EQ(out.account, "1000000000");
  EXPECT_EQ(out.nickname, "alice");
  EXPECT_EQ(out.role, "user");
  EXPECT_EQ(out.reset_pwd_flag, 0);
  EXPECT_GT(out.id, 0);
}

TEST_F(DbTest, CreateDistinguishesAccountVsNicknameCollision) {
  UserStore store(*db_);
  UserRecord out;
  std::string err;
  ASSERT_EQ(store.create("1000000000", "alice", "hash1", out, err),
            UserStore::CreateStatus::Success);

  // 同账号、不同昵称 -> 账号碰撞。
  EXPECT_EQ(store.create("1000000000", "bob", "hash2", out, err),
            UserStore::CreateStatus::AccountTaken);
  // 不同账号、同昵称 -> 昵称冲突。
  EXPECT_EQ(store.create("1000000001", "alice", "hash3", out, err),
            UserStore::CreateStatus::NicknameTaken);
}

TEST_F(DbTest, FindByAccountAndNickname) {
  UserStore store(*db_);
  UserRecord out;
  std::string err;
  ASSERT_EQ(store.create("1000000000", "alice", "hash1", out, err),
            UserStore::CreateStatus::Success);

  bool found = false;
  UserRecord rec;
  ASSERT_TRUE(store.find_by_account("1000000000", found, rec, err));
  EXPECT_TRUE(found);
  EXPECT_EQ(rec.nickname, "alice");
  EXPECT_EQ(rec.password_hash, "hash1");

  ASSERT_TRUE(store.find_by_nickname("alice", found, rec, err));
  EXPECT_TRUE(found);
  EXPECT_EQ(rec.account, "1000000000");

  ASSERT_TRUE(store.find_by_account("9999999999", found, rec, err));
  EXPECT_FALSE(found);
}

// ---------------------------------------------------------------------------
// 注册服务
// ---------------------------------------------------------------------------

TEST_F(DbTest, RegisterSucceeds) {
  FakeAccountGenerator gen({"1234567890"});
  RegisterService svc(*db_, gen);

  auto outcome = svc.register_user("alice", "Secret123");
  ASSERT_EQ(outcome.kind, RegisterOutcome::Kind::Success);
  EXPECT_EQ(outcome.user.account, "1234567890");
  EXPECT_EQ(outcome.user.nickname, "alice");
  EXPECT_EQ(outcome.user.role, "user");
  EXPECT_EQ(outcome.user.reset_pwd_flag, 0);
  EXPECT_EQ(outcome.user.password_hash.find("$argon2id$"), 0u);

  std::string verr;
  EXPECT_TRUE(
      oj::auth::verify_password(outcome.user.password_hash, "Secret123", verr));
  EXPECT_FALSE(
      oj::auth::verify_password(outcome.user.password_hash, "WrongPass", verr));

  // 数据库记录与返回结果一致。
  UserStore store(*db_);
  bool found = false;
  UserRecord rec;
  ASSERT_TRUE(store.find_by_account("1234567890", found, rec, verr));
  EXPECT_TRUE(found);
  EXPECT_EQ(rec.nickname, "alice");
  EXPECT_EQ(rec.role, "user");
}

TEST_F(DbTest, RegisterNicknameConflict) {
  FakeAccountGenerator gen({"1000000000", "1000000001"});
  RegisterService svc(*db_, gen);

  ASSERT_EQ(svc.register_user("bob", "Pw1").kind,
            RegisterOutcome::Kind::Success);
  auto second = svc.register_user("bob", "Pw2");
  EXPECT_EQ(second.kind, RegisterOutcome::Kind::NicknameTaken);
  EXPECT_EQ(CountUsers("bob"), 1);
}

TEST_F(DbTest, RegisterTrimsNicknameBeforeInsert) {
  FakeAccountGenerator gen({"1000000000", "1000000001"});
  RegisterService svc(*db_, gen);

  auto first = svc.register_user("  carol  ", "Pw1");
  ASSERT_EQ(first.kind, RegisterOutcome::Kind::Success);
  EXPECT_EQ(first.user.nickname, "carol");

  // 去空白后与已存在昵称相同 -> 冲突。
  auto second = svc.register_user("carol", "Pw2");
  EXPECT_EQ(second.kind, RegisterOutcome::Kind::NicknameTaken);
}

TEST_F(DbTest, RegisterRetriesOnAccountCollision) {
  UserStore store(*db_);
  UserRecord dummy;
  std::string err;
  ASSERT_EQ(store.create("1000000000", "existing", "x", dummy, err),
            UserStore::CreateStatus::Success);

  // 首次生成碰撞账号，换号后成功。
  FakeAccountGenerator gen({"1000000000", "1000000001"});
  RegisterService svc(*db_, gen);
  auto outcome = svc.register_user("newuser", "pw123456");
  ASSERT_EQ(outcome.kind, RegisterOutcome::Kind::Success);
  EXPECT_EQ(outcome.user.account, "1000000001");
  EXPECT_EQ(CountUsers("newuser"), 1);
}

TEST_F(DbTest, RegisterFailsWhenAccountRetriesExhausted) {
  UserStore store(*db_);
  UserRecord dummy;
  std::string err;
  ASSERT_EQ(store.create("1000000000", "existing", "x", dummy, err),
            UserStore::CreateStatus::Success);

  // 账号始终碰撞，重试 3 次耗尽 -> 内部错误，且不留部分数据。
  FakeAccountGenerator gen({"1000000000"});
  RegisterService svc(*db_, gen, /*max_account_attempts=*/3);
  auto outcome = svc.register_user("newuser", "pw123456");
  EXPECT_EQ(outcome.kind, RegisterOutcome::Kind::InternalError);
  EXPECT_EQ(CountUsers("newuser"), 0);
}

TEST_F(DbTest, RegisterRejectsInvalidNicknameAndPassword) {
  FakeAccountGenerator gen({"1000000000"});
  RegisterService svc(*db_, gen);

  EXPECT_EQ(svc.register_user("", "pw").kind,
            RegisterOutcome::Kind::InvalidNickname);
  EXPECT_EQ(svc.register_user("   ", "pw").kind,
            RegisterOutcome::Kind::InvalidNickname);
  EXPECT_EQ(svc.register_user(std::string(31, 'a'), "pw").kind,
            RegisterOutcome::Kind::InvalidNickname);

  EXPECT_EQ(svc.register_user("nick", "").kind,
            RegisterOutcome::Kind::InvalidPassword);
  EXPECT_EQ(svc.register_user("nick", std::string(129, 'p')).kind,
            RegisterOutcome::Kind::InvalidPassword);
}

TEST_F(DbTest, DataSurvivesReopenAndAccountNotReused) {
  {
    FakeAccountGenerator gen({"2000000000"});
    RegisterService svc(*db_, gen);
    ASSERT_EQ(svc.register_user("persist", "Pw1").kind,
              RegisterOutcome::Kind::Success);
  }
  db_->close();

  std::string err;
  db_ = oj::Database::open(dir_->db_path(), err);
  ASSERT_NE(db_, nullptr) << err;

  UserStore store(*db_);
  bool found = false;
  UserRecord rec;
  ASSERT_TRUE(store.find_by_account("2000000000", found, rec, err));
  EXPECT_TRUE(found);
  EXPECT_EQ(rec.nickname, "persist");

  // 现存账号不复用：重启后已分配账号仍在库中，再次随机到同一账号会触发唯一性
  // 冲突并换号。此处仅验证「现存账号唯一」这一层，不等于完整证明「永久不复用」——
  // 该保证在当前无用户删除接口的前提下成立；若未来引入物理删除，需另行用墓碑表/
  // 软删除机制验证被删除账号不会重新分配（见 SPEC M1.1 实施说明）。
  FakeAccountGenerator gen({"2000000000", "2000000001"});
  RegisterService svc(*db_, gen);
  auto outcome = svc.register_user("persist2", "Pw2");
  ASSERT_EQ(outcome.kind, RegisterOutcome::Kind::Success);
  EXPECT_NE(outcome.user.account, "2000000000");
}

} // namespace
