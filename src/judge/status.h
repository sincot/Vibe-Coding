#pragma once

namespace oj {
namespace judge {

// 判题状态枚举，与 SPEC.md JUDGE-08 定义一致。
//   AC     全部测试点通过
//   WA     正常结束但输出不匹配
//   CE     编译错误
//   TLE    超出时间限制
//   RE     运行错误（崩溃 / 非零退出 / 输出超限）
//   MLE    超出内存限制（M3 完整实现，M1.5 仅保留枚举）
//   SYSERR 判题环境内部错误（编译器缺失、无法创建运行目录等）
enum class JudgeStatus {
  AC,
  WA,
  CE,
  TLE,
  RE,
  MLE,
  SYSERR,
};

// 状态的可读名称（与 SPEC 中的字符串一致，供日志/持久化使用）。
inline const char *judge_status_name(JudgeStatus status) {
  switch (status) {
  case JudgeStatus::AC:
    return "AC";
  case JudgeStatus::WA:
    return "WA";
  case JudgeStatus::CE:
    return "CE";
  case JudgeStatus::TLE:
    return "TLE";
  case JudgeStatus::RE:
    return "RE";
  case JudgeStatus::MLE:
    return "MLE";
  case JudgeStatus::SYSERR:
    return "SYSERR";
  }
  return "SYSERR";
}

// 汇总时使用的严重度排序：SYSERR > TLE > MLE > RE > WA > AC。
// 仅用于混合测试点结果的汇总，不影响单点状态。
inline int judge_status_severity(JudgeStatus status) {
  switch (status) {
  case JudgeStatus::AC:
    return 0;
  case JudgeStatus::WA:
    return 1;
  case JudgeStatus::RE:
    return 2;
  case JudgeStatus::MLE:
    return 3;
  case JudgeStatus::TLE:
    return 4;
  case JudgeStatus::SYSERR:
    return 5;
  case JudgeStatus::CE:
    // 单点不会出现 CE（编译整体失败在判题汇总层处理），置为最高以便防御性处理。
    return 6;
  }
  return 5;
}

} // namespace judge
} // namespace oj
