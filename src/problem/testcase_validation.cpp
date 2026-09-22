#include "problem/testcase_validation.h"

namespace oj {
namespace problem {

namespace {

// 校验整数（仅接受 JSON 整数，拒绝浮点/布尔/字符串），并落在 [min, max] 内；
// 对超范围的无符号数做安全判断，避免取值溢出。与 problem/validation.cpp 的规则一致。
bool parse_int_in_range(const nlohmann::json &value, long long min, long long max,
                        int &out, const char *field, std::string &error) {
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

// 校验文本字段：须为字符串、字节数不超过上限。空字符串合法；不做任何 trim 或
// 输出归一化，原样保留空格、制表符与换行（归一化只在判题比对阶段进行）。
bool parse_text(const nlohmann::json &value, std::string &out, const char *field,
                std::string &error) {
  if (!value.is_string()) {
    error = std::string(field) + " 须为字符串";
    return false;
  }
  out = value.get<std::string>();
  if (out.size() > kMaxTestcaseTextBytes) {
    error = std::string(field) + " 过长（最多 " +
            std::to_string(kMaxTestcaseTextBytes) + " 字节）";
    return false;
  }
  return true;
}

} // namespace

bool TestcasePatch::empty() const {
  return !input.has_value() && !output.has_value() && !ord.has_value();
}

bool parse_create_testcase(const nlohmann::json &body, TestcaseData &out,
                           std::string &error) {
  if (!body.is_object()) {
    error = "请求体必须是 JSON 对象";
    return false;
  }
  if (!body.contains("input")) {
    error = "缺少必填字段：input";
    return false;
  }
  if (!parse_text(body["input"], out.input, "input", error)) {
    return false;
  }
  if (!body.contains("output")) {
    error = "缺少必填字段：output";
    return false;
  }
  if (!parse_text(body["output"], out.output, "output", error)) {
    return false;
  }
  if (body.contains("ord")) {
    int ord = 0;
    if (!parse_int_in_range(body["ord"], kMinTestcaseOrd, kMaxTestcaseOrd, ord,
                            "ord", error)) {
      return false;
    }
    out.ord = ord;
  }
  return true;
}

bool parse_update_testcase(const nlohmann::json &body, TestcasePatch &out,
                           std::string &error) {
  if (!body.is_object()) {
    error = "请求体必须是 JSON 对象";
    return false;
  }
  if (body.contains("input")) {
    std::string input;
    if (!parse_text(body["input"], input, "input", error)) {
      return false;
    }
    out.input = std::move(input);
  }
  if (body.contains("output")) {
    std::string output;
    if (!parse_text(body["output"], output, "output", error)) {
      return false;
    }
    out.output = std::move(output);
  }
  if (body.contains("ord")) {
    int ord = 0;
    if (!parse_int_in_range(body["ord"], kMinTestcaseOrd, kMaxTestcaseOrd, ord,
                            "ord", error)) {
      return false;
    }
    out.ord = ord;
  }
  if (out.empty()) {
    error = "没有可更新的字段";
    return false;
  }
  return true;
}

} // namespace problem
} // namespace oj
