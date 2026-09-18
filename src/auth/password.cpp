#include "auth/password.h"

#include <argon2.h>

#include <cstdint>
#include <fstream>
#include <vector>

namespace oj {
namespace auth {

namespace {

// argon2id 参数（AUTH-03）。t_cost=2 次迭代、m_cost=64 MiB、单 lane，
// 盐 16 字节、输出 32 字节。编码后哈希长度由 argon2_encodedlen 计算。
constexpr std::uint32_t kTimeCost = 2;
constexpr std::uint32_t kMemoryCost = 1u << 16; // 65536 KiB = 64 MiB
constexpr std::uint32_t kParallelism = 1;
constexpr std::size_t kSaltLen = 16;
constexpr std::size_t kHashLen = 32;

// 从 /dev/urandom 读取密码学安全的随机字节作为盐。
bool random_bytes(void *buf, std::size_t len) {
  std::ifstream urandom("/dev/urandom", std::ios::binary);
  if (!urandom) {
    return false;
  }
  urandom.read(reinterpret_cast<char *>(buf),
               static_cast<std::streamsize>(len));
  return urandom.gcount() == static_cast<std::streamsize>(len);
}

} // namespace

bool hash_password(const std::string &password, std::string &encoded,
                   std::string &error) {
  unsigned char salt[kSaltLen] = {0};
  if (!random_bytes(salt, sizeof(salt))) {
    error = "无法从 /dev/urandom 读取随机盐";
    return false;
  }

  const std::size_t enc_len =
      argon2_encodedlen(kTimeCost, kMemoryCost, kParallelism,
                        static_cast<std::uint32_t>(kSaltLen),
                        static_cast<std::uint32_t>(kHashLen), Argon2_id);
  std::vector<char> buffer(enc_len + 1, '\0');

  int rc = argon2id_hash_encoded(
      kTimeCost, kMemoryCost, kParallelism, password.data(), password.size(),
      salt, sizeof(salt), kHashLen, buffer.data(), buffer.size());
  if (rc != ARGON2_OK) {
    error = std::string("argon2id 哈希失败: ") + argon2_error_message(rc);
    return false;
  }

  encoded.assign(buffer.data());
  return true;
}

bool verify_password(const std::string &encoded, const std::string &password,
                     std::string &error) {
  int rc = argon2id_verify(encoded.c_str(), password.data(), password.size());
  if (rc == ARGON2_OK) {
    return true;
  }
  if (rc == ARGON2_VERIFY_MISMATCH) {
    return false;
  }
  error = std::string("argon2id 校验失败: ") + argon2_error_message(rc);
  return false;
}

} // namespace auth
} // namespace oj
