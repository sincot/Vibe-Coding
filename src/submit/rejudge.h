#pragma once

#include <cstdint>
#include <string>

#include "db/problems.h"
#include "db/submissions.h"
#include "judge/executor.h"
#include "judge/judge.h"
#include "judge/manager.h"

namespace oj {

class Database;

namespace submit {

// 重判服务：使用原提交保存的源码、语言与当前题目配置（含完整测试用例）重新判题，
// 在原提交记录上更新结果，并联动重算受影响用户的做题状态。
//
// 设计要点：
//   - 不新增提交记录，不增加 submit_count；
//   - 结果更新与状态重算在同一个短事务内完成；
//   - 状态重算完全依据数据库中该用户该题的最新提交记录，不沿用“已有 AC 不清除”的
//     累积逻辑；
//   - 由调用方（HTTP 层 / JudgeManager 调度）保证并发去重。
class RejudgeService {
public:
  RejudgeService(Database &db, judge::IExecutor &executor,
                 judge::JudgeOptions options = {});

  // 复用 SubmitService 的 Outcome/Kind 作为结果类型，便于 JudgeManager 统一处理。
  using Kind = SubmitService::Kind;
  using Outcome = SubmitService::Outcome;

  // 对指定提交 ID 执行重判。task 中须携带 rejudge_submission_id 以及当前操作者信息，
  // 具体源码/语言/题目从数据库读取，不接受客户端替换。
  Outcome rejudge(const judge::SubmissionTask &task);

private:
  Database &db_;
  ProblemStore problems_;
  SubmissionStore submissions_;
  UserProblemStatusStore statuses_;
  judge::IExecutor &executor_;
  judge::JudgeOptions options_;
};

} // namespace submit
} // namespace oj
