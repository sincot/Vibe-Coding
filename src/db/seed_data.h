#ifndef OJ_DB_SEED_DATA_H
#define OJ_DB_SEED_DATA_H

#include <vector>

namespace oj {

// 一条种子用例。is_sample=true 为公开样例，false 为隐藏用例。
struct SeedTestcase {
  const char *input;
  const char *output;
  bool is_sample;
};

// 一道种子题的定义。seed_key 为稳定标识，用于幂等导入。
struct SeedProblem {
  const char *seed_key;
  const char *title;
  const char *description;
  const char *difficulty;
  const char *tags;
  int time_limit_ms;
  int memory_limit_kb;
  int visible;
  std::vector<SeedTestcase> cases;
};

// 内置种子题（标准 ACM 输入输出模式）。样例内容与隐藏内容刻意不同，便于验证
// 隐藏用例不会随题面下发。
std::vector<SeedProblem> builtin_seeds();

} // namespace oj

#endif // OJ_DB_SEED_DATA_H
