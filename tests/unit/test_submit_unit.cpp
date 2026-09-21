// 提交参数校验与做题状态计算单元测试（M1.6）。
//
// 只测试可独立验证的纯逻辑，不触碰数据库、HTTP 与子进程：
//   - language 取值解析（仅 cpp17 / c11，大小写不敏感）
//   - 源码校验（空、纯空白、超长、边界）
//   - user_problem_status 状态计算（首次 AC、重复 AC、成功后再失败等）
//
// 运行方式：ctest --test-dir build -R submit_unit --output-on-failure
// 或直接执行 build/oj_submit_unit。

#include <gtest/gtest.h>

#include <string>

#include "submit/submit.h"

namespace {

using oj::submit::compute_status_update;
using oj::submit::kMaxSourceBytes;
using oj::submit::parse_submission_language;
using oj::submit::StatusState;
using oj::submit::StatusUpdate;
using oj::submit::validate_source_code;

// ---------------------------------------------------------------------------
// parse_submission_language
// ---------------------------------------------------------------------------

TEST(SubmitLanguage, AcceptsCanonicalValues) {
  std::string canonical;
  ASSERT_TRUE(parse_submission_language("cpp17", canonical));
  EXPECT_EQ(canonical, "cpp17");
  ASSERT_TRUE(parse_submission_language("c11", canonical));
  EXPECT_EQ(canonical, "c11");
}

TEST(SubmitLanguage, CaseInsensitive) {
  std::string canonical;
  ASSERT_TRUE(parse_submission_language("CPP17", canonical));
  EXPECT_EQ(canonical, "cpp17");
  ASSERT_TRUE(parse_submission_language("C11", canonical));
  EXPECT_EQ(canonical, "c11");
}

TEST(SubmitLanguage, RejectsUnsupportedValues) {
  std::string canonical = "sentinel";
  for (const char *value : {"", "c", "c++", "cpp", "c++17", "python", "java",
                            "cpp20", "cpp17 ", " c11"}) {
    EXPECT_FALSE(parse_submission_language(value, canonical))
        << "不应接受: '" << value << "'";
  }
  // 失败时不改写 canonical。
  EXPECT_EQ(canonical, "sentinel");
}

// ---------------------------------------------------------------------------
// validate_source_code
// ---------------------------------------------------------------------------

TEST(SourceValidation, AcceptsNormalSource) {
  std::string error;
  EXPECT_TRUE(validate_source_code("int main(){return 0;}\n", error));
  EXPECT_TRUE(error.empty());
}

TEST(SourceValidation, RejectsEmptyAndWhitespaceOnly) {
  std::string error;
  EXPECT_FALSE(validate_source_code("", error));
  EXPECT_FALSE(error.empty());

  error.clear();
  EXPECT_FALSE(validate_source_code("   \t\r\n  ", error));
  EXPECT_FALSE(error.empty());
}

TEST(SourceValidation, BoundaryAtMaxBytes) {
  std::string error;
  std::string exact(kMaxSourceBytes, 'a');
  EXPECT_TRUE(validate_source_code(exact, error));

  error.clear();
  std::string too_long(kMaxSourceBytes + 1, 'a');
  EXPECT_FALSE(validate_source_code(too_long, error));
  EXPECT_FALSE(error.empty());
}

TEST(SourceValidation, DoesNotTrimContent) {
  // 只做校验，不修改内容：前后空白属于源码的一部分，非纯空白时仍接受。
  std::string error;
  const std::string source = "  int x = 1;  ";
  EXPECT_TRUE(validate_source_code(source, error));
  EXPECT_EQ(source, "  int x = 1;  ");
}

// ---------------------------------------------------------------------------
// compute_status_update
// ---------------------------------------------------------------------------

const char *kTime = "2026-01-02 03:04:05";
const char *kLaterTime = "2026-01-02 03:05:05";

TEST(StatusUpdate, FirstFailureCreatesNonAcceptedRecord) {
  StatusState state; // 无记录
  StatusUpdate update = compute_status_update(state, /*AC=*/false, kTime);
  EXPECT_FALSE(update.accepted);
  EXPECT_FALSE(update.has_first_ac_at);
  EXPECT_TRUE(update.first_ac_at.empty());
  EXPECT_EQ(update.submit_count, 1);
}

TEST(StatusUpdate, FirstAcSetsAcceptedAndTime) {
  StatusState state;
  StatusUpdate update = compute_status_update(state, /*AC=*/true, kTime);
  EXPECT_TRUE(update.accepted);
  EXPECT_TRUE(update.has_first_ac_at);
  EXPECT_EQ(update.first_ac_at, kTime);
  EXPECT_EQ(update.submit_count, 1);
}

TEST(StatusUpdate, FailureThenAcPreservesCountIncrement) {
  StatusState state;
  state.has_record = true;
  state.accepted = false;
  state.submit_count = 2; // 两次失败
  StatusUpdate update = compute_status_update(state, /*AC=*/true, kTime);
  EXPECT_TRUE(update.accepted);
  EXPECT_TRUE(update.has_first_ac_at);
  EXPECT_EQ(update.first_ac_at, kTime);
  EXPECT_EQ(update.submit_count, 3);
}

TEST(StatusUpdate, RepeatedAcKeepsFirstAcTime) {
  StatusState state;
  state.has_record = true;
  state.accepted = true;
  state.first_ac_at = kTime;
  state.submit_count = 1;
  StatusUpdate update =
      compute_status_update(state, /*AC=*/true, kLaterTime);
  EXPECT_TRUE(update.accepted);
  EXPECT_EQ(update.first_ac_at, kTime); // 不覆盖
  EXPECT_EQ(update.submit_count, 2);
}

TEST(StatusUpdate, FailureAfterAcKeepsAcceptedStatus) {
  StatusState state;
  state.has_record = true;
  state.accepted = true;
  state.first_ac_at = kTime;
  state.submit_count = 1;
  StatusUpdate update =
      compute_status_update(state, /*AC=*/false, kLaterTime);
  EXPECT_TRUE(update.accepted); // AC 状态不被失败清除
  EXPECT_EQ(update.first_ac_at, kTime);
  EXPECT_EQ(update.submit_count, 2);
}

TEST(StatusUpdate, RepeatedFailureStaysNone) {
  StatusState state;
  state.has_record = true;
  state.accepted = false;
  state.submit_count = 4;
  StatusUpdate update = compute_status_update(state, /*AC=*/false, kTime);
  EXPECT_FALSE(update.accepted);
  EXPECT_FALSE(update.has_first_ac_at);
  EXPECT_EQ(update.submit_count, 5);
}

TEST(StatusUpdate, AcceptedStateWithoutTimeBackfillsOnAc) {
  // 防御性边界：状态为 accepted 但首次 AC 时间为空时，本次 AC 补写时间。
  StatusState state;
  state.has_record = true;
  state.accepted = true;
  state.first_ac_at.clear();
  state.submit_count = 1;
  StatusUpdate update = compute_status_update(state, /*AC=*/true, kTime);
  EXPECT_TRUE(update.accepted);
  EXPECT_TRUE(update.has_first_ac_at);
  EXPECT_EQ(update.first_ac_at, kTime);
  EXPECT_EQ(update.submit_count, 2);
}

TEST(StatusUpdate, AcceptedStateWithoutTimeStaysAcceptedOnFailure) {
  // 防御性边界：accepted 但首次 AC 时间为空，失败提交保持 accepted 且不编造时间。
  StatusState state;
  state.has_record = true;
  state.accepted = true;
  state.first_ac_at.clear();
  state.submit_count = 3;
  StatusUpdate update = compute_status_update(state, /*AC=*/false, kLaterTime);
  EXPECT_TRUE(update.accepted);
  EXPECT_FALSE(update.has_first_ac_at);
  EXPECT_TRUE(update.first_ac_at.empty());
  EXPECT_EQ(update.submit_count, 4);
}

} // namespace
