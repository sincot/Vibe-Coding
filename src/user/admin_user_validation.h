#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace oj {
namespace useradmin {

// 管理员用户接口（M2.4）的纯参数解析与校验逻辑。
//
// 本文件只做与数据库/HTTP 无关的字符串处理与校验，便于独立单元测试：
//   - 解析 PUT /api/admin/users 的请求体为明确的「操作类型 + 目标用户 + 载荷」；
//   - 解析用户列表的分页参数。
// 实际的数据库读写、事务、最后管理员保护由 UserAdminStore 与 HTTP 层完成。
//
// 设计约束：请求体只读取下面明确列出的字段，其余字段（如 account/nickname/
// reset_pwd_flag/password_hash 等）一律忽略，不同操作只允许修改对应字段，
// 绝不把请求体任意映射到 users 表。

// 支持的管理员用户操作类型。
enum class Action {
  ResetPassword, // 重置目标用户密码（只改 password_hash + reset_pwd_flag）
  ChangeRole,    // 修改目标用户角色（只改 role）
};

// 解析后的请求。不同操作只填充对应字段：
//   - ResetPassword：user_id + new_password
//   - ChangeRole：user_id + role
struct Request {
  Action action = Action::ResetPassword;
  std::int64_t user_id = 0;
  std::string new_password; // 仅 ResetPassword
  std::string role;         // 仅 ChangeRole，取值 "admin" | "user"
};

// role 字段允许的取值集合（与 users.role 的 CHECK 约束一致）。
bool is_valid_role(const std::string &role);

// 解析并校验「修改用户」请求体：
//   - body 必须是 JSON 对象；
//   - 必填 action：字符串，严格取值 "reset_password" | "change_role"（大小写敏感）；
//   - 必填 user_id：正整数（拒绝 0、负数、小数、字符串及超大无符号数）；
//   - reset_password：必填 new_password（字符串，复用注册密码规则：非空、≤128、
//     不裁剪不截断）；不要求、也不读取目标用户旧密码；
//   - change_role：必填 role（字符串，严格取值 admin/user）。
// 成功返回 true 并填充 out；失败返回 false 并写入面向客户端的 error。
// 未知 action、缺失/类型错误/非法取值一律返回 false，不产生任何副作用。
bool parse_request(const nlohmann::json &body, Request &out, std::string &error);

// 用户列表分页（沿用 GET /api/problems 的既有约定）：
//   - 每页固定 20 条；
//   - page 默认 1，仅接受十进制正整数，范围 [1, kMaxUserPage]；
//   - 非法格式（非数字、小数、负数、0、空以外的空白）与超出上限一律拒绝。
constexpr int kUserPageSize = 20;
constexpr long long kMaxUserPage = 1000000;

// 解析 page 参数。空串（参数缺失）表示默认第 1 页。成功返回 true。
bool parse_page(const std::string &text, int &out, std::string &error);

} // namespace useradmin
} // namespace oj
