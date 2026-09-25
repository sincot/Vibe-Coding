#include "db/seed_data.h"

namespace oj {

std::vector<SeedProblem> builtin_seeds() {
  std::vector<SeedProblem> seeds;

  seeds.push_back(SeedProblem{
      "a-plus-b",
      "A+B Problem",
      "给定两个整数 a 和 b，输出它们的和。\n"
      "\n"
      "输入格式：\n"
      "一行两个整数 a 与 b，以空格分隔。\n"
      "\n"
      "输出格式：\n"
      "输出一个整数，表示 a + b 的值。\n",
      "easy",
      "入门,数学",
      2000,
      65536,
      1,
      {
          {"1 2\n", "3\n", true},
          {"100 -50\n", "50\n", true},
          {"111111111 222222222\n", "333333333\n", false},
          {"-444444444 -555555555\n", "-999999999\n", false},
          {"123456789 987654321\n", "1111111110\n", false},
      }});

  seeds.push_back(SeedProblem{
      "sum-of-integers",
      "整数求和",
      "给定 n 个整数，求它们的和。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 1000）。\n"
      "第二行 n 个整数，以空格分隔。\n"
      "\n"
      "输出格式：\n"
      "输出一个整数，表示这 n 个整数的和。\n",
      "easy",
      "数组,入门",
      2000,
      65536,
      1,
      {
          {"5\n1 2 3 4 5\n", "15\n", true},
          {"3\n-1 -2 -3\n", "-6\n", true},
          {"1\n987654321\n", "987654321\n", false},
          {"4\n111111111 222222222 333333333 444444444\n", "1111111110\n", false},
          {"6\n-111111111 -222222222 333333333 444444444 -555555555 666666666\n", "555555555\n", false},
      }});

  seeds.push_back(SeedProblem{
      "max-value",
      "求最大值",
      "给定 n 个整数，输出其中的最大值。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 1000）。\n"
      "第二行 n 个整数，以空格分隔。\n"
      "\n"
      "输出格式：\n"
      "输出一个整数，表示这 n 个整数的最大值。\n",
      "easy",
      "数组,入门",
      2000,
      65536,
      1,
      {
          {"5\n3 1 4 1 5\n", "5\n", true},
          {"3\n-7 -2 -9\n", "-2\n", true},
          {"1\n876543210\n", "876543210\n", false},
          {"5\n-987654321 -123456789 -456789123 -321654987 -147258369\n", "-123456789\n", false},
          {"4\n7654321 7654321 7654321 7654321\n", "7654321\n", false},
      }});

  seeds.push_back(SeedProblem{
      "two-sum",
      "两数之和 (LeetCode 1)",
      "给定一个整数数组和一个目标值 target，请找出数组中和为 target 的两个数，并输出它们的下标（从 0 开始，且第一个下标小于第二个）。保证恰好存在一组解。\n"
      "\n"
      "输入格式：\n"
      "第一行两个整数 n 和 target（2 <= n <= 10000）。\n"
      "第二行 n 个整数。\n"
      "\n"
      "输出格式：\n"
      "一行两个整数 i 和 j（i < j），表示数组下标。\n",
      "easy",
      "数组,哈希表",
      2000,
      65536,
      1,
      {
          {"4 9\n2 7 11 15\n", "0 1\n", true},
          {"3 6\n3 2 4\n", "1 2\n", true},
          {"2 6\n3 3\n", "0 1\n", false},
          {"6 0\n-3 4 3 90 -1 0\n", "0 2\n", false},
      }});

  seeds.push_back(SeedProblem{
      "palindrome-number",
      "回文数 (LeetCode 9)",
      "给定一个整数 x，判断它是否为回文数。回文数是指正序读和倒序读都相同的整数。负数不是回文数。\n"
      "\n"
      "输入格式：\n"
      "一个整数 x（-2^31 <= x <= 2^31 - 1）。\n"
      "\n"
      "输出格式：\n"
      "是回文数输出 true，否则输出 false。\n",
      "easy",
      "数学",
      2000,
      65536,
      1,
      {
          {"121\n", "true\n", true},
          {"-121\n", "false\n", true},
          {"10\n", "false\n", false},
          {"12321\n", "true\n", false},
          {"0\n", "true\n", false},
      }});

  seeds.push_back(SeedProblem{
      "roman-to-integer",
      "罗马数字转整数 (LeetCode 13)",
      "给定一个罗马数字字符串，将其转换为整数。\n"
      "\n"
      "输入格式：\n"
      "一行一个罗马数字字符串 s（仅含 I、V、X、L、C、D、M，长度不超过 15，表示范围 1 到 3999）。\n"
      "\n"
      "输出格式：\n"
      "输出对应的整数。\n",
      "easy",
      "数学,字符串",
      2000,
      65536,
      1,
      {
          {"III\n", "3\n", true},
          {"LVIII\n", "58\n", true},
          {"MCMXCIV\n", "1994\n", false},
          {"IX\n", "9\n", false},
          {"MMMCMXCIX\n", "3999\n", false},
      }});

  seeds.push_back(SeedProblem{
      "longest-common-prefix",
      "最长公共前缀 (LeetCode 14)",
      "给定 n 个字符串，输出它们的最长公共前缀。若不存在公共前缀，输出空行。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 200）。\n"
      "接下来 n 行，每行一个仅含小写字母的字符串。\n"
      "\n"
      "输出格式：\n"
      "一行，最长公共前缀（可能为空行）。\n",
      "easy",
      "字符串",
      2000,
      65536,
      1,
      {
          {"3\nflower\nflow\nflight\n", "fl\n", true},
          {"3\ndog\nracecar\ncar\n", "\n", true},
          {"1\nsingle\n", "single\n", false},
          {"4\na\na\na\na\n", "a\n", false},
      }});

  seeds.push_back(SeedProblem{
      "valid-parentheses",
      "有效的括号 (LeetCode 20)",
      "给定一个只包含 (、)、[、]、{、} 的字符串，判断括号是否有效。有效括号要求左括号必须以正确的顺序闭合。\n"
      "\n"
      "输入格式：\n"
      "一行一个括号字符串 s（长度不超过 10000）。\n"
      "\n"
      "输出格式：\n"
      "有效输出 true，否则输出 false。\n",
      "easy",
      "字符串,栈",
      2000,
      65536,
      1,
      {
          {"()\n", "true\n", true},
          {"()[]{}\n", "true\n", true},
          {"(]\n", "false\n", false},
          {"([{}])\n", "true\n", false},
          {"(((\n", "false\n", false},
      }});

  seeds.push_back(SeedProblem{
      "remove-duplicates-sorted-array",
      "删除有序数组中的重复项 (LeetCode 26)",
      "给定一个按非递减顺序排列的整数数组，删除重复出现的元素，使得每个元素只出现一次。输出去重后的元素个数 k，以及去重后数组的前 k 个元素（保持原顺序）。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 10000）。\n"
      "第二行 n 个非递减排列的整数。\n"
      "\n"
      "输出格式：\n"
      "第一行输出 k。\n"
      "第二行输出去重后的 k 个元素，以空格分隔。\n",
      "easy",
      "数组,双指针",
      2000,
      65536,
      1,
      {
          {"3\n1 1 2\n", "2\n1 2\n", true},
          {"5\n0 0 1 1 2\n", "3\n0 1 2\n", true},
          {"1\n5\n", "1\n5\n", false},
          {"6\n1 2 3 4 5 6\n", "6\n1 2 3 4 5 6\n", false},
      }});

  seeds.push_back(SeedProblem{
      "remove-element",
      "移除元素 (LeetCode 27)",
      "给定一个数组和目标值 val，原地移除所有等于 val 的元素。输出剩余元素个数 k，以及剩余元素（保持原有相对顺序）。\n"
      "\n"
      "输入格式：\n"
      "第一行两个整数 n 和 val（0 <= n <= 10000）。\n"
      "第二行 n 个整数。\n"
      "\n"
      "输出格式：\n"
      "第一行输出 k。\n"
      "第二行输出剩余的元素，以空格分隔（k 为 0 时输出空行）。\n",
      "easy",
      "数组,双指针",
      2000,
      65536,
      1,
      {
          {"4 3\n3 2 2 3\n", "2\n2 2\n", true},
          {"5 2\n0 1 2 2 3\n", "3\n0 1 3\n", true},
          {"1 1\n1\n", "0\n\n", false},
          {"5 0\n0 0 1 0 2\n", "2\n1 2\n", false},
      }});

  seeds.push_back(SeedProblem{
      "implement-strstr",
      "找出字符串中第一个匹配项的下标 (LeetCode 28)",
      "给定两个字符串 haystack 和 needle，在 haystack 中找出 needle 第一次出现的下标；如果不存在，输出 -1。\n"
      "\n"
      "输入格式：\n"
      "第一行字符串 haystack。\n"
      "第二行字符串 needle（非空，长度不超过 10000）。\n"
      "\n"
      "输出格式：\n"
      "输出第一次出现的下标，不存在则输出 -1。\n",
      "easy",
      "字符串,双指针",
      2000,
      65536,
      1,
      {
          {"sadbutsad\nsad\n", "0\n", true},
          {"leetcode\nleeto\n", "-1\n", true},
          {"hello\nll\n", "2\n", false},
          {"a\na\n", "0\n", false},
      }});

  seeds.push_back(SeedProblem{
      "search-insert-position",
      "搜索插入位置 (LeetCode 35)",
      "给定一个升序排列的、无重复元素的数组和一个目标值 target，返回 target 在数组中的下标；若不存在，返回它将会被按顺序插入的位置。\n"
      "\n"
      "输入格式：\n"
      "第一行两个整数 n 和 target（1 <= n <= 10000）。\n"
      "第二行 n 个升序且互不相同的整数。\n"
      "\n"
      "输出格式：\n"
      "输出下标或插入位置。\n",
      "easy",
      "数组,二分查找",
      2000,
      65536,
      1,
      {
          {"4 5\n1 3 5 6\n", "2\n", true},
          {"4 2\n1 3 5 6\n", "1\n", true},
          {"4 7\n1 3 5 6\n", "4\n", false},
          {"4 0\n1 3 5 6\n", "0\n", false},
      }});

  seeds.push_back(SeedProblem{
      "maximum-subarray",
      "最大子数组和 (LeetCode 53)",
      "给定一个整数数组，找出和最大的连续子数组（至少包含一个元素），输出其最大和。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 100000）。\n"
      "第二行 n 个整数。\n"
      "\n"
      "输出格式：\n"
      "输出最大子数组的和。\n",
      "medium",
      "数组,动态规划",
      2000,
      65536,
      1,
      {
          {"9\n-2 1 -3 4 -1 2 1 -5 4\n", "6\n", true},
          {"1\n1\n", "1\n", true},
          {"5\n5 4 -1 7 8\n", "23\n", false},
          {"5\n-3 -2 -5 -1 -4\n", "-1\n", false},
      }});

  seeds.push_back(SeedProblem{
      "length-of-last-word",
      "最后一个单词的长度 (LeetCode 58)",
      "给定一个由若干单词组成的字符串，单词之间以单个空格分隔，输出最后一个单词的长度。\n"
      "\n"
      "输入格式：\n"
      "一行字符串 s（仅含英文字母和空格，长度不超过 10000）。\n"
      "\n"
      "输出格式：\n"
      "输出最后一个单词的长度。\n",
      "easy",
      "字符串",
      2000,
      65536,
      1,
      {
          {"Hello World\n", "5\n", true},
          {"luffy is still joyboy\n", "6\n", true},
          {"a\n", "1\n", false},
          {"fly me to the moon\n", "4\n", false},
      }});

  seeds.push_back(SeedProblem{
      "plus-one",
      "加一 (LeetCode 66)",
      "给定一个用数组表示的整数（数组元素为 0-9，无前导零），将其加一，输出结果数组。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 1000）。\n"
      "第二行 n 个数字，以空格分隔。\n"
      "\n"
      "输出格式：\n"
      "一行，表示加一后的整数各位数字，以空格分隔。\n",
      "easy",
      "数组,数学",
      2000,
      65536,
      1,
      {
          {"3\n1 2 3\n", "1 2 4\n", true},
          {"4\n4 3 2 1\n", "4 3 2 2\n", true},
          {"1\n9\n", "1 0\n", false},
          {"3\n9 9 9\n", "1 0 0 0\n", false},
      }});

  seeds.push_back(SeedProblem{
      "add-binary",
      "二进制求和 (LeetCode 67)",
      "给定两个二进制字符串，输出它们的和（二进制表示）。\n"
      "\n"
      "输入格式：\n"
      "第一行字符串 a，第二行字符串 b（均不含前导零，除非字符串本身为 0，长度不超过 10000）。\n"
      "\n"
      "输出格式：\n"
      "输出二进制和的字符串。\n",
      "easy",
      "数学,字符串,位运算",
      2000,
      65536,
      1,
      {
          {"11\n1\n", "100\n", true},
          {"1010\n1011\n", "10101\n", true},
          {"0\n0\n", "0\n", false},
          {"1111\n1111\n", "11110\n", false},
      }});

  seeds.push_back(SeedProblem{
      "climbing-stairs",
      "爬楼梯 (LeetCode 70)",
      "假设你正在爬楼梯，需要 n 阶才能到达楼顶。每次可以爬 1 或 2 个台阶，求有多少种不同的方法到达楼顶。\n"
      "\n"
      "输入格式：\n"
      "一个整数 n（1 <= n <= 45）。\n"
      "\n"
      "输出格式：\n"
      "输出方法总数。\n",
      "easy",
      "动态规划,数学",
      2000,
      65536,
      1,
      {
          {"2\n", "2\n", true},
          {"3\n", "3\n", true},
          {"1\n", "1\n", false},
          {"10\n", "89\n", false},
      }});

  seeds.push_back(SeedProblem{
      "merge-sorted-array",
      "合并两个有序数组 (LeetCode 88)",
      "给定两个已经按非递减顺序排列的整数数组，将它们合并为一个仍然有序的数组并输出。\n"
      "\n"
      "输入格式：\n"
      "第一行两个整数 m 和 n（0 <= m, n <= 10000）。\n"
      "第二行 m 个升序整数。\n"
      "第三行 n 个升序整数。\n"
      "\n"
      "输出格式：\n"
      "一行，合并后的 m + n 个升序整数，以空格分隔。\n",
      "easy",
      "数组,双指针,排序",
      2000,
      65536,
      1,
      {
          {"3 3\n1 2 3\n2 5 6\n", "1 2 2 3 5 6\n", true},
          {"1 0\n1\n\n", "1\n", true},
          {"2 2\n-1 0\n0 3\n", "-1 0 0 3\n", false},
          {"0 1\n\n2\n", "2\n", false},
      }});

  seeds.push_back(SeedProblem{
      "best-time-buy-sell-stock",
      "买卖股票的最佳时机 (LeetCode 121)",
      "给定一支股票每天的价格，你只能选择某一天买入并在之后的某一天卖出，求能获得的最大利润。如果不能获得利润，返回 0。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 100000）。\n"
      "第二行 n 个整数，表示每天的价格。\n"
      "\n"
      "输出格式：\n"
      "输出最大利润。\n",
      "easy",
      "数组,动态规划",
      2000,
      65536,
      1,
      {
          {"6\n7 1 5 3 6 4\n", "5\n", true},
          {"5\n7 6 4 3 1\n", "0\n", true},
          {"1\n5\n", "0\n", false},
          {"6\n2 4 1 7 5 11\n", "10\n", false},
      }});

  seeds.push_back(SeedProblem{
      "valid-palindrome",
      "验证回文串 (LeetCode 125)",
      "给定一个字符串，只考虑其中的字母和数字字符并忽略字母大小写，判断它是否为回文串。\n"
      "\n"
      "输入格式：\n"
      "一行字符串 s（长度不超过 20000，可含空格与标点）。\n"
      "\n"
      "输出格式：\n"
      "是回文串输出 true，否则输出 false。\n",
      "easy",
      "字符串,双指针",
      2000,
      65536,
      1,
      {
          {"A man, a plan, a canal: Panama\n", "true\n", true},
          {"race a car\n", "false\n", true},
          {" \n", "true\n", false},
          {"0P\n", "false\n", false},
      }});

  seeds.push_back(SeedProblem{
      "single-number",
      "只出现一次的数字 (LeetCode 136)",
      "给定一个整数数组，其中除某个元素只出现一次外，其余元素都出现两次。找出那个只出现一次的元素。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 100000）。\n"
      "第二行 n 个整数。\n"
      "\n"
      "输出格式：\n"
      "输出只出现一次的数字。\n",
      "easy",
      "数组,位运算",
      2000,
      65536,
      1,
      {
          {"3\n2 2 1\n", "1\n", true},
          {"5\n4 1 2 1 2\n", "4\n", true},
          {"5\n-1 -1 5 5 7\n", "7\n", false},
      }});

  seeds.push_back(SeedProblem{
      "majority-element",
      "多数元素 (LeetCode 169)",
      "给定一个大小为 n 的数组，找出其中的多数元素。多数元素指出现次数大于 n/2 的元素。保证数组非空且多数元素一定存在。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 100000）。\n"
      "第二行 n 个整数。\n"
      "\n"
      "输出格式：\n"
      "输出多数元素。\n",
      "easy",
      "数组,哈希表",
      2000,
      65536,
      1,
      {
          {"3\n3 2 3\n", "3\n", true},
          {"7\n2 2 1 1 1 2 2\n", "2\n", true},
          {"1\n1\n", "1\n", false},
          {"5\n7 7 7 3 3\n", "7\n", false},
      }});

  seeds.push_back(SeedProblem{
      "factorial-trailing-zeroes",
      "阶乘后的零 (LeetCode 172)",
      "给定整数 n，返回 n! 结果尾数中零的数量。\n"
      "\n"
      "输入格式：\n"
      "一个整数 n（0 <= n <= 100000）。\n"
      "\n"
      "输出格式：\n"
      "输出尾数中零的数量。\n",
      "medium",
      "数学",
      2000,
      65536,
      1,
      {
          {"3\n", "0\n", true},
          {"5\n", "1\n", true},
          {"0\n", "0\n", false},
          {"100\n", "24\n", false},
      }});

  seeds.push_back(SeedProblem{
      "house-robber",
      "打家劫舍 (LeetCode 198)",
      "给定一个代表每个房屋存放金额的非负整数数组，如果两间相邻的房屋在同一晚上被闯入，系统会自动报警。在不触动警报的情况下，计算能偷窃到的最高金额。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 10000）。\n"
      "第二行 n 个非负整数。\n"
      "\n"
      "输出格式：\n"
      "输出最高金额。\n",
      "medium",
      "数组,动态规划",
      2000,
      65536,
      1,
      {
          {"4\n1 2 3 1\n", "4\n", true},
          {"5\n2 7 9 3 1\n", "12\n", true},
          {"1\n5\n", "5\n", false},
          {"3\n2 1 1\n", "3\n", false},
      }});

  seeds.push_back(SeedProblem{
      "happy-number",
      "快乐数 (LeetCode 202)",
      "对一个正整数，每次将它替换为它每个位置上的数字的平方和，重复这个过程。如果最终得到 1，则它是快乐数；如果陷入循环则不是。\n"
      "\n"
      "输入格式：\n"
      "一个正整数 n（1 <= n <= 2^31 - 1）。\n"
      "\n"
      "输出格式：\n"
      "是快乐数输出 true，否则输出 false。\n",
      "easy",
      "数学,哈希表",
      2000,
      65536,
      1,
      {
          {"19\n", "true\n", true},
          {"2\n", "false\n", true},
          {"1\n", "true\n", false},
          {"20\n", "false\n", false},
      }});

  seeds.push_back(SeedProblem{
      "count-primes",
      "计数质数 (LeetCode 204)",
      "统计所有小于非负整数 n 的质数的数量。\n"
      "\n"
      "输入格式：\n"
      "一个整数 n（0 <= n <= 5000000）。\n"
      "\n"
      "输出格式：\n"
      "输出小于 n 的质数个数。\n",
      "medium",
      "数学,数组",
      2000,
      65536,
      1,
      {
          {"10\n", "4\n", true},
          {"0\n", "0\n", true},
          {"100\n", "25\n", false},
          {"1\n", "0\n", false},
      }});

  seeds.push_back(SeedProblem{
      "isomorphic-strings",
      "同构字符串 (LeetCode 205)",
      "给定两个长度相同的字符串 s 和 t，判断它们是否是同构的。同构指 s 中的每个字符都可以被 t 中唯一一个字符替换，且不同字符不能映射到同一字符，同时保留字符顺序。\n"
      "\n"
      "输入格式：\n"
      "第一行字符串 s，第二行字符串 t（长度不超过 50000）。\n"
      "\n"
      "输出格式：\n"
      "同构输出 true，否则输出 false。\n",
      "easy",
      "字符串,哈希表",
      2000,
      65536,
      1,
      {
          {"egg\nadd\n", "true\n", true},
          {"foo\nbar\n", "false\n", true},
          {"paper\ntitle\n", "true\n", false},
          {"badc\nbaba\n", "false\n", false},
      }});

  seeds.push_back(SeedProblem{
      "contains-duplicate",
      "存在重复元素 (LeetCode 217)",
      "给定一个整数数组，如果任意一个值在数组中出现至少两次，输出 true；如果每个元素都互不相同，输出 false。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 100000）。\n"
      "第二行 n 个整数。\n"
      "\n"
      "输出格式：\n"
      "存在重复输出 true，否则输出 false。\n",
      "easy",
      "数组,哈希表",
      2000,
      65536,
      1,
      {
          {"4\n1 2 3 1\n", "true\n", true},
          {"4\n1 2 3 4\n", "false\n", true},
          {"1\n1\n", "false\n", false},
          {"4\n1 1 1 3\n", "true\n", false},
      }});

  seeds.push_back(SeedProblem{
      "valid-anagram",
      "有效的字母异位词 (LeetCode 242)",
      "给定两个字符串 s 和 t，判断 t 是否是 s 的字母异位词（即两者由相同字母重排而成）。\n"
      "\n"
      "输入格式：\n"
      "第一行字符串 s，第二行字符串 t（均仅含小写字母，长度不超过 50000）。\n"
      "\n"
      "输出格式：\n"
      "是异位词输出 true，否则输出 false。\n",
      "easy",
      "字符串,哈希表,排序",
      2000,
      65536,
      1,
      {
          {"anagram\nnagaram\n", "true\n", true},
          {"rat\ncar\n", "false\n", true},
          {"a\na\n", "true\n", false},
          {"ab\naa\n", "false\n", false},
      }});

  seeds.push_back(SeedProblem{
      "missing-number",
      "丢失的数字 (LeetCode 268)",
      "给定一个包含 n 个互不相同整数的数组，这些整数都在范围 [0, n] 内，找出 [0, n] 中没有出现在数组中的那个数。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（1 <= n <= 100000）。\n"
      "第二行 n 个整数。\n"
      "\n"
      "输出格式：\n"
      "输出丢失的数字。\n",
      "easy",
      "数组,数学,位运算",
      2000,
      65536,
      1,
      {
          {"3\n3 0 1\n", "2\n", true},
          {"2\n0 1\n", "2\n", true},
          {"1\n0\n", "1\n", false},
          {"6\n3 0 6 4 2 1\n", "5\n", false},
      }});

  seeds.push_back(SeedProblem{
      "move-zeroes",
      "移动零 (LeetCode 283)",
      "给定一个数组，将所有 0 移动到数组末尾，同时保持非零元素的相对顺序，输出移动后的数组。\n"
      "\n"
      "输入格式：\n"
      "第一行一个整数 n（0 <= n <= 100000）。\n"
      "第二行 n 个整数。\n"
      "\n"
      "输出格式：\n"
      "一行，移动后的数组，以空格分隔。\n",
      "easy",
      "数组,双指针",
      2000,
      65536,
      1,
      {
          {"5\n0 1 0 3 12\n", "1 3 12 0 0\n", true},
          {"1\n0\n", "0\n", true},
          {"3\n1 2 3\n", "1 2 3\n", false},
          {"4\n0 0 0 1\n", "1 0 0 0\n", false},
      }});

  seeds.push_back(SeedProblem{
      "word-pattern",
      "单词规律 (LeetCode 290)",
      "给定一种规律 pattern 和一个字符串 s，判断 s 是否遵循相同的规律（pattern 中每个字母与 s 中每个单词双向一一对应）。\n"
      "\n"
      "输入格式：\n"
      "第一行字符串 pattern（仅含小写字母，长度不超过 10000）。\n"
      "第二行字符串 s，由若干单词组成，单词之间以单个空格分隔。\n"
      "\n"
      "输出格式：\n"
      "遵循规律输出 true，否则输出 false。\n",
      "easy",
      "字符串,哈希表",
      2000,
      65536,
      1,
      {
          {"abba\ndog cat cat dog\n", "true\n", true},
          {"abba\ndog cat cat fish\n", "false\n", true},
          {"abc\na b c\n", "true\n", false},
          {"abba\ndog dog dog dog\n", "false\n", false},
      }});

  seeds.push_back(SeedProblem{
      "power-of-three",
      "3 的幂 (LeetCode 326)",
      "给定一个整数 n，判断它是否是 3 的幂次方。\n"
      "\n"
      "输入格式：\n"
      "一个整数 n（-2^31 <= n <= 2^31 - 1）。\n"
      "\n"
      "输出格式：\n"
      "是 3 的幂输出 true，否则输出 false。\n",
      "easy",
      "数学,递归",
      2000,
      65536,
      1,
      {
          {"27\n", "true\n", true},
          {"0\n", "false\n", true},
          {"9\n", "true\n", false},
          {"45\n", "false\n", false},
      }});

  seeds.push_back(SeedProblem{
      "reverse-vowels-of-a-string",
      "反转字符串中的元音字母 (LeetCode 345)",
      "反转字符串中的元音字母（a、e、i、o、u 及其大写形式），并输出结果。\n"
      "\n"
      "输入格式：\n"
      "一行字符串 s（仅含可打印 ASCII 字符，长度不超过 100000）。\n"
      "\n"
      "输出格式：\n"
      "输出反转元音后的字符串。\n",
      "easy",
      "字符串,双指针",
      2000,
      65536,
      1,
      {
          {"hello\n", "holle\n", true},
          {"leetcode\n", "leotcede\n", true},
          {"aA\n", "Aa\n", false},
          {"xyz\n", "xyz\n", false},
      }});

  seeds.push_back(SeedProblem{
      "intersection-of-two-arrays",
      "两个数组的交集 (LeetCode 349)",
      "给定两个数组，输出它们的交集。交集中的每个元素一定是唯一的，结果按升序排列。\n"
      "\n"
      "输入格式：\n"
      "第一行两个整数 n 和 m（0 <= n, m <= 10000）。\n"
      "第二行 n 个整数。\n"
      "第三行 m 个整数。\n"
      "\n"
      "输出格式：\n"
      "一行，升序且去重的交集元素，以空格分隔（交集为空时输出空行）。\n",
      "easy",
      "数组,哈希表,排序",
      2000,
      65536,
      1,
      {
          {"4 4\n1 2 2 1\n2 2 3 1\n", "1 2\n", true},
          {"3 3\n4 9 5\n9 4 9\n", "4 9\n", true},
          {"1 1\n1\n1\n", "1\n", false},
          {"3 3\n1 2 3\n4 5 6\n", "\n", false},
      }});

  seeds.push_back(SeedProblem{
      "intersection-of-two-arrays-ii",
      "两个数组的交集 II (LeetCode 350)",
      "给定两个数组，输出它们的交集。交集中每个元素出现的次数应与该元素在两个数组中都出现的次数一致。结果按升序排列。\n"
      "\n"
      "输入格式：\n"
      "第一行两个整数 n 和 m（0 <= n, m <= 10000）。\n"
      "第二行 n 个整数。\n"
      "第三行 m 个整数。\n"
      "\n"
      "输出格式：\n"
      "一行，升序排列的交集元素（保留重复），以空格分隔（交集为空时输出空行）。\n",
      "easy",
      "数组,哈希表,排序",
      2000,
      65536,
      1,
      {
          {"4 2\n1 2 2 1\n2 2\n", "2 2\n", true},
          {"3 5\n4 9 5\n9 4 9 8 4\n", "4 9\n", true},
          {"1 1\n1\n1\n", "1\n", false},
          {"6 4\n1 1 2 2 3 3\n1 2 2 3\n", "1 2 2 3\n", false},
      }});

  seeds.push_back(SeedProblem{
      "valid-perfect-square",
      "有效的完全平方数 (LeetCode 367)",
      "给定一个正整数 num，判断它是否是一个完全平方数（存在整数 x 使 x*x == num）。\n"
      "\n"
      "输入格式：\n"
      "一个整数 num（1 <= num <= 2^31 - 1）。\n"
      "\n"
      "输出格式：\n"
      "是完全平方数输出 true，否则输出 false。\n",
      "easy",
      "数学,二分查找",
      2000,
      65536,
      1,
      {
          {"16\n", "true\n", true},
          {"14\n", "false\n", true},
          {"1\n", "true\n", false},
          {"2147483647\n", "false\n", false},
      }});

  seeds.push_back(SeedProblem{
      "ransom-note",
      "赎金信 (LeetCode 383)",
      "给定两个仅含小写字母的字符串 ransomNote 和 magazine，判断 ransomNote 能否由 magazine 中的字符构成（magazine 中每个字符只能使用一次）。\n"
      "\n"
      "输入格式：\n"
      "第一行字符串 ransomNote，第二行字符串 magazine。\n"
      "\n"
      "输出格式：\n"
      "可以构成输出 true，否则输出 false。\n",
      "easy",
      "字符串,哈希表,计数",
      2000,
      65536,
      1,
      {
          {"a\nb\n", "false\n", true},
          {"aa\naab\n", "true\n", true},
          {"abc\ncba\n", "true\n", false},
          {"abc\nab\n", "false\n", false},
      }});

  seeds.push_back(SeedProblem{
      "first-unique-character",
      "字符串中的第一个唯一字符 (LeetCode 387)",
      "给定一个字符串，找到它的第一个不重复字符，并返回它的下标；如果不存在，返回 -1。\n"
      "\n"
      "输入格式：\n"
      "一行字符串 s（仅含小写字母，长度不超过 100000）。\n"
      "\n"
      "输出格式：\n"
      "输出第一个唯一字符的下标，不存在则输出 -1。\n",
      "easy",
      "字符串,哈希表,计数",
      2000,
      65536,
      1,
      {
          {"leetcode\n", "0\n", true},
          {"loveleetcode\n", "2\n", true},
          {"aabb\n", "-1\n", false},
          {"aabbccd\n", "6\n", false},
      }});

  seeds.push_back(SeedProblem{
      "is-subsequence",
      "判断子序列 (LeetCode 392)",
      "给定字符串 s 和 t，判断 s 是否为 t 的子序列（即 s 可由 t 删除若干字符且不改变剩余字符相对位置得到）。\n"
      "\n"
      "输入格式：\n"
      "第一行字符串 s，第二行字符串 t（均仅含小写字母，长度不超过 100000）。\n"
      "\n"
      "输出格式：\n"
      "是子序列输出 true，否则输出 false。\n",
      "easy",
      "字符串,双指针,动态规划",
      2000,
      65536,
      1,
      {
          {"abc\nahbgdc\n", "true\n", true},
          {"axc\nahbgdc\n", "false\n", true},
          {"abc\nabc\n", "true\n", false},
          {"abc\nacb\n", "false\n", false},
      }});

  seeds.push_back(SeedProblem{
      "fizz-buzz",
      "Fizz Buzz (LeetCode 412)",
      "输出从 1 到 n 的每个数字的字符串表示：如果 i 能被 3 和 5 同时整除，输出 FizzBuzz；能被 3 整除输出 Fizz；能被 5 整除输出 Buzz；否则输出 i 本身。\n"
      "\n"
      "输入格式：\n"
      "一个整数 n（1 <= n <= 100000）。\n"
      "\n"
      "输出格式：\n"
      "共 n 行，每行对应一个数字。\n",
      "easy",
      "数学,字符串,模拟",
      2000,
      65536,
      1,
      {
          {"3\n", "1\n2\nFizz\n", true},
          {"5\n", "1\n2\nFizz\n4\nBuzz\n", true},
          {"1\n", "1\n", false},
          {"15\n", "1\n2\nFizz\n4\nBuzz\nFizz\n7\n8\nFizz\nBuzz\n11\nFizz\n13\n14\nFizzBuzz\n", false},
      }});

  return seeds;
}

} // namespace oj
