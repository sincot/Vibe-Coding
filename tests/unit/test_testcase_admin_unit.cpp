// 管理员测试用例写入字段校验单元测试（M2.2）。
//
// 覆盖新增与部分更新请求体的解析/校验：必填字段、类型、空串与字段缺失的区分、
// 文本长度上限、ord 取值范围，以及服务端管理字段（id/problem_id/is_sample）被忽略。
// 数据库写入、事务、归属与权限、判题衔接由 admin_testcases_api 集成测试验证。
//
// 运行方式：ctest --test-dir build -R testcase_admin_unit --output-on-failure
// 或直接执行 build/oj_testcase_admin_unit。

#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "problem/testcase_validation.h"

namespace {

using nlohmann::json;
using oj::problem::parse_create_testcase;
using oj::problem::parse_update_testcase;
using oj::problem::TestcaseData;
using oj::problem::TestcasePatch;

json parse(const std::string &raw) { return json::parse(raw); }

TestcaseData create_ok(const std::string &raw) {
  TestcaseData data;
  std::string error;
  EXPECT_TRUE(parse_create_testcase(parse(raw), data, error)) << error;
  return data;
}

// ---------------------------------------------------------------------------
// 新增：字段与默认值
// ---------------------------------------------------------------------------

TEST(CreateTestcase, RequiredFieldsAcceptedAndTrimNotApplied) {
  TestcaseData data = create_ok(
      R"({"input":"  1\t2\n\n","output":" 3\n"})");
  // 不 trim、不做输出归一化：原样保留空格、制表符与换行。
  EXPECT_EQ(data.input, "  1\t2\n\n");
  EXPECT_EQ(data.output, " 3\n");
  EXPECT_FALSE(data.ord.has_value());
}

TEST(CreateTestcase, EmptyStringsAreLegalAndDistinctFromMissing) {
  TestcaseData data = create_ok(R"({"input":"","output":""})");
  EXPECT_EQ(data.input, "");
  EXPECT_EQ(data.output, "");

  TestcaseData missing;
  std::string error;
  EXPECT_FALSE(parse_create_testcase(parse(R"({"output":"x"})"), missing, error));
  EXPECT_FALSE(
      parse_create_testcase(parse(R"({"input":"x"})"), missing, error));
}

TEST(CreateTestcase, ExplicitOrdAccepted) {
  TestcaseData data = create_ok(R"({"input":"1","output":"2","ord":7})");
  ASSERT_TRUE(data.ord.has_value());
  EXPECT_EQ(*data.ord, 7);
}

TEST(CreateTestcase, RejectsWrongTypesAndNull) {
  TestcaseData data;
  std::string error;
  EXPECT_FALSE(parse_create_testcase(parse(R"({"input":1,"output":"x"})"), data,
                                     error));
  EXPECT_FALSE(parse_create_testcase(parse(R"({"input":"x","output":[]})"),
                                     data, error));
  EXPECT_FALSE(parse_create_testcase(parse(R"({"input":null,"output":"x"})"),
                                     data, error));
  EXPECT_FALSE(parse_create_testcase(parse(R"("not an object")"), data, error));
}

TEST(CreateTestcase, RejectsMissingFields) {
  TestcaseData data;
  std::string error;
  EXPECT_FALSE(parse_create_testcase(parse(R"({})"), data, error));
  EXPECT_FALSE(parse_create_testcase(parse(R"({"input":"x"})"), data, error));
  EXPECT_FALSE(parse_create_testcase(parse(R"({"output":"y"})"), data, error));
}

TEST(CreateTestcase, RejectsOverlongText) {
  const std::string big =
      std::string(oj::problem::kMaxTestcaseTextBytes + 1, 'a');
  TestcaseData data;
  std::string error;
  EXPECT_FALSE(parse_create_testcase(
      json{{"input", big}, {"output", "y"}}, data, error));
  EXPECT_FALSE(parse_create_testcase(
      json{{"input", "x"}, {"output", big}}, data, error));

  const std::string exact(oj::problem::kMaxTestcaseTextBytes, 'a');
  EXPECT_TRUE(parse_create_testcase(json{{"input", exact}, {"output", exact}},
                                    data, error))
      << error;
}

TEST(CreateTestcase, OrdRange) {
  TestcaseData data;
  std::string error;
  EXPECT_FALSE(parse_create_testcase(
      parse(R"({"input":"x","output":"y","ord":-1})"), data, error));
  EXPECT_FALSE(parse_create_testcase(
      parse(R"({"input":"x","output":"y","ord":1000001})"), data, error));
  // 超出 64 位无符号整数的取值必须安全拒绝，不能因溢出误判为合法。
  EXPECT_FALSE(parse_create_testcase(
      parse(R"({"input":"x","output":"y","ord":18446744073709551615})"), data,
      error));
  EXPECT_FALSE(parse_create_testcase(
      parse(R"({"input":"x","output":"y","ord":1.5})"), data, error));
  EXPECT_FALSE(parse_create_testcase(
      parse(R"({"input":"x","output":"y","ord":true})"), data, error));
  EXPECT_FALSE(parse_create_testcase(
      parse(R"({"input":"x","output":"y","ord":"3"})"), data, error));
  EXPECT_FALSE(parse_create_testcase(
      parse(R"({"input":"x","output":"y","ord":null})"), data, error));

  EXPECT_TRUE(parse_create_testcase(
      parse(R"({"input":"x","output":"y","ord":0})"), data, error))
      << error;
  EXPECT_TRUE(parse_create_testcase(
      parse(R"({"input":"x","output":"y","ord":1000000})"), data, error))
      << error;
}

TEST(CreateTestcase, IgnoresServerManagedFields) {
  TestcaseData data = create_ok(
      R"({"input":"1","output":"2","id":999,"problem_id":123,"is_sample":1,
          "created_at":"1999-01-01 00:00:00"})");
  EXPECT_EQ(data.input, "1");
  EXPECT_EQ(data.output, "2");
  EXPECT_FALSE(data.ord.has_value());
}

