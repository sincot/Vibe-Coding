#pragma once

#include <string>

namespace oj {
namespace auth {

// 生成 argon2id 密码哈希（编码字符串形式，包含算法标识、参数、盐与哈希值）。
// 成功返回 true 并将结果写入 encoded；失败返回 false 并写入 error。
bool hash_password(const std::string &password, std::string &encoded,
                   std::string &error);

// 校验密码是否与已保存的编码哈希匹配。
//   - 匹配返回 true
//   - 不匹配返回 false 且 error 为空
//   - 校验过程出错（如哈希格式非法）返回 false 并写入 error
bool verify_password(const std::string &encoded, const std::string &password,
                     std::string &error);

} // namespace auth
} // namespace oj
