#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "judge/sandbox.h"

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace oj {
namespace judge {

namespace {

struct SeccompRule {
  int nr;
  std::uint32_t action;
};

void add_rule(std::vector<SeccompRule> &rules, int nr, std::uint32_t action) {
  rules.push_back({nr, action});
}

// 读取一个整型 sysctl（不存在或不可读返回 false，不视为错误）。
bool read_sysctl_long(const char *path, long &out) {
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  char buffer[64];
  ssize_t n = ::read(fd, buffer, sizeof(buffer) - 1);
  ::close(fd);
  if (n <= 0) {
    return false;
  }
  buffer[n] = '\0';
  char *end = nullptr;
  long value = std::strtol(buffer, &end, 10);
  if (end == buffer) {
    return false;
  }
  out = value;
  return true;
}

// 解码 /proc/self/mounts 中的八进制转义（\040 空格、\011 制表、\134 反斜杠）。
std::string decode_mount_field(const std::string &field) {
  std::string out;
  out.reserve(field.size());
  for (std::size_t i = 0; i < field.size(); ++i) {
    if (field[i] == '\\' && i + 3 < field.size() && field[i + 1] >= '0' &&
        field[i + 1] <= '7') {
      int value = 0;
      for (int k = 1; k <= 3; ++k) {
        value = value * 8 + (field[i + k] - '0');
      }
      out.push_back(static_cast<char>(value));
      i += 3;
    } else {
      out.push_back(field[i]);
    }
  }
  return out;
}

// 找到覆盖 path 的挂载点（最长前缀）。返回是否找到；找到时写出 fstype 与 mountpoint。
bool find_mount_for(const std::string &path, std::string &fstype,
                    std::string &mountpoint) {
  std::ifstream mounts("/proc/self/mounts");
  if (!mounts) {
    return false;
  }
  std::string line;
  bool found = false;
  std::size_t best_len = 0;
  while (std::getline(mounts, line)) {
    std::istringstream stream(line);
    std::string dev, mnt, type;
    if (!(stream >> dev >> mnt >> type)) {
      continue;
    }
    std::string decoded = decode_mount_field(mnt);
    bool match;
    if (decoded == "/") {
      match = true;
    } else {
      match = path.compare(0, decoded.size(), decoded) == 0 &&
              (path.size() == decoded.size() || path[decoded.size()] == '/');
    }
    if (match && decoded.size() >= best_len) {
      best_len = decoded.size();
      fstype = type;
      mountpoint = decoded;
      found = true;
    }
  }
  return found;
}

// 计算最近存在的祖先后再做规范化，保证对尚不存在的目标目录也能判断挂载类型。
std::string nearest_existing_path(const std::string &path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path p = fs::weakly_canonical(path, ec);
  if (!ec) {
    return p.string();
  }
  fs::path current = path;
  while (!current.empty() && !fs::exists(current, ec)) {
    fs::path parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }
  fs::path canonical = fs::weakly_canonical(current, ec);
  return ec ? current.string() : canonical.string();
}

} // namespace

const char *sandbox_stage_name(SandboxStage stage) {
  switch (stage) {
  case SandboxStage::None:
    return "none";
  case SandboxStage::Unshare:
    return "namespace";
  case SandboxStage::Root:
    return "mount";
  case SandboxStage::Chroot:
    return "chroot";
  case SandboxStage::Limits:
    return "setrlimit";
  case SandboxStage::Seccomp:
    return "seccomp";
  case SandboxStage::Chdir:
    return "chdir";
  case SandboxStage::Exec:
    return "exec";
  }
  return "unknown";
}

SandboxLimits compute_limits(SandboxPhase phase, long long memory_limit_kb,
                             int time_limit_ms) {
  SandboxLimits limits;
  if (memory_limit_kb > 0) {
    limits.memory_limit_kb = memory_limit_kb;
  }

  int seconds = (time_limit_ms > 0) ? (time_limit_ms + 999) / 1000 : 1;
  if (seconds < 1) {
    seconds = 1;
  }

  if (phase == SandboxPhase::Run) {
    // CPU 为墙钟看门狗的兜底：即使调度异常也不允许无限 CPU 消耗。
    limits.cpu_limit_sec = seconds + 1;
    limits.fsize_limit_bytes = 64LL << 20;
    limits.stack_limit_bytes = 64LL << 20;
    limits.nofile_limit = 64;
  } else {
    // 编译阶段允许更大的 CPU/文件/栈预算，避免正常编译被误杀。
    limits.cpu_limit_sec = seconds + 5;
    limits.fsize_limit_bytes = 512LL << 20;
    limits.stack_limit_bytes = 256LL << 20;
    limits.nofile_limit = 256;
  }
  limits.core_limit_bytes = 0;
  return limits;
}

bool build_seccomp_program(SandboxPhase phase, SeccompProgram &out,
                           std::string &error) {
  std::vector<SeccompRule> rules;
  const std::uint32_t deny = SECCOMP_RET_ERRNO | EPERM;

  // 网络相关系统调用：即使网络命名空间已隔离，仍在系统调用层拒绝，覆盖
  // socket 家族与可绕过网络/IO 的新接口（io_uring）。
  add_rule(rules, __NR_socket, deny);
  add_rule(rules, __NR_socketpair, deny);
  add_rule(rules, __NR_bind, deny);
  add_rule(rules, __NR_connect, deny);
  add_rule(rules, __NR_listen, deny);
  add_rule(rules, __NR_accept, deny);
  add_rule(rules, __NR_accept4, deny);
  add_rule(rules, __NR_sendto, deny);
  add_rule(rules, __NR_recvfrom, deny);
  add_rule(rules, __NR_sendmsg, deny);
  add_rule(rules, __NR_recvmsg, deny);
#ifdef __NR_sendmmsg
  add_rule(rules, __NR_sendmmsg, deny);
#endif
#ifdef __NR_recvmmsg
  add_rule(rules, __NR_recvmmsg, deny);
#endif
  add_rule(rules, __NR_getsockname, deny);
  add_rule(rules, __NR_getpeername, deny);
  add_rule(rules, __NR_setsockopt, deny);
  add_rule(rules, __NR_getsockopt, deny);
  add_rule(rules, __NR_shutdown, deny);
#ifdef __NR_io_uring_setup
  add_rule(rules, __NR_io_uring_setup, deny);
  add_rule(rules, __NR_io_uring_enter, deny);
  add_rule(rules, __NR_io_uring_register, deny);
#endif

  // 逃逸、命名空间与挂载：用户程序一律不得改变隔离边界。
  add_rule(rules, __NR_mount, deny);
  add_rule(rules, __NR_umount2, deny);
#ifdef __NR_pivot_root
  add_rule(rules, __NR_pivot_root, deny);
#endif
  add_rule(rules, __NR_chroot, deny);
  add_rule(rules, __NR_unshare, deny);
#ifdef __NR_setns
  add_rule(rules, __NR_setns, deny);
#endif
#ifdef __NR_open_tree
  add_rule(rules, __NR_open_tree, deny);
  add_rule(rules, __NR_move_mount, deny);
  add_rule(rules, __NR_fsopen, deny);
  add_rule(rules, __NR_fsconfig, deny);
  add_rule(rules, __NR_fsmount, deny);
  add_rule(rules, __NR_fspick, deny);
  add_rule(rules, __NR_mount_setattr, deny);
#endif
  add_rule(rules, __NR_open_by_handle_at, deny);
  add_rule(rules, __NR_name_to_handle_at, deny);

  // 进程干扰 / 调试 / 内核接口。
  add_rule(rules, __NR_ptrace, deny);
  add_rule(rules, __NR_process_vm_readv, deny);
  add_rule(rules, __NR_process_vm_writev, deny);
#ifdef __NR_kcmp
  add_rule(rules, __NR_kcmp, deny);
#endif
#ifdef __NR_bpf
  add_rule(rules, __NR_bpf, deny);
#endif
  add_rule(rules, __NR_perf_event_open, deny);
#ifdef __NR_userfaultfd
  add_rule(rules, __NR_userfaultfd, deny);
#endif
  add_rule(rules, __NR_keyctl, deny);
  add_rule(rules, __NR_add_key, deny);
  add_rule(rules, __NR_request_key, deny);
  add_rule(rules, __NR_init_module, deny);
  add_rule(rules, __NR_delete_module, deny);
#ifdef __NR_finit_module
  add_rule(rules, __NR_finit_module, deny);
#endif
#ifdef __NR_kexec_load
  add_rule(rules, __NR_kexec_load, deny);
#endif
#ifdef __NR_kexec_file_load
  add_rule(rules, __NR_kexec_file_load, deny);
#endif
  add_rule(rules, __NR_reboot, deny);
  add_rule(rules, __NR_swapon, deny);
  add_rule(rules, __NR_swapoff, deny);
  add_rule(rules, __NR_acct, deny);
  add_rule(rules, __NR_settimeofday, deny);
  add_rule(rules, __NR_clock_settime, deny);
  add_rule(rules, __NR_adjtimex, deny);
#ifdef __NR_clock_adjtime
  add_rule(rules, __NR_clock_adjtime, deny);
#endif
  add_rule(rules, __NR_sethostname, deny);
  add_rule(rules, __NR_setdomainname, deny);
#ifdef __NR_iopl
  add_rule(rules, __NR_iopl, deny);
  add_rule(rules, __NR_ioperm, deny);
#endif
#ifdef __NR_modify_ldt
  add_rule(rules, __NR_modify_ldt, deny);
#endif
  add_rule(rules, __NR_lookup_dcookie, deny);
#ifdef __NR_quotactl
  add_rule(rules, __NR_quotactl, deny);
#endif
#ifdef __NR_sysfs
  add_rule(rules, __NR_sysfs, deny);
#endif
  add_rule(rules, __NR_uselib, deny);
#ifdef __NR_pidfd_open
  add_rule(rules, __NR_pidfd_open, deny);
#endif
#ifdef __NR_pidfd_getfd
  add_rule(rules, __NR_pidfd_getfd, deny);
#endif
#ifdef __NR_pidfd_send_signal
  add_rule(rules, __NR_pidfd_send_signal, deny);
#endif
#ifdef __NR_fanotify_init
  add_rule(rules, __NR_fanotify_init, deny);
  add_rule(rules, __NR_fanotify_mark, deny);
#endif
#ifdef __NR_memfd_secret
  add_rule(rules, __NR_memfd_secret, deny);
#endif
  add_rule(rules, __NR_seccomp, deny);

  // 运行阶段：禁止创建任何进程/线程，防止 fork 轰炸与进程逃逸。
  // 编译阶段必须允许编译器派生 cc1plus/as/ld，故不加入本组。
  if (phase == SandboxPhase::Run) {
    add_rule(rules, __NR_fork, deny);
    add_rule(rules, __NR_vfork, deny);
    add_rule(rules, __NR_clone, deny);
#ifdef __NR_clone3
    add_rule(rules, __NR_clone3, deny);
#endif
  }

  // 布局：0 载入 arch；1 校验；2 非 x86_64 直接杀；3 载入 nr；
  //       4 x32 ABI 拒绝；5.. 规则；allow；deny。
  const std::size_t n = rules.size();
  const std::size_t total = 7 + n;
  const std::size_t deny_index = 6 + n;
  std::vector<sock_filter> insns;
  insns.reserve(total);
  insns.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                           offsetof(struct seccomp_data, arch)));
  insns.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0));
  insns.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
  insns.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                           offsetof(struct seccomp_data, nr)));
  // x32 ABI 系统调用号带 0x40000000 位，一律拒绝，避免绕过精确号码匹配。
  insns.push_back(BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x40000000u,
                           static_cast<std::uint8_t>(deny_index - 4 - 1), 0));
  for (std::size_t k = 0; k < n; ++k) {
    const std::size_t at = 5 + k;
    const std::size_t off = deny_index - at - 1;
    if (off > 255) {
      error = "seccomp 过滤器过大（跳转越界）";
      return false;
    }
    insns.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                             static_cast<std::uint32_t>(rules[k].nr),
                             static_cast<std::uint8_t>(off), 0));
  }
  insns.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
  insns.push_back(BPF_STMT(BPF_RET | BPF_K, deny));

  if (insns.size() > 4096) {
    error = "seccomp 过滤器指令数超过内核上限";
    return false;
  }
  out.instructions = std::move(insns);
  return true;
}