// ---------------------------------------------------------------------------
// 修改：部分更新语义
// ---------------------------------------------------------------------------

TEST(UpdateTestcase, OnlyProvidedFieldsAreSet) {
  TestcasePatch patch;
  std::string error;
  ASSERT_TRUE(parse_update_testcase(parse(R"({"input":"只有输入"})"), patch,
                                    error))
      << error;
  ASSERT_TRUE(patch.input.has_value());
  EXPECT_EQ(*patch.input, "只有输入");
  EXPECT_FALSE(patch.output.has_value());
  EXPECT_FALSE(patch.ord.has_value());

  TestcasePatch ord_patch;
  ASSERT_TRUE(parse_update_testcase(parse(R"({"ord":3})"), ord_patch, error))
      << error;
  EXPECT_FALSE(ord_patch.input.has_value());
  EXPECT_FALSE(ord_patch.output.has_value());
  ASSERT_TRUE(ord_patch.ord.has_value());
  EXPECT_EQ(*ord_patch.ord, 3);
}

TEST(UpdateTestcase, ExplicitEmptyStringMeansClear) {
  TestcasePatch patch;
  std::string error;
  ASSERT_TRUE(parse_update_testcase(
      parse(R"({"input":"","output":""})"), patch, error))
      << error;
  ASSERT_TRUE(patch.input.has_value());
  ASSERT_TRUE(patch.output.has_value());
  EXPECT_EQ(*patch.input, "");
  EXPECT_EQ(*patch.output, "");
}

TEST(UpdateTestcase, RejectsEmptyOrUnknownOnly) {
  TestcasePatch patch;
  std::string error;
  EXPECT_FALSE(parse_update_testcase(parse(R"({})"), patch, error));
  EXPECT_FALSE(parse_update_testcase(
      parse(R"({"id":5,"problem_id":1,"is_sample":1})"), patch, error));
  EXPECT_FALSE(parse_update_testcase(
      parse(R"({"created_at":"1999-01-01 00:00:00"})"), patch, error));
}

TEST(UpdateTestcase, RejectsInvalidValues) {
  TestcasePatch patch;
  std::string error;
  EXPECT_FALSE(parse_update_testcase(parse(R"({"input":1})"), patch, error));
  EXPECT_FALSE(parse_update_testcase(parse(R"({"output":null})"), patch, error));
  EXPECT_FALSE(parse_update_testcase(parse(R"({"ord":-1})"), patch, error));
  EXPECT_FALSE(parse_update_testcase(parse(R"({"ord":1000001})"), patch, error));
  EXPECT_FALSE(parse_update_testcase(parse(R"({"ord":2.5})"), patch, error));
  EXPECT_FALSE(parse_update_testcase(parse(R"("not an object")"), patch, error));

  const std::string big =
      std::string(oj::problem::kMaxTestcaseTextBytes + 1, 'a');
  EXPECT_FALSE(parse_update_testcase(json{{"output", big}}, patch, error));
}

TEST(UpdateTestcase, PatchEmptyFlag) {
  TestcasePatch patch;
  EXPECT_TRUE(patch.empty());
  patch.input = std::string("");
  EXPECT_FALSE(patch.empty());
}

} // namespace
