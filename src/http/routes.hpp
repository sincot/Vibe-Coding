#pragma once

#include <cstdint>
#include <string>

#include "httplib.h"
#include "nlohmann/json.hpp"

#include "auth/rate_limit.hpp"
#include "db/database.hpp"

namespace oj {
namespace http {

// 认证会话：RequireAuth/RequireAdmin 校验通过后携带的上下文，
// 直接来自数据库（非 token 载荷），保证 role/reset_pwd_flag 新鲜。
struct Session {
  std::int64_t user_id = 0;
  std::string account;
  std::string nickname;
  std::string role;
  bool reset_pwd_flag = false;
  std::string created_at;
};

// 提取 Authorization 头的 Bearer token；无此头返回空串。
std::string ExtractBearerToken(const httplib::Request& req);

// 认证中间件：要求登录。成功返回 true 并填充 session（数据从数据库实时读取，
// 保证账号被删除/权限变化后立即失效）；失败写 JSON 错误响应并返回 false。
// admin 且 reset_pwd_flag=1 时除非 allow_reset_pwd 指定，否则视为未改密而拒绝。
bool RequireAuth(const httplib::Request& req, httplib::Response& res, Database& db,
                 const std::string& jwt_secret, Session* session);

// 权限中间件：要求管理员。内部先调 RequireAuth，再校验 role=='admin'；
// 管理员首次登录未改密（reset_pwd_flag=1）时拒绝访问管理功能。
bool RequireAdmin(const httplib::Request& req, httplib::Response& res, Database& db,
                  const std::string& jwt_secret, Session* session);

// 校验密码策略：非空、6~128 个字符。返回是否合法。
bool IsValidPassword(const std::string& password);

// 校验昵称策略：去除首尾空白后 1~30 个字符。返回是否合法。
bool IsValidNickname(const std::string& nickname);

// 注册 M1 认证相关的全部路由（/api/register|login|me|me/password）。
void RegisterAuthRoutes(httplib::Server& svr, Database& db,
                        const std::string& jwt_secret,
                        auth::LoginRateLimiter& limiter);

}  // namespace http
}  // namespace oj