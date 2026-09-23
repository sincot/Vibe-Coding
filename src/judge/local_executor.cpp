#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "judge/local_executor.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "judge/workspace.h"

namespace oj {
namespace judge {

namespace {

// 子进程向父进程回传的失败信息。stage 对应 SandboxStage，Exec 表示 execve 失败，
// Chdir 表示工作目录切换失败。父进程据此给出明确诊断，绝不静默降级。
struct ExecFailure {
  int stage;
  int error;
};

// 忽略 SIGPIPE：父进程向已退出的子进程标准输入写入时会得到 EPIPE 错误而非被
// 信号杀死。进程级设置一次即可，服务器写 socket 时同样希望忽略 SIGPIPE。
void ignore_sigpipe_once() {
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &action, nullptr);
  });
}

// RAII 文件描述符，保证所有退出路径都会关闭。
class ScopedFd {
public:
  ScopedFd() = default;
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() { reset(); }

  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;

  ScopedFd(ScopedFd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  ScopedFd &operator=(ScopedFd &&other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

  void reset(int fd = -1) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

private:
  int fd_ = -1;
};

bool set_nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// 子进程在 exec 前关闭所有无关文件描述符，避免继承服务监听套接字、数据库连接、
// 其它任务的管道等。保留 keep_fd 与可选的 keep_fd2（用于 exec 失败回传与回传载荷
// PID）。
void close_extra_fds(int keep_fd, int keep_fd2 = -1) {
#ifdef SYS_close_range
  if (keep_fd >= 3) {
    int ka = keep_fd;
    int kb = keep_fd2;
    if (kb >= 0 && kb < ka) {
      std::swap(ka, kb);
    }
    bool ok = true;
    if (ka > 3) {
      ok = ::syscall(SYS_close_range, 3u, static_cast<unsigned int>(ka - 1),
                     0u) == 0;
    }
    if (ok && kb >= 0) {
      if (kb > ka + 1) {
        ok = ::syscall(SYS_close_range, static_cast<unsigned int>(ka + 1),
                       static_cast<unsigned int>(kb - 1), 0u) == 0;
      }
      if (ok) {
        ok = ::syscall(SYS_close_range, static_cast<unsigned int>(kb + 1),
                       ~0u, 0u) == 0;
      }
    } else if (ok) {
      ok = ::syscall(SYS_close_range, static_cast<unsigned int>(ka + 1), ~0u,
                     0u) == 0;
    }
    if (ok) {
      return;
    }
  }
#endif
  long max_fd = 1024;
  struct rlimit limit;
  if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 &&
      limit.rlim_cur != RLIM_INFINITY) {
    max_fd = static_cast<long>(limit.rlim_cur);
  }
  if (max_fd > 65536) {
    max_fd = 65536;
  }
  for (int fd = 3; fd < max_fd; ++fd) {
    if (fd != keep_fd && fd != keep_fd2) {
      ::close(fd);
    }
  }
}

// 终止整个进程组，覆盖用户程序或编译器派生出的后代进程。
void kill_process_group(pid_t pid) {
  if (pid <= 0) {
    return;
  }
  if (::kill(-pid, SIGKILL) != 0 && errno == ESRCH) {
    ::kill(pid, SIGKILL);
  }
}

pid_t waitpid_retry(pid_t pid, int *status, int options) {
  for (;;) {
    pid_t result = ::waitpid(pid, status, options);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return result;
  }
}

void append_bounded(std::string &buffer, const char *data, std::size_t size,
                    std::size_t limit, bool &truncated) {
  if (buffer.size() >= limit) {
    truncated = true;
    return;
  }
  std::size_t room = limit - buffer.size();
  std::size_t take = std::min(room, size);
  buffer.append(data, take);
  if (take < size) {
    truncated = true;
  }
}

bool resolve_in_path(const std::string &name, std::string &out) {
  if (name.empty()) {
    return false;
  }
  const char *env = std::getenv("PATH");
  std::string path =
      (env != nullptr && *env != '\0')
          ? std::string(env)
          : std::string("/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
  std::size_t start = 0;
  while (start <= path.size()) {
    std::size_t end = path.find(':', start);
    std::string dir = path.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (dir.empty()) {
      dir = ".";
    }
    std::string candidate = dir + "/" + name;
    if (::access(candidate.c_str(), X_OK) == 0) {
      out = candidate;
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

bool starts_with(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

// 判断诊断内容是否包含主流 Sanitizer 报告的特征字符串。仅用于在进程已异常终止
// 时补充诊断文案，绝不作为状态判定依据（用户可自行打印类似文本）。
bool looks_like_sanitizer_report(const std::string &stderr_data) {
  static const char *const kMarkers[] = {
      "AddressSanitizer", "UndefinedBehaviorSanitizer",
      "LeakSanitizer",   "runtime error:",
      "heap-buffer-overflow", "stack-buffer-overflow",
      "SUMMARY: AddressSanitizer", "SUMMARY: UndefinedBehaviorSanitizer",
  };
  for (const char *marker : kMarkers) {
    if (stderr_data.find(marker) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string dir_name(const std::string &path) {
  std::size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return ".";
  }
  if (pos == 0) {
    return "/";
  }
  return path.substr(0, pos);
}

std::string base_name(const std::string &path) {
  std::size_t pos = path.find_last_of('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

// 系统目录内的可执行文件在沙箱最小根中已存在，直接使用原路径；否则需将所在目录
// 只读 bind 到 /oj-tools。
bool is_system_prefix(const std::string &path) {
  const char *prefixes[] = {"/usr/", "/bin/", "/sbin/", "/lib/",
                            "/lib64/", "/usr", "/bin", "/sbin",
                            "/lib",   "/lib64", nullptr};
  for (int i = 0; prefixes[i] != nullptr; ++i) {
    if (path == prefixes[i] || starts_with(path, std::string(prefixes[i]) + "/")) {
      return true;
    }
  }
  return false;
}

std::string describe_failure(const ExecFailure &failure) {
  const char *stage_name =
      sandbox_stage_name(static_cast<SandboxStage>(failure.stage));
  std::string reason = std::strerror(failure.error);
  switch (static_cast<SandboxStage>(failure.stage)) {
  case SandboxStage::Unshare:
    return std::string("沙箱初始化失败（命名空间）: ") + reason;
  case SandboxStage::Root:
    return std::string("沙箱初始化失败（最小根目录/挂载）: ") + reason;
  case SandboxStage::Chroot:
    return std::string("沙箱初始化失败（chroot）: ") + reason;
  case SandboxStage::Limits:
    return std::string("沙箱资源限制设置失败（setrlimit）: ") + reason;
  case SandboxStage::Seccomp:
    return std::string("沙箱系统调用过滤安装失败（seccomp）: ") + reason;
  case SandboxStage::Chdir:
    return std::string("切换工作目录失败: ") + reason;
  case SandboxStage::Exec:
    return std::string("启动进程失败: ") + reason;
  default:
    return std::string("进程启动失败（") + stage_name + "）: " + reason;
  }
}

bool write_small_file(const std::string &path, const std::string &content) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  ssize_t n = ::write(fd, content.data(), content.size());
  ::close(fd);
  return n == static_cast<ssize_t>(content.size());
}

// 在父进程（拥有映射权限）中为子进程写入 uid/gid 映射与新用户命名空间。
bool write_user_maps(pid_t pid) {
  const std::string base = "/proc/" + std::to_string(pid);
  write_small_file(base + "/setgroups", "deny");
  const std::string uid_map =
      "0 " + std::to_string(::getuid()) + " 1";
  const std::string gid_map =
      "0 " + std::to_string(::getgid()) + " 1";
  if (!write_small_file(base + "/uid_map", uid_map)) {
    return false;
  }
  if (!write_small_file(base + "/gid_map", gid_map)) {
    return false;
  }
  return true;
}

// 读取单个进程的 RSS（kB）。失败返回 -1。
long long read_pid_rss_kb(pid_t pid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/status", pid);
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  char buffer[4096];
  ssize_t n = ::read(fd, buffer, sizeof(buffer) - 1);
  ::close(fd);
  if (n <= 0) {
    return -1;
  }
  buffer[n] = '\0';
  const char *key = std::strstr(buffer, "VmRSS:");
  if (key == nullptr) {
    return -1;
  }
  key += std::strlen("VmRSS:");
  while (*key == ' ' || *key == '\t') {
    ++key;
  }
  char *end = nullptr;
  long long kb = std::strtoll(key, &end, 10);
  if (end == key) {
    return -1;
  }
  return kb;
}

// 汇总某个进程组（pgid）内所有进程的 RSS（kB）。用于编译阶段（编译器会派生
// cc1plus/as/ld）。运行阶段禁止创建进程，单进程 RSS 即可。
long long read_group_rss_kb(pid_t pgid) {
  DIR *dir = ::opendir("/proc");
  if (dir == nullptr) {
    return -1;
  }
  const long page = ::sysconf(_SC_PAGESIZE);
  long long total_kb = 0;
  struct dirent *entry;
  while ((entry = ::readdir(dir)) != nullptr) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
      continue;
    }
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%s/stat", entry->d_name);
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    char buffer[4096];
    ssize_t n = ::read(fd, buffer, sizeof(buffer) - 1);
    ::close(fd);
    if (n <= 0) {
      continue;
    }
    buffer[n] = '\0';
    char *rparen = std::strrchr(buffer, ')');
    if (rparen == nullptr || rparen[1] != ' ') {
      continue;
    }
    char *p = rparen + 2;
    long long group = 0;
    long long rss_pages = 0;
    int token = 0;
    char *save = nullptr;
    for (char *t = strtok_r(p, " ", &save); t != nullptr;
         t = strtok_r(nullptr, " ", &save)) {
      ++token;
      if (token == 3) {
        group = std::strtoll(t, nullptr, 10);
      } else if (token == 22) {
        rss_pages = std::strtoll(t, nullptr, 10);
        break;
      }
    }
    if (group == pgid && rss_pages > 0) {
      total_kb += rss_pages * (page / 1024);
    }
  }
  ::closedir(dir);
  return total_kb;
}

// 沙箱根 tmpfs 容量：编译阶段需要较多临时空间（汇编/中间文件），运行阶段保持较小
// 以避免占用宿主机内存。均为上限而非预分配（tmpfs 按实际写入计费）。
unsigned long long sandbox_root_tmpfs_mb(bool compile_phase) {
  return compile_phase ? 256ULL : 64ULL;
}

} // namespace

ProcessResult LocalExecutor::compile(const CompileRequest &request) {
  std::vector<std::string> argv;
  argv.push_back(request.compiler);
  if (request.language == Language::Cpp17) {
    argv.push_back("-O2");
    argv.push_back("-std=c++17");
  } else {
    argv.push_back("-O2");
    argv.push_back("-std=c11");
  }
  argv.push_back(request.source_path);
  argv.push_back("-o");
  argv.push_back(request.output_path);
  argv.push_back("-lm");
  // SPEC JUDGE-01：C++17 / C11 均全开 AddressSanitizer 与 UndefinedBehaviorSanitizer。
  // -fno-sanitize-recover=all 使 UBSan 一旦报告未定义行为即中止（而非打印诊断后继续
  // 执行），从而不会出现「UB 已报错但输出仍匹配而被判 AC」的情况；ASan 本身即不可
  // 恢复。确切的选项由执行器集中维护，不接受用户源码或请求参数拼接。
  if (request.sanitizers) {
    argv.push_back("-fsanitize=address,undefined");
    argv.push_back("-fno-omit-frame-pointer");
    argv.push_back("-fno-sanitize-recover=all");
  }
  for (const std::string &flag : request.extra_flags) {
    argv.push_back(flag);
  }

  return spawn(argv, request.working_directory, /*input=*/"",
               request.time_limit_ms, request.output_limit_bytes,
               request.output_limit_bytes, /*merge_stderr=*/true,
               request.cancel, request.sandbox, SandboxPhase::Compile,
               request.memory_limit_kb);
}

ProcessResult LocalExecutor::run(const RunRequest &request,
                                 const std::string &input) {
  std::vector<std::string> argv;
  argv.push_back(request.executable_path);
  return spawn(argv, request.working_directory, input, request.time_limit_ms,
               request.stdout_limit_bytes, request.stderr_limit_bytes,
               /*merge_stderr=*/false, request.cancel, request.sandbox,
               SandboxPhase::Run, request.memory_limit_kb);
}

ProcessResult LocalExecutor::spawn(const std::vector<std::string> &argv,
                                   const std::string &working_directory,
                                   const std::string &input, int time_limit_ms,
                                   std::size_t stdout_limit,
                                   std::size_t stderr_limit,
                                   bool merge_stderr,
                                   const CancellationToken *cancel,
                                   bool sandbox, SandboxPhase phase,
                                   long long memory_limit_kb) {
  ignore_sigpipe_once();

  ProcessResult result;
  if (argv.empty() || argv[0].empty()) {
    result.launch_error = true;
    result.launch_error_message = "空的可执行文件路径";
    return result;
  }
  if (cancel != nullptr && cancel->cancelled()) {
    result.cancelled = true;
    result.termination = TerminationReason::Cancelled;
    result.launch_error_message = "任务已取消";
    return result;
  }

  // 解析可执行文件路径（父进程完成 PATH 搜索，子进程不做内存分配）。
  std::string program = argv[0];
  if (program.find('/') == std::string::npos) {
    std::string resolved;
    if (!resolve_in_path(program, resolved)) {
      result.launch_error = true;
      result.launch_error_message =
          "启动进程失败: 未在 PATH 中找到可执行文件 " + program;
      return result;
    }
    program = std::move(resolved);
  }
  if (::access(program.c_str(), X_OK) != 0) {
    result.launch_error = true;
    result.launch_error_message =
        std::string("启动进程失败: 无法执行 ") + program + " (" +
        std::strerror(errno) + ")";
    return result;
  }

  const bool compile_phase = (phase == SandboxPhase::Compile);

  // 预构建 seccomp 程序与资源限制，避免子进程内部分配。
  SeccompProgram filter;
  SandboxLimits limits;
  if (sandbox) {
    std::string filter_error;
    if (!build_seccomp_program(phase, filter, filter_error)) {
      result.launch_error = true;
      result.sandbox_error = true;
      result.launch_error_message = "构建 seccomp 过滤失败: " + filter_error;
      return result;
    }
    limits = compute_limits(phase, memory_limit_kb, time_limit_ms);
  }

  // 沙箱路径参数（在 fork 前准备，子进程仅读取）。
  const std::string root_dir = working_directory + "/.oj_sandbox";
  std::string extra_bind_src;
  std::string extra_bind_dst;
  std::string inside_program = program;
  if (sandbox) {
    const std::string box_prefix = working_directory + "/";
    if (starts_with(program, box_prefix)) {
      inside_program = std::string(kSandboxBoxPath) + "/" +
                       program.substr(working_directory.size() + 1);
    } else if (is_system_prefix(program)) {
      inside_program = program;
    } else {
      extra_bind_src = dir_name(program);
      extra_bind_dst = kSandboxToolsPath;
      inside_program =
          std::string(kSandboxToolsPath) + "/" + base_name(program);
    }
  }

  // 运行阶段的载荷若位于工作目录内（/box 映射），sandbox_enter_root 会把它复制到
  // 由沙箱自身拥有、只读的 /box tmpfs；路径在父进程算好，子进程不再分配内存。
  const char *run_copy_src =
      (sandbox && !compile_phase &&
       starts_with(inside_program, std::string(kSandboxBoxPath) + "/"))
          ? program.c_str()
          : nullptr;

  // 构造 argv：沙箱内把工作目录下的路径映射到 /box 前缀（源码、编译产物）。
  std::vector<std::string> exec_argv = argv;
  exec_argv[0] = inside_program;
  if (sandbox) {
    const std::string box_prefix = working_directory + "/";
    for (std::size_t i = 1; i < exec_argv.size(); ++i) {
      if (starts_with(exec_argv[i], box_prefix)) {
        exec_argv[i] = std::string(kSandboxBoxPath) + "/" +
                       exec_argv[i].substr(working_directory.size() + 1);
      }
    }
  }
  std::vector<char *> cargv;
  cargv.reserve(exec_argv.size() + 1);
  for (const std::string &arg : exec_argv) {
    cargv.push_back(const_cast<char *>(arg.c_str()));
  }
  cargv.push_back(nullptr);

  // 受控的最小环境：绝不传递服务密钥（OJ_JWT_SECRET/OJ_ADMIN_PASSWORD 等）。
  std::vector<std::string> env_strings = {
      "PATH=/usr/bin:/bin", "HOME=/nonexistent", "TMPDIR=/tmp",
      "LANG=C", "LC_ALL=C"};
  if (phase == SandboxPhase::Run) {
    // LeakSanitizer 需要 ptrace/clone，与“禁止创建进程 + 禁 ptrace”策略冲突，
    // 故关闭泄漏检测；ASan/UBSan 的错误检测仍完全生效（不影响越界等诊断）。
    // halt_on_error=1 让 UBSan 在运行时错误处立即停止（与编译期
    // -fno-sanitize-recover=all 双保险），避免带着未定义行为继续执行到输出匹配。
    env_strings.push_back("ASAN_OPTIONS=detect_leaks=0");
    env_strings.push_back("UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1");
  }
  std::vector<char *> envp;
  envp.reserve(env_strings.size() + 1);
  for (const std::string &value : env_strings) {
    envp.push_back(const_cast<char *>(value.c_str()));
  }
  envp.push_back(nullptr);

  ScopedFd in_r, in_w, out_r, out_w, err_r, err_w, exec_r, exec_w;
  ScopedFd sync1_r, sync1_w, sync2_r, sync2_w, pid_r, pid_w;
  auto make_pipe = [](ScopedFd &read_end, ScopedFd &write_end) -> bool {
    int p[2];
    if (::pipe2(p, O_CLOEXEC) != 0) {
      return false;
    }
    read_end.reset(p[0]);
    write_end.reset(p[1]);
    return true;
  };
  if (!make_pipe(in_r, in_w) || !make_pipe(out_r, out_w) ||
      !make_pipe(err_r, err_w) || !make_pipe(exec_r, exec_w) ||
      !make_pipe(sync1_r, sync1_w) || !make_pipe(sync2_r, sync2_w) ||
      !make_pipe(pid_r, pid_w)) {
    result.launch_error = true;
    result.launch_error_message =
        std::string("创建管道失败: ") + std::strerror(errno);
    return result;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    result.launch_error = true;
    result.launch_error_message =
        std::string("fork 失败: ") + std::strerror(errno);
    return result;
  }

  if (pid == 0) {
    // 中间子进程 P：仅执行异步信号安全的系统调用。
    ::setpgid(0, 0);
    in_w.reset();
    out_r.reset();
    err_r.reset();
    exec_r.reset();
    sync1_r.reset();
    sync2_w.reset();
    pid_r.reset();

    if (sandbox) {
      const unsigned long flags = CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET |
                                  CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS;
      if (::unshare(flags) != 0) {
        ExecFailure failure{static_cast<int>(SandboxStage::Unshare), errno};
        ssize_t ignored = ::write(exec_w.get(), &failure, sizeof(failure));
        (void)ignored;
        _exit(127);
      }
      char token = 'x';
      if (::write(sync1_w.get(), &token, 1) != 1) {
        _exit(127);
      }
      if (::read(sync2_r.get(), &token, 1) != 1) {
        ExecFailure failure{static_cast<int>(SandboxStage::Unshare), EIO};
        ssize_t ignored = ::write(exec_w.get(), &failure, sizeof(failure));
        (void)ignored;
        _exit(127);
      }
    }

    // 沙箱模式下先派生 init 进程 G（新 PID 命名空间中的 PID 1），再由 P 直接派生
    // 真正的载荷 H（PID 2）。关键原因：P 处于宿主 PID 命名空间，fork 返回宿主 PID，
    // 父进程据此可精确采样用户程序 RSS；而 G 只作为 PID 1 回收 H 退出后可能产生的
    // 孤儿进程。若改由 G（PID 1）再 fork，则只能拿到命名空间内 PID，无法在宿主
    // /proc 中定位用户程序。
    pid_t init_pid = -1;
    if (sandbox) {
      init_pid = ::fork();
      if (init_pid < 0) {
        ExecFailure failure{static_cast<int>(SandboxStage::Root), errno};
        ssize_t ignored = ::write(exec_w.get(), &failure, sizeof(failure));
        (void)ignored;
        _exit(127);
      }
      if (init_pid == 0) {
        // G：命名空间 init（PID 1）。关闭全部继承 fd，循环回收孤儿进程，直到载荷
        // 结束后被 P 终止。绝不执行用户代码。
        in_r.reset();
        in_w.reset();
        out_r.reset();
        out_w.reset();
        err_r.reset();
        err_w.reset();
        exec_r.reset();
        exec_w.reset();
        sync1_r.reset();
        sync1_w.reset();
        sync2_r.reset();
        sync2_w.reset();
        pid_r.reset();
        pid_w.reset();
        for (;;) {
          int status = 0;
          pid_t reaped = ::waitpid(-1, &status, 0);
          if (reaped < 0) {
            if (errno == EINTR) {
              continue;
            }
            ::pause(); // 暂无子进程：等待信号或被终止
          }
        }
      }
    }

    pid_t payload = ::fork();
    if (payload < 0) {
      if (init_pid > 0) {
        ::kill(init_pid, SIGKILL);
        int st = 0;
        waitpid_retry(init_pid, &st, 0);
      }
      ExecFailure failure{static_cast<int>(SandboxStage::Root), errno};
      ssize_t ignored = ::write(exec_w.get(), &failure, sizeof(failure));
      (void)ignored;
      _exit(127);
    }

    if (payload == 0) {
      // H：真正的载荷进程（沙箱模式下为 PID 2，绝非 PID 1，因此默认信号处置正常
      // 生效，信号崩溃可被观测）。
      if (::dup2(in_r.get(), STDIN_FILENO) < 0 ||
          ::dup2(out_w.get(), STDOUT_FILENO) < 0 ||
          ::dup2(merge_stderr ? out_w.get() : err_w.get(), STDERR_FILENO) < 0) {
        ExecFailure failure{static_cast<int>(SandboxStage::Exec), errno};
        ssize_t ignored = ::write(exec_w.get(), &failure, sizeof(failure));
        (void)ignored;
        _exit(127);
      }
      sync1_w.reset();
      sync2_r.reset();
      // 原始管道 fd 已复制到 0/1/2，先关闭，避免 close_extra_fds 的二次关闭。
      in_r.reset();
      out_w.reset();
      err_w.reset();

      if (sandbox) {
        int stage = 0;
        int error = 0;
        if (sandbox_enter_root(working_directory.c_str(), root_dir.c_str(),
                               extra_bind_src.empty() ? nullptr
                                                      : extra_bind_src.c_str(),
                               extra_bind_dst.empty() ? nullptr
                                                      : extra_bind_dst.c_str(),
                               compile_phase, run_copy_src,
                               sandbox_root_tmpfs_mb(compile_phase), stage,
                               error) != 0) {
          ExecFailure failure{stage, error};
          ssize_t ignored = ::write(exec_w.get(), &failure, sizeof(failure));
          (void)ignored;
          _exit(127);
        }
        close_extra_fds(exec_w.get());
        if (sandbox_apply_limits(limits, stage, error) != 0) {
          ExecFailure failure{stage, error};
          ssize_t ignored = ::write(exec_w.get(), &failure, sizeof(failure));
          (void)ignored;
          _exit(127);
        }
        if (sandbox_apply_seccomp(filter, stage, error) != 0) {
          ExecFailure failure{stage, error};
          ssize_t ignored = ::write(exec_w.get(), &failure, sizeof(failure));
          (void)ignored;
          _exit(127);
        }
        ::execve(inside_program.c_str(), cargv.data(), envp.data());
        ExecFailure failure{static_cast<int>(SandboxStage::Exec), errno};
        ssize_t ignored = ::write(exec_w.get(), &failure, sizeof(failure));
        (void)ignored;
        _exit(127);
      }

      close_extra_fds(exec_w.get());
      if (!working_directory.empty() &&
          ::chdir(working_directory.c_str()) != 0) {
        ExecFailure failure{static_cast<int>(SandboxStage::Chdir), errno};
        ssize_t ignored = ::write(exec_w.get(), &failure, sizeof(failure));
        (void)ignored;
        _exit(127);
      }
      ::execve(program.c_str(), cargv.data(), envp.data());
      ExecFailure failure{static_cast<int>(SandboxStage::Exec), errno};
      ssize_t ignored = ::write(exec_w.get(), &failure, sizeof(failure));
      (void)ignored;
      _exit(127);
    }

    // P：回传载荷 H 的宿主 PID（供父进程精确采样用户程序 RSS）。
    std::int32_t payload_value = static_cast<std::int32_t>(payload);
    ssize_t pid_written =
        ::write(pid_w.get(), &payload_value, sizeof(payload_value));
    (void)pid_written;
    pid_w.reset();

    // P：关闭自己的输出/状态写端，避免父进程采集等不到 EOF。
    in_r.reset();
    out_w.reset();
    err_w.reset();
    exec_w.reset();
    sync1_w.reset();
    sync2_r.reset();

    int status = 0;
    pid_t reaped = waitpid_retry(payload, &status, 0);

    // 载荷结束后终止命名空间 init，销毁 PID 命名空间并回收孤儿进程。
    if (init_pid > 0) {
      ::kill(init_pid, SIGKILL);
      int init_status = 0;
      waitpid_retry(init_pid, &init_status, 0);
    }

    if (reaped == payload && WIFSIGNALED(status)) {
      const int signal = WTERMSIG(status);
      ::signal(signal, SIG_DFL);
      ::kill(::getpid(), signal);
      _exit(128 + signal);
    }
    if (reaped == payload && WIFEXITED(status)) {
      _exit(WEXITSTATUS(status));
    }
    _exit(127);
  }

  // 父进程：与子进程的 setpgid 竞态兜底。
  (void)::setpgid(pid, pid);
  in_r.reset();
  out_w.reset();
  err_w.reset();
  exec_w.reset();
  sync1_w.reset();
  sync2_r.reset();
  // 关闭父进程持有的 PID 管道写端，避免子进程未回传时读端永久阻塞。
  pid_w.reset();

  if (sandbox) {
    char token = 0;
    ssize_t n = ::read(sync1_r.get(), &token, 1);
    if (n < 0 && errno == EINTR) {
      n = ::read(sync1_r.get(), &token, 1);
    }
    // 子进程已 unshare 才写入映射；失败时子进程直接退出并回传错误。
    if (n == 1) {
      write_user_maps(pid);
    }
    ssize_t ignored = ::write(sync2_w.get(), "x", 1);
    (void)ignored;
  }
  sync1_r.reset();
  sync2_w.reset();

  // 载荷进程 PID：沙箱模式为 H，非沙箱模式为 G。用于按单进程采样 RSS；未取得
  // （启动失败）时为 -1，内存采样退回观测不到（不会据此误判）。
  pid_t payload_pid = -1;
  {
    std::int32_t value = 0;
    ssize_t n = ::read(pid_r.get(), &value, sizeof(value));
    if (n < 0 && errno == EINTR) {
      n = ::read(pid_r.get(), &value, sizeof(value));
    }
    if (n == static_cast<ssize_t>(sizeof(value)) && value > 0) {
      payload_pid = static_cast<pid_t>(value);
    }
  }
  pid_r.reset();

  // exec 状态：成功时错误管道因 close-on-exec 直接 EOF；失败时回传结构化原因。
  {
    ExecFailure failure{};
    ssize_t n = ::read(exec_r.get(), &failure, sizeof(failure));
    if (n < 0 && errno == EINTR) {
      n = ::read(exec_r.get(), &failure, sizeof(failure));
    }
    exec_r.reset();
    if (n == static_cast<ssize_t>(sizeof(failure))) {
      int status = 0;
      waitpid_retry(pid, &status, 0);
      result.launch_error = true;
      result.launch_error_message = describe_failure(failure);
      const auto stage = static_cast<SandboxStage>(failure.stage);
      result.sandbox_error =
          stage != SandboxStage::Exec && stage != SandboxStage::Chdir;
      return result;
    }
  }

  result.launched = true;

  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::milliseconds(time_limit_ms);

  if (input.empty()) {
    in_w.reset();
  } else {
    set_nonblocking(in_w.get());
  }
  set_nonblocking(out_r.get());
  set_nonblocking(err_r.get());

  bool in_open = in_w.valid();
  bool out_open = out_r.valid();
  bool err_open = err_r.valid();
  std::size_t in_offset = 0;

  bool have_status = false;
  bool child_reaped = false;
  int status = 0;
  bool killed = false;
  bool read_failed = false;
  std::chrono::steady_clock::time_point child_reaped_at;

  const std::size_t kReadChunk = 64 * 1024;
  std::vector<char> read_buffer(kReadChunk);
  // 轮询周期上限与内存采样周期一致（20ms）：即使程序没有输出也能周期性采样 RSS，
  // 并让短命程序也能被观测到内存峰值。
  const int kPollCapMs = 20;
  const long long kMemoryLimitKb = memory_limit_kb;
  auto last_memory_check = start;
  long long peak_memory_kb = 0;

  // 启动后的首次采样：此时载荷已成功 exec（否则不会返回 launched=true），可立即
  // 取得其基线 RSS；否则极短命的程序可能在首次周期采样前就结束，导致内存值为 0。
  if (kMemoryLimitKb > 0) {
    long long initial_rss = -1;
    if (compile_phase) {
      initial_rss = read_group_rss_kb(pid);
    } else if (payload_pid > 0) {
      initial_rss = read_pid_rss_kb(payload_pid);
    }
    if (initial_rss > peak_memory_kb) {
      peak_memory_kb = initial_rss;
    }
  }

  while (true) {
    if (!child_reaped) {
      int st = 0;
      pid_t reaped = waitpid_retry(pid, &st, WNOHANG);
      if (reaped == pid) {
        child_reaped = true;
        have_status = true;
        status = st;
        child_reaped_at = std::chrono::steady_clock::now();
      } else if (reaped < 0 && errno == ECHILD) {
        child_reaped = true;
        child_reaped_at = std::chrono::steady_clock::now();
      }
    }

    if (child_reaped && !out_open && !err_open) {
      break;
    }

    auto now = std::chrono::steady_clock::now();

    if (!killed && cancel != nullptr && cancel->cancelled()) {
      result.cancelled = true;
      killed = true;
      kill_process_group(pid);
    }

    if (!killed && !child_reaped && now >= deadline) {
      result.timed_out = true;
      killed = true;
      kill_process_group(pid);
    }

    // RSS 采样并强制内存上限：内存超限不是“仅采集数值”，而是终止进程组。
    // 20ms 周期兼顾短命程序（正常提交常在数十毫秒内结束）与 /proc 读取开销。
    if (!killed && kMemoryLimitKb > 0 && !child_reaped &&
        now - last_memory_check >= std::chrono::milliseconds(20)) {
      last_memory_check = now;
      long long rss_kb = -1;
      if (compile_phase) {
        rss_kb = read_group_rss_kb(pid);
      } else if (payload_pid > 0) {
        rss_kb = read_pid_rss_kb(payload_pid);
      }
      if (rss_kb > peak_memory_kb) {
        peak_memory_kb = rss_kb;
      }
      if (rss_kb > kMemoryLimitKb) {
        result.memory_exceeded = true;
        killed = true;
        kill_process_group(pid);
      }
    }

    if (child_reaped && (out_open || err_open) &&
        now - child_reaped_at > std::chrono::milliseconds(200)) {
      if (!killed) {
        killed = true;
        kill_process_group(pid);
      }
      out_r.reset();
      err_r.reset();
      out_open = false;
      err_open = false;
      break;
    }

    struct pollfd fds[3];
    int nf = 0;
    int idx_in = -1;
    int idx_out = -1;
    int idx_err = -1;
    if (in_open && !killed && in_offset < input.size()) {
      fds[nf].fd = in_w.get();
      fds[nf].events = POLLOUT;
      fds[nf].revents = 0;
      idx_in = nf++;
    }
    if (out_open) {
      fds[nf].fd = out_r.get();
      fds[nf].events = POLLIN;
      fds[nf].revents = 0;
      idx_out = nf++;
    }
    if (err_open) {
      fds[nf].fd = err_r.get();
      fds[nf].events = POLLIN;
      fds[nf].revents = 0;
      idx_err = nf++;
    }

    int poll_timeout = kPollCapMs;
    if (!killed) {
      long long remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - now)
                                .count();
      if (remaining < 0) {
        remaining = 0;
      }
      poll_timeout =
          static_cast<int>(std::min<long long>(remaining, kPollCapMs));
    }

    int rc = (nf == 0) ? ::poll(nullptr, 0, poll_timeout)
                       : ::poll(fds, static_cast<nfds_t>(nf), poll_timeout);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      read_failed = true;
      break;
    }
    if (rc == 0) {
      continue;
    }

    if (idx_out >= 0 &&
        (fds[idx_out].revents & (POLLIN | POLLHUP | POLLERR))) {
      while (true) {
        ssize_t n =
            ::read(out_r.get(), read_buffer.data(), read_buffer.size());
        if (n > 0) {
          append_bounded(result.stdout_data, read_buffer.data(),
                         static_cast<std::size_t>(n), stdout_limit,
                         result.stdout_truncated);
          continue;
        }
        if (n == 0) {
          out_r.reset();
          out_open = false;
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        out_r.reset();
        out_open = false;
        break;
      }
    }

    if (idx_err >= 0 &&
        (fds[idx_err].revents & (POLLIN | POLLHUP | POLLERR))) {
      while (true) {
        ssize_t n =
            ::read(err_r.get(), read_buffer.data(), read_buffer.size());
        if (n > 0) {
          append_bounded(result.stderr_data, read_buffer.data(),
                         static_cast<std::size_t>(n), stderr_limit,
                         result.stderr_truncated);
          continue;
        }
        if (n == 0) {
          err_r.reset();
          err_open = false;
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        err_r.reset();
        err_open = false;
        break;
      }
    }

    if (idx_in >= 0 &&
        (fds[idx_in].revents & (POLLOUT | POLLERR | POLLHUP))) {
      if (fds[idx_in].revents & (POLLERR | POLLHUP)) {
        in_w.reset();
        in_open = false;
      } else {
        ssize_t n = ::write(in_w.get(), input.data() + in_offset,
                            input.size() - in_offset);
        if (n > 0) {
          in_offset += static_cast<std::size_t>(n);
          if (in_offset >= input.size()) {
            in_w.reset();
            in_open = false;
          }
        } else if (n < 0 && errno != EINTR && errno != EAGAIN &&
                   errno != EWOULDBLOCK) {
          in_w.reset();
          in_open = false;
        }
      }
    }
  }

  if (read_failed && !child_reaped) {
    kill_process_group(pid);
  }

  if (!child_reaped) {
    int st = 0;
    if (waitpid_retry(pid, &st, 0) == pid) {
      child_reaped = true;
      have_status = true;
      status = st;
    }
  }

  in_w.reset();
  out_r.reset();
  err_r.reset();

  result.exited = have_status && WIFEXITED(status);
  if (result.exited) {
    result.exit_code = WEXITSTATUS(status);
  }
  if (have_status && WIFSIGNALED(status)) {
    result.term_signal = WTERMSIG(status);
  }
  result.time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

  // 结构化终止原因：由执行层集中记录，分类层不再各自组合布尔量。主动终止
  // （取消/超时/内存）优先于退出状态；仅当诊断确实异常时才标注 Sanitizer。
  if (result.cancelled) {
    result.termination = TerminationReason::Cancelled;
  } else if (result.timed_out) {
    result.termination = TerminationReason::TimedOut;
  } else if (result.memory_exceeded) {
    result.termination = TerminationReason::MemoryExceeded;
  } else if (result.exited && result.exit_code == 0) {
    result.termination = TerminationReason::Completed;
  } else if (result.exited) {
    result.termination = TerminationReason::NonZeroExit;
  } else if (result.term_signal != 0) {
    result.termination = TerminationReason::Signaled;
  } else {
    result.termination = TerminationReason::LaunchFailure;
  }
  if (result.termination != TerminationReason::Completed &&
      looks_like_sanitizer_report(result.stderr_data)) {
    result.sanitizer_error = true;
  }

  // 进程已被回收，最后一次采样峰值内存，避免错过短命程序的内存峰值。
  if (kMemoryLimitKb > 0) {
    long long rss_kb = -1;
    if (compile_phase) {
      rss_kb = read_group_rss_kb(pid);
    } else if (payload_pid > 0) {
      rss_kb = read_pid_rss_kb(payload_pid);
    }
    if (rss_kb > peak_memory_kb) {
      peak_memory_kb = rss_kb;
    }
  }
  result.memory_kb = peak_memory_kb > 0 ? peak_memory_kb : 0;

  return result;
}

bool LocalExecutor::sandbox_self_test(const std::string &workspace_root,
                                      std::string &error) {
  std::string workspace_error;
  std::unique_ptr<Workspace> workspace =
      Workspace::create(workspace_root, workspace_error);
  if (!workspace) {
    error = "无法创建沙箱自检工作目录: " + workspace_error;
    return false;
  }

  LocalExecutor executor;
  RunRequest request;
  // /bin/true 为系统动态链接程序：可同时验证最小根目录中的动态加载器/库、
  // setrlimit 与 seccomp 安装。
  request.executable_path = "/bin/true";
  request.working_directory = workspace->path();
  request.time_limit_ms = 10000;
  request.sandbox = true;
  request.memory_limit_kb = 262144;

  ProcessResult result = executor.run(request, "");
  if (result.launch_error) {
    error = "沙箱自检失败: " + result.launch_error_message;
    return false;
  }
  if (result.timed_out) {
    error = "沙箱自检超时";
    return false;
  }
  if (!result.exited || result.exit_code != 0) {
    error = "沙箱自检运行异常（exit=" + std::to_string(result.exit_code) +
            "，signal=" + std::to_string(result.term_signal) + "）";
    return false;
  }
  return true;
}

} // namespace judge
} // namespace oj
