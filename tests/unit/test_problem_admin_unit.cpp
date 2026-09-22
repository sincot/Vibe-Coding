// 管理员题目写入字段校验单元测试（M2.1）。
//
// 覆盖创建与部分更新请求体的解析/校验：必填字段、类型、长度、难度枚举、标签结构、
// 数值范围与默认值；以及标签拼接。数据库写入、事务与权限由 admin_problems_api
// 集成测试验证。
//
// 运行方式：ctest --test-dir build -R problem_admin_unit --output-on-failure
// 或直接执行 build/oj_problem_admin_unit。

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "problem/validation.h"

namespace {

using nlohmann::json;
using oj::problem::join_tags;
using oj::problem::parse_create_problem;
using oj::problem::parse_update_problem;
using oj::problem::ProblemData;
using oj::problem::ProblemPatch;

json parse(const std::string &raw) { return json::parse(raw); }

ProblemData create(const std::string &raw) {
  ProblemData data;
  std::string error;
  EXPECT_TRUE(parse_create_problem(parse(raw), data, error)) << error;
  return data;
}

// ---------------------------------------------------------------------------
// 创建：默认值与字段解析
// ---------------------------------------------------------------------------

TEST(CreateProblem, RequiredFieldsUseSpecDefaults) {
  ProblemData data;
  std::string error;
  ASSERT_TRUE(
      parse_create_problem(parse(R"({"title":"A+B","difficulty":"easy"})"),
                           data, error))
      << error;
  EXPECT_EQ(data.title, "A+B");
  EXPECT_EQ(data.difficulty, "easy");
  EXPECT_EQ(data.description, "");
  EXPECT_TRUE(data.tags.empty());
  EXPECT_EQ(data.time_limit_ms, 2000);
  EXPECT_EQ(data.memory_limit_kb, 65536);
  EXPECT_TRUE(data.visible);
  EXPECT_TRUE(data.samples.empty());
}

TEST(CreateProblem, FullFieldsParsedAndNormalized) {
  ProblemData data = create(R"({
    "title":"  求最大值  ",
    "description":"给定 n 个整数",
    "difficulty":"hard",
    "tags":[" 数组 ","入门"],
    "time_limit_ms":1500,
    "memory_limit_kb":131072,
    "visible":false,
    "samples":[{"input":"1 2\n","output":"2\n"}]
  })");
  EXPECT_EQ(data.title, "求最大值");
  EXPECT_EQ(data.description, "给定 n 个整数");
  EXPECT_EQ(data.difficulty, "hard");
  ASSERT_EQ(data.tags.size(), 2u);
  EXPECT_EQ(data.tags[0], "数组");
  EXPECT_EQ(data.tags[1], "入门");
  EXPECT_EQ(data.time_limit_ms, 1500);
  EXPECT_EQ(data.memory_limit_kb, 131072);
  EXPECT_FALSE(data.visible);
  ASSERT_EQ(data.samples.size(), 1u);
  EXPECT_EQ(data.samples[0].input, "1 2\n");
  EXPECT_EQ(data.samples[0].output, "2\n");
}

TEST(CreateProblem, EmptySamplesAndTagsAreAllowed) {
  ProblemData data =
      create(R"({"title":"T","difficulty":"medium","tags":[],"samples":[]})");
  EXPECT_TRUE(data.tags.empty());
  EXPECT_TRUE(data.samples.empty());
}

TEST(CreateProblem, RejectsNonObjectBody) {
  ProblemData data;
  std::string error;
  EXPECT_FALSE(parse_create_problem(parse(R"([])"), data, error));
  EXPECT_FALSE(error.empty());
}

TEST(CreateProblem, RejectsMissingRequiredFields) {
  ProblemData data;
  std::string error;
  EXPECT_FALSE(parse_create_problem(parse(R"({"difficulty":"easy"})"), data,
                                    error));
  EXPECT_FALSE(parse_create_problem(parse(R"({"title":"T"})"), data, error));
}

TEST(CreateProblem, RejectsWrongTypes) {
  ProblemData data;
  std::string error;
  EXPECT_FALSE(
      parse_create_problem(parse(R"({"title":123,"difficulty":"easy"})"),
                           data, error));
  EXPECT_FALSE(
      parse_create_problem(parse(R"({"title":"T","difficulty":1})"), data,
                           error));
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","description":5})"), data,
      error));
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","visible":"yes"})"), data,
      error));
}

