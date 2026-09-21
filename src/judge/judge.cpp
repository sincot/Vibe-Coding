#include "judge/judge.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

#include "judge/comparator.h"
#include "judge/workspace.h"

namespace oj {
namespace judge {

JudgeStatus summarize_cases(const std::vector<TestcaseResult> &cases) {
  if (cases.empty()) {
    return JudgeStatus::SYSERR;
  }
  JudgeStatus worst = JudgeStatus::AC;
  int worst_severity = judge_status_severity(JudgeStatus::AC);
  for (const TestcaseResult &result : cases) {
    int severity = judge_status_severity(result.status);
    if (severity > worst_severity) {
      worst_severity = severity;
      worst = result.status;
    }
  }
  return worst;
}

JudgeEngine::JudgeEngine(IExecutor &executor, JudgeOptions options)
    : executor_(executor), options_(std::move(options)) {}

JudgeResult JudgeEngine::judge(const JudgeTask &task) {
  JudgeResult result;
  result.total = static_cast<int>(task.testcases.size());

  // 1. 输入校验：非法输入给出明确的 SYSERR，绝不返回 AC。
  Language language;
  if (!parse_language(task.language, language)) {
    result.status = JudgeStatus::SYSERR;
    result.message = "不支持的语言: " + task.language;
    return result;
  }
  if (task.testcases.empty()) {
    result.status = JudgeStatus::SYSERR;
    result.message = "没有可执行的测试用例";
    return result;
  }
  if (task.time_limit_ms <= 0) {
    result.status = JudgeStatus::SYSERR;
    result.message =
        "无效的时间限制: " + std::to_string(task.time_limit_ms) + " ms";
    return result;
  }

  int time_limit_ms = task.time_limit_ms;
  if (time_limit_ms > options_.max_time_limit_ms) {
    // SPEC JUDGE-10：即使题目时限配置过大，也受全局硬上限约束。
    time_limit_ms = options_.max_time_limit_ms;
  }

  // 2. 独立临时工作目录。
  std::string error;
  std::unique_ptr<Workspace> workspace =
      Workspace::create(options_.workspace_root, error);
  if (!workspace) {
    result.status = JudgeStatus::SYSERR;
    result.message = "创建判题工作目录失败: " + error;
    return result;
  }

  const std::string source_name =
      (language == Language::Cpp17) ? "main.cpp" : "main.c";
  const std::string source_path =
      (std::filesystem::path(workspace->path()) / source_name).string();
  const std::string executable_path =
      (std::filesystem::path(workspace->path()) / "program").string();

  if (!workspace->write_file(source_name, task.source_code, error)) {
    result.status = JudgeStatus::SYSERR;
    result.message = "写入源码失败: " + error;
    return result;
  }

  // 3. 编译一次。
  CompileRequest compile_request;
  compile_request.language = language;
  compile_request.compiler =
      (language == Language::Cpp17) ? options_.cpp_compiler
                                    : options_.c_compiler;
  compile_request.source_path = source_path;
  compile_request.output_path = executable_path;
  compile_request.working_directory = workspace->path();
  compile_request.time_limit_ms = options_.compile_time_limit_ms;
  compile_request.output_limit_bytes = options_.compile_output_limit_bytes;

  ProcessResult compile_result = executor_.compile(compile_request);
  result.compile_output = compile_result.stdout_data;
  if (!compile_result.stderr_data.empty()) {
    if (!result.compile_output.empty()) {
      result.compile_output.push_back('\n');
    }
    result.compile_output += compile_result.stderr_data;
  }

  if (compile_result.launch_error) {
    // 编译器不存在等属于执行环境故障，绝不伪装成 CE。
    result.status = JudgeStatus::SYSERR;
    result.message = "编译环境故障: " + compile_result.launch_error_message;
    return result;
  }
  if (compile_result.timed_out) {
    result.status = JudgeStatus::CE;
    result.message = "编译超时（" +
                     std::to_string(options_.compile_time_limit_ms) + " ms）";
    return result;
  }
  if (!compile_result.exited) {
    result.status = JudgeStatus::SYSERR;
    result.message = "编译器未正常结束";
    return result;
  }
  if (compile_result.term_signal != 0) {
    result.status = JudgeStatus::SYSERR;
    result.message = "编译器被信号 " +
                     std::to_string(compile_result.term_signal) + " 终止";
    return result;
  }
  if (compile_result.exit_code != 0) {
    result.status = JudgeStatus::CE;
    result.message = "编译错误";
    return result;
  }
  if (!std::filesystem::exists(executable_path)) {
    result.status = JudgeStatus::SYSERR;
    result.message = "编译成功但未生成可执行文件";
    return result;
  }
  result.compile_ok = true;

  // 4. 顺序执行测试点。普通 WA 不阻止后续测试点；仅内部执行故障会提前终止。
  for (std::size_t i = 0; i < task.testcases.size(); ++i) {
    const Testcase &testcase = task.testcases[i];

    RunRequest run_request;
    run_request.executable_path = executable_path;
    run_request.working_directory = workspace->path();
    run_request.time_limit_ms = time_limit_ms;
    run_request.stdout_limit_bytes = options_.stdout_limit_bytes;
    run_request.stderr_limit_bytes = options_.stderr_limit_bytes;

    ProcessResult run_result = executor_.run(run_request, testcase.input);

    TestcaseResult case_result;
    case_result.index = static_cast<int>(i);
    case_result.time_ms = run_result.time_ms;
    case_result.timed_out = run_result.timed_out;
    case_result.output_truncated = run_result.stdout_truncated;
    case_result.exit_code = run_result.exit_code;
    case_result.term_signal = run_result.term_signal;
    case_result.stderr_output = run_result.stderr_data;

    if (run_result.launch_error) {
      case_result.status = JudgeStatus::SYSERR;
      case_result.message = "无法启动运行进程: " +
                            run_result.launch_error_message;
      result.cases.push_back(std::move(case_result));
      break; // 系统故障：无法继续可靠判题，提前终止
    }
    if (run_result.timed_out) {
      case_result.status = JudgeStatus::TLE;
      case_result.message =
          "超出时间限制（" + std::to_string(time_limit_ms) + " ms）";
    } else if (run_result.term_signal != 0) {
      case_result.status = JudgeStatus::RE;
      case_result.message = "运行时被信号 " +
                            std::to_string(run_result.term_signal) + " 终止";
    } else if (!run_result.exited) {
      case_result.status = JudgeStatus::RE;
      case_result.message = "运行进程未正常结束";
    } else if (run_result.exit_code != 0) {
      case_result.status = JudgeStatus::RE;
      case_result.message =
          "非零退出码 " + std::to_string(run_result.exit_code);
    } else if (run_result.stdout_truncated) {
      // 输出超限时结果已被截断，绝不能当作正常输出判为 AC。
      case_result.status = JudgeStatus::RE;
      case_result.message =
          "标准输出超过上限 " +
          std::to_string(options_.stdout_limit_bytes) + " 字节";
    } else if (outputs_match(testcase.output, run_result.stdout_data)) {
      case_result.status = JudgeStatus::AC;
      ++result.passed;
    } else {
      case_result.status = JudgeStatus::WA;
      case_result.message = "输出不匹配";
    }

    if (case_result.status != JudgeStatus::AC) {
      case_result.actual_output = run_result.stdout_data;
    }
    result.cases.push_back(std::move(case_result));
  }

  // 5. 汇总。
  result.status = summarize_cases(result.cases);
  if (result.status == JudgeStatus::AC) {
    result.message = "全部测试点通过";
  } else if (result.status == JudgeStatus::WA) {
    result.message = "存在输出不匹配的测试点";
  }

  return result;
}

} // namespace judge
} // namespace oj
