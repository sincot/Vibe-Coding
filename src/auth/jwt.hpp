#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace oj {
namespace auth {

// JWT 负载：认证中间件验证通过后携带的会话数据。
struct JwtPayload {
  int64_t user_id = 0;
  std::string account;
  std::string nickname;
  std::string role;  // 'admin' | 'user'
  // 管理员首次登录必须改密的标记，直接取自数据库（每次请求刷新）。
  bool reset_pwd_flag = false;
};

// 生成 HS256 签名密钥（32 字节随机），取自 /dev/urandom；失败返回空串。
std::string GenerateJwtSecret();

// 签发 JWT（HS256，ttl 秒）。返回空串表示失败。
std::string EncodeToken(const std::string& secret, const JwtPayload& payload,
                        std::chrono::seconds ttl);

// 校验并解码 JWT。合法返回 true 并填充 payload；非法/过期返回 false。
bool DecodeToken(const std::string& secret, const std::string& token,
                 JwtPayload* payload);

}  // namespace auth
}  // namespace oj