bool sandbox_supported(std::string &error) {
  long value = 0;
  if (read_sysctl_long("/proc/sys/kernel/unprivileged_userns_clone", value) &&
      value == 0) {
    error = "内核禁用了非特权用户命名空间"
            "（/proc/sys/kernel/unprivileged_userns_clone=0）";
    return false;
  }
  if (read_sysctl_long("/proc/sys/kernel/apparmor_restrict_unprivileged_userns",
                       value) &&
      value != 0) {
    error = "AppArmor 限制了非特权用户命名空间"
            "（kernel.apparmor_restrict_unprivileged_userns=1）";
    return false;
  }
  if (read_sysctl_long("/proc/sys/user/max_user_namespaces", value) &&
      value == 0) {
    error = "系统未分配用户命名空间（/proc/sys/user/max_user_namespaces=0）";
    return false;
  }

  // 真正尝试一次 unshare，验证当前内核/权限确实允许。
  pid_t pid = ::fork();
  if (pid < 0) {
    error = std::string("fork 探测失败: ") + std::strerror(errno);
    return false;
  }
  if (pid == 0) {
    if (::unshare(CLONE_NEWUSER) != 0) {
      _exit(1);
    }
    _exit(0);
  }
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    error = std::string("waitpid 探测失败: ") + std::strerror(errno);
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    error = "内核不允许非特权用户命名空间（unshare(CLONE_NEWUSER) 失败）";
    return false;
  }
  return true;
}

