#include "auth/jwt.h"

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

#include <chrono>
#include <cstdlib>

namespace oj {
namespace auth {

namespace {

constexpr const char *kIssuer = "oj";
constexpr const char *kAudience = "oj-api";

// 解析正整数（用于有效期环境变量）。
bool parse_positive_int(const std::string &text, int &out) {
  if (text.empty()) {
    return false;
  }
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  long value = 0;
  try {
    value = std::stol(text);
  } catch (...) {
    return false;
  }
  if (value < 1 || value > 31536000) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

// 校验 sub 是否为合法的用户 ID（十进制正整数，无前导加号/空白）。
bool parse_user_id(const std::string &text, std::int64_t &out) {
  if (text.empty() || text.size() > 19) {
    return false;
  }
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  try {
    out = std::stoll(text);
  } catch (...) {
    return false;
  }
  return out > 0;
}

} // namespace

bool load_jwt_config(JwtConfig &cfg, std::string &error) {
  const char *secret = std::getenv("OJ_JWT_SECRET");
  if (secret == nullptr || secret[0] == '\0') {
    error = "缺少 JWT 签名密钥：请通过环境变量 OJ_JWT_SECRET 提供（不得使用"
            "公开默认值，长度不少于 " +
            std::to_string(kMinJwtSecretLen) + " 字节）";
    return false;
  }
  cfg.secret = std::string(secret);
  if (cfg.secret.size() < kMinJwtSecretLen) {
    error = "JWT 签名密钥无效：OJ_JWT_SECRET 长度不足 " +
            std::to_string(kMinJwtSecretLen) + " 字节";
    return false;
  }

  const char *expires = std::getenv("OJ_JWT_EXPIRES_SECONDS");
  if (expires != nullptr) {
    if (!parse_positive_int(expires, cfg.expires_seconds)) {
      error = "JWT 有效期无效：OJ_JWT_EXPIRES_SECONDS 须为 1..31536000 之间的"
              "整数";
      return false;
    }
  }
  return true;
}

JwtService::JwtService(std::string secret, int expires_seconds)
    : secret_(std::move(secret)), expires_seconds_(expires_seconds) {}

bool JwtService::sign(std::int64_t user_id, std::string &token,
                      std::string &error) const {
  if (user_id <= 0) {
    error = "非法用户 ID";
    return false;
  }
  const auto now = std::chrono::system_clock::now();
  try {
    token = jwt::create()
                .set_type("JWT")
                .set_issuer(kIssuer)
                .set_audience(kAudience)
                .set_issued_at(now)
                .set_expires_at(now + std::chrono::seconds(expires_seconds_))
                .set_payload_claim("sub", jwt::claim(std::to_string(user_id)))
                .sign(jwt::algorithm::hs256{secret_});
    return true;
  } catch (const std::exception &e) {
    error = std::string("JWT 签发失败: ") + e.what();
    return false;
  }
}

bool JwtService::verify(const std::string &token, std::int64_t &user_id,
                        std::string &error) const {
  try {
    const auto decoded = jwt::decode(token);

    // 仅允许 HS256，且固定校验 iss/aud/exp/iat。alg 缺失或为 none/其它算法
    // 时 verifier 返回 wrong_algorithm。
    auto verifier = jwt::verify()
                        .allow_algorithm(jwt::algorithm::hs256{secret_})
                        .with_issuer(kIssuer)
                        .with_audience(kAudience);
    std::error_code ec;
    verifier.verify(decoded, ec);
    if (ec) {
      error = std::string("JWT 校验失败: ") + ec.message();
      return false;
    }

    // 强制要求必要的 claims 存在（exp/iat/sub），拒绝缺失必要 claims 的 token。
    if (!decoded.has_expires_at()) {
      error = "JWT 缺少 exp 声明";
      return false;
    }
    if (!decoded.has_issued_at()) {
      error = "JWT 缺少 iat 声明";
      return false;
    }
    if (!decoded.has_subject()) {
      error = "JWT 缺少 sub 声明";
      return false;
    }

    // 身份字段格式校验：sub 须为十进制正整数（对应数据库用户 id）。
    if (!parse_user_id(decoded.get_subject(), user_id)) {
      error = "JWT sub 声明格式非法";
      return false;
    }
    return true;
  } catch (const std::exception &e) {
    // 覆盖 base64/JSON 解析失败、类型错误等所有异常路径。
    error = std::string("JWT 解析失败: ") + e.what();
    return false;
  }
}

} // namespace auth
} // namespace oj
