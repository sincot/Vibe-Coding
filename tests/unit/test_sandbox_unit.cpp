// M3.3 沙箱与资源限制单元测试（基于 gtest，不启动真实判题进程）。
//
// 覆盖：
//   - CompileGate：并发上限、超时放弃、取消响应、RAII 释放；
//   - compute_limits：编译/运行阶段资源上限与单点时限换算；
//   - build_seccomp_program：编译/运行阶段过滤器可构建且运行阶段约束更多；
//   - sandbox_supported / path_is_tmpfs / mount_capacity_bytes 能力探测；
//   - LocalExecutor::sandbox_self_test 在不可用工作目录时明确失败；
//   - Workspace 清理：不越界、不跟随符号链接、不误删他人目录。
//
// 真实隔离与资源限制效果由 tests/integration/test_sandbox_integration.cpp 覆盖。
//
// 运行方式：ctest --test-dir build -R sandbox_unit --output-on-failure
// 或直接执行 build/oj_sandbox_unit。

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include <linux/audit.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>

#include "judge/compile_gate.h"
#include "judge/deadline.h"
#include "judge/executor.h"
#include "judge/judge.h"
#include "judge/local_executor.h"
#include "judge/sandbox.h"
#include "judge/workspace.h"

namespace {

using oj::judge::CancellationToken;
using oj::judge::CompileGate;
using oj::judge::CompileGateGuard;
using oj::judge::Deadline;
using oj::judge::JudgeEngine;
using oj::judge::JudgeOptions;
using oj::judge::JudgeResult;
using oj::judge::JudgeStatus;
using oj::judge::JudgeTask;
using oj::judge::LocalExecutor;
using oj::judge::SandboxLimits;
using oj::judge::SandboxPhase;
using oj::judge::SeccompProgram;
using oj::judge::Workspace;

long long elapsed_ms(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

// ---------------------------------------------------------------------------
// CompileGate
// ---------------------------------------------------------------------------

TEST(CompileGate, MaxOneSerializes) {
  CompileGate gate(1);
  EXPECT_TRUE(gate.acquire(Deadline::after_ms(5000), nullptr));
  EXPECT_EQ(gate.active(), 1);

  // 已占满：在短截止时间内无法再取得许可。
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(gate.acquire(Deadline::after_ms(120), nullptr));
  EXPECT_GE(elapsed_ms(start), 80);

  gate.release();
  EXPECT_EQ(gate.active(), 0);
  EXPECT_TRUE(gate.acquire(Deadline::after_ms(1000), nullptr));
  gate.release();
}

TEST(CompileGate, RespectsConfiguredLimit) {
  CompileGate gate(2);
  EXPECT_EQ(gate.max_concurrent(), 2);
  EXPECT_TRUE(gate.acquire(Deadline::after_ms(5000), nullptr));
  EXPECT_TRUE(gate.acquire(Deadline::after_ms(5000), nullptr));
  EXPECT_EQ(gate.active(), 2);
  EXPECT_FALSE(gate.acquire(Deadline::after_ms(100), nullptr));
  gate.release();
  EXPECT_TRUE(gate.acquire(Deadline::after_ms(1000), nullptr));
  gate.release();
  gate.release();
  EXPECT_EQ(gate.active(), 0);
}

TEST(CompileGate, CancelUnblocksWait) {
  CompileGate gate(1);
  EXPECT_TRUE(gate.acquire(Deadline::after_ms(5000), nullptr));

  CancellationToken token;
  std::thread canceller([&token] {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    token.cancel();
  });

  auto start = std::chrono::steady_clock::now();
  // 截止时间很长，但取消应在数十毫秒内使其返回 false。
  EXPECT_FALSE(gate.acquire(Deadline::after_ms(20000), &token));
  EXPECT_LT(elapsed_ms(start), 2000);
  canceller.join();
  gate.release();
}

TEST(CompileGate, GuardReleasesOnScopeExit) {
  CompileGate gate(1);
  {
    CompileGateGuard guard(&gate, Deadline::after_ms(1000), nullptr);
    EXPECT_TRUE(guard.acquired());
    EXPECT_EQ(gate.active(), 1);
  }
  EXPECT_EQ(gate.active(), 0);
}

TEST(CompileGate, NullGateIsAlwaysAcquired) {
  CompileGateGuard guard(nullptr, Deadline::after_ms(1000), nullptr);
  EXPECT_TRUE(guard.acquired());
  guard.release_now(); // 无门限时应为无操作
}

TEST(CompileGate, ReleaseNowFreesSlotBeforeDestruction) {
  CompileGate gate(1);
  CompileGateGuard guard(&gate, Deadline::after_ms(1000), nullptr);
  EXPECT_EQ(gate.active(), 1);
  guard.release_now();
  EXPECT_EQ(gate.active(), 0);
  // 析构时不应重复释放导致 active 变负（作用域结束时自动析构）。
}

// ---------------------------------------------------------------------------
// compute_limits
// ---------------------------------------------------------------------------

TEST(SandboxLimits, RunUsesProblemMemoryAndWallClockCpu) {
  SandboxLimits limits = oj::judge::compute_limits(SandboxPhase::Run, 65536, 2000);
  EXPECT_EQ(limits.memory_limit_kb, 65536);
  // 2s 时限 -> CPU 上限 3s（向上取整 +1s 余量）。
  EXPECT_EQ(limits.cpu_limit_sec, 3);
  EXPECT_FALSE(limits.set_address_space); // 默认不设 RLIMIT_AS（ASan 兼容）
}

TEST(SandboxLimits, CompileGetsLargerBudgetThanRun) {
  SandboxLimits run = oj::judge::compute_limits(SandboxPhase::Run, 65536, 2000);
  SandboxLimits compile =
      oj::judge::compute_limits(SandboxPhase::Compile, 1048576, 2000);
  EXPECT_GT(compile.cpu_limit_sec, run.cpu_limit_sec);
  EXPECT_GT(compile.nofile_limit, run.nofile_limit);
  EXPECT_GT(compile.fsize_limit_bytes, run.fsize_limit_bytes);
}

// ---------------------------------------------------------------------------
// build_seccomp_program
// ---------------------------------------------------------------------------

TEST(SeccompProgram, RunHasMoreRulesThanCompile) {
  SeccompProgram compile_filter;
  SeccompProgram run_filter;
  std::string error;
  ASSERT_TRUE(oj::judge::build_seccomp_program(SandboxPhase::Compile,
                                               compile_filter, error))
      << error;
  ASSERT_TRUE(
      oj::judge::build_seccomp_program(SandboxPhase::Run, run_filter, error))
      << error;
  EXPECT_FALSE(compile_filter.empty());
  EXPECT_FALSE(run_filter.empty());
  // 运行阶段额外禁止 fork/clone 等创建进程的系统调用，规则数应更多。
  EXPECT_GT(run_filter.instructions.size(), compile_filter.instructions.size());
}

// ---------------------------------------------------------------------------
// 能力探测
// ---------------------------------------------------------------------------

TEST(SandboxSupport, SupportedOnThisHost) {
  std::string error;
  EXPECT_TRUE(oj::judge::sandbox_supported(error)) << error;
}

TEST(TmpfsProbe, ProcIsNotTmpfs) {
  std::string error;
  EXPECT_FALSE(oj::judge::path_is_tmpfs("/proc", error));
  EXPECT_FALSE(error.empty());
}

TEST(TmpfsProbe, DevShmIsTmpfsWhenPresent) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::exists("/dev/shm", ec)) {
    GTEST_SKIP() << "/dev/shm 不存在";
  }
  std::string error;
  EXPECT_TRUE(oj::judge::path_is_tmpfs("/dev/shm", error)) << error;
  EXPECT_GT(oj::judge::mount_capacity_bytes("/dev/shm"), 0);
}

