# 依赖清单与安装指南

> 更新于 M6.3（部署文档）。适用环境：Ubuntu 22.04 LTS（Jammy）x86_64。
> 依据 `SPEC.md` 的实现方案制定（后端 C++ / cpp-httplib，判题 g++/gcc + ASan/UBSan，
> 存储 SQLite，沙箱为手写 seccomp-bpf + Linux 命名空间）。
>
> 本文件区分**构建依赖 / 运行依赖 / 可选测试依赖**，并记录每个依赖的**来源**
> （系统包 / 源码 / 项目内置 / CMake 下载）、用途与安装方式。版本为在 Ubuntu 22.04
> 软件源中核实到的候选版本（`apt-cache policy`），随系统更新可能小幅变化。
>
> **兼容性边界**：本项目只在 Ubuntu 22.04 LTS x86_64（内核 5.15 系列）上验证过。
> 其它发行版/内核未经验证，依赖名称、沙箱内核能力（非特权用户命名空间、seccomp-bpf、
> tmpfs）与命令可能不同；**不声称支持所有 Linux 环境**。

---

## 1. 目标环境与前置条件

- 发行版：**Ubuntu 22.04 LTS（Jammy Jellyfish）**
- 架构：x86_64；内核 5.15.0-*（本项目沙箱依赖较新的命名空间/seccomp 特性）
- 权限：安装系统包需 `sudo`；服务与判题本身**不以 root 运行**
- 说明：以下安装命令以「空白系统 + 拥有 sudo 权限的普通用户」为前提。

### 1.1 服务器资源约束（重要）

- 目标机内存约 **3.3 GiB**，历史环境**无 Swap**（部署/运行前请用 `free -h`、`swapon --show`
  重新确认；资源可能已被其它进程占用）。
- **项目构建必须固定单并发**：`cmake --build build --parallel 1`（或 `-j 1`）。
  禁止使用无数量的 `-j`（GNU Make 会无限并发）或 `-j$(nproc)`，否则可能耗尽内存、
  引发严重 I/O 等待甚至 SSH 断连。详见 README 构建一节。
- 运行阶段的并发由下述判题配置限制（见 8 节），不要与「构建单并发」混为一谈。

### 1.2 沙箱所需内核能力

判题沙箱（M3.3）在启动时会检查以下内核能力，任一不可用即**拒绝启动**
（不会降级为无保护执行）：

- 非特权用户命名空间（`unshare(CLONE_NEWUSER)` 成功）；
- `user`/`mount`/`net`/`pid`/`ipc`/`uts` 命名空间；
- `seccomp-bpf`（`prctl(PR_SET_SECCOMP)`）；
- 判题工作目录为 **tmpfs**（未显式允许非 tmpfs 时）。

自检命令（只读）：

```bash
cat /proc/sys/kernel/unprivileged_userns_clone 2>/dev/null || true   # 期望 1 或文件不存在
cat /proc/sys/user/max_user_namespaces                              # 期望非 0
grep -E 'Seccomp|Seccomp_filters' /proc/self/status                 # 期望 Seccomp: 0
mount | grep -E '/opt/oj-tmpfs|type tmpfs'
```

- 若发行版使用 AppArmor 限制非特权用户命名空间
  （`kernel.apparmor_restrict_unprivileged_userns=1`，常见于较新 Ubuntu），需按发行版
  说明调整策略；**不要通过关闭整个沙箱隔离来解决启动失败**。
- 无挂载权限的开发/测试环境可显式 `OJ_JUDGE_ALLOW_NON_TMPFS=1`
  （会显著告警），这是**显式开发例外**；正式部署必须挂载 tmpfs。

---

## 2. 依赖总览（按用途分类）

### 2.1 构建依赖（编译服务端与测试所必需）

| 软件/库 | 版本（Jammy 候选） | 用途 |
|---|---|---|
| `build-essential` | 12.9（gcc/g++ 11.2.0） | 编译服务端；同时作为判题语言 C++17 / C11 的编译器（ASan/UBSan 随 gcc 自带） |
| `cmake` | 3.22.1（项目要求 ≥ 3.16） | 构建工程（M0.2 起使用） |
| `libcpp-httplib-dev` | 0.10.3 | 后端 HTTP 服务（静态资源托管 + JSON API），构建时链接 |
| `nlohmann-json3-dev` | 3.10.5 | JSON 序列化 / 解析（API 与判题逐点结果），header-only |
| `libsqlite3-dev` | 3.37.2 | SQLite 的 C 开发头文件/库，服务端数据层接入 |
| `libargon2-dev` | 0~20171227 | argon2id 密码哈希（AUTH-03） |
| `libssl-dev` | 3.0.2 | OpenSSL 3.0，JWT HS256 签名所需 libcrypto |
| `jwt-cpp` | 源码安装（header-only，见 3.5） | JWT 生成 / 校验（AUTH-04） |
| `pthread`（`libc6-dev`） | 随 `build-essential` | 线程池（`Threads::Threads`） |

