#include "http/routes.hpp"

#include <cstdint>
#include <string>
#include <utility>

#include "auth/jwt.hpp"
#include "db/users.hpp"
#include "nlohmann/json.hpp"

namespace oj {
namespace http {
namespace {

using SessionData = db::UserInfo;

void SendJson(httplib::Response& res, int status, const nlohmann::json& body) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

void SendError(httplib::Response& res, int status, const std::string& message) {
  SendJson(res, status, nlohmann::json{{"error", message}});
}

std::string Trim(const std::string& s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
    ++b;
  }
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
    --e;
  }
  return s.substr(b, e - b);
}

bool ParseBodyJson(const httplib::Request& req, nlohmann::json* out,
                   httplib::Response& res) {
  try {
    *out = nlohmann::json::parse(req.body);
    return true;
  } catch (const std::exception&) {
    SendError(res, 400, "invalid JSON body");
    return false;
  }
}

// 读请求中 String 字段；缺失/非字符串返回 false（不写响应）。
bool BodyString(const nlohmann::json& j, const char* key, std::string* out) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) {
    return false;
  }
  *out = it->get<std::string>();
  return true;
}

Session FromUserInfo(const db::UserInfo& u) {
  Session s;
  s.user_id = u.id;
  s.account = u.account;
  s.nickname = u.nickname;
  s.role = u.role;
  s.reset_pwd_flag = u.reset_pwd_flag;
  s.created_at = u.created_at;
  return s;
}

nlohmann::json SessionJson(const Session& s) {
  return {{"account", s.account},
          {"nickname", s.nickname},
          {"role", s.role},
          {"reset_pwd_flag", s.reset_pwd_flag},
          {"created_at", s.created_at}};
}

}  // namespace

std::string ExtractBearerToken(const httplib::Request& req) {
  const std::string& auth = req.get_header_value("Authorization");
  static const std::string kPrefix = "Bearer ";
  if (auth.size() > kPrefix.size() &&
      auth.compare(0, kPrefix.size(), kPrefix) == 0) {
    return auth.substr(kPrefix.size());
  }
  return {};
}

bool RequireAuth(const httplib::Request& req, httplib::Response& res, Database& db,
                 const std::string& jwt_secret, Session* session) {
  auth::JwtPayload payload;
  const std::string token = ExtractBearerToken(req);
  if (token.empty() ||
      !auth::DecodeToken(jwt_secret, token, &payload)) {
    SendError(res, 401, "missing or invalid token; please log in");
    return false;
  }

  db::UserInfo user;
  if (!db::GetUserById(db, payload.user_id, &user)) {
    SendError(res, 401, "user no longer exists; please log in again");
    return false;
  }
  if (session) {
    *session = FromUserInfo(user);
  }
  return true;
}

bool RequireAdmin(const httplib::Request& req, httplib::Response& res, Database& db,
                  const std::string& jwt_secret, Session* session) {
  Session s;
  if (!RequireAuth(req, res, db, jwt_secret, &s)) {
    return false;
  }
  if (s.role != "admin") {
    SendError(res, 403, "admin privilege required");
    return false;
  }
  if (s.reset_pwd_flag) {
    SendError(res, 403,
              "admin must change the initial password via POST /api/me/password "
              "before using the management console");
    return false;
  }
  if (session) {
    *session = s;
  }
  return true;
}

bool IsValidPassword(const std::string& password) {
  return !password.empty() && password.size() >= 6 && password.size() <= 128;
}

bool IsValidNickname(const std::string& nickname) {
  const std::string t = Trim(nickname);
  return !t.empty() && t.size() <= 30;
}

