#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <linux/filter.h> // struct sock_filter

namespace oj {
namespace judge {

// 判题子进程的沙箱阶段：编译与运行采用不同策略（SPEC M3.3）。
enum class SandboxPhase {
  Compile, // 编译器：允许必要的头文件/库/子进程，禁网络与危险系统调用
  Run,     // 用户程序：最小根目录、禁网络、禁创建进程、禁逃逸
};

// setrlimit 资源限制集合。内存不通过 RLIMIT_AS 施加（与 ASan 不兼容），
// 而由执行器按 RSS 采样并强制终止，详见 compute_limits 与 local_executor。
struct SandboxLimits {
  long long memory_limit_kb = 65536;          // RSS 上限（kB）
  int cpu_limit_sec = 3;                      // RLIMIT_CPU 秒
  long long fsize_limit_bytes = 64LL << 20;   // RLIMIT_FSIZE 单文件字节
  long long stack_limit_bytes = 64LL << 20;   // RLIMIT_STACK
  int nofile_limit = 64;                      // RLIMIT_NOFILE
  long long core_limit_bytes = 0;             // RLIMIT_CORE（0=禁用 core）
  // RLIMIT_AS 默认不设置：ASan/UBSan 运行时会预留海量虚拟地址空间，
  // 设置虚拟地址空间上限会在程序启动阶段即失败。仅在明确未启用 Sanitizer
  // 且确有需要时才开启，并同时设置 address_space_kb。
  bool set_address_space = false;
  long long address_space_kb = 0;
};

// 在父进程（多线程环境）中预先构建的 seccomp-bpf 程序。子进程在 fork 后仅做
// 系统调用加载（prctl + seccomp），不进行内存分配，避免多线程 fork 后死锁。
struct SeccompProgram {
  std::vector<sock_filter> instructions;
  bool empty() const { return instructions.empty(); }
};

// 沙箱失败阶段，供父进程给出明确诊断（工程要求 14：保留证据）。
enum class SandboxStage {
  None = 0,
  Unshare = 1,
  Root = 2,       // tmpfs/目录/挂载
  Chroot = 3,
  Limits = 4,
  Seccomp = 5,
  Chdir = 6,
  Exec = 7,
};

const char *sandbox_stage_name(SandboxStage stage);

// 依据阶段与题目限制计算具体资源上限。
//   - Run：CPU 取单点时限（秒，向上取整 + 1s 余量），RSS 取题目内存上限；
//   - Compile：使用较大的独立预算，避免正常编译被资源限制误杀。
SandboxLimits compute_limits(SandboxPhase phase, long long memory_limit_kb,
                             int time_limit_ms);

// 构建阶段对应的 seccomp-bpf 程序（在父进程调用）。成功返回 true。
bool build_seccomp_program(SandboxPhase phase, SeccompProgram &out,
                           std::string &error);

// 轻量能力探测：内核是否允许非特权用户命名空间。返回 false 并给出原因。
bool sandbox_supported(std::string &error);

// 判断 path 是否位于 tmpfs 挂载点。path 不存在时按最近存在的父目录判断。
// 成功（能确定）返回 true；无法判断返回 false 并置 error。
bool path_is_tmpfs(const std::string &path, std::string &error);

// 返回 path 所在挂载点的容量上限（字节），无法确定返回 0。
long long mount_capacity_bytes(const std::string &path);

// 沙箱根目录内各挂载点的目标路径（均在 chroot 后可见）。
inline constexpr const char *kSandboxBoxPath = "/box";
inline constexpr const char *kSandboxToolsPath = "/oj-tools";

// 在沙箱子进程中执行命名空间与最小根目录搭建，随后 chroot 到根目录并 chdir
// 到 /box。仅使用异步信号安全的系统调用与定长缓冲，不进行任何堆分配。
//   - workspace：宿主机路径。编译阶段可写地 bind mount 到 /box（编译器落产物）；
//   - root_dir：宿主机上用于承载沙箱根 tmpfs 的目录（位于 workspace 内）；
//   - extra_bind_src/dst：可选额外只读 bind（如编译器所在目录），空则跳过；
//   - compile_phase：为真时把 workspace 可写 bind 到 /box；为假（运行阶段）则单独
//     挂载一个由本命名空间拥有的 /box tmpfs，只复制待执行的程序进去并设为只读，
//     不暴露宿主工作目录，也不依赖对父命名空间 tmpfs 的只读重挂载（后者在部分
//     环境下不被允许）；
//   - run_program_src：运行阶段待执行程序的宿主机路径（位于 workspace 内时非空），
//     复制为 /box/<basename>；为空则 /box 保持空目录；
//   - tmpfs_mb：沙箱根 tmpfs 容量上限，限制 /tmp 等临时写入，防止占用宿主机内存。
// 成功返回 0；失败返回 -1 并设置 stage/err_no。
int sandbox_enter_root(const char *workspace, const char *root_dir,
                       const char *extra_bind_src, const char *extra_bind_dst,
                       bool compile_phase, const char *run_program_src,
                       unsigned long long tmpfs_mb, int &stage, int &err_no);

// 在沙箱子进程中施加 setrlimit 限制。成功返回 0，失败返回 -1。
int sandbox_apply_limits(const SandboxLimits &limits, int &stage, int &err_no);

// 在沙箱子进程中加载 seccomp-bpf（先 PR_SET_NO_NEW_PRIVS）。成功返回 0。
int sandbox_apply_seccomp(const SeccompProgram &program, int &stage, int &err_no);

} // namespace judge
} // namespace oj
