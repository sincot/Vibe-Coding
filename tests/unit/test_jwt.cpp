// JWT 签发与验证单元测试（M1.2）。
//
// 覆盖：正常签发/验证往返、错误密钥、过期 token、伪造签名、篡改内容、无签名
// （alg=none）、算法不匹配（HS384）、缺少必要 claims（sub/exp）、身份字段非法、
// 损坏 token，以及 JWT 配置读取与校验（缺失/过短密钥、非法有效期）。
//
// 运行方式：ctest --test-dir build -R jwt_unit --output-on-failure
// 或直接执行 build/oj_jwt_test。

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "auth/jwt.h"

namespace {

int g_failures = 0;

void check(bool cond, const std::string &msg) {
  if (cond) {
    std::cout << "  [PASS] " << msg << "\n";
  } else {
    std::cout << "  [FAIL] " << msg << "\n";
    ++g_failures;
  }
}

const std::string kSecret = "test-secret-0123456789abcdef";

oj::auth::JwtService make_service(int ttl = 3600) {
  return oj::auth::JwtService(kSecret, ttl);
}

// 构造一个自定义 token：允许指定算法、claims 内容，便于模拟非法/边缘 token。
// alg = 0 -> none；1 -> HS256（正确）；2 -> HS384。
std::string build_token(const std::string &alg_name, bool with_sub,
                        bool with_exp, bool with_iat,
                        std::int64_t exp_offset_seconds, bool sub_numeric) {
  auto now = std::chrono::system_clock::now();
  auto b = jwt::create().set_issuer("oj").set_audience("oj-api");
  if (with_iat) {
    b.set_issued_at(now);
  }
  if (with_exp) {
    b.set_expires_at(now + std::chrono::seconds(exp_offset_seconds));
  }
  if (with_sub) {
    if (sub_numeric) {
      b.set_payload_claim("sub", jwt::claim(static_cast<std::int64_t>(42)));
    } else {
      b.set_payload_claim("sub", jwt::claim(std::string("42")));
    }
  }
  if (alg_name == "none") {
    return b.sign(jwt::algorithm::none{});
  }
  if (alg_name == "HS384") {
    return b.sign(jwt::algorithm::hs384{kSecret});
  }
  return b.sign(jwt::algorithm::hs256{kSecret});
}

void test_sign_verify_roundtrip() {
  std::cout << "签发/验证往返\n";
  auto svc = make_service();
  std::string token, err;
  check(svc.sign(12345, token, err), "签发成功");
  check(!token.empty() && token.find('.') != std::string::npos,
        "token 为三段式 JWT");

  std::int64_t uid = 0;
  check(svc.verify(token, uid, err), "验证成功");
  check(uid == 12345, "验证返回的用户 ID 与签发一致");
}

void test_wrong_secret_rejected() {
  std::cout << "错误密钥拒绝\n";
  auto svc = make_service();
  std::string token, err;
  svc.sign(1, token, err);

  oj::auth::JwtService other("another-secret-999999999999999", 3600);
  std::int64_t uid = 0;
  check(!other.verify(token, uid, err), "错误密钥验证失败");
}

void test_expired_token_rejected() {
  std::cout << "过期 token 拒绝\n";
  auto svc = make_service();
  std::string token = build_token("HS256", true, true, true, -10, false);
  std::int64_t uid = 0;
  std::string err;
  check(!svc.verify(token, uid, err), "过期 token 被拒绝");
}

void test_none_algorithm_rejected() {
  std::cout << "无签名（alg=none）拒绝\n";
  auto svc = make_service();
  std::string token = build_token("none", true, true, true, 3600, false);
  std::int64_t uid = 0;
  std::string err;
  check(!svc.verify(token, uid, err), "alg=none 被拒绝");
}

void test_algorithm_mismatch_rejected() {
  std::cout << "算法不匹配（HS384）拒绝\n";
  auto svc = make_service();
  std::string token = build_token("HS384", true, true, true, 3600, false);
  std::int64_t uid = 0;
  std::string err;
  check(!svc.verify(token, uid, err), "HS384 被拒绝");
}

void test_missing_claims_rejected() {
  std::cout << "缺少必要 claims 拒绝\n";
  auto svc = make_service();
  std::int64_t uid = 0;
  std::string err;

  check(!svc.verify(build_token("HS256", false, true, true, 3600, false), uid,
                    err),
        "缺少 sub 被拒绝");
  check(!svc.verify(build_token("HS256", true, false, true, 3600, false), uid,
                    err),
        "缺少 exp 被拒绝");
  check(!svc.verify(build_token("HS256", true, true, false, 3600, false), uid,
                    err),
        "缺少 iat 被拒绝");
}

void test_invalid_sub_rejected() {
  std::cout << "身份字段非法拒绝\n";
  auto svc = make_service();
  std::int64_t uid = 0;
  std::string err;

  // 数字类型的 sub（非字符串）。
  check(!svc.verify(build_token("HS256", true, true, true, 3600, true), uid,
                    err),
        "数字类型 sub 被拒绝");

  // 字符串但非合法 ID。
  auto b = jwt::create().set_issuer("oj").set_audience("oj-api")
               .set_issued_at(std::chrono::system_clock::now())
               .set_expires_at(std::chrono::system_clock::now() +
                               std::chrono::seconds(3600));
  auto token_abc = b.set_payload_claim("sub", jwt::claim(std::string("abc")))
                       .sign(jwt::algorithm::hs256{kSecret});
  check(!svc.verify(token_abc, uid, err), "sub=abc 被拒绝");

  auto token_zero = b.set_payload_claim("sub", jwt::claim(std::string("0")))
                        .sign(jwt::algorithm::hs256{kSecret});
  check(!svc.verify(token_zero, uid, err), "sub=0 被拒绝");
}

void test_corrupted_token_rejected() {
  std::cout << "损坏 token 拒绝\n";
  auto svc = make_service();
  std::int64_t uid = 0;
  std::string err;
  check(!svc.verify("not-a-jwt", uid, err), "非 JWT 字符串被拒绝");
  check(!svc.verify("a.b", uid, err), "缺少签名段被拒绝");
  check(!svc.verify("a.b.c", uid, err), "非法 base64 被拒绝");
}

void test_tampered_payload_rejected() {
  std::cout << "篡改内容拒绝\n";
  auto svc = make_service();
  std::string token, err;
  svc.sign(42, token, err);

  // 修改中间 payload 段的第一个字符（base64url 字符域），签名未变 -> 应被拒绝。
  std::size_t first = token.find('.');
  std::size_t second = token.find('.', first + 1);
  std::string tampered = token;
  // 翻转 payload 首字符，若刚好等于原字符则换一个。
  char &c = tampered[first + 1];
  c = (c == 'A') ? 'B' : 'A';

  std::int64_t uid = 0;
  check(!svc.verify(tampered, uid, err), "篡改 payload 被拒绝");
}

void test_forged_signature_rejected() {
  std::cout << "伪造签名拒绝\n";
  auto svc = make_service();
  std::string token, err;
  svc.sign(42, token, err);

  std::size_t second = token.find('.', token.find('.') + 1);
  std::string forged = token;
  // 修改签名段最后一个字符。
  char &c = forged.back();
  c = (c == 'A') ? 'B' : 'A';

  std::int64_t uid = 0;
  check(!svc.verify(forged, uid, err), "伪造签名被拒绝");
}

void test_same_key_across_restart() {
  std::cout << "相同密钥下重新构造服务仍可验证（模拟重启）\n";
  std::string token, err;
  {
    auto svc = make_service();
    svc.sign(7, token, err);
  }
  auto svc2 = make_service();
  std::int64_t uid = 0;
  check(svc2.verify(token, uid, err) && uid == 7, "重启后原 token 仍有效");
}

// 设置环境变量后调用 load_jwt_config 的辅助（返回并恢复原值）。
bool load_with_env(const char *secret, const char *expires,
                   oj::auth::JwtConfig &cfg, std::string &error) {
  const char *old_secret = std::getenv("OJ_JWT_SECRET");
  const char *old_exp = std::getenv("OJ_JWT_EXPIRES_SECONDS");
  auto restore = [&]() {
    if (old_secret) setenv("OJ_JWT_SECRET", old_secret, 1);
    else unsetenv("OJ_JWT_SECRET");
    if (old_exp) setenv("OJ_JWT_EXPIRES_SECONDS", old_exp, 1);
    else unsetenv("OJ_JWT_EXPIRES_SECONDS");
  };
  if (secret) setenv("OJ_JWT_SECRET", secret, 1);
  else unsetenv("OJ_JWT_SECRET");
  if (expires) setenv("OJ_JWT_EXPIRES_SECONDS", expires, 1);
  else unsetenv("OJ_JWT_EXPIRES_SECONDS");
  bool ok = oj::auth::load_jwt_config(cfg, error);
  restore();
  return ok;
}

void test_load_config() {
  std::cout << "JWT 配置读取与校验\n";
  oj::auth::JwtConfig cfg;
  std::string err;

  check(!load_with_env(nullptr, nullptr, cfg, err), "缺少密钥被拒绝");
  check(!load_with_env("short", nullptr, cfg, err), "过短密钥被拒绝");
  check(!load_with_env("", nullptr, cfg, err), "空密钥被拒绝");

  check(load_with_env("a-valid-secret-0123456789", nullptr, cfg, err),
        "合法密钥加载成功");
  check(cfg.expires_seconds == 3600, "默认有效期 3600");

  check(load_with_env("a-valid-secret-0123456789", "7200", cfg, err),
        "自定义有效期加载成功");
  check(cfg.expires_seconds == 7200, "有效期解析为 7200");

  check(!load_with_env("a-valid-secret-0123456789", "abc", cfg, err),
        "非法有效期被拒绝");
  check(!load_with_env("a-valid-secret-0123456789", "0", cfg, err),
        "零有效期被拒绝");
  check(!load_with_env("a-valid-secret-0123456789", "-5", cfg, err),
        "负有效期被拒绝");
}

} // namespace

int main() {
  test_sign_verify_roundtrip();
  test_wrong_secret_rejected();
  test_expired_token_rejected();
  test_none_algorithm_rejected();
  test_algorithm_mismatch_rejected();
  test_missing_claims_rejected();
  test_invalid_sub_rejected();
  test_corrupted_token_rejected();
  test_tampered_payload_rejected();
  test_forged_signature_rejected();
  test_same_key_across_restart();
  test_load_config();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部 JWT 单元测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