TEST(SandboxSelfTest, FailsClearlyOnUnusableWorkspace) {
  std::string error;
  EXPECT_FALSE(LocalExecutor::sandbox_self_test("/proc/oj-no-such-workspace",
                                                error));
  EXPECT_FALSE(error.empty());
}

// ---------------------------------------------------------------------------
// Workspace 清理安全
// ---------------------------------------------------------------------------

std::string make_temp_base(const std::string &label) {
  namespace fs = std::filesystem;
  fs::path base = fs::temp_directory_path();
  for (int i = 0; i < 1000; ++i) {
    fs::path candidate =
        base / (label + "_" + std::to_string(::getpid()) + "_" +
                std::to_string(i));
    std::error_code ec;
    fs::create_directories(candidate, ec);
    if (!ec) {
      return candidate.string();
    }
  }
  return {};
}

TEST(WorkspaceCleanup, RemovesOwnDirectoryOnly) {
  namespace fs = std::filesystem;
  const std::string base = make_temp_base("ws_cleanup");
  ASSERT_FALSE(base.empty());
  std::string error;
  auto first = Workspace::create(base, error);
  ASSERT_TRUE(first != nullptr) << error;
  auto second = Workspace::create(base, error);
  ASSERT_TRUE(second != nullptr) << error;
  const std::string first_path = first->path();
  const std::string second_path = second->path();
  EXPECT_NE(first_path, second_path);

  first.reset();
  EXPECT_FALSE(fs::exists(first_path));
  EXPECT_TRUE(fs::exists(second_path));

  second.reset();
  fs::remove_all(base);
}