### 2.2 运行依赖（服务与判题实际运行所需）

| 软件/库 | 来源 | 用途 / 说明 |
|---|---|---|
| 上述动态库（cpp-httplib、libsqlite3、libargon2、libcrypto） | 系统包 | 运行时动态链接，已随 2.1 安装 |
| `g++` / `gcc` | `build-essential` | 判题编译用户提交的 C++17 / C11 代码，**运行时必需**（不是构建产物） |
| 内核命名空间 + seccomp-bpf + tmpfs | 内核/挂载 | 判题沙箱隔离，见 1.2 与 3.9 |
| `sqlite3` CLI | 系统包（3.2） | **仅备份脚本 `scripts/backup.sh` 需要**；不备份可不装 |
| `flock`/`timeout`/`mktemp`/`stat`/`awk` | util-linux + coreutils | 备份脚本依赖（Ubuntu 默认已装） |
| `cron` | 系统包（3.7，可选） | 定时备份（PERS-04），也可手动执行备份而不装 |

### 2.3 可选测试 / 开发依赖（不参与服务运行）

| 软件/库 | 来源 | 用途 / 说明 |
|---|---|---|
| `libgtest-dev` | 系统包（3.6） | GoogleTest 单元测试（`tests/unit`），仅测试构建需要 |
| `curl` | 系统包（3.7） | `scripts/regression.sh` 发起 HTTP 请求 |
| `python3` | 系统包（通常预装） | `scripts/regression.sh` 解析 JSON 与挑选空闲端口 |
| `openssl` CLI | 系统包（3.3 附带） | 生成随机 JWT 密钥（`openssl rand -hex 32`）；回归脚本可选 |
| Node.js + jsdom / Chromium + playwright-cli | 外部，**未纳入仓库** | `tests/frontend/` 前端验证（可选，不注册 CTest） |
| `gcovr`/`lcov` | 外部，**未纳入仓库** | 可选覆盖率报告（`tests/coverage/`） |

### 2.4 源码安装（APT 无包）

| 软件/库 | 来源 | 用途 |
|---|---|---|
| `jwt-cpp` | GitHub `Thalhammer/jwt-cpp`（header-only） | JWT 生成 / 校验（AUTH-04） |

> 说明：jwt-cpp 为 header-only 库，Ubuntu 22.04 未收录（仅有 C 语言版 `libjwt`，非本项目所用 C++ 库），故采用源码安装。
> 安装后 CMake 通过 `find_path(JWT_CPP_INCLUDE_DIR jwt-cpp/jwt.h)` 定位。

### 2.5 依赖来源与「未使用组件」澄清

- **系统包**：上表 apt 项，由发行版提供。
- **源码安装**：仅 `jwt-cpp` 一项（header-only，拷贝到 `/usr/local/include`）。
- **项目内置文件**：种子题目、前端 `web/` 资源、测试用例等均为仓库代码，不是外部依赖。
- **CMake 下载**：**没有**。`CMakeLists.txt` 只使用 `find_package` / `find_path` /
  `find_library`，不通过 `FetchContent`/`ExternalProject` 下载任何依赖。前端 CodeMirror
  由浏览器在运行时从 CDN（cdnjs，固定版本 5.65.21）加载，属运行环境网络条件，非构建依赖。
- **未使用组件（不要当作必需）**：
  - `libseccomp-dev`：项目 seccomp 过滤器为**手写经典 BPF**（`linux/seccomp.h` +
    `linux/filter.h`），**未链接 libseccomp**。该包仅在将来替换沙箱策略时可能有用，
    当前**非必需**。
  - 前端无构建工具链（无 Node/npm/webpack/Vite/React/Vue），不要安装。

---

## 3. 安装命令

### 3.1 系统更新与构建工具