bool path_is_tmpfs(const std::string &path, std::string &error) {
  const std::string target = nearest_existing_path(path);
  std::string fstype;
  std::string mountpoint;
  if (!find_mount_for(target, fstype, mountpoint)) {
    error = "无法确定路径所在挂载点: " + target;
    return false;
  }
  if (fstype != "tmpfs") {
    error = "判题工作目录 " + target + " 位于挂载点 " + mountpoint + "（文件系统 " +
            fstype + "），不是 tmpfs。请按 dependence.md 挂载 tmpfs（默认 "
            "/opt/oj-tmpfs），或显式设置 OJ_JUDGE_ALLOW_NON_TMPFS=1 进行开发验证。";
    return false;
  }
  return true;
}

long long mount_capacity_bytes(const std::string &path) {
  const std::string target = nearest_existing_path(path);
  struct statvfs info;
  if (::statvfs(target.c_str(), &info) != 0) {
    return 0;
  }
  return static_cast<long long>(info.f_blocks) *
         static_cast<long long>(info.f_frsize);
}

// ---------------------------------------------------------------------------
// 子进程侧的沙箱搭建（异步信号安全：仅系统调用与定长缓冲，不做堆分配）
// ---------------------------------------------------------------------------

namespace {

bool join_path(char *out, std::size_t out_size, const char *base,
               const char *suffix) {
  int n = std::snprintf(out, out_size, "%s%s", base, suffix);
  return n > 0 && static_cast<std::size_t>(n) < out_size;
}

bool mount_bind_readonly(const char *src, const char *dst) {
  if (::mount(src, dst, nullptr, MS_BIND | MS_REC, nullptr) != 0) {
    return false;
  }
  // 重新挂载为只读；失败也不致命，但尽量收紧。
  ::mount(nullptr, dst, nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_REC,
          nullptr);
  return true;
}

const char *base_name_ptr(const char *path) {
  const char *slash = std::strrchr(path, '/');
  return slash == nullptr ? path : slash + 1;
}

// 将 src 复制为 dst_dir/<basename(src)>，权限 0755。仅使用系统调用与定长缓冲，
// 异步信号安全。
int copy_file_executable(const char *src, const char *dst_dir) {
  char dst[1024];
  const char *name = base_name_ptr(src);
  int n = std::snprintf(dst, sizeof(dst), "%s/%s", dst_dir, name);
  if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(dst)) {
    return -1;
  }
  int in = ::open(src, O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    return -1;
  }
  int out = ::open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
  if (out < 0) {
    ::close(in);
    return -1;
  }
  char buffer[65536];
  int result = 0;
  for (;;) {
    ssize_t r = ::read(in, buffer, sizeof(buffer));
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      result = -1;
      break;
    }
    if (r == 0) {
      break;
    }
    ssize_t offset = 0;
    while (offset < r) {
      ssize_t w =
          ::write(out, buffer + offset, static_cast<std::size_t>(r - offset));
      if (w < 0) {
        if (errno == EINTR) {
          continue;
        }
        result = -1;
        break;
      }
      offset += w;
    }
    if (result != 0) {
      break;
    }
  }
  ::close(in);
  ::close(out);
  if (result == 0) {
    ::chmod(dst, 0755);
  }
  return result;
}

