#include "auth/account.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>

namespace oj {
namespace auth {

namespace {

constexpr std::uint64_t kAccountSpace = 10000000000ULL; // 10^10，10 位数字

// 从 /dev/urandom 读取 8 字节随机数；失败返回 false。
bool read_random_u64(std::uint64_t &out) {
  std::ifstream urandom("/dev/urandom", std::ios::binary);
  if (!urandom) {
    return false;
  }
  urandom.read(reinterpret_cast<char *>(&out), sizeof(out));
  return urandom.gcount() == static_cast<std::streamsize>(sizeof(out));
}

} // namespace

std::string RandomAccountGenerator::generate() {
  std::uint64_t value = 0;
  if (!read_random_u64(value)) {
    // 兜底：std::random_device（Linux 上一般也基于 /dev/urandom）。
    // 函数内静态对象在 C++11 起保证线程安全初始化。
    static std::random_device rd;
    std::uint64_t lo = rd();
    std::uint64_t hi = rd();
    value = (hi << 32) ^ lo;
  }

  // 取模到 10^10 空间。此处存在可忽略的取模偏置；账号唯一性由数据库约束保证，
  // 随机源只需「不可预测且分布均匀」即可避免频繁碰撞。
  value %= kAccountSpace;

  char buf[16] = {0};
  std::snprintf(buf, sizeof(buf), "%010llu",
                static_cast<unsigned long long>(value));
  return std::string(buf);
}

} // namespace auth
} // namespace oj
