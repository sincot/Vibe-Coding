// 管理员用户接口请求参数校验单元测试（M2.4）。
//
// 覆盖 PUT /api/admin/users 请求体的纯解析与校验（action/user_id/new_password/
// role）：必填字段、类型、枚举取值、密码规则复用、越权/无关字段被忽略，以及
// 用户列表分页参数 page 的边界。数据库读写、最后管理员保护与权限由
// admin_users_api 集成测试验证。
//
// 运行方式：ctest --test-dir build -R user_admin_unit --output-on-failure
// 或直接执行 build/oj_user_admin_unit。

#include <gtest/gtest.h>

#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "user/admin_user_validation.h"

namespace {

using nlohmann::json;
using oj::useradmin::Action;
using oj::useradmin::is_valid_role;
using oj::useradmin::parse_page;
using oj::useradmin::parse_request;
using oj::useradmin::Request;

json parse(const std::string &raw) { return json::parse(raw); }

Request parse_ok(const std::string &raw) {
  Request out;
  std::string error;
  EXPECT_TRUE(parse_request(parse(raw), out, error)) << error;
  return out;
}

// ---------------------------------------------------------------------------
// action 与请求体结构
// ---------------------------------------------------------------------------

TEST(ParseRequest, ResetPasswordAccepted) {
  Request out = parse_ok(
      R"({"action":"reset_password","user_id":7,"new_password":"NewPass123"})");
  EXPECT_EQ(out.action, Action::ResetPassword);
  EXPECT_EQ(out.user_id, 7);
  EXPECT_EQ(out.new_password, "NewPass123");
}

TEST(ParseRequest, ChangeRoleAccepted) {
  Request out =
      parse_ok(R"({"action":"change_role","user_id":9,"role":"admin"})");
  EXPECT_EQ(out.action, Action::ChangeRole);
  EXPECT_EQ(out.user_id, 9);
  EXPECT_EQ(out.role, "admin");
}

TEST(ParseRequest, NonObjectBodyRejected) {
  Request out;
  std::string error;
  for (const char *raw : {R"([1,2,3])", R"("x")", R"(null)", R"(123)",
                          R"(true)"}) {
    out = Request{};
    EXPECT_FALSE(parse_request(parse(raw), out, error)) << raw;
  }
}

TEST(ParseRequest, ActionRequiredStringAndKnown) {
  Request out;
  std::string error;
  EXPECT_FALSE(parse_request(parse(R"({"user_id":1})"), out, error));
  EXPECT_FALSE(parse_request(parse(R"({"action":1,"user_id":1})"), out, error));
  EXPECT_FALSE(parse_request(parse(R"({"action":"","user_id":1})"), out, error));
  EXPECT_FALSE(parse_request(
      parse(R"({"action":"delete_user","user_id":1})"), out, error));
  // 大小写敏感、不裁剪空白：仅接受规范小写且无空白的取值。
  EXPECT_FALSE(parse_request(
      parse(R"({"action":"Reset_Password","user_id":1})"), out, error));
  EXPECT_FALSE(parse_request(
      parse(R"({"action":" reset_password ","user_id":1,"new_password":"Abcd1234"})"),
      out, error));
}

// ---------------------------------------------------------------------------
// user_id
// ---------------------------------------------------------------------------

TEST(ParseRequest, UserIdMustBePositiveInteger) {
  Request out;
  std::string error;
  for (const char *raw : {
           R"({"action":"change_role","user_id":0,"role":"user"})",
           R"({"action":"change_role","user_id":-1,"role":"user"})",
           R"({"action":"change_role","user_id":1.5,"role":"user"})",
           R"({"action":"change_role","user_id":1.0,"role":"user"})",
           R"({"action":"change_role","user_id":"1","role":"user"})",
           R"({"action":"change_role","user_id":true,"role":"user"})",
           R"({"action":"change_role","user_id":null,"role":"user"})",
           R"({"action":"change_role","role":"user"})",
       }) {
    out = Request{};
    EXPECT_FALSE(parse_request(parse(raw), out, error)) << raw;
  }
  // 超大整数（超出 19 位）不崩溃且被拒绝。
  EXPECT_FALSE(parse_request(
      parse(R"({"action":"change_role","user_id":99999999999999999999999,"role":"user"})"),
      out, error));
  // 边界：int64 最大值合法；超过 int64 的无符号整数被拒绝。
  out = parse_ok(
      R"({"action":"change_role","user_id":9223372036854775807,"role":"user"})");
  EXPECT_EQ(out.user_id, std::numeric_limits<std::int64_t>::max());
  EXPECT_FALSE(parse_request(
      parse(R"({"action":"change_role","user_id":9223372036854775808,"role":"user"})"),
      out, error));
}

// ---------------------------------------------------------------------------
// reset_password：密码规则复用，不读取旧密码，越权字段忽略
// ---------------------------------------------------------------------------

TEST(ParseRequest, ResetPasswordReusesPasswordRule) {
  Request out;
  std::string error;
  EXPECT_FALSE(parse_request(
      parse(R"({"action":"reset_password","user_id":1,"new_password":""})"),
      out, error));
  std::string too_long(129, 'a');
  EXPECT_FALSE(parse_request(
      parse(R"({"action":"reset_password","user_id":1,"new_password":")" +
            too_long + R"("})"),
      out, error));
  // 边界：恰好 128 合法。
  std::string max_len(128, 'a');
  out = parse_ok(R"({"action":"reset_password","user_id":1,"new_password":")" +
                 max_len + R"("})");
  EXPECT_EQ(out.new_password.size(), 128u);
  // 缺失与类型错误。
  EXPECT_FALSE(parse_request(
      parse(R"({"action":"reset_password","user_id":1})"), out, error));
  EXPECT_FALSE(parse_request(
      parse(R"({"action":"reset_password","user_id":1,"new_password":123})"),
      out, error));
}