void RegisterAuthRoutes(httplib::Server& svr, Database& db,
                        const std::string& jwt_secret,
                        auth::LoginRateLimiter& limiter) {
  // Token 有效期：24 小时。JWT 密钥每次进程启动随机生成（见 main.cpp）。
  static constexpr std::chrono::minutes kTokenTtl = std::chrono::hours(24);

  // 公开：注册（分配 10 位唯一数字账号 + 昵称 + 密码）。
  svr.Post("/api/register", [&db, &jwt_secret](
                                const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!ParseBodyJson(req, &body, res)) {
      return;
    }
    std::string nickname;
    std::string password;
    if (!BodyString(body, "nickname", &nickname) ||
        !BodyString(body, "password", &password)) {
      SendError(res, 400, "nickname and password (string) are required");
      return;
    }
    const std::string nickname_trimmed = Trim(nickname);
    if (!IsValidNickname(nickname_trimmed)) {
      SendError(res, 400, "nickname must be 1~30 characters");
      return;
    }
    // 昵称唯一性优先检查（AUTH-02），重复一律 409。
    {
      db::UserInfo existing;
      if (db::GetUserByNickname(db, nickname_trimmed, &existing)) {
        SendError(res, 409, "nickname already taken");
        return;
      }
    }
    if (!IsValidPassword(password)) {
      SendError(res, 400, "password must be 6~128 characters");
      return;
    }

    db::UserInfo user;
    const db::CreateUserResult rc =
        db::CreateUser(db, nickname_trimmed, password, &user);
    if (rc == db::CreateUserResult::kNicknameTaken) {
      SendError(res, 409, "nickname already taken");
      return;
    }
    if (rc != db::CreateUserResult::kOk) {
      SendError(res, 500, "failed to create user");
      return;
    }

    auth::JwtPayload payload;
    payload.user_id = user.id;
    payload.account = user.account;
    payload.nickname = user.nickname;
    payload.role = user.role;
    payload.reset_pwd_flag = false;
    const std::string token = auth::EncodeToken(
        jwt_secret, payload, kTokenTtl);
    if (token.empty()) {
      SendError(res, 500, "failed to issue token");
      return;
    }

    nlohmann::json out = SessionJson(FromUserInfo(user));
    out["token"] = token;
    SendJson(res, 201, out);
  });

  // 公开：登录（带限速）。
  svr.Post("/api/login", [&db, &jwt_secret, &limiter](
                             const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!ParseBodyJson(req, &body, res)) {
      return;
    }
    std::string account;
    std::string password;
    if (!BodyString(body, "account", &account) ||
        !BodyString(body, "password", &password)) {
      SendError(res, 400, "account and password (string) are required");
      return;
    }
    account = Trim(account);
    if (account.empty()) {
      SendError(res, 400, "account is required");
      return;
    }

    const std::string key = account + "|" + req.remote_addr;
    if (limiter.IsLocked(key)) {
      SendError(res, 429, "too many failed attempts; please wait and retry later");
      return;
    }

    db::UserInfo user;
    const db::LoginResult lr = db::VerifyLogin(db, account, password, &user);
    if (lr != db::LoginResult::kOk) {
      limiter.AddFailure(key);
      SendError(res, 401, "invalid account or password");
      return;
    }
    limiter.Reset(key);

    auth::JwtPayload payload;
    payload.user_id = user.id;
    payload.account = user.account;
    payload.nickname = user.nickname;
    payload.role = user.role;
    payload.reset_pwd_flag = user.reset_pwd_flag;
    const std::string token = auth::EncodeToken(
        jwt_secret, payload, kTokenTtl);
    if (token.empty()) {
      SendError(res, 500, "failed to issue token");
      return;
    }

    nlohmann::json out = SessionJson(FromUserInfo(user));
    out["token"] = token;
    SendJson(res, 200, out);
  });

  // 登录：个人信息。
  svr.Get("/api/me", [&db, &jwt_secret](const httplib::Request& req,
                                        httplib::Response& res) {
    Session s;
    if (!RequireAuth(req, res, db, jwt_secret, &s)) {
      return;
    }
    SendJson(res, 200, SessionJson(s));
  });

  // 登录：修改密码（含 admin 首次登录强制改密）。
  svr.Post("/api/me/password",
           [&db, &jwt_secret](const httplib::Request& req, httplib::Response& res) {
    Session s;
    if (!RequireAuth(req, res, db, jwt_secret, &s)) {
      return;
    }
    nlohmann::json body;
    if (!ParseBodyJson(req, &body, res)) {
      return;
    }
    std::string old_password;
    std::string new_password;
    if (!BodyString(body, "old_password", &old_password) ||
        !BodyString(body, "new_password", &new_password)) {
      SendError(res, 400, "old_password and new_password (string) are required");
      return;
    }
    if (!IsValidPassword(new_password)) {
      SendError(res, 400, "new password must be 6~128 characters");
      return;
    }
    if (new_password == old_password) {
      SendError(res, 400, "new password must differ from the old one");
      return;
    }

    const int rc = db::ChangePassword(db, s.user_id, old_password, new_password);
    if (rc == 1) {
      SendError(res, 400, "old password is incorrect");
      return;
    }
    if (rc == 2) {
      SendError(res, 404, "user not found");
      return;
    }
    if (rc != 0) {
      SendError(res, 500, "failed to change password");
      return;
    }
    SendJson(res, 200, nlohmann::json{{"ok", true}});
  });
}

}  // namespace http
}  // namespace oj