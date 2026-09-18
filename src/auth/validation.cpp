#include "auth/validation.h"

#include <string>

namespace oj {
namespace auth {

namespace {

// 去除首尾 ASCII 空白。仅处理首尾，内部空白保留。
std::string trim_ascii(const std::string &s) {
  const auto is_ws = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
  };
  std::size_t begin = 0;
  while (begin < s.size() && is_ws(static_cast<unsigned char>(s[begin]))) {
    ++begin;
  }
  std::size_t end = s.size();
  while (end > begin && is_ws(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(begin, end - begin);
}

} // namespace

bool validate_nickname(const std::string &input, std::string &normalized,
                       std::string &error) {
  normalized = trim_ascii(input);
  if (normalized.empty()) {
    error = "昵称不能为空";
    return false;
  }
  if (normalized.size() > kNicknameMaxLen) {
    error = "昵称过长（最多 " + std::to_string(kNicknameMaxLen) + " 个字符）";
    return false;
  }
  return true;
}

bool validate_password(const std::string &password, std::string &error) {
  if (password.empty()) {
    error = "密码不能为空";
    return false;
  }
  if (password.size() > kPasswordMaxLen) {
    error = "密码过长（最多 " + std::to_string(kPasswordMaxLen) + " 个字符）";
    return false;
  }
  return true;
}

} // namespace auth
} // namespace oj
