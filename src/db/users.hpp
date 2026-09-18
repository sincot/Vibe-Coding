#pragma once

#include <cstdint>
#include <string>

#include "db/database.hpp"

namespace oj {
namespace db {

// 用户信息统一结构。
struct UserInfo {
  std::int64_t id = 0;
  std::string account;
  std::string nickname;
  std::string role;  // 'admin' | 'user'
  bool reset_pwd_flag = false;
  std::string created_at;
  // 仅 VerifyLogin 查询时填充（登录场景内部使用），其余查询不返回密码哈希。
  std::string password_hash;
};

// 用户注册结果码。
enum class CreateUserResult {
  kOk,                // 创建成功，UserInfo 有效
  kNicknameTaken,     // 昵称已占用
  kInvalidPassword,   // 密码为空等非法值
  kInternalError,     // 内部错误（哈希失败、数据库故障、账号分配失败）
};

// 登录校验结果码。
enum class LoginResult {
  kOk,           // 账号存在且密码正确
  kBadCredential // 账号不存在或密码错误（对外统一，不泄露账号是否存在）
};

// 注册新用户：服务端生成 10 位纯数字唯一账号（首位 1-9），存入 argon2id 哈希。
// 账号生成与插入在同一互斥区内完成（共享 Database 的全局锁）。
CreateUserResult CreateUser(Database& db, const std::string& nickname,
                            const std::string& plain_password, UserInfo* out);

// 按账号校验账号+密码（argon2id 比对）。账号不存在与密码错误均返回
// kBadCredential，供登录限速统一统计。校验通过时填充 out（含 role/reset 标记）。
LoginResult VerifyLogin(Database& db, const std::string& account,
                        const std::string& plain_password, UserInfo* out);

// 按账号查用户（admin 账号为文本，普通用户为 10 位数字）。不存在返回 false。
bool GetUserByAccount(Database& db, const std::string& account, UserInfo* out);

// 按昵称查用户（AUTH-02 唯一性检查用）。不存在返回 false。
bool GetUserByNickname(Database& db, const std::string& nickname, UserInfo* out);

// 按主键查用户。
bool GetUserById(Database& db, std::int64_t id, UserInfo* out);

// 校验旧密码并设置新密码；成功后清除首次改密标记（reset_pwd_flag=0）。
// 返回 0=成功，1=旧密码错误，2=用户不存在，3=内部错误。
int ChangePassword(Database& db, std::int64_t user_id,
                   const std::string& old_password,
                   const std::string& new_password);

}  // namespace db
}  // namespace oj