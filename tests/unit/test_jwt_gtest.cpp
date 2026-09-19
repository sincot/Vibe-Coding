// M1.2 登录与身份验证 —— JWT 签发与验证单元测试（GoogleTest）。
//
// 覆盖对象：oj::auth::JwtService（HS256 固定算法）
//   - 签发/验证往返、身份标识（sub）一致性、有效期（expires_seconds）透出
//   - 非法用户 ID 拒绝签发
//   - 错误密钥、过期 token、无签名（alg=none）、算法不匹配（HS384）
//   - 缺少必要 claims（sub / exp / iat / iss / aud）
//   - 身份字段非法（数字 sub、sub=abc、sub=0、sub=负数）
//   - 损坏 token、篡改 payload、伪造签名
//   - 错误 issuer / 错误 audience（签发与验证规则一致）
//   - 相同密钥下重建服务（模拟重启）后原 token 仍可验证
//
// 运行方式：ctest --test-dir build -R jwt_gtest --output-on-failure
// 或直接执行 build/oj_jwt_gtest（支持 gtest 全部过滤参数）。

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "auth/jwt.h"

namespace {

const std::string kSecret = "test-secret-0123456789abcdef";

oj::auth::JwtService MakeService(int ttl = 3600) {
  return oj::auth::JwtService(kSecret, ttl);
}

// 自定义 token 构造参数，用于模拟各类合法/非法 token。
struct TokenSpec {
  std::string alg = "HS256"; // HS256 / none / HS384
  bool has_iss = true;
  std::string iss = "oj";
  bool has_aud = true;
  std::string aud = "oj-api";
  bool has_sub = true;
  bool numeric_sub = false; // sub 使用数字类型（非字符串）
  std::string sub = "42";   // 字符串 sub（has_sub 且 !numeric_sub 时生效）
  bool has_iat = true;
  bool has_exp = true;
  std::int64_t exp_offset = 3600;
  std::string secret = kSecret;
};

std::string MakeToken(const TokenSpec &spec) {
  const auto now = std::chrono::system_clock::now();
  auto b = jwt::create();
  if (spec.has_iss) {
    b.set_issuer(spec.iss);
  }
  if (spec.has_aud) {
    b.set_audience(spec.aud);
  }
  if (spec.has_iat) {
    b.set_issued_at(now);
  }
  if (spec.has_exp) {
    b.set_expires_at(now + std::chrono::seconds(spec.exp_offset));
  }
  if (spec.has_sub) {
    if (spec.numeric_sub) {
      b.set_payload_claim("sub", jwt::claim(static_cast<std::int64_t>(42)));
    } else {
      b.set_payload_claim("sub", jwt::claim(spec.sub));
    }
  }
  if (spec.alg == "none") {
    return b.sign(jwt::algorithm::none{});
  }
  if (spec.alg == "HS384") {
    return b.sign(jwt::algorithm::hs384{spec.secret});
  }
  return b.sign(jwt::algorithm::hs256{spec.secret});
}

// 基于默认 spec 构造 token，可通过 mutate 修改若干字段，便于聚焦单个变化。
std::string Token(const std::function<void(TokenSpec &)> &mutate = {}) {
  TokenSpec spec;
  if (mutate) {
    mutate(spec);
  }
  return MakeToken(spec);
}

bool Verify(const oj::auth::JwtService &svc, const std::string &token,
            std::int64_t &uid) {
  std::string err;
  return svc.verify(token, uid, err);
}

} // namespace

// ---------------------------------------------------------------------------
// 签发与验证往返
// ---------------------------------------------------------------------------

TEST(JwtServiceTest, SignAndVerifyRoundTrip) {
  auto svc = MakeService();
  std::string token, err;
  ASSERT_TRUE(svc.sign(12345, token, err)) << err;

  std::int64_t uid = 0;
  ASSERT_TRUE(svc.verify(token, uid, err)) << err;
  EXPECT_EQ(uid, 12345);
}

TEST(JwtServiceTest, TokenHasThreeDotSeparatedSegments) {
  auto svc = MakeService();
  std::string token, err;
  ASSERT_TRUE(svc.sign(1, token, err));
  std::size_t first = token.find('.');
  std::size_t second = token.find('.', first + 1);
  EXPECT_NE(first, std::string::npos);
  EXPECT_NE(second, std::string::npos);
  EXPECT_EQ(token.find('.', second + 1), std::string::npos);
}

