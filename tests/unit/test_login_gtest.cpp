// M1.2 登录与身份验证 —— 登录服务与鉴权上下文单元测试（GoogleTest）。
//
// 覆盖对象：
//   - oj::auth::LoginService —— 正确/错误密码、账号不存在、失败提示一致、
//     密码不裁剪、admin 首次改密标记、内部故障返回 500 语义（InternalError）
//   - oj::auth::extract_bearer_token —— Bearer 头解析（大小写、空白、非法格式）
//   - oj::auth::authenticate_request —— token 验证 + 数据库回查（存在性、最新
//     角色/昵称、不信任 token 中过时权限、数据库故障区分于认证失败）
//
// 均使用 /tmp 下的隔离临时数据库，不触碰正式数据库 data/oj.db。
//
// 运行方式：ctest --test-dir build -R login_gtest --output-on-failure
// 或直接执行 build/oj_login_gtest（支持 gtest 全部过滤参数）。

#include <gtest/gtest.h>

#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "auth/context.h"
#include "auth/jwt.h"
#include "auth/login.h"
#include "auth/password.h"
#include "db/database.h"
#include "db/schema.h"
#include "db/users.h"

namespace {

const std::string kSecret = "test-secret-0123456789abcdef";

using oj::UserStore;
using oj::auth::AuthResult;
using oj::auth::LoginOutcome;
using oj::auth::LoginService;

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

// 供登录服务与鉴权上下文测试使用的 Fixture：隔离临时库 + 建表 + 预置 admin。
class AuthDbTest : public ::testing::Test {
protected:
  AuthDbTest() : jwt_(kSecret, 3600) {}

  void SetUp() override {
    dir_ = std::make_unique<TempDir>("gtest_login");
    std::string err;
    db_ = oj::Database::open(dir_->db_path(), err);
    ASSERT_NE(db_, nullptr) << err;
    ASSERT_TRUE(
        oj::initialize_schema(*db_, std::string("AdminSecret123!"), err))
        << err;
  }

  // 创建普通用户（argon2id 哈希），成功返回 id，失败返回 -1。
  std::int64_t CreateUser(const std::string &account,
                          const std::string &nickname,
                          const std::string &password) {
    std::string hash, err;
    if (!oj::auth::hash_password(password, hash, err)) {
      return -1;
    }
    UserStore store(*db_);
    oj::UserRecord rec;
    if (store.create(account, nickname, hash, rec, err) !=
        UserStore::CreateStatus::Success) {
      return -1;
    }
    return rec.id;
  }

  std::string Sign(std::int64_t id) {
    std::string token, err;
    EXPECT_TRUE(jwt_.sign(id, token, err)) << err;
    return token;
  }

  oj::auth::JwtService jwt_;
  std::unique_ptr<TempDir> dir_;
  std::unique_ptr<oj::Database> db_;
};

} // namespace

// ---------------------------------------------------------------------------
// 登录服务
// ---------------------------------------------------------------------------

TEST_F(AuthDbTest, LoginSucceedsWithCorrectCredentials) {
  std::int64_t id = CreateUser("1234567890", "alice", "Secret123");
  ASSERT_GT(id, 0);

  LoginService svc(*db_, jwt_);
  auto outcome = svc.login("1234567890", "Secret123");

  ASSERT_EQ(outcome.kind, LoginOutcome::Kind::Success);
  EXPECT_EQ(outcome.user.id, id);
  EXPECT_EQ(outcome.user.account, "1234567890");
  EXPECT_EQ(outcome.user.nickname, "alice");
  EXPECT_EQ(outcome.user.role, "user");
  EXPECT_EQ(outcome.user.reset_pwd_flag, 0);
  EXPECT_EQ(outcome.expires_in_seconds, 3600);
  EXPECT_FALSE(outcome.token.empty());

  // 返回的 token 可被验证，且身份标识与登录用户一致。
  std::int64_t uid = 0;
  std::string err;
  ASSERT_TRUE(jwt_.verify(outcome.token, uid, err)) << err;
  EXPECT_EQ(uid, id);
}

