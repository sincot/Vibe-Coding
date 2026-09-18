#include "auth/jwt.hpp"

#include <sys/random.h>

#include <cerrno>
#include <chrono>
#include <string>
#include <system_error>

#include "jwt-cpp/traits/nlohmann-json/defaults.h"
#include "jwt-cpp/jwt.h"

namespace oj {
namespace auth {

namespace {

// jwt::create() 使用 default_clock，其 now() 返回 std::chrono::system_clock::time_point。
using Clock = std::chrono::system_clock;

}  // namespace

std::string GenerateJwtSecret() {
  unsigned char buf[32];
  size_t done = 0;
  while (done < sizeof(buf)) {
    const ssize_t n = ::getrandom(buf + done, sizeof(buf) - done, 0);
    if (n > 0) {
      done += static_cast<size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      return {};
    }
  }
  return std::string(reinterpret_cast<const char*>(buf), sizeof(buf));
}

std::string EncodeToken(const std::string& secret, const JwtPayload& payload,
                        const std::chrono::seconds ttl) {
  try {
    const Clock::time_point now = Clock::now();
    return jwt::create()
        .set_type("JWT")
        .set_algorithm("HS256")
        .set_issuer("oj-system")
        .set_subject(std::to_string(payload.user_id))
        .set_payload_claim("account", jwt::claim(payload.account))
        .set_payload_claim("nickname", jwt::claim(payload.nickname))
        .set_payload_claim("role", jwt::claim(payload.role))
        .set_payload_claim("reset_pwd_flag", jwt::claim(payload.reset_pwd_flag))
        .set_issued_at(now)
        .set_expires_at(now + ttl)
        .sign(jwt::algorithm::hs256{secret});
  } catch (const std::exception&) {
    return {};
  }
}

bool DecodeToken(const std::string& secret, const std::string& token,
                 JwtPayload* payload) {
  if (payload == nullptr || token.empty()) {
    return false;
  }
  try {
    auto decoded = jwt::decode(token);
    auto verifier = jwt::verify()
                        .allow_algorithm(jwt::algorithm::hs256{secret})
                        .with_issuer("oj-system");
    verifier.verify(decoded);

    payload->user_id =
        std::stoll(decoded.get_subject());  // claims 校验不通过会抛异常
    payload->account =
        decoded.get_payload_claim("account").as_string();
    payload->nickname =
        decoded.get_payload_claim("nickname").as_string();
    payload->role = decoded.get_payload_claim("role").as_string();
    const jwt::claim flag = decoded.get_payload_claim("reset_pwd_flag");
    payload->reset_pwd_flag = flag.get_type() == jwt::json::type::boolean &&
                              flag.as_boolean();
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace auth
}  // namespace oj