// ---------------------------------------------------------------------------
// JudgeEngine 与编译门限协作
// ---------------------------------------------------------------------------

class QuickFakeExecutor : public oj::judge::IExecutor {
public:
  int compile_calls = 0;
  int run_calls = 0;

  oj::judge::ProcessResult compile(
      const oj::judge::CompileRequest &request) override {
    ++compile_calls;
    std::ofstream out(request.output_path, std::ios::binary);
    out << "fake-binary";
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    return result;
  }
  oj::judge::ProcessResult run(const oj::judge::RunRequest &,
                               const std::string &) override {
    ++run_calls;
    oj::judge::ProcessResult result;
    result.launched = true;
    result.exited = true;
    result.exit_code = 0;
    result.stdout_data = "ok\n";
    return result;
  }
};

oj::judge::JudgeResult run_with_gate(QuickFakeExecutor &executor,
                                     const std::string &workspace,
                                     std::shared_ptr<CompileGate> gate,
                                     int global_time_limit_ms,
                                     const CancellationToken *cancel = nullptr) {
  JudgeOptions options;
  options.workspace_root = workspace;
  options.compile_gate = std::move(gate);
  options.global_time_limit_ms = global_time_limit_ms;
  options.sandbox_enabled = false; // 使用 FakeExecutor，不启动真实进程
  JudgeEngine engine(executor, options);
  JudgeTask task;
  task.language = "cpp17";
  task.source_code = "int main(){}";
  task.testcases = {{"", "ok\n"}};
  task.time_limit_ms = 1000;
  return engine.judge(task, cancel);
}

TEST(JudgeEngineCompileGate, WaitCountsTowardGlobalBudget) {
  const std::string workspace = make_temp_base("gate_budget");
  ASSERT_FALSE(workspace.empty());

  auto gate = std::make_shared<CompileGate>(1);
  ASSERT_TRUE(gate->acquire(Deadline::after_ms(5000), nullptr)); // 占住唯一名额

  QuickFakeExecutor executor;
  auto start = std::chrono::steady_clock::now();
  JudgeResult result = run_with_gate(executor, workspace, gate,
                                     /*global_time_limit_ms=*/300);
  const long long elapsed = elapsed_ms(start);

  EXPECT_LT(elapsed, 3000) << "等待编译门限不应无限等待";
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  EXPECT_TRUE(result.global_deadline_hit);
  EXPECT_EQ(executor.compile_calls, 0); // 未取得许可，绝不启动编译

  gate->release();
  std::filesystem::remove_all(workspace);
}

TEST(JudgeEngineCompileGate, CancelInterruptsWait) {
  const std::string workspace = make_temp_base("gate_cancel");
  ASSERT_FALSE(workspace.empty());

  auto gate = std::make_shared<CompileGate>(1);
  ASSERT_TRUE(gate->acquire(Deadline::after_ms(5000), nullptr));

  QuickFakeExecutor executor;
  CancellationToken token;
  std::thread canceller([&token] {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    token.cancel();
  });
  auto start = std::chrono::steady_clock::now();
  JudgeResult result = run_with_gate(executor, workspace, gate,
                                     /*global_time_limit_ms=*/30000, &token);
  const long long elapsed = elapsed_ms(start);
  canceller.join();

  EXPECT_LT(elapsed, 3000) << "取消应立即中断门限等待";
  EXPECT_EQ(result.status, JudgeStatus::SYSERR);
  EXPECT_TRUE(result.cancelled);
  EXPECT_EQ(executor.compile_calls, 0);

  gate->release();
  std::filesystem::remove_all(workspace);
}