```bash
sudo apt update
sudo apt install -y build-essential cmake
```

### 3.2 存储（SQLite）

```bash
sudo apt install -y sqlite3 libsqlite3-dev
```

- `libsqlite3-dev` 为服务端数据层与构建必需；`sqlite3` CLI 仅备份脚本需要。

### 3.3 认证与安全（argon2、OpenSSL）

```bash
sudo apt install -y libargon2-dev libssl-dev
```

### 3.4 JSON 与 HTTP 服务

```bash
sudo apt install -y nlohmann-json3-dev libcpp-httplib-dev
```

### 3.5 jwt-cpp（header-only，源码安装）

```bash
cd /tmp
git clone https://github.com/Thalhammer/jwt-cpp.git
# 建议固定版本：cd jwt-cpp && git checkout <release-tag>
sudo cp -r jwt-cpp/include/jwt-cpp /usr/local/include/
rm -rf jwt-cpp
```

- 安装后以 `#include <jwt-cpp/jwt.h>` 使用；HS256 依赖 OpenSSL（已由 `libssl-dev` 提供）。
- 本项目使用 jwt-cpp 的 **nlohmann-json** traits（`#include <jwt-cpp/traits/nlohmann-json/defaults.h>`），
  与项目 JSON 依赖 `nlohmann-json3-dev` 保持一致，无需额外引入 picojson。
- 运行时需通过环境变量 `OJ_JWT_SECRET` 提供 HS256 签名密钥（详见 README 与本节 6.5）。

### 3.6 测试依赖（可选）

```bash
sudo apt install -y libgtest-dev
```

- 提供 `GTest::gtest_main` / `GTest::gtest`（头文件与静态库），供 `tests/unit` 的
  gtest 单元测试（如配置管理 `test_config.cpp`）链接，不参与服务端运行时依赖。
- `curl`、`python3`、`openssl` 由 3.7 与系统预装提供，用于回归脚本。

### 3.7 备份与运维辅助（可选）

```bash
sudo apt install -y cron curl python3
sudo systemctl enable --now cron
```

- `cron`：定期备份（PERS-04）。**本轮部署文档不安装/不修改 crontab**；如不启用定时
  备份，可不装 cron，手动运行 `scripts/backup.sh` 即可。
- `curl`/`python3`：`scripts/regression.sh` 冒烟回归所需；`openssl` 用于生成随机密钥。

### 3.8 libseccomp（当前非必需，可选保留）

```bash
# 仅为将来可能替换沙箱策略时安装；当前实现未使用 libseccomp，不必安装。
# sudo apt install -y libseccomp-dev
```

### 3.9 判题 tmpfs 运行目录（正式部署必需）

```bash
sudo mkdir -p /opt/oj-tmpfs
sudo mount -t tmpfs -o size=512M,mode=1777 tmpfs /opt/oj-tmpfs
```

- 说明：
  - **不能加 `noexec`**：判题需在此目录执行编译产物。
  - `size=512M` 为容量上限（tmpfs 按实际写入计费，非预分配）。此上限依据 3.3 GiB
    服务器内存预算确定：每个判题任务的工作目录仅需保存源码（≤64 KiB）与编译产物
    （含 ASan 时数 MB），运行阶段工作目录被重新挂载为只读，用户程序无法写入，
    故 512 MiB 足以覆盖并发产物且不会挤占系统所需内存。内存充裕的机器可上调至 1G。
  - `mode=1777` 允许判题子进程写入；工作目录本身由服务以 `mkdtemp` 原子创建为 0700。
  - **权限**：挂载点属主为 root 不影响使用（服务以普通账号在其中创建子目录）。
  - 该挂载点在 `/opt` 下、仓库目录之外，**重启后需重新挂载**。

- 开机自动挂载（在 `/etc/fstab` 追加一行）：

  ```
  tmpfs /opt/oj-tmpfs tmpfs defaults,size=512M,mode=1777 0 0
  ```

  也可用 systemd mount unit（M5.3 第 14 节已验证 `enabled`+`active`），本轮不安装。

- **正式部署 vs 显式开发例外**：
  - 正式部署：必须挂载 tmpfs，启动时 `path_is_tmpfs` 与沙箱自检必须通过。
  - 开发例外：无挂载权限时可显式 `OJ_JUDGE_WORKSPACE=<可写目录>`
    与 `OJ_JUDGE_ALLOW_NON_TMPFS=1`，服务会显著告警；**不得**用于接收不可信代码。
  - **不要**通过关闭隔离（禁用命名空间/seccomp）来解决启动失败。

