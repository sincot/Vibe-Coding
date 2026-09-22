// 题目列表查询参数解析与条件构造单元测试（M2.3）。
//
// 覆盖 problem/list_query 的纯函数：空白裁剪、LIKE 特殊字符转义、标题匹配模式、
// 标签完整匹配 needle、难度取值校验，以及 q/difficulty/tag/page/visible 的
// 归一化与非法值处理。实际筛选、分页、统计与权限由 problems_list_api 集成测试验证。
//
// 运行方式：ctest --test-dir build -R problem_list_query_unit --output-on-failure
// 或直接执行 build/oj_problem_list_query_unit。

#include <gtest/gtest.h>

#include <string>

#include "problem/list_query.h"

namespace {

using oj::problem::escape_like;
using oj::problem::is_valid_difficulty;
using oj::problem::kProblemPageSize;
using oj::problem::ListFilter;
using oj::problem::parse_list_query;
using oj::problem::RawListParams;
using oj::problem::tag_match_needle;
using oj::problem::title_like_pattern;
using oj::problem::trim_ascii;
using oj::problem::VisibleFilter;

RawListParams make(const std::string &q = "", const std::string &difficulty = "",
                   const std::string &tag = "", const std::string &page = "",
                   const std::string &visible = "") {
  RawListParams raw;
  raw.q = q;
  raw.difficulty = difficulty;
  raw.tag = tag;
  raw.page = page;
  raw.visible = visible;
  return raw;
}

TEST(TrimAscii, RemovesSurroundingWhitespace) {
  EXPECT_EQ(trim_ascii(""), "");
  EXPECT_EQ(trim_ascii("   "), "");
  EXPECT_EQ(trim_ascii("  a b  "), "a b");
  EXPECT_EQ(trim_ascii("\t\n a \r\n"), "a");
  EXPECT_EQ(trim_ascii("内部 空格"), "内部 空格");
}

TEST(EscapeLike, EscapesWildcardsAndBackslash) {
  EXPECT_EQ(escape_like("abc"), "abc");
  EXPECT_EQ(escape_like("100%"), "100\\%");
  EXPECT_EQ(escape_like("a_b"), "a\\_b");
  EXPECT_EQ(escape_like("a\\b"), "a\\\\b");
  EXPECT_EQ(escape_like("%_\\"), "\\%\\_\\\\");
  EXPECT_EQ(escape_like("中文图"), "中文图");
}

TEST(TitleLikePattern, WrapsEscapedKeyword) {
  EXPECT_EQ(title_like_pattern("A+B"), "%A+B%");
  EXPECT_EQ(title_like_pattern("50%"), "%50\\%%");
  EXPECT_EQ(title_like_pattern("a_b"), "%a\\_b%");
}

TEST(TagMatchNeedle, WrapsWithCommaDelimiters) {
  EXPECT_EQ(tag_match_needle("图"), ",图,");
  EXPECT_EQ(tag_match_needle("two words"), ",two words,");
}

TEST(Difficulty, AcceptsOnlyStoredValues) {
  EXPECT_TRUE(is_valid_difficulty("easy"));
  EXPECT_TRUE(is_valid_difficulty("medium"));
  EXPECT_TRUE(is_valid_difficulty("hard"));
  EXPECT_FALSE(is_valid_difficulty("Easy"));
  EXPECT_FALSE(is_valid_difficulty(""));
  EXPECT_FALSE(is_valid_difficulty("expert"));
}

TEST(ParseListQuery, DefaultsWhenAllEmpty) {
  ListFilter out;
  std::string error;
  ASSERT_TRUE(parse_list_query(make(), out, error)) << error;
  EXPECT_TRUE(out.keyword.empty());
  EXPECT_TRUE(out.difficulty.empty());
  EXPECT_TRUE(out.tag.empty());
  EXPECT_EQ(out.page, 1);
  EXPECT_EQ(out.page_size, kProblemPageSize);
  EXPECT_EQ(out.visible, VisibleFilter::All);
}

TEST(ParseListQuery, TrimsKeywordAndTag) {
  ListFilter out;
  std::string error;
  ASSERT_TRUE(parse_list_query(make("  A+B  ", "", "  图  "), out, error))
      << error;
  EXPECT_EQ(out.keyword, "A+B");
  EXPECT_EQ(out.tag, "图");
}

TEST(ParseListQuery, WhitespaceOnlyIsUnlimited) {
  ListFilter out;
  std::string error;
  ASSERT_TRUE(parse_list_query(make("   ", "  ", "\t"), out, error)) << error;
  EXPECT_TRUE(out.keyword.empty());
  EXPECT_TRUE(out.difficulty.empty());
  EXPECT_TRUE(out.tag.empty());
}

TEST(ParseListQuery, DifficultyValidValues) {
  for (const std::string &value :
       {std::string("easy"), std::string("medium"), std::string("hard")}) {
    ListFilter out;
    std::string error;
    ASSERT_TRUE(parse_list_query(make("", " " + value + " "), out, error))
        << value << ": " << error;
    EXPECT_EQ(out.difficulty, value);
  }
}

TEST(ParseListQuery, DifficultyInvalidRejected) {
  ListFilter out;
  std::string error;
  EXPECT_FALSE(parse_list_query(make("", "Easy"), out, error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(parse_list_query(make("", "expert"), out, error));
  EXPECT_FALSE(parse_list_query(make("", "0"), out, error));
}

TEST(ParseListQuery, PageDefaultsAndLeadingZeros) {
  {
    ListFilter out;
    std::string error;
    ASSERT_TRUE(parse_list_query(make("", "", "", ""), out, error)) << error;
    EXPECT_EQ(out.page, 1);
  }
  {
    ListFilter out;
    std::string error;
    ASSERT_TRUE(parse_list_query(make("", "", "", "0001"), out, error)) << error;
    EXPECT_EQ(out.page, 1);
  }
  {
    ListFilter out;
    std::string error;
    ASSERT_TRUE(parse_list_query(make("", "", "", " 20 "), out, error)) << error;
    EXPECT_EQ(out.page, 20);
  }
}

TEST(ParseListQuery, PageMaxAccepted) {
  ListFilter out;
  std::string error;
  ASSERT_TRUE(parse_list_query(make("", "", "", "1000000"), out, error)) << error;
  EXPECT_EQ(out.page, 1000000);
}

TEST(ParseListQuery, PageInvalidRejected) {
  const char *bad[] = {"0", "-1", "1.5", "abc", "+1", "1e3", "1000001",
                       "9999999", "99999999999999999999"};
  for (const char *value : bad) {
    ListFilter out;
    std::string error;
    EXPECT_FALSE(parse_list_query(make("", "", "", value), out, error))
        << "page=" << value;
    EXPECT_FALSE(error.empty()) << "page=" << value;
  }
}

TEST(ParseListQuery, VisibleValues) {
  struct Case {
    const char *text;
    VisibleFilter expected;
  };
  const Case cases[] = {
      {"", VisibleFilter::All},      {"all", VisibleFilter::All},
      {"ALL", VisibleFilter::All},   {"1", VisibleFilter::OnlyVisible},
      {"true", VisibleFilter::OnlyVisible},
      {"TRUE", VisibleFilter::OnlyVisible},
      {"0", VisibleFilter::OnlyHidden},
      {"false", VisibleFilter::OnlyHidden},
      {" false ", VisibleFilter::OnlyHidden},
  };
  for (const Case &c : cases) {
    ListFilter out;
    std::string error;
    ASSERT_TRUE(parse_list_query(make("", "", "", "", c.text), out, error))
        << c.text << ": " << error;
    EXPECT_EQ(out.visible, c.expected) << c.text;
  }
}

TEST(ParseListQuery, VisibleInvalidRejected) {
  ListFilter out;
  std::string error;
  EXPECT_FALSE(parse_list_query(make("", "", "", "", "yes"), out, error));
  EXPECT_FALSE(parse_list_query(make("", "", "", "", "2"), out, error));
}

TEST(ParseListQuery, CombinedValidConditions) {
  ListFilter out;
  std::string error;
  ASSERT_TRUE(parse_list_query(make("A+B", "easy", "入门", "2", "all"), out,
                               error))
      << error;
  EXPECT_EQ(out.keyword, "A+B");
  EXPECT_EQ(out.difficulty, "easy");
  EXPECT_EQ(out.tag, "入门");
  EXPECT_EQ(out.page, 2);
  EXPECT_EQ(out.visible, VisibleFilter::All);
  EXPECT_EQ(out.page_size, kProblemPageSize);
}

TEST(ParseListQuery, KeywordWithInternalSpacesAndQuotesPreserved) {
  ListFilter out;
  std::string error;
  ASSERT_TRUE(parse_list_query(make("  two words  ", "", "", ""), out, error))
      << error;
  EXPECT_EQ(out.keyword, "two words");
  ASSERT_TRUE(parse_list_query(make("' OR '1'='1", "", "", ""), out, error))
      << error;
  EXPECT_EQ(out.keyword, "' OR '1'='1");
}

TEST(ParseListQuery, InvalidConditionRejectedRegardlessOfOthers) {
  // 即使 page 合法，非法 difficulty 仍应使整体解析失败。
  ListFilter out;
  std::string error;
  EXPECT_FALSE(parse_list_query(make("ok", "expert", "tag", "1"), out, error));
  EXPECT_FALSE(error.empty());
}

} // namespace