TEST(JudgeEngineCompileGate, ReleasedAfterCompileBeforeRun) {
  const std::string workspace = make_temp_base("gate_release");
  ASSERT_FALSE(workspace.empty());

  auto gate = std::make_shared<CompileGate>(1);
  QuickFakeExecutor executor;
  JudgeResult result = run_with_gate(executor, workspace, gate,
                                     /*global_time_limit_ms=*/10000);

  EXPECT_EQ(result.status, JudgeStatus::AC);
  EXPECT_EQ(executor.compile_calls, 1);
  EXPECT_EQ(executor.run_calls, 1);
  EXPECT_EQ(gate->active(), 0) << "编译结束后应立即释放门限名额";

  std::filesystem::remove_all(workspace);
}

TEST(WorkspaceCleanup, DoesNotFollowSymlinkAtPath) {
  namespace fs = std::filesystem;
  const std::string base = make_temp_base("ws_symlink");
  ASSERT_FALSE(base.empty());
  std::string error;
  auto workspace = Workspace::create(base, error);
  ASSERT_TRUE(workspace != nullptr) << error;
  const std::string ws_path = workspace->path();

  // 在别处准备一个“受害者”目录，并把工作目录路径替换为指向它的符号链接。
  const std::string victim = base + "_victim";
  fs::create_directories(victim);
  {
    std::ofstream out(victim + "/keep.txt");
    out << "keep";
  }
  std::error_code ec;
  fs::remove_all(ws_path, ec);
  fs::create_directory_symlink(victim, ws_path, ec);
  ASSERT_FALSE(ec);

  workspace.reset(); // 析构：只应删除符号链接本身，绝不跟随删除 victim

  EXPECT_FALSE(fs::exists(ws_path)) << "符号链接应被删除";
  EXPECT_TRUE(fs::exists(victim + "/keep.txt")) << "受害者目录内容不应被删除";
  fs::remove_all(victim, ec);
  fs::remove_all(base, ec);
}

// ---------------------------------------------------------------------------
// M3.3 审查补充：阶段名、资源换算回退、seccomp 规则内容、门限钳制、
//             挂载容量、Workspace 失败路径
// ---------------------------------------------------------------------------

TEST(SandboxStage, NamesDistinctAndNonEmpty) {
  using oj::judge::SandboxStage;
  const SandboxStage stages[] = {
      SandboxStage::None, SandboxStage::Unshare, SandboxStage::Root,
      SandboxStage::Chroot, SandboxStage::Limits, SandboxStage::Seccomp,
      SandboxStage::Chdir, SandboxStage::Exec};
  std::set<std::string> names;
  for (SandboxStage stage : stages) {
    const std::string name = oj::judge::sandbox_stage_name(stage);
    EXPECT_FALSE(name.empty()) << "阶段 " << static_cast<int>(stage);
    names.insert(name);
  }
  EXPECT_EQ(names.size(), sizeof(stages) / sizeof(stages[0]))
      << "各阶段应给出可区分的诊断名";
  EXPECT_EQ(std::string(oj::judge::sandbox_stage_name(
                static_cast<SandboxStage>(999))),
            "unknown");
}

TEST(SandboxLimits, FallbacksForInvalidInputs) {
  // memory_limit_kb<=0 时保持结构体默认内存上限，不产生 0 限制。
  SandboxLimits run = oj::judge::compute_limits(SandboxPhase::Run, 0, 0);
  EXPECT_EQ(run.memory_limit_kb, 65536);
  // time_limit_ms<=0 按 1 秒处理；Run 的 CPU 上限 = 1 + 1 = 2 秒。
  EXPECT_EQ(run.cpu_limit_sec, 2);
  EXPECT_FALSE(run.set_address_space);

  SandboxLimits compile =
      oj::judge::compute_limits(SandboxPhase::Compile, -1, -500);
  EXPECT_EQ(compile.memory_limit_kb, 65536);
  // Compile 的 CPU 上限 = 1 + 5 = 6 秒。
  EXPECT_EQ(compile.cpu_limit_sec, 6);
}