---

## 4. 安装后验证

```bash
g++ --version && gcc --version
cmake --version
sqlite3 --version
dpkg -l libargon2-dev libssl-dev nlohmann-json3-dev libcpp-httplib-dev | tail -n +6
test -f /usr/include/gtest/gtest.h && echo "gtest OK"        # 可选测试依赖
test -f /usr/local/include/jwt-cpp/jwt.h && echo "jwt-cpp OK"
mount | grep oj-tmpfs
```

> 注意：`libseccomp-dev` 不在验证清单内（当前未使用）。

---

## 5. 不需要安装的部分

- 前端：原生 HTML/CSS/JS + CodeMirror（CDN 引入，UI-04），无构建流程、无需安装。
- JWT 逻辑、argon 哈希调用、判题器、Rejudge、日志等均为本项目代码实现。
- tmpfs 挂载与 cron 定时任务属于运行时配置，非软件安装。
- `libseccomp-dev` 当前非必需（见 2.5、3.8）。

---

## 6. 数据库运行说明（M0.3 起）

### 6.1 数据库路径

- 默认 `data/oj.db`（相对启动目录），可用 `--db <路径>` 覆盖。
- 首次启动自动创建父目录、数据库文件及表结构；重复启动复用已有数据，不会删表重建。

### 6.2 连接与事务配置

- 每个连接开启 `PRAGMA journal_mode=WAL`、`PRAGMA synchronous=NORMAL`、
  `PRAGMA foreign_keys=ON`，并设置 `PRAGMA busy_timeout=5000`（锁等待 5 秒）。
- 连接以 `SQLITE_OPEN_FULLMUTEX`（serialized）模式打开，单个长连接可被多线程安全共享；
  服务进程生命周期内持有一个连接，HTTP 并发访问时无需也不应新建/乱序共享连接。

### 6.3 初始管理员密码

- 首次初始化且数据库中尚无 `admin` 时，通过环境变量 `OJ_ADMIN_PASSWORD` 提供初始密码，
  密码只以 argon2id 哈希落库，不写入源码、版本控制或日志。
- 数据库中已有 `admin` 时无需（也不会）再要求该变量，重复初始化不覆盖密码、不重置首次改密标记。

### 6.4 依赖

数据库（`libsqlite3-dev`）与密码哈希（`libargon2-dev`）在 3.2 / 3.3 节已列出，
安装命令见对应小节；无额外新增系统包。

### 6.5 JWT 签名密钥（M1.2 起）

- 通过环境变量 `OJ_JWT_SECRET` 提供 HS256 签名密钥（必填，长度不少于 16 字节，无默认值）。
- 有效期通过 `OJ_JWT_EXPIRES_SECONDS` 配置（可选，默认 3600 秒）。
- 密钥不写入源码、版本控制或日志；保持同一密钥重启后，未过期的 token 仍可验证。

---

## 7. 判题器运行说明（M1.5 起）

### 7.1 新增依赖

M1.5 判题器**不新增系统依赖**：

- 判题语言 C++17 / C11 的编译器 `g++` / `gcc` 由 2.1 节的 `build-essential` 提供；
- 单元测试继续使用 3.6 节的 gtest；
- 本阶段仅通过 `fork` + `execvp`、`pipe`/`poll` 与 `waitpid` 执行进程，未使用
  `libseccomp`（seccomp 在 M3 接入）。

### 7.2 运行方式

- 判题核心为普通静态库代码，随 `oj_core` 构建，当前不经过 HTTP 与数据库即可调用。
- 开发验证入口（隔离临时工作目录，不触碰 `data/oj.db`）：

  ```bash
  cmake --build build --parallel 1
  ctest --test-dir build -R judge_unit --output-on-failure
  ctest --test-dir build -R judge_integration --output-on-failure
  ```

### 7.3 运行环境注意

- 默认在系统临时目录（`std::filesystem::temp_directory_path()`）下用 `mkdtemp` 创建
  每次判题的独立目录，可经 `JudgeOptions::workspace_root` 覆盖。M3 将改为 3.9 节
  挂载的 tmpfs 目录 `/opt/oj-tmpfs`。
- 本阶段只有基础超时与子进程回收，**不是完整沙箱**，不得用于公开接收不可信代码；
  完整资源限制与系统调用限制在 M3 完成。