TEST(CreateProblem, RejectsEmptyAndOverlongTitle) {
  ProblemData data;
  std::string error;
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"   ","difficulty":"easy"})"), data, error));
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"","difficulty":"easy"})"), data, error));
  const std::string long_title(201, 'x');
  EXPECT_FALSE(parse_create_problem(
      json{{"title", long_title}, {"difficulty", "easy"}}, data, error));
}

TEST(CreateProblem, RejectsInvalidDifficulty) {
  ProblemData data;
  std::string error;
  for (const char *value : {"Easy", "EASY", "simple", "", "中等"}) {
    EXPECT_FALSE(parse_create_problem(
        json{{"title", "T"}, {"difficulty", value}}, data, error))
        << value;
  }
}

TEST(CreateProblem, RejectsInvalidTags) {
  ProblemData data;
  std::string error;
  // 非数组
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","tags":"a,b"})"), data, error));
  // 元素非字符串
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","tags":[1,2]})"), data, error));
  // 空标签 / 仅空白
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","tags":["  "]})"), data,
      error));
  // 含逗号（会破坏逗号分隔存储）
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","tags":["a,b"]})"), data,
      error));
  // 过长
  EXPECT_FALSE(parse_create_problem(
      json{{"title", "T"},
           {"difficulty", "easy"},
           {"tags", json::array({std::string(31, 'x')})}},
      data, error));
  // 过多
  json many = json::array();
  for (int i = 0; i < 21; ++i) {
    many.push_back("t" + std::to_string(i));
  }
  EXPECT_FALSE(parse_create_problem(
      json{{"title", "T"}, {"difficulty", "easy"}, {"tags", many}}, data,
      error));
  // 重复
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","tags":["a","a"]})"), data,
      error));
}

TEST(CreateProblem, TimeLimitRange) {
  ProblemData data;
  std::string error;
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","time_limit_ms":0})"), data,
      error));
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","time_limit_ms":-5})"), data,
      error));
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","time_limit_ms":1.5})"), data,
      error));
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","time_limit_ms":60001})"),
      data, error));
  EXPECT_EQ(create(R"({"title":"T","difficulty":"easy","time_limit_ms":1})")
                .time_limit_ms,
            1);
  EXPECT_EQ(create(R"({"title":"T","difficulty":"easy","time_limit_ms":60000})")
                .time_limit_ms,
            60000);
}

TEST(CreateProblem, MemoryLimitRange) {
  ProblemData data;
  std::string error;
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","memory_limit_kb":0})"), data,
      error));
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","memory_limit_kb":-1})"),
      data, error));
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","memory_limit_kb":2.5})"),
      data, error));
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","memory_limit_kb":1048577})"),
      data, error));
  EXPECT_EQ(create(R"({"title":"T","difficulty":"easy","memory_limit_kb":1})")
                .memory_limit_kb,
            1);
  EXPECT_EQ(
      create(R"({"title":"T","difficulty":"easy","memory_limit_kb":1048576})")
          .memory_limit_kb,
      1048576);
}

TEST(CreateProblem, RejectsInvalidSamples) {
  ProblemData data;
  std::string error;
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","samples":"x"})"), data,
      error));
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","samples":[{"input":"1"}]})"),
      data, error));
  EXPECT_FALSE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","samples":[{"input":1,"output":"2"}]})"),
      data, error));
  // 超过 50 组
  json samples = json::array();
  for (int i = 0; i < 51; ++i) {
    samples.push_back(json{{"input", "1"}, {"output", "2"}});
  }
  EXPECT_FALSE(parse_create_problem(
      json{{"title", "T"}, {"difficulty", "easy"}, {"samples", samples}}, data,
      error));
  // 字段过长
  EXPECT_FALSE(parse_create_problem(
      json{{"title", "T"},
           {"difficulty", "easy"},
           {"samples", json::array({json{{"input", std::string(65537, 'x')},
                                         {"output", "2"}}})}},
      data, error));
}

// ---------------------------------------------------------------------------
// 更新：部分更新语义
// ---------------------------------------------------------------------------

