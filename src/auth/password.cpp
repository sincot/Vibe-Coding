#include "auth/password.hpp"

#include <argon2.h>
#include <sys/random.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>

namespace oj {
namespace {

// argon2id 推荐参数：t=2, m=64MiB, p=1（教学规模单机部署，兼顾安全与速度）。
constexpr uint32_t kTCost = 2;
constexpr uint32_t kMCost = 1u << 16;
constexpr uint32_t kParallelism = 1;
constexpr size_t kSaltLen = 16;
constexpr size_t kHashLen = 32;

bool FillRandom(unsigned char* buf, size_t len) {
  size_t done = 0;
  while (done < len) {
    const ssize_t n = ::getrandom(buf + done, len - done, 0);
    if (n > 0) {
      done += static_cast<size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

bool FillRandomFile(unsigned char* buf, size_t len) {
  std::FILE* f = std::fopen("/dev/urandom", "rb");
  if (f == nullptr) {
    return false;
  }
  const bool ok = std::fread(buf, 1, len, f) == len;
  std::fclose(f);
  return ok;
}

}  // namespace

std::string HashPassword(const std::string& plain) {
  unsigned char salt[kSaltLen];
  if (!FillRandom(salt, sizeof(salt)) && !FillRandomFile(salt, sizeof(salt))) {
    return {};
  }
  char encoded[256];
  const int rc = argon2id_hash_encoded(kTCost, kMCost, kParallelism, plain.data(),
                                       plain.size(), salt, sizeof(salt), kHashLen,
                                       encoded, sizeof(encoded));
  if (rc != ARGON2_OK) {
    return {};
  }
  return std::string(encoded);
}

bool VerifyPassword(const std::string& plain, const std::string& stored_hash) {
  if (stored_hash.empty()) {
    return false;
  }
  return argon2id_verify(stored_hash.c_str(), plain.data(), plain.size()) == ARGON2_OK;
}

}  // namespace oj