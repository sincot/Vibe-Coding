#pragma once

#include <string>

namespace oj {

// 使用 argon2id 计算密码哈希（内含随机盐），失败返回空字符串。
std::string HashPassword(const std::string& plain);

// 校验明文密码是否与存储的 argon2id 哈希匹配。
bool VerifyPassword(const std::string& plain, const std::string& stored_hash);

}  // namespace oj