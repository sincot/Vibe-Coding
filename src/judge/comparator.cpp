#include "judge/comparator.h"

#include <vector>

namespace oj {
namespace judge {

namespace {

// 行尾空白字符：空格、制表符、回车（CRLF 中的 \r）以及纵向制表/换页。
// 不含 '\n'，因为归一化已先按 '\n' 切分。
bool is_trailing_whitespace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

} // namespace

std::string normalize_output(const std::string &text) {
  std::vector<std::string> lines;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    std::size_t newline = text.find('\n', pos);
    std::size_t end = (newline == std::string::npos) ? text.size() : newline;
    std::string line = text.substr(pos, end - pos);
    while (!line.empty() && is_trailing_whitespace(line.back())) {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    if (newline == std::string::npos) {
      break;
    }
    pos = newline + 1;
  }

  // 去除文末空行（中间空行保留）。
  while (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }

  std::string result;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) {
      result.push_back('\n');
    }
    result += lines[i];
  }
  return result;
}

bool outputs_match(const std::string &expected, const std::string &actual) {
  return normalize_output(expected) == normalize_output(actual);
}

} // namespace judge
} // namespace oj
