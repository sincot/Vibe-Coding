#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "db/problems.h"
#include "db/submissions.h"
#include "judge/executor.h"
#include "judge/judge.h"

namespace oj {

class Database;

namespace submit {

// language 字段的规范取值（小写）。接口仅接受这两个值（大小写不敏感）。
inline constexpr const char *kLanguageCpp17 = "cpp17";
inline constexpr const char *kLanguageC11 = "c11";

// 源码字节上限与 HTTP 请求体上限。超长源码返回 400；请求体整体超过上限由
// cpp-httplib 以 413 拒绝（在 HttpServer 中通过 set_payload_max_length 配置）。
inline constexpr std::size_t kMaxSourceBytes = 64 * 1024;        // 64 KiB
inline constexpr std::size_t kMaxRequestBodyBytes = 1024 * 1024; // 1 MiB

// 解析 language：仅接受 "cpp17" 与 "c11"（大小写不敏感），成功后写出规范小写值。
// 失败返回 false，调用方返回 400。
bool parse_submission_language(const std::string &text, std::string &canonical);

// 校验源码：非空白且字节数不超过 kMaxSourceBytes。仅做校验，绝不修改源码内容。
// 源码同时存入数据库并送交判题器，保持与用户提交完全一致。
bool validate_source_code(const std::string &code, std::string &error);

// 现有做题状态（纯数据，便于对状态计算单独做单元测试）。
struct StatusState {
  bool has_record = false;
  bool accepted = false;
  std::string first_ac_at; // accepted 时有效的首次 AC 时间
  int submit_count = 0;
};

// 本次提交后的新状态。
struct StatusUpdate {
  bool accepted = false;
  bool has_first_ac_at = false;
  std::string first_ac_at;
  int submit_count = 0;
};

// 状态计算（纯函数）：
//   - 每条已持久化的提交使 submit_count 加一；
//   - 首次 AC 设置 accepted 与 first_ac_at（取本次提交时间）；
//   - 重复 AC 保留既有首次 AC 时间，不覆盖；并发任务乱序完成时，若本次 AC 的
//     原提交时间早于已记录值，则收敛为更早者，保证首次 AC 时间取最早符合条件的
//     原提交时间，不因完成顺序倒置而出错；
//   - AC 之后的失败提交保留既有 accepted 与首次 AC 时间，不清除；
//   - 从未 AC 的失败提交保持 none 且不写首次 AC 时间。
StatusUpdate compute_status_update(const StatusState &current,
                                   bool submission_accepted,
                                   const std::string &submission_time);

// UTC 时间 "YYYY-MM-DD HH:MM:SS"，与 SQLite datetime('now') 同格式，作为提交时间
// 的统一口径；submissions.created_at 与 user_problem_status.first_ac_at 取自同一
// 次计算，便于后续 Rejudge 按原提交时间重算。
std::string utc_timestamp_now();

// 提交服务：串联可见性检查、隐藏用例读取、同步判题与单事务持久化。
//
// 设计要点：
//   - 用户身份由调用方（HTTP 层）从已验证的当前用户上下文传入，本服务不接受任何
//     客户端提供的用户 ID / 判题状态 / 标准答案 / 用例 / 资源限制覆盖值；
//   - 可见性复用 M1.4 规则：不存在或对当前用户不可见的题目统一返回 ProblemNotFound；
//   - 判题所需的完整用例（含隐藏）在判题前从数据库读出并构造为内存中的任务快照，
//     判题期间不持有任何数据库事务；
//   - 判题结果（含 CE/WA/TLE/RE/MLE/SYSERR）属于提交记录，一律持久化并计数；
//   - 提交记录写入与用户题目状态更新在同一个短事务内完成，任一失败整体回滚。
class SubmitService {
public:
  SubmitService(Database &db, judge::IExecutor &executor,
                judge::JudgeOptions options = {});

  enum class Kind {
    Ok,              // 判题完成并已持久化（判题结果可为 AC/WA/CE/TLE/RE/MLE/SYSERR）
    ProblemNotFound, // 题目不存在，或当前用户无权访问（统一处理，不泄露存在性）
    InternalError,   // 数据库等内部故障
  };

  struct Outcome {
    Kind kind = Kind::InternalError;
    SubmissionRecord submission; // 已持久化的记录（含新 id 与 created_at）
    judge::JudgeResult judge;    // 判题器原始结果
    std::string error;
  };

  // viewer_is_admin=true 时允许向隐藏题目提交（仅供已通过管理员检查的调用方传入）。
  //
  // submitted_at 为原始提交时间（调度器接受入队时采集的 UTC 时间字符串），
  // 用作 submissions.created_at 与 first_ac_at 的统一口径；传入空串时退回当前时间。
  // 排队等待判题的时间不计入该时间戳，也不计入任何测试点耗时。
  //
  // cancel 非空时支持服务停止取消：不再启动新进程，正在运行/编译的进程组会被终止，
  // 取消结果按内部错误 SYSERR 正常持久化（先于数据库关闭）。
  Outcome submit(std::int64_t user_id, std::int64_t problem_id,
                 const std::string &language, const std::string &source_code,
                 bool viewer_is_admin, const std::string &submitted_at,
                 const judge::CancellationToken *cancel = nullptr);

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