TEST(UpdateProblem, RejectsEmptyOrUnknownFields) {
  ProblemPatch patch;
  std::string error;
  EXPECT_FALSE(parse_update_problem(parse(R"({})"), patch, error));
  EXPECT_FALSE(error.empty());
  patch = ProblemPatch{};
  error.clear();
  // 服务端管理字段 / 未知字段被忽略，视同没有可更新字段
  EXPECT_FALSE(parse_update_problem(
      parse(R"({"id":9,"created_at":"2000-01-01","seed_key":"x","foo":1})"),
      patch, error));
  EXPECT_FALSE(
      parse_update_problem(parse(R"([])"), patch, error));
}

TEST(UpdateProblem, OnlyProvidedFieldsAreSet) {
  ProblemPatch patch;
  std::string error;
  ASSERT_TRUE(
      parse_update_problem(parse(R"({"title":"  新标题 "})"), patch, error))
      << error;
  ASSERT_TRUE(patch.title.has_value());
  EXPECT_EQ(*patch.title, "新标题");
  EXPECT_FALSE(patch.description.has_value());
  EXPECT_FALSE(patch.difficulty.has_value());
  EXPECT_FALSE(patch.tags.has_value());
  EXPECT_FALSE(patch.time_limit_ms.has_value());
  EXPECT_FALSE(patch.memory_limit_kb.has_value());
  EXPECT_FALSE(patch.visible.has_value());
  EXPECT_FALSE(patch.samples.has_value());
}

TEST(UpdateProblem, ParsesVisibilityAndLimits) {
  ProblemPatch patch;
  std::string error;
  ASSERT_TRUE(parse_update_problem(
      parse(R"({"visible":false,"time_limit_ms":3000,"memory_limit_kb":32768})"),
      patch, error));
  ASSERT_TRUE(patch.visible.has_value());
  EXPECT_FALSE(*patch.visible);
  ASSERT_TRUE(patch.time_limit_ms.has_value());
  EXPECT_EQ(*patch.time_limit_ms, 3000);
  ASSERT_TRUE(patch.memory_limit_kb.has_value());
  EXPECT_EQ(*patch.memory_limit_kb, 32768);
}

TEST(UpdateProblem, ExplicitEmptySamplesMeansClear) {
  ProblemPatch patch;
  std::string error;
  ASSERT_TRUE(
      parse_update_problem(parse(R"({"samples":[]})"), patch, error));
  ASSERT_TRUE(patch.samples.has_value());
  EXPECT_TRUE(patch.samples->empty());
}

TEST(UpdateProblem, RejectsInvalidValues) {
  ProblemPatch patch;
  std::string error;
  EXPECT_FALSE(
      parse_update_problem(parse(R"({"difficulty":"nope"})"), patch, error));
  patch = ProblemPatch{};
  EXPECT_FALSE(
      parse_update_problem(parse(R"({"time_limit_ms":0})"), patch, error));
  patch = ProblemPatch{};
  EXPECT_FALSE(
      parse_update_problem(parse(R"({"visible":1})"), patch, error));
  patch = ProblemPatch{};
  EXPECT_FALSE(
      parse_update_problem(parse(R"({"tags":["a,b"]})"), patch, error));
}

// ---------------------------------------------------------------------------
// 补充：边界值与类型
// ---------------------------------------------------------------------------

TEST(CreateProblem, AcceptsBoundaryValues) {
  ProblemData data;
  std::string error;
  // title 恰好 200 字节
  ASSERT_TRUE(parse_create_problem(
      json{{"title", std::string(200, 'x')}, {"difficulty", "easy"}}, data,
      error)) << error;
  // description 恰好 64 KiB
  ASSERT_TRUE(parse_create_problem(
      json{{"title", "T"},
           {"difficulty", "easy"},
           {"description", std::string(65536, 'd')}},
      data, error)) << error;
  // 单个标签恰好 30 字节
  ASSERT_TRUE(parse_create_problem(
      json{{"title", "T"},
           {"difficulty", "easy"},
           {"tags", json::array({std::string(30, 'g')})}},
      data, error)) << error;
  // 标签数量恰好 20
  json tags = json::array();
  for (int i = 0; i < 20; ++i) {
    tags.push_back("t" + std::to_string(i));
  }
  ASSERT_TRUE(parse_create_problem(
      json{{"title", "T"}, {"difficulty", "easy"}, {"tags", tags}}, data,
      error)) << error;
  // 样例数量恰好 50、样例字段恰好 64 KiB
  json samples = json::array();
  for (int i = 0; i < 49; ++i) {
    samples.push_back(json{{"input", "1"}, {"output", "2"}});
  }
  samples.push_back(json{{"input", std::string(65536, 'i')}, {"output", "2"}});
  ASSERT_TRUE(parse_create_problem(
      json{{"title", "T"}, {"difficulty", "easy"}, {"samples", samples}},
      data, error)) << error;
}