TEST_F(AuthDbTest, AdminLoginPreservesResetFlag) {
  LoginService svc(*db_, jwt_);
  auto outcome = svc.login("admin", "AdminSecret123!");

  ASSERT_EQ(outcome.kind, LoginOutcome::Kind::Success);
  EXPECT_EQ(outcome.user.role, "admin");
  EXPECT_EQ(outcome.user.account, "admin");
  EXPECT_EQ(outcome.user.reset_pwd_flag, 1);
}

TEST_F(AuthDbTest, WrongPasswordRejected) {
  CreateUser("1234567890", "alice", "Secret123");
  LoginService svc(*db_, jwt_);
  auto outcome = svc.login("1234567890", "WrongPass");
  EXPECT_EQ(outcome.kind, LoginOutcome::Kind::InvalidCredentials);
}

TEST_F(AuthDbTest, NonexistentAccountRejected) {
  LoginService svc(*db_, jwt_);
  auto outcome = svc.login("9999999999", "Whatever1");
  EXPECT_EQ(outcome.kind, LoginOutcome::Kind::InvalidCredentials);
}

// 错误密码与不存在账号的失败提示必须完全一致，避免泄露账号是否存在。
TEST_F(AuthDbTest, FailureMessagesIdentical) {
  CreateUser("1234567890", "alice", "Secret123");
  LoginService svc(*db_, jwt_);

  auto wrong_pw = svc.login("1234567890", "WrongPass");
  auto no_user = svc.login("9999999999", "Whatever1");

  ASSERT_EQ(wrong_pw.kind, LoginOutcome::Kind::InvalidCredentials);
  ASSERT_EQ(no_user.kind, LoginOutcome::Kind::InvalidCredentials);
  EXPECT_EQ(wrong_pw.error, no_user.error);
  EXPECT_FALSE(wrong_pw.error.empty());
}

// 密码不做裁剪/截断：含首尾空白的密码必须原样匹配。
TEST_F(AuthDbTest, PasswordIsNotTrimmed) {
  CreateUser("1234567890", "alice", " pass ");
  LoginService svc(*db_, jwt_);

  EXPECT_EQ(svc.login("1234567890", " pass ").kind,
            LoginOutcome::Kind::Success);
  EXPECT_EQ(svc.login("1234567890", "pass").kind,
            LoginOutcome::Kind::InvalidCredentials);
}

// 数据库故障不应伪装成用户密码错误。
TEST_F(AuthDbTest, DbFailureReturnsInternalError) {
  CreateUser("1234567890", "alice", "Secret123");
  LoginService svc(*db_, jwt_);

  db_->close();
  auto outcome = svc.login("1234567890", "Secret123");
  EXPECT_EQ(outcome.kind, LoginOutcome::Kind::InternalError);
  EXPECT_EQ(outcome.error, "内部错误");
}

// ---------------------------------------------------------------------------
// Bearer token 解析
// ---------------------------------------------------------------------------

TEST(ExtractBearerTokenTest, ParsesStandardHeader) {
  std::string token;
  ASSERT_TRUE(oj::auth::extract_bearer_token("Bearer abc.def.ghi", token));
  EXPECT_EQ(token, "abc.def.ghi");
}

TEST(ExtractBearerTokenTest, SchemeIsCaseInsensitive) {
  std::string token;
  EXPECT_TRUE(oj::auth::extract_bearer_token("bearer abc", token));
  EXPECT_EQ(token, "abc");
  EXPECT_TRUE(oj::auth::extract_bearer_token("BEARER abc", token));
  EXPECT_EQ(token, "abc");
}

TEST(ExtractBearerTokenTest, AllowsSurroundingWhitespace) {
  std::string token;
  ASSERT_TRUE(oj::auth::extract_bearer_token("  Bearer   abc  ", token));
  EXPECT_EQ(token, "abc");
}

TEST(ExtractBearerTokenTest, RejectsMissingToken) {
  std::string token;
  EXPECT_FALSE(oj::auth::extract_bearer_token("Bearer", token));
  EXPECT_FALSE(oj::auth::extract_bearer_token("Bearer   ", token));
}

