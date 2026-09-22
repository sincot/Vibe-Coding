#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace oj {
namespace problem {

// 题目写入字段的默认值与取值范围（SPEC PRB-01/PRB-02）。
//
// - 时间/内存限制未提供时采用 SPEC 默认值 2000ms / 65536KB。
// - 项目未定义「无限制」取值约定，故不以 0 或负数表示无限制；限制必须为正整数，
//   时限上限 60000ms 与判题全局硬上限（SPEC JUDGE-10）一致。
inline constexpr int kDefaultTimeLimitMs = 2000;
inline constexpr int kDefaultMemoryLimitKb = 65536;
inline constexpr int kMinTimeLimitMs = 1;
inline constexpr int kMaxTimeLimitMs = 60000;
inline constexpr int kMinMemoryLimitKb = 1;
inline constexpr int kMaxMemoryLimitKb = 1048576; // 1 GiB

inline constexpr std::size_t kMaxTitleBytes = 200;
inline constexpr std::size_t kMaxDescriptionBytes = 64 * 1024;
inline constexpr std::size_t kMaxTagBytes = 30;
inline constexpr std::size_t kMaxTags = 20;
inline constexpr std::size_t kMaxSamples = 50;
inline constexpr std::size_t kMaxSampleFieldBytes = 64 * 1024;

// 一组公开样例（纯文本输入/输出）。
struct SampleInput {
  std::string input;
  std::string output;
};

// 创建题目所需的完整字段集合（已通过校验与规范化）。
struct ProblemData {
  std::string title;
  std::string description;
  std::string difficulty; // "easy" | "medium" | "hard"
  std::vector<std::string> tags;
  int time_limit_ms = kDefaultTimeLimitMs;
  int memory_limit_kb = kDefaultMemoryLimitKb;
  bool visible = true;
  std::vector<SampleInput> samples;
};

// 修改题目的部分更新字段：仅包含请求体中实际出现的字段。
// 未出现的字段保持数据库中的原值，不会被清空；需要清空时显式传入空值
// （如 "description" 传 ""、"tags"/"samples" 传 []）。
struct ProblemPatch {
  std::optional<std::string> title;
  std::optional<std::string> description;
  std::optional<std::string> difficulty;
  std::optional<std::vector<std::string>> tags;
  std::optional<int> time_limit_ms;
  std::optional<int> memory_limit_kb;
  std::optional<bool> visible;
  std::optional<std::vector<SampleInput>> samples;

  bool empty() const;
};

// 解析并校验「创建题目」请求体：
//   - 必填：title（非空、≤200 字节，去除首尾空白后入库）、difficulty（easy/medium/hard）；
//   - 可选：description（≤64KiB，默认 ""）、tags（字符串数组，最多 20 个，默认空）、
//     time_limit_ms（默认 2000）、memory_limit_kb（默认 65536）、visible（默认 true）、
//     samples（对象数组，最多 50 组，默认空）。
// 成功返回 true 并填充 out；失败返回 false 并写入用户可读的 error。
// 服务端管理的字段（id/created_at/updated_at/seed_key）不在读取范围内，客户端传入无效。
bool parse_create_problem(const nlohmann::json &body, ProblemData &out,
                          std::string &error);

// 解析并校验「修改题目」请求体（部分更新）。仅读取本文件中定义的字段，其余忽略。
// 请求体不含任何可更新字段时返回 false（error 说明没有可更新字段）。
bool parse_update_problem(const nlohmann::json &body, ProblemPatch &out,
                          std::string &error);

// 将标签列表按存储约定拼接为逗号分隔文本（标签已在校验阶段去除首尾空白且不含逗号）。
std::string join_tags(const std::vector<std::string> &tags);

} // namespace problem
} // namespace oj
