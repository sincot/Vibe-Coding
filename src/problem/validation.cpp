#include "problem/validation.h"

#include <algorithm>
#include <cctype>

namespace oj {
namespace problem {

namespace {

// 去除首尾 ASCII 空白；内部空白保留。
std::string trim(const std::string &text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool is_valid_difficulty(const std::string &value) {
  return value == "easy" || value == "medium" || value == "hard";
}

// 校验整数并落在 [min, max] 内。仅接受 JSON 整数（拒绝浮点、布尔与字符串），
// 且对超范围的无符号数做安全判断，避免取值溢出。
bool parse_int_in_range(const nlohmann::json &value, long long min,
                        long long max, int &out, const char *field,
                        std::string &error) {
  if (!value.is_number_integer()) {
    error = std::string(field) + " 须为整数";
    return false;
  }
  long long parsed = 0;
  if (value.is_number_unsigned()) {
    unsigned long long raw = value.get<unsigned long long>();
    if (raw > static_cast<unsigned long long>(max)) {
      error = std::string(field) + " 超出允许范围 [" + std::to_string(min) +
              ", " + std::to_string(max) + "]";
      return false;
    }
    parsed = static_cast<long long>(raw);
  } else {
    parsed = value.get<long long>();
  }
  if (parsed < min || parsed > max) {
    error = std::string(field) + " 超出允许范围 [" + std::to_string(min) +
            ", " + std::to_string(max) + "]";
    return false;
  }
  out = static_cast<int>(parsed);
  return true;
}

// 校验标签数组：字符串数组、去空白后非空、不含逗号、长度与数量受限、不允许重复。
// 存储约定为逗号分隔文本，故标签本身不得包含逗号，否则会破坏解析一致性。
bool parse_tags(const nlohmann::json &value, std::vector<std::string> &out,
                std::string &error) {
  if (!value.is_array()) {
    error = "tags 须为字符串数组";
    return false;
  }
  if (value.size() > kMaxTags) {
    error = "tags 数量过多（最多 " + std::to_string(kMaxTags) + " 个）";
    return false;
  }
  out.clear();
  for (const auto &item : value) {
    if (!item.is_string()) {
      error = "tags 须为字符串数组";
      return false;
    }
    std::string tag = trim(item.get<std::string>());
    if (tag.empty()) {
      error = "标签不能为空";
      return false;
    }
    if (tag.size() > kMaxTagBytes) {
      error = "标签过长（最多 " + std::to_string(kMaxTagBytes) + " 字节）";
      return false;
    }
    if (tag.find(',') != std::string::npos) {
      error = "标签不能包含逗号";
      return false;
    }
    if (std::find(out.begin(), out.end(), tag) != out.end()) {
      error = "标签重复：" + tag;
      return false;
    }
    out.push_back(std::move(tag));
  }
  return true;
}

// 校验公开样例数组：每个元素为含字符串字段 input/output 的对象。
// 输入/输出允许为空串（部分题目无输入），但不允许缺失或类型错误。
bool parse_samples(const nlohmann::json &value,
                   std::vector<SampleInput> &out, std::string &error) {
  if (!value.is_array()) {
    error = "samples 须为对象数组";
    return false;
  }
  if (value.size() > kMaxSamples) {
    error = "samples 数量过多（最多 " + std::to_string(kMaxSamples) + " 组）";
    return false;
  }
  out.clear();
  for (const auto &item : value) {
    if (!item.is_object() || !item.contains("input") ||
        !item["input"].is_string() || !item.contains("output") ||
        !item["output"].is_string()) {
      error = "样例须为含字符串字段 input 与 output 的对象";
      return false;
    }
    SampleInput sample;
    sample.input = item["input"].get<std::string>();
    sample.output = item["output"].get<std::string>();
    if (sample.input.size() > kMaxSampleFieldBytes ||
        sample.output.size() > kMaxSampleFieldBytes) {
      error = "样例字段过长（最多 " + std::to_string(kMaxSampleFieldBytes) +
              " 字节）";
      return false;
    }
    out.push_back(std::move(sample));
  }
  return true;
}

// 校验并规范化 title：字符串、去首尾空白后非空、长度不超过上限。
bool parse_title(const nlohmann::json &value, std::string &out,
                 std::string &error) {
  if (!value.is_string()) {
    error = "title 须为字符串";
    return false;
  }
  std::string title = trim(value.get<std::string>());
  if (title.empty()) {
    error = "title 不能为空";
    return false;
  }
  if (title.size() > kMaxTitleBytes) {
    error = "title 过长（最多 " + std::to_string(kMaxTitleBytes) + " 字节）";
    return false;
  }
  out = std::move(title);
  return true;
}

bool parse_description(const nlohmann::json &value, std::string &out,
                       std::string &error) {
  if (!value.is_string()) {
    error = "description 须为字符串";
    return false;
  }
  out = value.get<std::string>();
  if (out.size() > kMaxDescriptionBytes) {
    error = "description 过长（最多 " + std::to_string(kMaxDescriptionBytes) +
            " 字节）";
    return false;
  }
  return true;
}

bool parse_difficulty(const nlohmann::json &value, std::string &out,
                      std::string &error) {
  if (!value.is_string()) {
    error = "difficulty 须为字符串";
    return false;
  }
  std::string difficulty = value.get<std::string>();
  if (!is_valid_difficulty(difficulty)) {
    error = "非法难度：仅支持 easy/medium/hard";
    return false;
  }
  out = std::move(difficulty);
  return true;
}

} // namespace

bool ProblemPatch::empty() const {
  return !title.has_value() && !description.has_value() &&
         !difficulty.has_value() && !tags.has_value() &&
         !time_limit_ms.has_value() && !memory_limit_kb.has_value() &&
         !visible.has_value() && !samples.has_value();
}

std::string join_tags(const std::vector<std::string> &tags) {
  std::string joined;
  for (std::size_t i = 0; i < tags.size(); ++i) {
    if (i > 0) {
      joined.push_back(',');
    }
    joined += tags[i];
  }
  return joined;
}

bool parse_create_problem(const nlohmann::json &body, ProblemData &out,
                          std::string &error) {
  if (!body.is_object()) {
    error = "请求体必须是 JSON 对象";
    return false;
  }
  if (!body.contains("title")) {
    error = "缺少必填字段：title";
    return false;
  }
  if (!parse_title(body["title"], out.title, error)) {
    return false;
  }
  if (!body.contains("difficulty")) {
    error = "缺少必填字段：difficulty";
    return false;
  }
  if (!parse_difficulty(body["difficulty"], out.difficulty, error)) {
    return false;
  }
  if (body.contains("description") &&
      !parse_description(body["description"], out.description, error)) {
    return false;
  }
  if (body.contains("tags") && !parse_tags(body["tags"], out.tags, error)) {
    return false;
  }
  if (body.contains("time_limit_ms") &&
      !parse_int_in_range(body["time_limit_ms"], kMinTimeLimitMs,
                          kMaxTimeLimitMs, out.time_limit_ms, "time_limit_ms",
                          error)) {
    return false;
  }
  if (body.contains("memory_limit_kb") &&
      !parse_int_in_range(body["memory_limit_kb"], kMinMemoryLimitKb,
                          kMaxMemoryLimitKb, out.memory_limit_kb,
                          "memory_limit_kb", error)) {
    return false;
  }
  if (body.contains("visible")) {
    if (!body["visible"].is_boolean()) {
      error = "visible 须为布尔值";
      return false;
    }
    out.visible = body["visible"].get<bool>();
  }
  if (body.contains("samples") &&
      !parse_samples(body["samples"], out.samples, error)) {
    return false;
  }
  return true;
}

bool parse_update_problem(const nlohmann::json &body, ProblemPatch &out,
                          std::string &error) {
  if (!body.is_object()) {
    error = "请求体必须是 JSON 对象";
    return false;
  }
  if (body.contains("title")) {
    std::string title;
    if (!parse_title(body["title"], title, error)) {
      return false;
    }
    out.title = std::move(title);
  }
  if (body.contains("description")) {
    std::string description;
    if (!parse_description(body["description"], description, error)) {
      return false;
    }
    out.description = std::move(description);
  }
  if (body.contains("difficulty")) {
    std::string difficulty;
    if (!parse_difficulty(body["difficulty"], difficulty, error)) {
      return false;
    }
    out.difficulty = std::move(difficulty);
  }
  if (body.contains("tags")) {
    std::vector<std::string> tags;
    if (!parse_tags(body["tags"], tags, error)) {
      return false;
    }
    out.tags = std::move(tags);
  }
  if (body.contains("time_limit_ms")) {
    int value = 0;
    if (!parse_int_in_range(body["time_limit_ms"], kMinTimeLimitMs,
                            kMaxTimeLimitMs, value, "time_limit_ms", error)) {
      return false;
    }
    out.time_limit_ms = value;
  }
  if (body.contains("memory_limit_kb")) {
    int value = 0;
    if (!parse_int_in_range(body["memory_limit_kb"], kMinMemoryLimitKb,
                            kMaxMemoryLimitKb, value, "memory_limit_kb",
                            error)) {
      return false;
    }
    out.memory_limit_kb = value;
  }
  if (body.contains("visible")) {
    if (!body["visible"].is_boolean()) {
      error = "visible 须为布尔值";
      return false;
    }
    out.visible = body["visible"].get<bool>();
  }
  if (body.contains("samples")) {
    std::vector<SampleInput> samples;
    if (!parse_samples(body["samples"], samples, error)) {
      return false;
    }
    out.samples = std::move(samples);
  }
  if (out.empty()) {
    error = "没有可更新的字段";
    return false;
  }
  return true;
}

} // namespace problem
} // namespace oj
