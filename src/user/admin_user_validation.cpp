#include "user/admin_user_validation.h"

#include <cctype>
#include <limits>

#include "auth/validation.h"

namespace oj {
namespace useradmin {

namespace {

constexpr const char *kActionResetPassword = "reset_password";
constexpr const char *kActionChangeRole = "change_role";

std::string trim_ascii(const std::string &text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  const auto is_ws = [](unsigned char c) {
    return std::isspace(c) != 0;
  };
  while (begin < end && is_ws(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && is_ws(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

// 将 JSON 中的 user_id 解析为 int64：只接受整数（含无符号整数，但须在 int64
// 正数范围内），拒绝浮点、字符串、布尔、null。value <= 0 由调用方拒绝。
bool parse_user_id(const nlohmann::json &value, std::int64_t &out) {
  if (value.is_number_unsigned()) {
    const auto raw = value.get<unsigned long long>();
    if (raw == 0 ||
        raw > static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    out = static_cast<std::int64_t>(raw);
    return true;
  }
  if (!value.is_number_integer()) {
    return false;
  }
  const auto raw = value.get<long long>();
  if (raw <= 0) {
    return false;
  }
  out = static_cast<std::int64_t>(raw);
  return true;
}

} // namespace

bool is_valid_role(const std::string &role) {
  return role == "admin" || role == "user";
}

bool parse_request(const nlohmann::json &body, Request &out, std::string &error) {
  out = Request{};

  if (!body.is_object()) {
    error = "请求体必须是 JSON 对象";
    return false;
  }

  if (!body.contains("action") || !body["action"].is_string()) {
    error = "缺少字段或类型错误：action 须为字符串";
    return false;
  }
  const std::string action = body["action"].get<std::string>();
  if (action == kActionResetPassword) {
    out.action = Action::ResetPassword;
  } else if (action == kActionChangeRole) {
    out.action = Action::ChangeRole;
  } else {
    error = "未知操作：action 仅支持 reset_password / change_role";
    return false;
  }

  if (!body.contains("user_id")) {
    error = "缺少字段或类型错误：user_id 须为正整数";
    return false;
  }
  if (!parse_user_id(body["user_id"], out.user_id)) {
    error = "缺少字段或类型错误：user_id 须为正整数";
    return false;
  }

  if (out.action == Action::ResetPassword) {
    if (!body.contains("new_password") ||
        !body["new_password"].is_string()) {
      error = "缺少字段或类型错误：new_password 须为字符串";
      return false;
    }
    out.new_password = body["new_password"].get<std::string>();
    // 复用注册密码规则（非空、≤128、不裁剪不截断）；与目标用户旧密码无关。
    std::string password_error;
    if (!auth::validate_password(out.new_password, password_error)) {
      error = "new_password 非法：" + password_error;
      return false;
    }
  } else {
    if (!body.contains("role") || !body["role"].is_string()) {
      error = "缺少字段或类型错误：role 须为字符串";
      return false;
    }
    out.role = body["role"].get<std::string>();
    if (!is_valid_role(out.role)) {
      error = "role 取值非法：仅支持 admin/user";
      return false;
    }
  }

  return true;
}

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
    error =
        "page 超出允许范围（最大 " + std::to_string(kMaxUserPage) + "）";
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
  if (value > kMaxUserPage) {
    error =
        "page 超出允许范围（最大 " + std::to_string(kMaxUserPage) + "）";
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

} // namespace useradmin
} // namespace oj