---

## 8. 判题沙箱与资源限制运行说明（M3.3 起）

### 8.1 新增依赖

M3.3 **不新增系统包**。seccomp 过滤器以手写经典 BPF（`linux/seccomp.h` +
`linux/filter.h`）在父进程预构建、子进程仅 `prctl(PR_SET_SECCOMP)` 加载，未使用
`libseccomp`（因此 `libseccomp-dev` 目前非必需，见 2.3 / 2.5 / 3.8 节）。

### 8.2 隔离机制

每次编译/运行在隔离的子进程中执行（`src/judge/local_executor.cpp` +
`src/judge/sandbox.cpp`）：

- **命名空间**：`user` / `mount` / `net` / `pid` / `ipc` / `uts`。写 uid/gid 映射后
  子进程在新用户命名空间内成为 `root`，在宿主上仍映射为运行服务的普通用户，绝不
  以 root 运行服务。
- **最小根目录 + chroot**：以 tmpfs 为根，只读 bind `/usr` 并重建 `/lib`、`/lib64`、
  `/bin`、`/sbin` 符号链接，挂载新 `/proc` 与最小 `/dev`。编译阶段把工作目录可写
  bind 到 `/box`；运行阶段把待执行程序复制进沙箱自有的只读 `/box` tmpfs，不暴露宿主
  工作目录（也避免了对父命名空间 tmpfs 的只读重挂载，该操作在 `/dev/shm` 等挂载上会
  返回 EPERM）。
- **seccomp-bpf**：拒绝网络（socket 家族、io_uring）、挂载/逃逸（mount/umount/
  pivot_root/setns/open_tree/open_by_handle_at 等）、调试与内核接口（ptrace/bpf/
  perf_event_open/keyctl/模块与 kexec 等）、时间/主机名修改；运行阶段额外拒绝
  `fork/vfork/clone/clone3`。x32 ABI 系统调用号一律拒绝。文件访问通过 chroot +
  只读挂载约束在沙箱内，而非仅拦截 `open` 名称（动态加载器与 ASan 需要读库与
  `/proc/self`）。
- **进程边界**：每个任务独立进程组；运行阶段用户程序为 PID ≠ 1 的载荷进程，
  另有一个 PID 1 init 回收孤儿进程（避免 PID 1 忽略默认信号导致信号崩溃被吞）。
- **文件描述符/环境**：`close_range` 关闭除状态/输出管道外的全部继承 fd；子进程只
  获得受控最小环境（`PATH`/`HOME=/nonexistent`/`TMPDIR=/tmp`/`LANG`/`LC_ALL`），
  绝不传递 `OJ_JWT_SECRET`、`OJ_ADMIN_PASSWORD` 等服务密钥。

### 8.3 资源限制

- **CPU**：`setrlimit(RLIMIT_CPU)`，取单点时限向上取整 + 1s（编译阶段更宽松），与
  墙钟 watchdog 双保险。
- **内存**：**不设置 `RLIMIT_AS`**（ASan/UBSan 会预留海量虚拟地址空间，设置后
  启动即失败）。改以 20ms 周期采样用户进程 RSS，超限即 `SIGKILL` 整个进程组并标记
  `memory_exceeded` → 判题核心据此判 `MLE`；观测峰值写入逐点结果，作为 M3.4 判
  `MLE` 的唯一可靠证据（无 RSS 证据的 SIGKILL/分配失败不判 MLE）。编译阶段按进程组
  汇总 RSS（覆盖 `cc1plus`/`as`/`ld`）。
- **文件/栈/描述符**：`RLIMIT_FSIZE`、`RLIMIT_STACK`、`RLIMIT_NOFILE`、`RLIMIT_CORE=0`。
- **输出**：标准输出 64 KiB、标准错误 16 KiB、编译诊断 64 KiB，采集时按字节上限
  截断并继续排空管道（不先无限读取），截断不判为 AC，采集不挂死。
- **编译并发门限**：`OJ_JUDGE_COMPILE_CONCURRENCY`（默认 2）限制同时编译数，运行
  阶段仍由 worker 数（`min(CPU 核数, 8)`）控制。等待许可的时间计入该次判题的全局
  60s 硬上限，并可被服务停止取消。