TEST(ParseRequest, ResetPasswordPreservesWhitespaceNoTrim) {
  Request out = parse_ok(
      R"({"action":"reset_password","user_id":1,"new_password":"  a b  "})");
  EXPECT_EQ(out.new_password, "  a b  ");
}

TEST(ParseRequest, ResetPasswordDoesNotRequireOldPassword) {
  // 管理员重置不需要目标用户旧密码：不提供 old_password 也可解析；
  // 即使客户端夹带 old_password，也不进入解析结果（忽略）。
  Request out = parse_ok(
      R"({"action":"reset_password","user_id":2,"new_password":"Fresh123"})");
  EXPECT_EQ(out.new_password, "Fresh123");

  Request with_old = parse_ok(
      R"({"action":"reset_password","user_id":2,"new_password":"Fresh123",)"
      R"("old_password":"whatever"})");
  EXPECT_EQ(with_old.new_password, "Fresh123");
  EXPECT_EQ(with_old.user_id, 2);
}

TEST(ParseRequest, ResetPasswordIgnoresUnrelatedFields) {
  // 客户端夹带 account/nickname/role/reset_pwd_flag/password_hash 等字段一律忽略，
  // 不进入解析结果，也无法借此改角色或指定其它用户。
  Request out = parse_ok(
      R"({"action":"reset_password","user_id":3,"new_password":"Abcd1234",)"
      R"("account":"admin","nickname":"hacker","role":"admin",)"
      R"("reset_pwd_flag":0,"password_hash":"x"})");
  EXPECT_EQ(out.action, Action::ResetPassword);
  EXPECT_EQ(out.user_id, 3);
  EXPECT_EQ(out.new_password, "Abcd1234");
  EXPECT_TRUE(out.role.empty());
}

// ---------------------------------------------------------------------------
// change_role：角色枚举，不读取密码
// ---------------------------------------------------------------------------

TEST(ParseRequest, ChangeRoleOnlyAcceptsKnownRoles) {
  Request out;
  std::string error;
  for (const char *role : {"admin", "user"}) {
    out = Request{};
    std::string raw = std::string("{\"action\":\"change_role\",\"user_id\":1,"
                                  "\"role\":\"") +
                      role + "\"}";
    EXPECT_TRUE(parse_request(parse(raw), out, error)) << error;
    EXPECT_EQ(out.role, role);
  }
  for (const char *raw : {
           R"({"action":"change_role","user_id":1,"role":"superadmin"})",
           R"({"action":"change_role","user_id":1,"role":"Admin"})",
           R"({"action":"change_role","user_id":1,"role":" admin "})",
           R"({"action":"change_role","user_id":1})",
           R"({"action":"change_role","user_id":1,"role":1})",
       }) {
    out = Request{};
    EXPECT_FALSE(parse_request(parse(raw), out, error)) << raw;
  }
}

TEST(ParseRequest, ChangeRoleIgnoresPasswordField) {
  // change_role 请求里夹带 new_password 不影响角色解析，也不会被当作密码更新。
  Request out = parse_ok(
      R"({"action":"change_role","user_id":5,"role":"user","new_password":"x"})");
  EXPECT_EQ(out.action, Action::ChangeRole);
  EXPECT_EQ(out.role, "user");
  EXPECT_TRUE(out.new_password.empty());
}

// ---------------------------------------------------------------------------
// role 取值校验
// ---------------------------------------------------------------------------

TEST(IsValidRole, Enumeration) {
  EXPECT_TRUE(is_valid_role("admin"));
  EXPECT_TRUE(is_valid_role("user"));
  EXPECT_FALSE(is_valid_role(""));
  EXPECT_FALSE(is_valid_role("Admin"));
  EXPECT_FALSE(is_valid_role("administrator"));
}

// ---------------------------------------------------------------------------
// page 分页参数
// ---------------------------------------------------------------------------

TEST(ParsePage, DefaultsAndBoundaries) {
  int page = -1;
  std::string error;
  EXPECT_TRUE(parse_page("", page, error));
  EXPECT_EQ(page, 1);
  EXPECT_TRUE(parse_page("1", page, error));
  EXPECT_EQ(page, 1);
  EXPECT_TRUE(parse_page("0002", page, error));
  EXPECT_EQ(page, 2);
  EXPECT_TRUE(parse_page(" 3 ", page, error));
  EXPECT_EQ(page, 3);
  EXPECT_TRUE(parse_page("   ", page, error));
  EXPECT_EQ(page, 1);
  EXPECT_TRUE(parse_page("1000000", page, error));
  EXPECT_EQ(page, 1000000);
}

TEST(ParsePage, InvalidRejected) {
  for (const char *raw : {"0", "-1", "abc", "1.5", "+1", "1000001",
                          "9999999", "00000000"}) {
    int page = 0;
    std::string error;
    EXPECT_FALSE(parse_page(raw, page, error)) << raw;
    EXPECT_FALSE(error.empty()) << raw;
  }
  // 超长数字串（有效位数 > 7）被拒绝，不触发溢出。
  int page = 0;
  std::string error;
  EXPECT_FALSE(parse_page("123456789", page, error));
  EXPECT_FALSE(parse_page("00000000010000000", page, error));
}

} // namespace
