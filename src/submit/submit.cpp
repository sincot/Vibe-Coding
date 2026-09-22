#include "submit/submit.h"

#include <cctype>
#include <ctime>
#include <exception>
#include <mutex>
#include <utility>

#include <nlohmann/json.hpp>

#include "db/database.h"
#include "log.h"

namespace oj {
namespace submit {

namespace {

// 构造逐测试点结果 JSON。仅当前提交者可见；通过（AC）测试点绝不附带隐藏测试输入
// 或标准答案。WA 测试点按 SPEC PRB-05 / JUDGE-07 附上该失败点的输入、期望输出与
// 用户实际输出；其它非 AC 点附上有界诊断（消息 / 实际输出 / 标准错误）。
//
// memory_kb 固定为 null：M1.6 判题器未采集内存，明确表示为未采集而非伪造 0。
nlohmann::json build_per_case(const judge::JudgeTask &task,
                              const judge::JudgeResult &result) {
  using nlohmann::json;
  json cases = json::array();
  for (const judge::TestcaseResult &item : result.cases) {
    json entry;
    entry["index"] = item.index;
    entry["status"] = judge::judge_status_name(item.status);
    entry["time_ms"] = item.time_ms;
    entry["memory_kb"] = nullptr;
    if (item.status != judge::JudgeStatus::AC) {
      entry["exit_code"] = item.exit_code;
      entry["term_signal"] = item.term_signal;
      if (!item.message.empty()) {
        entry["message"] = item.message;
      }
      if (!item.actual_output.empty()) {
        entry["actual_output"] = item.actual_output;
      }
      if (!item.stderr_output.empty()) {
        entry["stderr_output"] = item.stderr_output;
      }
      if (item.status == judge::JudgeStatus::WA && item.index >= 0 &&
          static_cast<std::size_t>(item.index) < task.testcases.size()) {
        entry["input"] = task.testcases[item.index].input;
        entry["expected_output"] = task.testcases[item.index].output;
      }
    }
    cases.push_back(std::move(entry));
  }
  return cases;
}

} // namespace

bool parse_submission_language(const std::string &text, std::string &canonical) {
  std::string lowered;
  lowered.reserve(text.size());
  for (char c : text) {
    lowered.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lowered == kLanguageCpp17) {
    canonical = kLanguageCpp17;
    return true;
  }
  if (lowered == kLanguageC11) {
    canonical = kLanguageC11;
    return true;
  }
  return false;
}

bool validate_source_code(const std::string &code, std::string &error) {
  if (code.size() > kMaxSourceBytes) {
    error = "源码过长：最大允许 " + std::to_string(kMaxSourceBytes) + " 字节";
    return false;
  }
  bool has_non_space = false;
  for (char c : code) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      has_non_space = true;
      break;
    }
  }
  if (!has_non_space) {
    error = "源码不能为空";
    return false;
  }
  return true;
}

StatusUpdate compute_status_update(const StatusState &current,
                                   bool submission_accepted,
                                   const std::string &submission_time) {
  StatusUpdate update;
  update.submit_count = current.submit_count + 1;

  if (current.accepted) {
    // 曾经 AC：保留 AC 状态；首次 AC 时间取「最早符合条件的原提交时间」。
    // 并发任务可能乱序完成，若本次 AC 的原提交时间早于已记录值，则收敛为更早者。
    update.accepted = true;
    std::string best = current.first_ac_at;
    if (submission_accepted && !submission_time.empty() &&
        (best.empty() || submission_time < best)) {
      best = submission_time;
    }
    if (!best.empty()) {
      update.has_first_ac_at = true;
      update.first_ac_at = best;
    }
    return update;
  }

  if (submission_accepted) {
    update.accepted = true;
    update.has_first_ac_at = true;
    update.first_ac_at = submission_time;
    return update;
  }

  update.accepted = false;
  update.has_first_ac_at = false;
  return update;
}

std::string utc_timestamp_now() {
  std::time_t now = std::time(nullptr);
  std::tm tm_utc{};
  gmtime_r(&now, &tm_utc);
  char buffer[32] = {0};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_utc);
  return std::string(buffer);
}

SubmitService::SubmitService(Database &db, judge::IExecutor &executor,
                             judge::JudgeOptions options)
    : db_(db), problems_(db), submissions_(db), statuses_(db),
      executor_(executor), options_(std::move(options)) {}

