#include "auth/context.h"

#include <cctype>
#include <string>

#include "auth/jwt.h"
#include "db/database.h"
#include "log.h"

namespace oj {
namespace auth {

bool extract_bearer_token(const std::string &authorization,
                          std::string &token) {
  // 找到首个非空白字符作为 scheme 起点。
  std::size_t i = 0;
  while (i < authorization.size() &&
         std::isspace(static_cast<unsigned char>(authorization[i]))) {
    ++i;
  }
  constexpr const char *kScheme = "Bearer";
  constexpr std::size_t kSchemeLen = 6;
  if (i + kSchemeLen > authorization.size()) {
    return false;
  }
  for (std::size_t j = 0; j < kSchemeLen; ++j) {
    char c = authorization[i + j];
    char expected = kScheme[j];
    if (std::toupper(static_cast<unsigned char>(c)) !=
        std::toupper(static_cast<unsigned char>(expected))) {
      return false;
    }
  }
  std::size_t pos = i + kSchemeLen;
  // scheme 后须为空白分隔。
  if (pos >= authorization.size() ||
      !std::isspace(static_cast<unsigned char>(authorization[pos]))) {
    return false;
  }
  // 跳过空白取 token。
  std::size_t begin = pos;
  while (begin < authorization.size() &&
         std::isspace(static_cast<unsigned char>(authorization[begin]))) {
    ++begin;
  }
  if (begin >= authorization.size()) {
    return false;
  }
  // token 本身不允许包含空白：取到下一个空白即停止，若其后仍有非空白则视为非法。
  std::size_t end = begin;
  while (end < authorization.size() &&
         !std::isspace(static_cast<unsigned char>(authorization[end]))) {
    ++end;
  }
  for (std::size_t k = end; k < authorization.size(); ++k) {
    if (!std::isspace(static_cast<unsigned char>(authorization[k]))) {
      return false;
    }
  }
  token = authorization.substr(begin, end - begin);
  return !token.empty();
}

AuthResult authenticate_request(JwtService &jwt, UserStore &store,
                                const std::string &token, AuthUser &user,
                                std::string &error) {
  std::int64_t user_id = 0;
  if (!jwt.verify(token, user_id, error)) {
    return AuthResult::Unauthorized;
  }

  bool found = false;
  UserRecord record;
  if (!store.find_by_id(user_id, found, record, error)) {
    log(LogLevel::Error, "身份验证：按 ID 查询用户失败: " + error);
    return AuthResult::InternalError;
  }
  if (!found) {
    error = "用户不存在";
    return AuthResult::Unauthorized;
  }

  user.id = record.id;
  user.account = record.account;
  user.nickname = record.nickname;
  user.role = record.role;
  user.reset_pwd_flag = record.reset_pwd_flag;
  return AuthResult::Ok;
}

} // namespace auth
} // namespace oj
