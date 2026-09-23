#include "submit/rejudge.h"

#include <exception>
#include <mutex>

#include <nlohmann/json.hpp>

#include "db/database.h"
#include "log.h"
#include "submit/submit.h"

namespace oj {
namespace submit {

RejudgeService::RejudgeService(Database &db, judge::IExecutor &executor,
                               judge::JudgeOptions options)
    : db_(db), problems_(db), submissions_(db), statuses_(db),
      executor_(executor), options_(std::move(options)) {}

RejudgeService::Outcome RejudgeService::rejudge(
    const judge::SubmissionTask &task) {
  Outcome outcome;
  const std::int64_t submission_id = task.rejudge_submission_id;

  // 1. 读取原提交记录：源码、语言、用户归属均来自数据库，客户端不可替换。
  bool found = false;
  SubmissionRecord original;
  std::string err;
  if (!submissions_.find_by_id(submission_id, found, original, err)) {
    log(LogLevel::Error,
        "重判：读取提交记录失败 #" + std::to_string(submission_id) + ": " + err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }
  if (!found) {
    outcome.kind = Kind::ProblemNotFound; // 借用“记录不存在”语义，HTTP 层映射为 404
    outcome.error = "提交记录不存在";
    return outcome;
  }

  // 2. 读取题目配置与当前完整测试用例，构造内存快照。
  bool problem_found = false;
  ProblemRecord problem;
  if (!problems_.find_by_id(original.problem_id, problem_found, problem, err)) {
    log(LogLevel::Error, "重判：题目查询失败 #" +
                              std::to_string(original.problem_id) + ": " + err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }
  if (!problem_found) {
    outcome.kind = Kind::ProblemNotFound;
    outcome.error = "题目不存在";
    return outcome;
  }

  std::vector<TestcaseRecord> records;
  if (!problems_.list_testcases(original.problem_id, records, err)) {
    log(LogLevel::Error, "重判：测试用例读取失败 #" +
                              std::to_string(original.problem_id) + ": " + err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  judge::JudgeTask judge_task;
  judge_task.language = original.language;
  judge_task.source_code = original.source_code;
  judge_task.time_limit_ms = problem.time_limit_ms;
  judge_task.memory_limit_kb = problem.memory_limit_kb;
  judge_task.testcases.reserve(records.size());
  for (const TestcaseRecord &record : records) {
    judge::Testcase testcase;
    testcase.input = record.input;
    testcase.output = record.output;
    judge_task.testcases.push_back(std::move(testcase));
  }

  // 3. 同步判题；判题期间不持有数据库事务。
  judge::JudgeResult judge_result;
  try {
    judge::JudgeEngine engine(executor_, options_);
    judge_result = engine.judge(judge_task, task.cancel.get());
  } catch (const std::exception &e) {
    judge_result = judge::JudgeResult{};
    judge_result.status = judge::JudgeStatus::SYSERR;
    judge_result.total = static_cast<int>(judge_task.testcases.size());
    judge_result.message = std::string("判题内部错误: ") + e.what();
  } catch (...) {
    judge_result = judge::JudgeResult{};
    judge_result.status = judge::JudgeStatus::SYSERR;
    judge_result.total = static_cast<int>(judge_task.testcases.size());
    judge_result.message = "判题内部错误";
  }

  // 策略：当最终状态为 SYSERR 时，视为“无法完成本次重判”而非“产生了新的有效结果”。
  // 这样做可避免环境/取消/全局硬上限等内部原因把原本有效的 AC 覆盖为 SYSERR。
  // 该策略在本阶段明确实施，待后续需求确认后可再调整。
  if (judge_result.status == judge::JudgeStatus::SYSERR) {
    log(LogLevel::Error,
        "重判：提交 #" + std::to_string(submission_id) +
            " 判题产生 SYSERR，按策略保留原结果与统计：" + judge_result.message);
    outcome.kind = Kind::InternalError;
    outcome.error = "重判未能完成：" + judge_result.message;
    outcome.judge = std::move(judge_result);
    return outcome;
  }

  long long total_runtime_ms = 0;
  long long peak_memory_kb = 0;
  for (const judge::TestcaseResult &item : judge_result.cases) {
    total_runtime_ms += item.time_ms;
    if (item.memory_kb > peak_memory_kb) {
      peak_memory_kb = item.memory_kb;
    }
  }

  SubmissionRecord updated = original;
  updated.status = judge::judge_status_name(judge_result.status);
  updated.per_case = build_per_case_json(judge_task, judge_result).dump();
  updated.compile_msg = judge_result.compile_output;
  updated.runtime_ms = total_runtime_ms;
  updated.memory_kb = peak_memory_kb;
  // created_at 保持原值不变。

  // 4. 单事务持久化：更新原提交 + 重算用户题目状态。
  std::lock_guard<std::mutex> transaction_lock(db_.transaction_mutex());
  if (!db_.begin(err)) {
    log(LogLevel::Error, "重判：开启事务失败 #" +
                              std::to_string(submission_id) + ": " + err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  // 事务内复核提交记录仍存在（避免判题期间被删除导致向幽灵记录写入）。
  bool still_found = false;
  SubmissionRecord rechecked;
  if (!submissions_.find_by_id(submission_id, still_found, rechecked, err)) {
    log(LogLevel::Error, "重判：复核提交失败 #" + std::to_string(submission_id) +
                              ": " + err);
    db_.rollback(err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }
  if (!still_found) {
    db_.rollback(err);
    outcome.kind = Kind::ProblemNotFound;
    outcome.error = "提交记录不存在";
    return outcome;
  }

  if (!submissions_.update(updated, err)) {
    log(LogLevel::Error, "重判：更新提交记录失败 #" +
                              std::to_string(submission_id) + ": " + err);
    db_.rollback(err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  if (!statuses_.recompute(original.user_id, original.problem_id, err)) {
    log(LogLevel::Error, "重判：重算状态失败（用户 " +
                              std::to_string(original.user_id) + "，题目 " +
                              std::to_string(original.problem_id) + "）：" + err);
    db_.rollback(err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  if (!db_.commit(err)) {
    log(LogLevel::Error, "重判：提交事务失败 #" + std::to_string(submission_id) +
                              ": " + err);
    db_.rollback(err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  outcome.kind = Kind::Ok;
  outcome.submission = std::move(updated);
  outcome.judge = std::move(judge_result);
  return outcome;
}

} // namespace submit
} // namespace oj