SubmitService::Outcome SubmitService::submit(std::int64_t user_id,
                                             std::int64_t problem_id,
                                             const std::string &language,
                                             const std::string &source_code,
                                             bool viewer_is_admin,
                                             const std::string &submitted_at) {
  Outcome outcome;

  // 1. 题目存在性与可见性（复用 M1.4 规则）。不存在与无权访问统一返回，
  //    避免通过状态码差异探测隐藏题目。
  bool found = false;
  ProblemRecord problem;
  std::string err;
  if (!problems_.find_by_id(problem_id, found, problem, err)) {
    log(LogLevel::Error, "提交：题目查询失败: " + err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }
  if (!found || (!problem.visible && !viewer_is_admin)) {
    outcome.kind = Kind::ProblemNotFound;
    return outcome;
  }

  // 2. 读取完整判题用例（含隐藏），构造本次判题的内存快照。
  //    用例、顺序与时限全部来自后端数据库，不接受客户端覆盖。
  std::vector<TestcaseRecord> records;
  if (!problems_.list_testcases(problem_id, records, err)) {
    log(LogLevel::Error, "提交：判题用例读取失败: " + err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  judge::JudgeTask task;
  task.language = language;
  task.source_code = source_code;
  task.time_limit_ms = problem.time_limit_ms;
  task.testcases.reserve(records.size());
  for (const TestcaseRecord &record : records) {
    judge::Testcase testcase;
    testcase.input = record.input;
    testcase.output = record.output;
    task.testcases.push_back(std::move(testcase));
  }

  // 3. 同步判题。判题期间不持有数据库事务；执行器与判题任务均为本次调用独立，
  //    可被多个 worker 线程并发调用（每任务一个 JudgeEngine）。
  judge::JudgeResult judge_result;
  try {
    judge::JudgeEngine engine(executor_, options_);
    judge_result = engine.judge(task);
  } catch (const std::exception &e) {
    // 判题过程抛出异常（如执行器内部故障）转换为既有约定的内部判题错误 SYSERR，
    // 不假死、不使 worker 退出，用户可重试。
    judge_result = judge::JudgeResult{};
    judge_result.status = judge::JudgeStatus::SYSERR;
    judge_result.total = static_cast<int>(task.testcases.size());
    judge_result.message = std::string("判题内部错误: ") + e.what();
  } catch (...) {
    judge_result = judge::JudgeResult{};
    judge_result.status = judge::JudgeStatus::SYSERR;
    judge_result.total = static_cast<int>(task.testcases.size());
    judge_result.message = "判题内部错误";
  }

  long long total_runtime_ms = 0;
  for (const judge::TestcaseResult &item : judge_result.cases) {
    total_runtime_ms += item.time_ms;
  }
  const bool accepted = judge_result.status == judge::JudgeStatus::AC;

  SubmissionRecord record;
  record.user_id = user_id;
  record.problem_id = problem_id;
  record.language = language;
  record.source_code = source_code;
  record.status = judge::judge_status_name(judge_result.status);
  record.per_case = build_per_case(task, judge_result).dump();
  record.compile_msg = judge_result.compile_output;
  record.runtime_ms = total_runtime_ms;
  record.memory_kb = 0; // 未采集（对外以 null 表示）
  // 采用调度器接受入队时采集的原始提交时间；排队等待不计入该时间戳。
  record.created_at = submitted_at.empty() ? utc_timestamp_now() : submitted_at;

  // 4. 单事务持久化：写入提交记录 + 更新用户题目状态。
  std::lock_guard<std::mutex> transaction_lock(db_.transaction_mutex());
  if (!db_.begin(err)) {
    log(LogLevel::Error, "提交：开启事务失败: " + err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  // 4a. 事务内复核题目仍存在：判题在事务外完成，期间题目可能被管理员删除。
  //     在同一事务内复核可避免向已删除题目写入提交，产生假成功或外键错误。
  //     若题目已不存在，按「题目不存在」处理（与判题前的可见性检查同义）。
  bool still_found = false;
  ProblemRecord rechecked;
  if (!problems_.find_by_id(problem_id, still_found, rechecked, err)) {
    log(LogLevel::Error, "提交：复核题目失败: " + err);
    db_.rollback(err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }
  if (!still_found) {
    db_.rollback(err);
    outcome.kind = Kind::ProblemNotFound;
    return outcome;
  }

  std::int64_t new_id = 0;
  if (!submissions_.insert(record, new_id, err)) {
    log(LogLevel::Error, "提交：写入提交记录失败: " + err);
    db_.rollback(err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  bool has_status = false;
  UserProblemStatusRecord current;
  if (!statuses_.find(user_id, problem_id, has_status, current, err)) {
    log(LogLevel::Error, "提交：读取做题状态失败: " + err);
    db_.rollback(err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  StatusState state;
  state.has_record = has_status;
  state.accepted = current.accepted;
  state.first_ac_at = current.first_ac_at;
  state.submit_count = current.submit_count;
  const StatusUpdate update =
      compute_status_update(state, accepted, record.created_at);

  if (!statuses_.upsert(user_id, problem_id, update.accepted,
                        update.has_first_ac_at, update.first_ac_at,
                        update.submit_count, err)) {
    log(LogLevel::Error, "提交：更新做题状态失败: " + err);
    db_.rollback(err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  if (!db_.commit(err)) {
    log(LogLevel::Error, "提交：提交事务失败: " + err);
    db_.rollback(err);
    outcome.kind = Kind::InternalError;
    outcome.error = "内部错误";
    return outcome;
  }

  record.id = new_id;
  outcome.kind = Kind::Ok;
  outcome.submission = std::move(record);
  outcome.judge = std::move(judge_result);
  return outcome;
}

} // namespace submit
} // namespace oj
