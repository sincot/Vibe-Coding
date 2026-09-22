#include "problem/list_query.h"

#include <cctype>

namespace oj {
namespace problem {

std::string trim_ascii(const std::string &text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end &&
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool is_valid_difficulty(const std::string &difficulty) {
  return difficulty == "easy" || difficulty == "medium" ||
         difficulty == "hard";
}

std::string escape_like(const std::string &text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (char c : text) {
    if (c == '\\' || c == '%' || c == '_') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return escaped;
}

std::string title_like_pattern(const std::string &keyword) {
  return "%" + escape_like(keyword) + "%";
}

std::string tag_match_needle(const std::string &tag) {
  return "," + tag + ",";
}

namespace {

// 解析 page：仅接受正十进制整数（允许前导零），范围 [1, kMaxProblemPage]。
bool parse_page(const std::string &text, int &out, std::string &error) {
  const std::string trimmed = trim_ascii(text);
  if (trimmed.empty()) {
    out = 1;
    return true;
  }
  for (char c : trimmed) {
    if (c < '0' || c > '9') {
      error = "page 必须为正整数";
      return false;
    }
  }
  // 去掉前导零后再判断位数，避免用超长数字串触发解析开销/溢出。
  std::size_t begin = 0;
  while (begin + 1 < trimmed.size() && trimmed[begin] == '0') {
    ++begin;
  }
  const std::string digits = trimmed.substr(begin);
  if (digits.size() > 7) {
    error = "page 超出允许范围（最大 " + std::to_string(kMaxProblemPage) +
            "）";
    return false;
  }
  long long value = 0;
  for (char c : digits) {
    value = value * 10 + (c - '0');
  }
  if (value < 1) {
    error = "page 必须为正整数";
    return false;
  }
  if (value > kMaxProblemPage) {
    error = "page 超出允许范围（最大 " + std::to_string(kMaxProblemPage) +
            "）";
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

// 解析 visible：all/1/true/0/false（大小写不敏感）；空视为 all。
bool parse_visible(const std::string &text, VisibleFilter &out,
                   std::string &error) {
  std::string value = trim_ascii(text);
  for (char &c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (value.empty() || value == "all") {
    out = VisibleFilter::All;
    return true;
  }
  if (value == "1" || value == "true") {
    out = VisibleFilter::OnlyVisible;
    return true;
  }
  if (value == "0" || value == "false") {
    out = VisibleFilter::OnlyHidden;
    return true;
  }
  error = "visible 取值非法：仅支持 all/1/true/0/false";
  return false;
}

} // namespace

bool parse_list_query(const RawListParams &raw, ListFilter &out,
                      std::string &error) {
  out = ListFilter{};

  out.keyword = trim_ascii(raw.q);
  out.tag = trim_ascii(raw.tag);

  out.difficulty = trim_ascii(raw.difficulty);
  if (!out.difficulty.empty() && !is_valid_difficulty(out.difficulty)) {
    error = "difficulty 取值非法：仅支持 easy/medium/hard";
    return false;
  }

  if (!parse_page(raw.page, out.page, error)) {
    return false;
  }
  if (!parse_visible(raw.visible, out.visible, error)) {
    return false;
  }
  out.page_size = kProblemPageSize;
  return true;
}

} // namespace problem
} // namespace oj