### 8.4 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `OJ_JUDGE_WORKSPACE` | `/opt/oj-tmpfs` | 判题工作目录（应为 tmpfs） |
| `OJ_JUDGE_ALLOW_NON_TMPFS` | 未设置 | `1/true/yes` 允许非 tmpfs（仅开发/测试） |
| `OJ_JUDGE_COMPILE_CONCURRENCY` | `2` | 编译阶段并发门限（1..64） |
| `OJ_JUDGE_QUEUE_CAPACITY` | `32` | 判题等待队列容量（1..256） |

> **三种「并发」不要混淆**：
> 1. **项目构建并发**（开发/部署时）：固定 `cmake --build build --parallel 1`，
>    这是编译本项目源码的并发，与运行时判题无关。
> 2. **判题 worker 数**（运行阶段）：`min(CPU 核数, 8)`，由 `JudgeManager` 在运行时
>    自动确定，无环境变量可直接覆盖；每个 worker 一次处理一个完整提交。
> 3. **判题编译门限**（编译阶段）：`OJ_JUDGE_COMPILE_CONCURRENCY`（默认 2），只限制
>    高内存的 ASan 编译阶段同时数量；运行阶段仍由 worker 数控制。
> 此外 `OJ_JUDGE_QUEUE_CAPACITY` 只计算「已接收但尚未开始执行」的等待任务数，
> 与正在执行的任务数分开。

### 8.5 失败行为与启动检查

- 启动时依次检查：`sandbox_supported`（内核允许非特权用户命名空间）、工作目录是否
  为 tmpfs（未显式允许时）、以及一次真实沙箱自检（`/bin/true`）。任一失败即报错
  退出，**绝不降级为无保护执行**。
- 运行期沙箱初始化（命名空间/挂载/chroot/setrlimit/seccomp）失败时，执行器返回
  `launch_error + sandbox_error`，判题核心判为 `SYSERR` 并保留明确诊断；不静默降级。
- tmpfs 不可用、容量不足或写入失败：工作目录创建/写入失败 → `SYSERR`；判题请求
  不会永久等待（全局硬上限 + watchdog）。
- 成功、崩溃、超时、内存超限、策略拒绝、服务停止后均清理本次任务的进程组与工作
  目录（`Workspace` RAII，且校验路径未越界、不跟随符号链接）；不删除其他任务目录，
  不卸载共享 tmpfs。

### 8.6 3.3 GiB 服务器资源预算（本次实测环境）

| 项目 | 预算 |
|---|---|
| 系统 + sshd + 服务/数据库 | 约 0.5–0.7 GiB |
| 运行阶段并发（worker=min(CPU,8)；本机 4）ASan 程序 RSS | 约 0.1–0.4 GiB |
| 编译阶段并发（门限 2）实际 RSS | 约 0.4–0.8 GiB |
| `/opt/oj-tmpfs` 容量上限 | 512 MiB（实际并发产物仅数十 MB） |
| 沙箱内根 tmpfs（每进程稀疏，按写入计费） | 编译 256 MiB / 运行 64 MiB 上限 |

> 本机（4 vCPU / 3.3 GiB，无 swap）实测：全量串行 `ctest` 期间系统已用峰值约
> 2.1 GiB（含开发工具约占 0.9 GiB），剩余可用最低约 1.0 GiB；4 路并发真实判题
> （编译门限 2）在 1 秒内全部 AC，无异常。
>
> **部署/运行前请重新检查**：`free -h`、`swapon --show`、`nproc`。历史环境无 Swap，
> 若可用内存明显低于上述预算，应先释放内存或下调并发（编译门限）后再启动，
> 不要在有并发判题时执行全量构建/测试。

---

## 9. 编译模板、Sanitizer 与结果分类（M3.4）

### 9.1 编译模板（`LocalExecutor::compile`）

两套语言均以参数数组 `execve` 启动，绝不经过 shell，用户源码与请求参数不能改变
编译器路径或注入额外命令。选项由执行器集中维护：

- C++17：`g++ -O2 -std=c++17 <src> -o <program> -lm`
- C11：`gcc -O2 -std=c11 <src> -o <program> -lm`
- 默认叠加：`-fsanitize=address,undefined -fno-omit-frame-pointer
  -fno-sanitize-recover=all`