TEST(ExtractBearerTokenTest, RejectsNonBearerScheme) {
  std::string token;
  EXPECT_FALSE(oj::auth::extract_bearer_token("Basic abc", token));
  EXPECT_FALSE(oj::auth::extract_bearer_token("Token abc", token));
}

TEST(ExtractBearerTokenTest, RejectsEmptyHeader) {
  std::string token;
  EXPECT_FALSE(oj::auth::extract_bearer_token("", token));
  EXPECT_FALSE(oj::auth::extract_bearer_token("   ", token));
}

TEST(ExtractBearerTokenTest, RejectsTokenContainingWhitespace) {
  std::string token;
  EXPECT_FALSE(oj::auth::extract_bearer_token("Bearer ab c", token));
}

TEST(ExtractBearerTokenTest, RejectsNoSeparatorAfterScheme) {
  std::string token;
  EXPECT_FALSE(oj::auth::extract_bearer_token("Bearerabc", token));
}

// ---------------------------------------------------------------------------
// 鉴权上下文（token 验证 + 数据库回查）
// ---------------------------------------------------------------------------

TEST_F(AuthDbTest, AuthenticateReturnsDbUser) {
  std::int64_t id = CreateUser("1234567890", "alice", "Secret123");
  ASSERT_GT(id, 0);

  UserStore store(*db_);
  oj::auth::AuthUser user;
  std::string err;
  ASSERT_EQ(oj::auth::authenticate_request(jwt_, store, Sign(id), user, err),
            AuthResult::Ok);
  EXPECT_EQ(user.id, id);
  EXPECT_EQ(user.account, "1234567890");
  EXPECT_EQ(user.nickname, "alice");
  EXPECT_EQ(user.role, "user");
  EXPECT_EQ(user.reset_pwd_flag, 0);
}

TEST_F(AuthDbTest, AuthenticateInvalidTokenUnauthorized) {
  UserStore store(*db_);
  oj::auth::AuthUser user;
  std::string err;
  EXPECT_EQ(oj::auth::authenticate_request(jwt_, store, "not-a-jwt", user, err),
            AuthResult::Unauthorized);
}

// 签名有效但引用不存在的用户 -> 拒绝，不能凭空获得身份。
TEST_F(AuthDbTest, AuthenticateNonexistentUserUnauthorized) {
  UserStore store(*db_);
  oj::auth::AuthUser user;
  std::string err;
  EXPECT_EQ(oj::auth::authenticate_request(jwt_, store, Sign(999999), user, err),
            AuthResult::Unauthorized);
}

// 鉴权应回查数据库取最新信息，而非信任 token 中可能过时的角色/昵称。
TEST_F(AuthDbTest, AuthenticateReflectsLatestDbChanges) {
  std::int64_t id = CreateUser("1234567890", "alice", "Secret123");
  ASSERT_GT(id, 0);
  std::string token = Sign(id);

  // 直接修改数据库中的昵称与角色。
  std::string err;
  oj::Statement stmt;
  ASSERT_TRUE(db_->prepare(
      "UPDATE users SET nickname = 'alice2', role = 'admin' WHERE id = ?",
      stmt, err))
      << err;
  stmt.bind(1, static_cast<sqlite3_int64>(id));
  ASSERT_EQ(stmt.step(), SQLITE_DONE);

  UserStore store(*db_);
  oj::auth::AuthUser user;
  ASSERT_EQ(oj::auth::authenticate_request(jwt_, store, token, user, err),
            AuthResult::Ok);
  EXPECT_EQ(user.nickname, "alice2");
  EXPECT_EQ(user.role, "admin");
}

TEST_F(AuthDbTest, AuthenticateDbFailureInternalError) {
  std::int64_t id = CreateUser("1234567890", "alice", "Secret123");
  ASSERT_GT(id, 0);
  std::string token = Sign(id);

  UserStore store(*db_);
  oj::auth::AuthUser user;
  std::string err;

  db_->close();
  EXPECT_EQ(oj::auth::authenticate_request(jwt_, store, token, user, err),
            AuthResult::InternalError);
}