// 运行阶段 /box 专用 tmpfs 容量上限：只需容纳待执行程序（含 ASan 时数 MB），
// 64 MiB 为稀疏上限，实际按写入计费。
constexpr unsigned long long run_box_tmpfs_mb = 64ULL;

} // namespace

int sandbox_enter_root(const char *workspace, const char *root_dir,
                       const char *extra_bind_src, const char *extra_bind_dst,
                       bool compile_phase, const char *run_program_src,
                       unsigned long long tmpfs_mb, int &stage, int &err_no) {
  char path[1024];

  stage = static_cast<int>(SandboxStage::Root);
  if (::mount("", "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
    err_no = errno;
    return -1;
  }

  // 沙箱根 tmpfs 的挂载点：位于本次任务工作目录内，尚未存在时创建（EEXIST 可忽略）。
  if (::mkdir(root_dir, 0700) != 0 && errno != EEXIST) {
    err_no = errno;
    return -1;
  }

  char opts[64];
  std::snprintf(opts, sizeof(opts), "size=%lluM,mode=755", tmpfs_mb);
  if (::mount("tmpfs", root_dir, "tmpfs", 0, opts) != 0) {
    err_no = errno;
    return -1;
  }

  const char *dirs[] = {"usr", "etc", "proc", "dev", "tmp", "box", nullptr};
  for (int i = 0; dirs[i] != nullptr; ++i) {
    if (!join_path(path, sizeof(path), root_dir, "") ||
        std::snprintf(path, sizeof(path), "%s/%s", root_dir, dirs[i]) <= 0) {
      err_no = ENAMETOOLONG;
      return -1;
    }
    ::mkdir(path, 0755);
  }

  if (!join_path(path, sizeof(path), root_dir, "/usr") ||
      !mount_bind_readonly("/usr", path)) {
    err_no = errno;
    return -1;
  }

  // /lib、/lib64、/bin、/sbin 在 Ubuntu 上均为指向 /usr 的符号链接，
  // 在新根目录内重建同样的符号链接即可，无需重复 bind。
  if (!join_path(path, sizeof(path), root_dir, "/lib")) {
    err_no = ENAMETOOLONG;
    return -1;
  }
  ::symlink("usr/lib", path);
  if (!join_path(path, sizeof(path), root_dir, "/lib64")) {
    err_no = ENAMETOOLONG;
    return -1;
  }
  ::symlink("usr/lib64", path);
  if (!join_path(path, sizeof(path), root_dir, "/bin")) {
    err_no = ENAMETOOLONG;
    return -1;
  }
  ::symlink("usr/bin", path);
  if (!join_path(path, sizeof(path), root_dir, "/sbin")) {
    err_no = ENAMETOOLONG;
    return -1;
  }
  ::symlink("usr/sbin", path);

  if (!join_path(path, sizeof(path), root_dir, "/proc")) {
    err_no = ENAMETOOLONG;
    return -1;
  }
  if (::mount("proc", path, "proc", 0, nullptr) != 0) {
    err_no = errno;
    return -1;
  }

  const char *devices[] = {"null", "zero", "urandom", "random", "full",
                           nullptr};
  for (int i = 0; devices[i] != nullptr; ++i) {
    char host_dev[64];
    std::snprintf(host_dev, sizeof(host_dev), "/dev/%s", devices[i]);
    char local_dev[1024];
    int n = std::snprintf(local_dev, sizeof(local_dev), "%s/dev/%s", root_dir,
                          devices[i]);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(local_dev)) {
      err_no = ENAMETOOLONG;
      return -1;
    }
    int fd = ::open(local_dev, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (fd >= 0) {
      ::close(fd);
    }
    ::mount(host_dev, local_dev, nullptr, MS_BIND, nullptr);
  }

  // /box 的构建按阶段区分：
  //   - 编译阶段：workspace 可写 bind 到 /box，编译器产物经同一底层文件系统落回
  //     宿主机 workspace，判题结束后随工作目录一并清理；
  //   - 运行阶段：/box 单独挂载一个由本命名空间拥有的 tmpfs，只把待执行程序复制进去
  //     并设为只读。既不暴露宿主工作目录，也不依赖对父命名空间 tmpfs 的只读重挂载
  //     （该操作在 /dev/shm 等挂载上会返回 EPERM）。
  if (!join_path(path, sizeof(path), root_dir, kSandboxBoxPath)) {
    err_no = ENAMETOOLONG;
    return -1;
  }
  if (compile_phase) {
    if (::mount(workspace, path, nullptr, MS_BIND, nullptr) != 0) {
      err_no = errno;
      return -1;
    }
  } else {
    char box_opts[32];
    std::snprintf(box_opts, sizeof(box_opts), "size=%lluM,mode=755",
                  run_box_tmpfs_mb);
    if (::mount("tmpfs", path, "tmpfs", 0, box_opts) != 0) {
      err_no = errno;
      return -1;
    }
    if (run_program_src != nullptr && run_program_src[0] != '\0') {
      if (copy_file_executable(run_program_src, path) != 0) {
        err_no = errno != 0 ? errno : EIO;
        return -1;
      }
    }
    if (::mount(nullptr, path, nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY,
                nullptr) != 0 &&
        ::mount("tmpfs", path, "tmpfs", MS_REMOUNT | MS_RDONLY, box_opts) !=
            0) {
      err_no = errno;
      return -1;
    }
  }

  if (extra_bind_src != nullptr && extra_bind_src[0] != '\0' &&
      extra_bind_dst != nullptr && extra_bind_dst[0] != '\0') {
    char dst[1024];
    int n = std::snprintf(dst, sizeof(dst), "%s%s", root_dir, extra_bind_dst);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(dst)) {
      err_no = ENAMETOOLONG;
      return -1;
    }
    ::mkdir(dst, 0755);
    if (!mount_bind_readonly(extra_bind_src, dst)) {
      err_no = errno;
      return -1;
    }
  }

  stage = static_cast<int>(SandboxStage::Chroot);
  if (::chroot(root_dir) != 0) {
    err_no = errno;
    return -1;
  }
  stage = static_cast<int>(SandboxStage::Chdir);
  if (::chdir(kSandboxBoxPath) != 0) {
    err_no = errno;
    return -1;
  }
  return 0;
}