`-fno-sanitize-recover=all` 使 UBSan 一旦报告未定义行为即中止（`abort`），避免
「已报告 UB 但继续执行、输出碰巧匹配被误判 AC」。运行阶段环境固定
`ASAN_OPTIONS=detect_leaks=0`、`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`
（关闭 LeakSanitizer 以兼容「禁 ptrace/禁创建进程」的 seccomp 策略）。`JudgeOptions`
的 `sanitizers_enabled`（默认 `true`）统一开关；测试可显式关闭以验证基础流程。

### 9.2 结构化原因与分类规则

执行层（`LocalExecutor`）在 `ProcessResult` 记录单一权威的 `TerminationReason`
（`Completed/NonZeroExit/Signaled/TimedOut/MemoryExceeded/Cancelled/LaunchFailure`）
与疑似 Sanitizer 标注；分类层 `classify_case`（`src/judge/classification.{h,cpp}`）
据此统一判定，规则（确定性优先级）：

1. 取消 → `SYSERR`（中止剩余）；
2. 启动/沙箱失败 → `SYSERR`（中止剩余）；
3. 可靠 RSS 证据超限 → `MLE`；
4. 全局预算裁剪导致的超时 → `TLE` + `global_deadline_hit`（中止剩余）；
5. 单点超时 → `TLE`（继续后续点）；
6. 信号终止/非正常退出/非零退出码 → `RE`；
7. 标准输出超限 → `RE`（沿用 SPEC JUDGE-05，不新增 OLE）；
8. 正常执行、输出未超限且归一化匹配 → `AC`；
9. 其余 → `WA`。

单点仅凭用户可打印的 stderr 文本（含类似 `AddressSanitizer` 字样）不判失败；只有
实际异常退出/非零退出码才判 `RE`。逐点汇总按 `SYSERR > TLE > MLE > RE > WA > AC`
取最严重者，零测试点固定 `SYSERR`；全局硬上限或服务取消会覆盖为 `SYSERR`，已获得
的逐点结果保留、未执行点不伪造。

### 9.3 指标口径

- **逐点 `time_ms`**：该点子进程启动到回收的墙钟经过时间（毫秒），不含排队与编译；
- **逐点 `memory_kb`**：20ms 周期采样的峰值 RSS（kB），未采集到为 `null`；
- **提交级 `runtime_ms`**：各点 `time_ms` 之和（**不含排队与编译**）；
- **提交级 `memory_kb`**：各点峰值 RSS 的**最大值**（非求和）；未采集到为 `null`；
- **`compile_time_ms`**：编译阶段墙钟耗时，独立字段，不混入运行耗时。

### 9.4 WA 反馈边界

仅在**整体 WA 的失败点**通过 `POST /api/problems/{id}/submit` 的响应向提交者返回
该点的 `input`/`expected_output`/`actual_output`（另含结构化 `reason` 与诊断）；
通过点只含 `index/status/time_ms/memory_kb`，绝不附带隐藏输入或标准答案。原始输出
文本原样保留，归一化仅用于比对。响应只返回给提交者本人（管理员可经提交详情访问）。

### 9.5 M3.4 实测（本机 3.3 GiB，无 swap）

- 构建：`cmake --build build --parallel 1`（单并发），成功。
- 新增/更新测试：`judge_classification_unit`（22 项 gtest）、`m34_classification`
  （真实进程，覆盖默认 ASan/UBSan 模板的 AC/WA/CE、越界/UB 判失败、伪 Sanitizer 文本
  不误判、TLE/RE/MLE、输出与诊断上限、编译器故障 `SYSERR` 后恢复、混合优先级、指标
  口径）；`sandbox_integration` 的 UBSan 期望由「可恢复诊断判 AC」更新为「不可恢复
  判失败」。
- 全量回归：`ctest --test-dir build`（串行）**34/34 通过**，总耗时约 156s。
- 5 用户并发持续提交（`submit_scheduling_api` 的 `test_five_users_sustained_concurrency`）：
  5 名用户并发、连续 3 轮共 15 次真实 ASan 判题（AC/WA 交替），编译门限 2、worker=5，
  结果互不混用、计数一致、无遗留子进程；实测最低可用内存约 **737 MiB**，无 OOM。
  该场景据此前的「长时间压测」范围收敛为产品目标规模（5 人同时）；超出该目标的更高
  并发尚未验证。
- 资源：全量回归期间可用内存最低约 850 MiB，始终高于预留保护线；构建与测试均单并发，
  未出现 OOM/卡顿。ASan 编译为单/双并发（受编译门限约束，测试叠加并发受控）。

