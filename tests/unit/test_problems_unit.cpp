// 题目模块纯函数单元测试（M1.4）。
//
// 覆盖 tags 文本解析：分隔、首尾空白裁剪、跳过空项、单标签与空串。
// 数据库查询、可见性与响应字段由 problems_api 集成测试验证。
//
// 运行方式：ctest --test-dir build -R problems_unit --output-on-failure
// 或直接执行 build/oj_problems_unit。

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "db/problems.h"

namespace {

using oj::split_tags;

std::vector<std::string> split(const std::string &text) {
  return split_tags(text);
}

TEST(SplitTags, EmptyStringYieldsNoTags) {
  EXPECT_TRUE(split("").empty());
  EXPECT_TRUE(split("   ").empty());
  EXPECT_TRUE(split(",,").empty());
  EXPECT_TRUE(split(" , , ").empty());
}

TEST(SplitTags, SingleTag) {
  ASSERT_EQ(split("入门").size(), 1u);
  EXPECT_EQ(split("入门")[0], "入门");
}

TEST(SplitTags, MultipleTagsTrimmed) {
  auto tags = split(" 数组 , 入门 ,数学");
  ASSERT_EQ(tags.size(), 3u);
  EXPECT_EQ(tags[0], "数组");
  EXPECT_EQ(tags[1], "入门");
  EXPECT_EQ(tags[2], "数学");
}

TEST(SplitTags, SkipsEmptySegments) {
  auto tags = split("a,,b, ,c,");
  ASSERT_EQ(tags.size(), 3u);
  EXPECT_EQ(tags[0], "a");
  EXPECT_EQ(tags[1], "b");
  EXPECT_EQ(tags[2], "c");
}

TEST(SplitTags, PreservesInternalSpaces) {
  auto tags = split("two words");
  ASSERT_EQ(tags.size(), 1u);
  EXPECT_EQ(tags[0], "two words");
}

TEST(SplitTags, TreatsTabsAndNewlinesAsWhitespace) {
  auto tags = split("a\t,\n b ,\r\nc ");
  ASSERT_EQ(tags.size(), 3u);
  EXPECT_EQ(tags[0], "a");
  EXPECT_EQ(tags[1], "b");
  EXPECT_EQ(tags[2], "c");
}

} // namespace
