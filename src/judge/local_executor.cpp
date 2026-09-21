#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "judge/local_executor.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace oj {
namespace judge {

namespace {

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

// 追加数据但有长度上限；超过上限只标记截断，不再增长缓冲。
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

} // namespace

ProcessResult LocalExecutor::compile(const CompileRequest &request) {
  std::vector<std::string> argv;
  argv.push_back(request.compiler);
  if (request.language == Language::Cpp17) {
    // SPEC JUDGE-01 规定的 C++17 选项。M1.5 尚未接入 ASan/UBSan（M3.4）。
    argv.push_back("-O2");
    argv.push_back("-std=c++17");
  } else {
    // SPEC JUDGE-01 规定的 C11 选项。M1.5 尚未接入 ASan/UBSan（M3.4）。
    argv.push_back("-O2");
    argv.push_back("-std=c11");
  }
  argv.push_back(request.source_path);
  argv.push_back("-o");
  argv.push_back(request.output_path);
  argv.push_back("-lm");
  for (const std::string &flag : request.extra_flags) {
    argv.push_back(flag);
  }

  // 编译诊断统一走标准错误，合并到同一缓冲以保留顺序。
  return spawn(argv, request.working_directory, /*input=*/"",
               request.time_limit_ms, request.output_limit_bytes,
               request.output_limit_bytes, /*merge_stderr=*/true);
}

ProcessResult LocalExecutor::run(const RunRequest &request,
                                 const std::string &input) {
  std::vector<std::string> argv;
  argv.push_back(request.executable_path);
  return spawn(argv, request.working_directory, input, request.time_limit_ms,
               request.stdout_limit_bytes, request.stderr_limit_bytes,
               /*merge_stderr=*/false);
}

ProcessResult LocalExecutor::spawn(const std::vector<std::string> &argv,
                                   const std::string &working_directory,
                                   const std::string &input, int time_limit_ms,
                                   std::size_t stdout_limit,
                                   std::size_t stderr_limit,
                                   bool merge_stderr) {
  ignore_sigpipe_once();

  ProcessResult result;

  if (argv.empty() || argv[0].empty()) {
    result.launch_error = true;
    result.launch_error_message = "空的可执行文件路径";
    return result;
  }

  ScopedFd in_r, in_w, out_r, out_w, err_r, err_w, exec_r, exec_w;
  {
    int p[2];
    if (::pipe2(p, O_CLOEXEC) != 0) {
      result.launch_error = true;
      result.launch_error_message =
          std::string("创建标准输入管道失败: ") + std::strerror(errno);
      return result;
    }
    in_r.reset(p[0]);
    in_w.reset(p[1]);
  }
  {
    int p[2];
    if (::pipe2(p, O_CLOEXEC) != 0) {
      result.launch_error = true;
      result.launch_error_message =
          std::string("创建标准输出管道失败: ") + std::strerror(errno);
      return result;
    }
    out_r.reset(p[0]);
    out_w.reset(p[1]);
  }
  {
    int p[2];
    if (::pipe2(p, O_CLOEXEC) != 0) {
      result.launch_error = true;
      result.launch_error_message =
          std::string("创建标准错误管道失败: ") + std::strerror(errno);
      return result;
    }
    err_r.reset(p[0]);
    err_w.reset(p[1]);
  }
  {
    // close-on-exec 的错误管道：子进程 exec 成功后自动关闭，父进程读到 EOF；
    // exec 失败时子进程写入 errno，父进程据此区分「启动失败」与「正常退出」。
    int p[2];
    if (::pipe2(p, O_CLOEXEC) != 0) {
      result.launch_error = true;
      result.launch_error_message =
          std::string("创建执行状态管道失败: ") + std::strerror(errno);
      return result;
    }
    exec_r.reset(p[0]);
    exec_w.reset(p[1]);
  }

  // 在 fork 之前构造 argv 数组，子进程内不再进行内存分配。
  std::vector<char *> cargv;
  cargv.reserve(argv.size() + 1);
  for (const std::string &arg : argv) {
    cargv.push_back(const_cast<char *>(arg.c_str()));
  }
  cargv.push_back(nullptr);

  pid_t pid = ::fork();
  if (pid < 0) {
    result.launch_error = true;
    result.launch_error_message =
        std::string("fork 失败: ") + std::strerror(errno);
    return result;
  }

  if (pid == 0) {
    // 子进程：仅调用异步信号安全的系统调用，失败时通过错误管道回传 errno。
    ::setpgid(0, 0);
    if (::dup2(in_r.get(), STDIN_FILENO) < 0 ||
        ::dup2(out_w.get(), STDOUT_FILENO) < 0 ||
        ::dup2(merge_stderr ? out_w.get() : err_w.get(), STDERR_FILENO) < 0) {
      int err = errno;
      ssize_t ignored = ::write(exec_w.get(), &err, sizeof(err));
      (void)ignored;
      _exit(127);
    }
    if (!working_directory.empty() &&
        ::chdir(working_directory.c_str()) != 0) {
      int err = errno;
      ssize_t ignored = ::write(exec_w.get(), &err, sizeof(err));
      (void)ignored;
      _exit(127);
    }
    ::execvp(cargv[0], cargv.data());
    int err = errno;
    ssize_t ignored = ::write(exec_w.get(), &err, sizeof(err));
    (void)ignored;
    _exit(127);
  }

  // 父进程：关闭子进程侧端口。
  in_r.reset();
  out_w.reset();
  err_w.reset();

  // 父进程必须先关闭自己的写端，否则即使子进程 exec 成功后关闭了写端，
  // 管道仍因父进程持有写端而不会 EOF，read 将永久阻塞。
  exec_w.reset();

  // 若 exec 失败，子进程会写入 errno；成功时错误管道因 close-on-exec 直接 EOF。
  {
    int child_errno = 0;
    ssize_t n = ::read(exec_r.get(), &child_errno, sizeof(child_errno));
    exec_r.reset();
    if (n == static_cast<ssize_t>(sizeof(child_errno))) {
      int status = 0;
      ::waitpid(pid, &status, 0); // 回收未成功启动的进程
      result.launch_error = true;
      result.launch_error_message =
          std::string("启动进程失败: ") + std::strerror(child_errno);
      return result;
    }
  }

  result.launched = true;

  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::milliseconds(time_limit_ms);

  if (input.empty()) {
    in_w.reset(); // 立即给出 EOF，程序读取标准输入时不会阻塞
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
  const int kPollCapMs = 50; // 保证超时精度

  while (true) {
    if (!child_reaped) {
      int st = 0;
      pid_t reaped = ::waitpid(pid, &st, WNOHANG);
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
    if (!killed && !child_reaped && now >= deadline) {
      result.timed_out = true;
      killed = true;
      ::kill(pid, SIGKILL);
    }

    // 子进程已被回收但仍持有读端（如孙进程）：短暂排空后强制关闭，避免挂死。
    if (child_reaped && (out_open || err_open) &&
        now - child_reaped_at > std::chrono::milliseconds(200)) {
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
      poll_timeout = static_cast<int>(
          std::min<long long>(remaining, kPollCapMs));
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
        // 读端已关闭（程序提前退出/未读取输入），停止写入即可。
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
          // EPIPE 等：程序不再读取输入。
          in_w.reset();
          in_open = false;
        }
      }
    }
  }

  if (read_failed && !child_reaped) {
    ::kill(pid, SIGKILL);
  }

  if (!child_reaped) {
    int st = 0;
    if (::waitpid(pid, &st, 0) == pid) {
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

  return result;
}

} // namespace judge
} // namespace oj