TEST(JwtServiceTest, ExpiresSecondsReflectsConfig) {
  auto svc = MakeService(7200);
  EXPECT_EQ(svc.expires_seconds(), 7200);
}

TEST(JwtServiceTest, SignRejectsNonPositiveUserId) {
  auto svc = MakeService();
  std::string token, err;
  EXPECT_FALSE(svc.sign(0, token, err));
  EXPECT_FALSE(svc.sign(-1, token, err));
  EXPECT_TRUE(token.empty());
}

// ---------------------------------------------------------------------------
// 密钥 / 算法 / 签名
// ---------------------------------------------------------------------------

TEST(JwtServiceTest, WrongSecretRejected) {
  auto svc = MakeService();
  std::string token, err;
  ASSERT_TRUE(svc.sign(1, token, err));

  oj::auth::JwtService other("another-secret-999999999999999", 3600);
  std::int64_t uid = 0;
  EXPECT_FALSE(other.verify(token, uid, err));
}

TEST(JwtServiceTest, NoneAlgorithmRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.alg = "none"; }), uid));
}

TEST(JwtServiceTest, AlgorithmMismatchRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.alg = "HS384"; }), uid));
}

TEST(JwtServiceTest, TamperedPayloadRejected) {
  auto svc = MakeService();
  std::string token, err;
  ASSERT_TRUE(svc.sign(42, token, err));

  std::size_t first = token.find('.');
  std::string tampered = token;
  char &c = tampered[first + 1];
  c = (c == 'A') ? 'B' : 'A';

  std::int64_t uid = 0;
  EXPECT_FALSE(svc.verify(tampered, uid, err));
}

TEST(JwtServiceTest, ForgedSignatureRejected) {
  auto svc = MakeService();
  std::string token, err;
  ASSERT_TRUE(svc.sign(42, token, err));

  std::string forged = token;
  char &c = forged.back();
  c = (c == 'A') ? 'B' : 'A';

  std::int64_t uid = 0;
  EXPECT_FALSE(svc.verify(forged, uid, err));
}

// ---------------------------------------------------------------------------
// 时间相关 claims
// ---------------------------------------------------------------------------

TEST(JwtServiceTest, ExpiredTokenRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.exp_offset = -10; }), uid));
}

TEST(JwtServiceTest, MissingExpRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.has_exp = false; }), uid));
}

TEST(JwtServiceTest, MissingIatRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.has_iat = false; }), uid));
}

// ---------------------------------------------------------------------------
// issuer / audience 一致性
// ---------------------------------------------------------------------------

TEST(JwtServiceTest, WrongIssuerRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.iss = "evil"; }), uid));
}

TEST(JwtServiceTest, MissingIssuerRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.has_iss = false; }), uid));
}

TEST(JwtServiceTest, WrongAudienceRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.aud = "other"; }), uid));
}

TEST(JwtServiceTest, MissingAudienceRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.has_aud = false; }), uid));
}

// ---------------------------------------------------------------------------
// 身份字段（sub）
// ---------------------------------------------------------------------------

TEST(JwtServiceTest, MissingSubRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.has_sub = false; }), uid));
}

TEST(JwtServiceTest, NumericSubRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.numeric_sub = true; }), uid));
}

TEST(JwtServiceTest, NonNumericSubRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.sub = "abc"; }), uid));
}

TEST(JwtServiceTest, ZeroSubRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.sub = "0"; }), uid));
}

TEST(JwtServiceTest, NegativeSubRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  EXPECT_FALSE(Verify(svc, Token([](TokenSpec &s) { s.sub = "-1"; }), uid));
}

// ---------------------------------------------------------------------------
// 损坏 / 畸形 token
// ---------------------------------------------------------------------------

TEST(JwtServiceTest, CorruptedTokensRejected) {
  auto svc = MakeService();
  std::int64_t uid = 0;
  for (const char *bad : {"not-a-jwt", "a.b", "a.b.c", "", "....", "a.b.c.d"}) {
    EXPECT_FALSE(Verify(svc, bad, uid)) << "应拒绝: \"" << bad << "\"";
  }
}

// ---------------------------------------------------------------------------
// 重启一致性
// ---------------------------------------------------------------------------

TEST(JwtServiceTest, SameKeyAcrossRestartStillValid) {
  std::string token, err;
  {
    auto svc = MakeService();
    ASSERT_TRUE(svc.sign(7, token, err));
  }
  auto svc2 = MakeService();
  std::int64_t uid = 0;
  ASSERT_TRUE(svc2.verify(token, uid, err));
  EXPECT_EQ(uid, 7);
}