TEST(CreateProblem, RejectsOverlongDescription) {
  ProblemData data;
  std::string error;
  EXPECT_FALSE(parse_create_problem(
      json{{"title", "T"},
           {"difficulty", "easy"},
           {"description", std::string(65537, 'd')}},
      data, error));
}

TEST(CreateProblem, RejectsNonIntegerLimits) {
  ProblemData data;
  std::string error;
  for (const json &value :
       {json("2000"), json(true), json(nullptr), json(1.5)}) {
    data = ProblemData{};
    error.clear();
    EXPECT_FALSE(parse_create_problem(
        json{{"title", "T"}, {"difficulty", "easy"}, {"time_limit_ms", value}},
        data, error)) << value.dump();
    data = ProblemData{};
    error.clear();
    EXPECT_FALSE(parse_create_problem(
        json{{"title", "T"},
             {"difficulty", "easy"},
             {"memory_limit_kb", value}},
        data, error)) << value.dump();
  }
}

TEST(CreateProblem, IgnoresServerManagedFields) {
  ProblemData data;
  std::string error;
  ASSERT_TRUE(parse_create_problem(
      parse(R"({"title":"T","difficulty":"easy","id":123456,
                "created_at":"1999-01-01 00:00:00",
                "updated_at":"1999-01-01 00:00:00","seed_key":"forged"})"),
      data, error)) << error;
  EXPECT_EQ(data.title, "T");
  EXPECT_EQ(data.time_limit_ms, 2000);
  EXPECT_EQ(data.memory_limit_kb, 65536);
}

TEST(CreateProblem, RejectsNullOptionalFields) {
  ProblemData data;
  std::string error;
  for (const char *field : {"description", "tags", "samples"}) {
    data = ProblemData{};
    error.clear();
    EXPECT_FALSE(parse_create_problem(
        json{{"title", "T"}, {"difficulty", "easy"}, {field, nullptr}}, data,
        error)) << field;
  }
  data = ProblemData{};
  error.clear();
  EXPECT_FALSE(parse_create_problem(
      json{{"title", "T"}, {"difficulty", "easy"}, {"visible", nullptr}},
      data, error));
}

TEST(UpdateProblem, AcceptsTagsSamplesAndExplicitClear) {
  ProblemPatch patch;
  std::string error;
  ASSERT_TRUE(parse_update_problem(
      parse(R"({"description":"","tags":[],"samples":[]})"), patch, error))
      << error;
  ASSERT_TRUE(patch.description.has_value());
  EXPECT_EQ(*patch.description, "");
  ASSERT_TRUE(patch.tags.has_value());
  EXPECT_TRUE(patch.tags->empty());
  ASSERT_TRUE(patch.samples.has_value());
  EXPECT_TRUE(patch.samples->empty());
}

TEST(UpdateProblem, RejectsNonIntegerLimits) {
  ProblemPatch patch;
  std::string error;
  for (const json &value : {json("2"), json(false), json(nullptr), json(2.5)}) {
    patch = ProblemPatch{};
    error.clear();
    EXPECT_FALSE(parse_update_problem(json{{"time_limit_ms", value}}, patch,
                                      error)) << value.dump();
    patch = ProblemPatch{};
    error.clear();
    EXPECT_FALSE(parse_update_problem(json{{"memory_limit_kb", value}}, patch,
                                      error)) << value.dump();
  }
}

TEST(UpdateProblem, PatchEmptyFlag) {
  EXPECT_TRUE(ProblemPatch{}.empty());
  ProblemPatch patch;
  patch.visible = false;
  EXPECT_FALSE(patch.empty());
}

// ---------------------------------------------------------------------------
// 标签拼接
// ---------------------------------------------------------------------------

TEST(JoinTags, MatchesStorageFormat) {
  EXPECT_EQ(join_tags({}), "");
  EXPECT_EQ(join_tags({"a"}), "a");
  EXPECT_EQ(join_tags({"a", "b", "c"}), "a,b,c");
}

} // namespace