namespace {

// 解析 BPF 过滤器中所有“等于某系统调用号则拒绝”的 JEQ 立即数集合，
// 排除架构校验那条 JEQ（立即数为 AUDIT_ARCH_X86_64）。
std::set<unsigned int> denied_syscall_numbers(const SeccompProgram &program) {
  std::set<unsigned int> numbers;
  for (const sock_filter &insn : program.instructions) {
    if (insn.code == (BPF_JMP | BPF_JEQ | BPF_K) &&
        insn.k != static_cast<unsigned int>(AUDIT_ARCH_X86_64)) {
      numbers.insert(static_cast<unsigned int>(insn.k));
    }
  }
  return numbers;
}

} // namespace

TEST(SeccompProgram, DeniesExpectedSyscallsPerPhase) {
  SeccompProgram compile_filter;
  SeccompProgram run_filter;
  std::string error;
  ASSERT_TRUE(oj::judge::build_seccomp_program(SandboxPhase::Compile,
                                               compile_filter, error))
      << error;
  ASSERT_TRUE(
      oj::judge::build_seccomp_program(SandboxPhase::Run, run_filter, error))
      << error;

  const std::set<unsigned int> compile_denied =
      denied_syscall_numbers(compile_filter);
  const std::set<unsigned int> run_denied = denied_syscall_numbers(run_filter);

  // 两阶段都禁网络、挂载逃逸与调试/进程干扰。
  EXPECT_TRUE(compile_denied.count(__NR_socket) == 1);
  EXPECT_TRUE(run_denied.count(__NR_socket) == 1);
  EXPECT_TRUE(compile_denied.count(__NR_mount) == 1);
  EXPECT_TRUE(run_denied.count(__NR_mount) == 1);
  EXPECT_TRUE(compile_denied.count(__NR_ptrace) == 1);
  EXPECT_TRUE(run_denied.count(__NR_ptrace) == 1);

  // 仅运行阶段禁止创建进程，编译阶段必须允许编译器派生 cc1plus/as/ld。
  EXPECT_TRUE(run_denied.count(__NR_clone) == 1);
  EXPECT_TRUE(run_denied.count(__NR_fork) == 1);
  EXPECT_TRUE(compile_denied.count(__NR_clone) == 0);
  EXPECT_TRUE(compile_denied.count(__NR_fork) == 0);
}

TEST(CompileGate, NonPositiveLimitClampsToOne) {
  EXPECT_EQ(CompileGate(0).max_concurrent(), 1);
  EXPECT_EQ(CompileGate(-5).max_concurrent(), 1);
  EXPECT_EQ(CompileGate(3).max_concurrent(), 3);
}

TEST(TmpfsProbe, RootMountCapacityPositiveAndMatchesStatvfs) {
  struct statvfs info;
  ASSERT_EQ(::statvfs("/", &info), 0);
  const long long expected = static_cast<long long>(info.f_blocks) *
                             static_cast<long long>(info.f_frsize);
  EXPECT_GT(expected, 0);
  EXPECT_EQ(oj::judge::mount_capacity_bytes("/"), expected);
}

TEST(WorkspaceCreate, FailsWhenBaseIsRegularFile) {
  namespace fs = std::filesystem;
  const std::string base = make_temp_base("ws_basefile");
  ASSERT_FALSE(base.empty());
  const std::string file = base + "/not_a_dir";
  {
    std::ofstream out(file);
    out << "x";
  }
  std::string error;
  auto workspace = Workspace::create(file, error);
  EXPECT_EQ(workspace, nullptr);
  EXPECT_FALSE(error.empty());
  fs::remove_all(base);
}

TEST(WorkspaceWriteFile, FailsWhenTargetSubdirMissing) {
  namespace fs = std::filesystem;
  const std::string base = make_temp_base("ws_writefail");
  ASSERT_FALSE(base.empty());
  std::string error;
  auto workspace = Workspace::create(base, error);
  ASSERT_TRUE(workspace != nullptr) << error;
  // name 中含不存在的子目录：底层打开失败，应返回 false 且给出原因。
  EXPECT_FALSE(workspace->write_file("missing_dir/out.txt", "x", error));
  EXPECT_FALSE(error.empty());
  workspace.reset();
  fs::remove_all(base);
}

} // namespace
