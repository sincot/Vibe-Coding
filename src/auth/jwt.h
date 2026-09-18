#pragma once

#include <cstdint>
#include <string>

namespace oj {
namespace auth {

// JWT 签名密钥最短长度（字节）。低于该长度视为「配置无效」，避免弱密钥。
constexpr std::size_t kMinJwtSecretLen = 16;

// JWT 默认有效期（秒）。可通过 OJ_JWT_EXPIRES_SECONDS 覆盖。
constexpr int kDefaultJwtExpiresSeconds = 3600;

// JWT 配置：签名密钥与有效期。密钥绝不硬编码、不写入日志或版本控制，
// 由环境变量 OJ_JWT_SECRET 提供；同一密钥下重启前后签发的 token 仍可验证。
struct JwtConfig {
  std::string secret;
  int expires_seconds = kDefaultJwtExpiresSeconds;
};

// 从环境变量读取并校验 JWT 配置：
//   - OJ_JWT_SECRET：必需；非空且长度 >= kMinJwtSecretLen，否则报错。
//   - OJ_JWT_EXPIRES_SECONDS：可选；默认 3600，须为 1..31536000 之间的整数。
// 成功返回 true；失败返回 false 并通过 error 给出明确原因（供启动时以非零码退出）。
bool load_jwt_config(JwtConfig &cfg, std::string &error);

// JWT 签发与验证封装（HS256，仅允许该算法）。身份标识使用稳定的数据库用户
// ID（sub 为十进制字符串）。签发 claims：iss / aud / iat / exp / sub；
// 验证时要求签名、算法、iss、aud、exp、iat、sub 全部满足。
class JwtService {
public:
  JwtService(std::string secret, int expires_seconds);

  // 为用户签发 token。成功返回 true；失败返回 false 并写入 error。
  bool sign(std::int64_t user_id, std::string &token, std::string &error) const;

  // 验证 token。成功返回 true 并写入 user_id；失败返回 false（原因写入 error，
  // 用于内部诊断，不对外暴露 token 细节）。任何解析/签名/算法/claims/格式错误
  // 均返回 false，不抛出异常。
  bool verify(const std::string &token, std::int64_t &user_id,
              std::string &error) const;

  int expires_seconds() const { return expires_seconds_; }

private:
  std::string secret_;
  int expires_seconds_;
};

} // namespace auth
} // namespace oj
