#include "judge/judge.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

#include "judge/comparator.h"
#include "judge/compile_gate.h"
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

JudgeResult JudgeEngine::judge(const JudgeTask &task,
                               const CancellationToken *cancel) {
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

  // 单测试点时限先按硬上限裁剪（SPEC JUDGE-10），再与剩余全局预算取较小者。
  int problem_limit_ms = task.time_limit_ms;
  if (problem_limit_ms > options_.max_time_limit_ms) {
    problem_limit_ms = options_.max_time_limit_ms;
  }

  // 全局截止时间在本次判题开始时确定，使用单调时钟，覆盖编译与全部测试点执行，
  // 不包含排队等待；即使题目时限为无限/极大也生效，进入新测试点不会重置。
  const Deadline global_deadline =
      Deadline::after_ms(options_.global_time_limit_ms);

  // 全局硬上限耗尽：终止剩余执行，保留已有逐点结果；未执行点不写入 cases。
  auto mark_global_exhausted = [&result, this]() {
    result.global_deadline_hit = true;
    result.status = JudgeStatus::SYSERR;
    result.message = "全局判题时间上限（" +
                     std::to_string(options_.global_time_limit_ms) +
                     " ms）耗尽，剩余测试点未执行";
  };
  // 服务停止：按内部错误约定处理，绝不归咎于用户程序。
  auto mark_cancelled = [&result]() {
    result.cancelled = true;
    result.status = JudgeStatus::SYSERR;
    result.message = "服务停止，判题已取消";
  };

  // 已收到取消信号：不创建目录、不启动任何进程。
  if (cancel != nullptr && cancel->cancelled()) {
    mark_cancelled();
    return result;
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

  // 3. 编译一次。先获取全局编译并发门限（若配置）：等待时间计入本次判题的全局
  //    硬上限，并周期性响应取消；既不无限等待，也不在未取得许可时启动编译。
  CompileGateGuard gate_guard(options_.compile_gate.get(), global_deadline,
                              cancel);
  if (!gate_guard.acquired()) {
    if (cancel != nullptr && cancel->cancelled()) {
      mark_cancelled();
    } else {
      mark_global_exhausted();
    }
    return result;
  }

  const int remaining_before_compile = global_deadline.remaining_ms();
  if (remaining_before_compile <= 0) {
    mark_global_exhausted();
    return result;
  }
  int compile_limit_ms = options_.compile_time_limit_ms;
  bool compile_global_capped = false;
  if (compile_limit_ms > remaining_before_compile) {
    compile_limit_ms = remaining_before_compile;
    compile_global_capped = true;
  }

  CompileRequest compile_request;
  compile_request.language = language;
  compile_request.compiler =
      (language == Language::Cpp17) ? options_.cpp_compiler
                                    : options_.c_compiler;
  compile_request.source_path = source_path;
  compile_request.output_path = executable_path;
  compile_request.working_directory = workspace->path();
  compile_request.time_limit_ms = compile_limit_ms;
  compile_request.output_limit_bytes = options_.compile_output_limit_bytes;
  compile_request.cancel = cancel;
  compile_request.sandbox = options_.sandbox_enabled;
  compile_request.memory_limit_kb = options_.compile_memory_limit_kb;
  compile_request.extra_flags = options_.extra_compile_flags;

  ProcessResult compile_result = executor_.compile(compile_request);
  // 编译结束立即释放编译许可，运行阶段不占用高内存阶段的并发名额。
  gate_guard.release_now();
  result.compile_output = compile_result.stdout_data;
  if (!compile_result.stderr_data.empty()) {
    if (!result.compile_output.empty()) {
      result.compile_output.push_back('\n');
    }
    result.compile_output += compile_result.stderr_data;
  }

  if (compile_result.cancelled) {
    mark_cancelled();
    return result;
  }
  if (compile_result.launch_error) {
    // 编译器不存在、沙箱初始化失败等属于执行环境/策略故障，绝不伪装成 CE，
    // 也绝不降级为无沙箱编译。
    result.status = JudgeStatus::SYSERR;
    result.message = (compile_result.sandbox_error ? "编译沙箱不可用: "
                                                   : "编译环境故障: ") +
                     compile_result.launch_error_message;
    return result;
  }
  if (compile_result.timed_out) {
    if (compile_global_capped) {
      mark_global_exhausted();
      return result;
    }
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

  // 4. 顺序执行测试点。普通 WA 不阻止后续测试点。每个测试点的有效时限为题目时限
  //    与剩余全局预算的较小者；全局预算耗尽则不再启动新测试点，已有结果保留。
  for (std::size_t i = 0; i < task.testcases.size(); ++i) {
    if (cancel != nullptr && cancel->cancelled()) {
      mark_cancelled();
      break;
    }
    const int remaining_global_ms = global_deadline.remaining_ms();
    if (remaining_global_ms <= 0) {
      mark_global_exhausted();
      break;
    }
    const int effective_limit_ms =
        effective_time_limit_ms(problem_limit_ms, remaining_global_ms);
    if (effective_limit_ms <= 0) {
      mark_global_exhausted();
      break;
    }
    const bool global_capped = effective_limit_ms < problem_limit_ms;

    const Testcase &testcase = task.testcases[i];

    RunRequest run_request;
    run_request.executable_path = executable_path;
    run_request.working_directory = workspace->path();
    run_request.time_limit_ms = effective_limit_ms;
    run_request.stdout_limit_bytes = options_.stdout_limit_bytes;
    run_request.stderr_limit_bytes = options_.stderr_limit_bytes;
    run_request.cancel = cancel;
    run_request.sandbox = options_.sandbox_enabled;
    run_request.memory_limit_kb = task.memory_limit_kb > 0
                                      ? task.memory_limit_kb
                                      : options_.default_memory_limit_kb;

    ProcessResult run_result = executor_.run(run_request, testcase.input);

    TestcaseResult case_result;
    case_result.index = static_cast<int>(i);
    case_result.time_ms = run_result.time_ms;
    case_result.memory_kb = run_result.memory_kb;
    case_result.memory_exceeded = run_result.memory_exceeded;
    case_result.timed_out = run_result.timed_out;
    case_result.output_truncated = run_result.stdout_truncated;
    case_result.exit_code = run_result.exit_code;
    case_result.term_signal = run_result.term_signal;
    case_result.stderr_output = run_result.stderr_data;

    if (run_result.cancelled) {
      case_result.status = JudgeStatus::SYSERR;
      case_result.message = "服务停止，判题已取消";
      result.cases.push_back(std::move(case_result));
      mark_cancelled();
      break;
    } else if (run_result.launch_error) {
      case_result.status = JudgeStatus::SYSERR;
      case_result.message =
          (run_result.sandbox_error ? std::string("运行沙箱不可用: ")
                                    : std::string("无法启动运行进程: ")) +
          run_result.launch_error_message;
      result.cases.push_back(std::move(case_result));
      break; // 系统/策略故障：无法继续可靠判题，提前终止
    } else if (run_result.timed_out && global_capped) {
      // 该点并未超过题目时限，而是可用全局预算不足以完成，属全局硬上限终止。
      case_result.status = JudgeStatus::TLE;
      case_result.global_deadline_hit = true;
      case_result.message = "触发全局判题时间上限（" +
                            std::to_string(options_.global_time_limit_ms) +
                            " ms）";
      result.cases.push_back(std::move(case_result));
      mark_global_exhausted();
      break;
    } else if (run_result.timed_out) {
      case_result.status = JudgeStatus::TLE;
      case_result.message =
          "超出时间限制（" + std::to_string(problem_limit_ms) + " ms）";
    } else if (run_result.memory_exceeded) {
      // 明确由 RSS 采样判定超内存：与所有 SIGKILL 区分，保留证据供 M3.4 分类。
      case_result.status = JudgeStatus::MLE;
      case_result.message =
          "超出内存限制（RSS > " +
          std::to_string(task.memory_limit_kb > 0 ? task.memory_limit_kb
                                                  : options_.default_memory_limit_kb) +
          " kB，峰值 " + std::to_string(run_result.memory_kb) + " kB）";
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

  // 5. 汇总。全局硬上限或服务取消终止时按内部错误处理，覆盖逐点汇总结果，
  //    但保留已经获得的逐点结果与 passed 计数。
  result.status = summarize_cases(result.cases);
  if (result.global_deadline_hit) {
    result.status = JudgeStatus::SYSERR;
  } else if (result.cancelled) {
    result.status = JudgeStatus::SYSERR;
  } else if (result.status == JudgeStatus::AC) {
    result.message = "全部测试点通过";
  } else if (result.status == JudgeStatus::WA) {
    result.message = "存在输出不匹配的测试点";
  }

  return result;
}

} // namespace judge
} // namespace oj