int sandbox_apply_limits(const SandboxLimits &limits, int &stage, int &err_no) {
  stage = static_cast<int>(SandboxStage::Limits);

  struct LimitSpec {
    int resource;
    long long value;
  };
  LimitSpec specs[5] = {
      {RLIMIT_CPU, limits.cpu_limit_sec},
      {RLIMIT_FSIZE, limits.fsize_limit_bytes},
      {RLIMIT_STACK, limits.stack_limit_bytes},
      {RLIMIT_NOFILE, limits.nofile_limit},
      {RLIMIT_CORE, limits.core_limit_bytes},
  };
  for (const LimitSpec &spec : specs) {
    struct rlimit rl;
    rl.rlim_cur = static_cast<rlim_t>(spec.value);
    // CPU 软硬限之间留 1 秒，先收到 SIGXCPU 再被 SIGKILL。
    rl.rlim_max = (spec.resource == RLIMIT_CPU)
                      ? static_cast<rlim_t>(spec.value + 1)
                      : static_cast<rlim_t>(spec.value);
    if (::setrlimit(spec.resource, &rl) != 0) {
      err_no = errno;
      return -1;
    }
  }
  if (limits.set_address_space) {
    struct rlimit rl;
    rl.rlim_cur = static_cast<rlim_t>(limits.address_space_kb * 1024);
    rl.rlim_max = rl.rlim_cur;
    if (::setrlimit(RLIMIT_AS, &rl) != 0) {
      err_no = errno;
      return -1;
    }
  }
  return 0;
}

int sandbox_apply_seccomp(const SeccompProgram &program, int &stage, int &err_no) {
  stage = static_cast<int>(SandboxStage::Seccomp);
  if (program.instructions.empty()) {
    err_no = EINVAL;
    return -1;
  }
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    err_no = errno;
    return -1;
  }
  struct sock_fprog fprog;
  fprog.len = static_cast<unsigned short>(program.instructions.size());
  fprog.filter = const_cast<struct sock_filter *>(program.instructions.data());
  if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog) != 0) {
    err_no = errno;
    return -1;
  }
  return 0;
}

} // namespace judge
} // namespace oj
