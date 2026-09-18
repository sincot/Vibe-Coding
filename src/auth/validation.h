#pragma once

#include <cstddef>
#include <string>

namespace oj {
namespace auth {

// 昵称与密码输入规则（详见 SPEC M1.1 实施说明与 README）。
constexpr std::size_t kNicknameMaxLen = 30;
constexpr std::size_t kPasswordMaxLen = 128;

// 校验并规范化昵称：
//   - 去除首尾空白（ASCII 空格/制表符/换行等）后非空；
//   - 规范化后长度不超过 kNicknameMaxLen；
//   - 内部空白保留（允许昵称含空格）。
// 成功返回 true 并写入规范化后的昵称（注册查询与入库均使用该值，
// 保证校验 / 查询 / 数据库唯一性规则一致）；失败返回 false 并写入用户可读信息。
bool validate_nickname(const std::string &input, std::string &normalized,
                       std::string &error);

// 校验密码：
//   - 非空；
//   - 长度不超过 kPasswordMaxLen；
//   - 不做任何裁剪或截断，空白字符视为有效内容（密码完全按原样哈希）。
// 成功返回 true；失败返回 false 并写入用户可读信息。
bool validate_password(const std::string &password, std::string &error);

// 校验改密的新密码：复用注册密码规则（见 validate_password），并额外要求
// 新密码不得与旧密码相同，避免首次强制改密流于形式。新密码不做任何裁剪或截断。
// 成功返回 true；失败返回 false 并写入用户可读信息。
bool validate_password_change(const std::string &old_password,
                              const std::string &new_password,
                              std::string &error);

} // namespace auth
} // namespace oj
