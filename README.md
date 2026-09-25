# 仿 LeetCode 在线判题系统（OJ）

> 后端：C++（cpp-httplib） · 前端：原生 HTML/CSS/JS + CodeMirror · 存储：SQLite
> 需求与架构的唯一依据见 [`SPEC.md`](SPEC.md)，依赖安装见 [`dependence.md`](dependence.md)。

## 当前进度

- [x] M0.1 项目结构
- [x] M0.2 构建与 HTTP 服务（CMake 接入 cpp-httplib，`GET /api/health` 健康检查，优雅停止）
- [x] M0.3 数据库基础（SQLite 接入、五张业务表建表 + 约束/索引、WAL 与外键、首次启动自动初始化、argon2id 预置 admin、生命周期接入；M3.7 追加在途任务表）
- [x] M1.1 注册（10 位随机账号分配 + 昵称唯一性 + argon2id 密码哈希 + `POST /api/register`）
- [x] M1.2 登录与身份验证（JWT 签发/校验 + `POST /api/login` + 登录限速 + Bearer 鉴权 + `GET /api/me`）
- [x] M1.3 改密与权限检查（`POST /api/me/password` + admin 首登强制改密 + 可复用管理员权限检查）
- [x] M1.4 最小题目数据与查询（`testcases.is_sample` 区分公开样例/隐藏用例 + 41 道幂等种子题 + `GET /api/problems` / `GET /api/problems/{id}` + 题目可见性）
- [x] M1.5 最小判题器（`IExecutor` 抽象 + `LocalExecutor` + `JudgeEngine`：C++17/C11 编译、逐点执行、基础超时、有界输出、归一化比对与 AC/WA/CE/TLE/RE/SYSERR 汇总；仅开发环境验证，完整沙箱见 M3）
- [x] M1.6 提交接口与持久化（`POST /api/problems/{id}/submit`：登录/首改/可见性校验 + 后端隐藏用例判题 + 单事务写入 `submissions` 与 `user_problem_status` + 同步返回逐点结果）
- [x] M1.7 最小前端（cpp-httplib 静态托管 `web/` + 原生 HTML/CSS/ES Module + hash 路由 + 注册/登录/改密/题目列表/题目页 `textarea` 提交 + 统一 API 封装；仅开发环境验证）
- [x] M2.1 管理员题目接口（`POST`/`PUT`/`DELETE /api/admin/problems`：字段校验与默认值 + 部分更新语义 + 可见性设置 + 删除关联数据规则 + 统一管理员权限检查）
- [x] M2.2 管理员测试用例接口（`GET`/`POST`/`PUT`/`DELETE /api/admin/problems/{id}/testcases[/{tid}]`：隐藏用例读/增/改/删 + 归属与权限校验 + `ord` 排序规则 + 公开样例隔离 + 判题快照与历史数据不受影响）
- [x] M2.3 题目列表查询（`GET /api/problems` 支持 `q`/`difficulty`/`tag`/`page`/`visible`：关键词搜索 + 难度/标签精确筛选 + AND 组合 + 每页 20 条分页 + 通过人数与本人状态 + 可见范围）
- [x] M2.4 管理员用户接口（`GET`/`PUT /api/admin/users`：用户列表分页查询 + 重置密码 + 修改角色 + 最后管理员保护 + 统一管理员权限检查）
- [x] M2.5 后台管理页面（原生前端后台：管理员入口与访问检查 + 题目管理（列表/创建/编辑/公开隐藏/删除）+ 测试用例管理（按 `ord` 增改删）+ 用户管理（列表/重置密码/改角色）；真正权限由后端接口执行）
- [x] M3.1 判题任务调度（`JudgeManager` 有界等待队列 + `min(CPU 核数,8)` worker 线程池 + 每任务独立 future 结果通道 + 队列满载 503 立即拒绝 + 同步返回 + 停止排空回收；复用现有判题与持久化逻辑）
- [x] M3.2 子进程与超时控制（fork/exec 与结果采集完善 + 单调时钟 watchdog + 单次判题全局 60s 硬上限 + 超时/取消 `SIGKILL` 进程组并由 `waitpid` 回收 + 服务停止取消与子进程清理）
- [x] M3.3 运行隔离与资源限制（默认 tmpfs 工作目录 + `mkdtemp` 随机目录与安全清理 + user/mount/net/pid/ipc/uts 命名空间 + chroot 最小根目录 + 只读工作目录 + setrlimit CPU/文件/栈/fd/CORE + RSS 采样内存限制判 `MLE` + seccomp-bpf 禁网络/逃逸/进程创建/危险调用 + 64KB 输出上限 + 编译并发门限；真实 Linux 进程验证隔离、限制与 ASan/UBSan 兼容）
- [x] M3.4 编译与结果分类（C++17/C11 生产编译模板默认接入 ASan/UBSan 且 UBSan 不可恢复 + 执行层结构化终止原因 + 统一分类 `AC/WA/CE/TLE/RE/MLE/SYSERR` + 可靠证据判 MLE + 逐点耗时/峰值内存与编译耗时采集 + WA 详情 + 编译诊断路径清洗与截断标识）
- [x] M3.5 持久化与停止清理（完整结果/逐点详情/指标持久化与重启可读 + 任务标识贯穿接收/执行/保存/取消/清理日志 + 终端责任归属与重复保存/计数防护 + 数据库锁竞争有界 + 客户端断开保留已接收任务 + 队列/worker/子进程清理接入优雅停止 + 清理失败可定位不误删）
- [x] M3.6 Rejudge（`POST /api/admin/submissions/{id}/rejudge`：原提交源码/语言 + 当前配置与用例快照重判 + 原记录更新不增次数 + 状态重算（最早 AC/无 AC 清空）+ 按提交 ID 并发去重 + 队列满载/重复重判明确错误 + SYSERR/取消保留原结果策略 + 后台 `#/admin/rejudge` 入口）
- [x] M3.7 崩溃恢复与在途任务持久化（独立 `in_flight_tasks` 表 + 接收边界「容量预留→写库→入队」+ 结算事务内删除在途记录保证同一任务只结算一次 + 启动扫描未结算任务按当前配置/用例重新入队 + 题目缺失标记中断 + 单机 `flock` 实例互斥 + 删题在途保护；已通过独立测试验证，见 `tests/M3.7-test-report.md`；M6.4 另以「备份含 pending 在途记录→恢复→启动结算且不重复计数」复核）
- [x] M4.1 页面基础设施（统一 hash 路由与访问条件、`unknown`/`guest`/`authenticated` 身份状态机与 `/api/me` 核实、统一 API 错误分类与 401/首改跳转、登录后返回原目标、页面生命周期与请求取消、请求超时）
- [x] M4.2 题目列表（`GET /api/problems` 搜索/难度/标签/可见性组合筛选 + 每页 20 条分页与紧凑页码范围 + 通过人数与本人 AC 状态 + 管理员隐藏题目标识与「全部/公开/隐藏」筛选 + 标签选项 `GET /api/problem-tags` + 查询条件承载于 hash 路由；已通过 M6.4 真实浏览器独立验证：搜索/难度/标签/可见性筛选、通过人数与本人 AC 状态、隐藏题隔离；见 `tests/M6.4-acceptance-report.md`）
- [x] M4.3 题目与做题页面（左题面/样例/限制/本人状态 + CDN CodeMirror 编辑器与 C/C++ 高亮、语言切换、`Ctrl+Enter` 提交 + 提交中/成功/失败状态与源码快照 + 全部逐点结果（状态/耗时/内存/原因）+ WA 输入/期望/实际输出 + 编译与诊断信息；已通过独立测试验证：后端 59 项断言 + 单元 4 用例、jsdom 54 项、真实 Chromium 28 项（含真实 CDN CodeMirror 高亮、ResizeObserver/refresh、窄视口单列、编辑器释放）、c8 前端覆盖率；全量回归 41/41。见 `tests/M4.3-test-report.md`）
- [x] M4.4 提交历史与详情（`GET /api/submissions?mine` 本人历史分页（最新优先、可选题目筛选）+ `GET /api/submissions/{id}` 详情（本人/管理员）+ `GET /api/status` 本人题目状态；前端 `#/submissions`、`#/submissions/{id}` 与导航入口，复用 M4.3 结果组件，只读源码、管理员就地重判；已通过独立测试验证：后端单元 7 用例 + 集成 90 项断言、jsdom 31 项，全量回归 43/43，旧前端回归 313/313。见 `tests/M4.4-test-report.md`）
- [x] M4.5 排行榜（`GET /api/leaderboard` 公开分页查询 + 前端 `#/leaderboard` 与导航入口：按 AC 数↓/总提交次数↑/首次 AC 时间↑/注册时间↑/用户 ID↑ 排序；统计复用既有 `user_problem_status` 持久化数据，仅计入可见题目与有已结算提交的用户，管理员同口径参与；已通过独立测试验证：后端单元 9 用例 + 集成 4 场景/52 项断言、jsdom 23 项，全量常规回归 45/45。见 `tests/M4.5-test-report.md`）

M4.5 排行榜已通过独立测试验证；M5 安全与异常回归、M6.1 自动化测试、M6.2 备份与恢复
均已完成并通过相应回归（M6.2 备份可在隔离库恢复并核对通过，见 `tests/M6.2-test-report.md`）。
M6.3 部署文档（依赖说明、部署/启动流程、配置表、数据位置、故障排查）与一键启动入口
已整理，并在 M6.4 复核实际构建/启动/种子/优雅停止/重启持久化。
**M6.4 最终验收已完成**：A 完整业务流程（真实 Chromium 29/29）、B 判题正确性与权限、
C 工程与交付（`ctest` 50/50、`scripts/regression.sh` 23/23、备份恢复）、D 安全与性能
（典型提交单次约 0.5s、5 并发全部 AC），逐项结果与已知限制见
`tests/M6.4-acceptance-report.md`。边界项：宿主机内存压力已在硬性 cgroup 上限内执行、填满 tmpfs 与 fork 拒绝已受控验证；
有头真实浏览器已由 `tests/frontend/browser/run_m64_browser_headed_xvfb.sh`（私有虚拟显示 +
窗口化 Chromium）完成 24/24；开机自动挂载已由真实重启验证（`verify_boot_mount.sh` 重启后
8/8、开机后 3s 自动挂载），见报告第 6 节。

## 环境要求

目标环境：**Ubuntu 22.04 LTS（Jammy）x86_64，内核 5.15 系列**。项目只在
该环境验证过，未声称支持所有 Linux 发行版。安装依赖：

```bash
sudo apt update
sudo apt install -y build-essential cmake libcpp-httplib-dev nlohmann-json3-dev \
  libsqlite3-dev libargon2-dev libssl-dev
```

- 构建/运行必需依赖如上；`sqlite3`（备份脚本）、`libgtest-dev`（单元测试）、
  `cron`（定时备份）、`curl`/`python3`（冒烟回归）等按需安装，均非服务运行必需。
- 判题沙箱为**手写 seccomp-bpf + Linux 命名空间**，**不使用 `libseccomp`**，
  故 `libseccomp-dev` 当前非必需。
- `jwt-cpp` 为 header-only，需源码安装到 `/usr/local/include`，见 `dependence.md` 3.5 节。
- 完整分类（构建依赖 / 运行依赖 / 可选测试依赖 / 来源 / 版本 / 内核能力 / 资源约束）
  见 [`dependence.md`](dependence.md)。

## 部署（Linux，M6.3）

> 本节按实际操作顺序整理在 Linux 上的部署流程，并给出配置、数据、备份与故障处理入口。
> **状态**：部署文档、一键启动入口与静态一致性检查已整理。M6.4 最终验收在目标服务器
> 复核了构建、配置、`--seed` 首次初始化、`scripts/run_dev_server.sh` 启动、健康检查、
> `SIGINT` 优雅停止与再次启动（管理员不重置、种子不重复导入），并确认 `/opt/oj-tmpfs`
> 为已挂载真实 tmpfs。依赖安装与系统挂载等环境变更步骤在已满足条件时未重复执行。

### 1. 准备环境

- 目标：Ubuntu 22.04 LTS x86_64，内核 5.15 系列；服务与判题**不以 root 运行**。
- 运行前检查资源（内存约 3.3 GiB、历史环境**无 Swap**）：

  ```bash
  grep PRETTY_NAME /etc/os-release   # Ubuntu 22.04 LTS
  uname -r                           # 5.15.0-*
  nproc                              # CPU 核数（决定判题 worker 数）
  free -h                            # 运行前重新确认可用内存
  swapon --show || echo "无 swap"
  ```

- 沙箱所需内核能力检查（非特权用户命名空间、seccomp-bpf、tmpfs）见
  `dependence.md` 1.2 节。任一能力缺失时服务会拒绝启动，**不要关闭隔离来绕过**。

### 2. 获取项目

```bash
git clone <仓库地址> Vibe-Coding
cd Vibe-Coding
```

- `<仓库地址>` 为占位符，请替换为实际 Git 地址。
- 确认目录含 `CMakeLists.txt`、`src/`、`web/`、`scripts/`、`data/.gitkeep`。

### 3. 安装依赖

按 [`dependence.md`](dependence.md) 第 3 节安装。正式部署最小集合：

```bash
sudo apt update
sudo apt install -y build-essential cmake libcpp-httplib-dev nlohmann-json3-dev \
  libsqlite3-dev libargon2-dev libssl-dev

# jwt-cpp（header-only，源码安装；详见 dependence.md 3.5）
cd /tmp && git clone https://github.com/Thalhammer/jwt-cpp.git \
  && sudo cp -r jwt-cpp/include/jwt-cpp /usr/local/include/ && rm -rf jwt-cpp
```

- 备份需 `sqlite3`；定时备份需 `cron`（见 3.7 与「备份与恢复（M6.2）」）。
- 跑测试/回归再装 `libgtest-dev`、`curl`、`python3`。

### 4. 准备必要目录与权限

| 路径 | 用途 | 权限/属主 | 持久性 |
|---|---|---|---|
| `<项目>/data/` | `oj.db` 及 `oj.db-wal`/`oj.db-shm`/`oj.db.lock` | 服务账号可读写 | **持久化**（WAL/SHM/lock 为运行辅助） |
| `<项目>/backup/` | `scripts/backup.sh` 输出 `oj-YYYYMMDD.sql` | 服务账号可读写，文件 0600 | **持久化**（外部备份） |
| `/opt/oj-tmpfs` | 判题一次性工作目录根 | root 挂载，`mode=1777` | **临时**（tmpfs，重启消失） |
| `<项目>/build/` | 构建产物 | 可重建 | 临时 |
| `build/regression-logs/` | 冒烟回归服务日志 | 可重建 | 临时 |

```bash
mkdir -p data backup
# 挂载判题 tmpfs（正式部署必需；可写入 /etc/fstab 开机自动挂载）
sudo mkdir -p /opt/oj-tmpfs
sudo mount -t tmpfs -o size=512M,mode=1777 tmpfs /opt/oj-tmpfs
```

- 不挂载 tmpfs 时服务启动即失败（退出码 1）。无挂载权限的开发环境可显式
  `OJ_JUDGE_ALLOW_NON_TMPFS=1`（显著告警，仅开发/测试）。

### 5. 配置密钥与初始管理员密码

```bash
# JWT 签名密钥：必需，长度 ≥ 16 字节；用强随机值，勿用示例值
export OJ_JWT_SECRET="$(openssl rand -hex 32)"
# 初始管理员密码：仅“首次初始化且库中尚无 admin”时需要；占位符须替换为真实强口令
export OJ_ADMIN_PASSWORD='<首次初始化时设置的强密码>'
```

- 密钥与密码只从环境变量读取，**不写入源码、版本控制或日志**；真实配置勿放进 `web/`。
- 服务环境变量可用 systemd `Environment=`/`EnvironmentFile=` 提供（本轮不新增 unit）。

### 6. 构建

```bash
cmake -S . -B build
cmake --build build --parallel 1
```

生成 `build/oj_server`。**固定单并发** `--parallel 1`；**不要**使用无数量的 `-j`
或 `-j$(nproc)`（本项目目标机内存小，会耗尽内存甚至断连），详见「构建」一节。

### 7. 首次初始化与种子数据

- **首次初始化**（库中尚无 `admin`）：启动时自动创建 `data/`、`oj.db` 与表结构，
  并预置管理员 `admin`（`reset_pwd_flag=1`，首次登录强制改密）；此步骤需要
  `OJ_ADMIN_PASSWORD`，否则启动报错退出。
- **已有数据库**：正常重启**不会**重置管理员密码/角色、不会重复创建 admin、
  不会清空数据，也**不会**自动导入种子题。
- 种子题（41 道）仅在**显式** `--seed` 时导入，且幂等：

  ```bash
  ./build/oj_server --db data/oj.db --seed
  ```

### 8. 启动（一键入口）

优先复用已有入口 `scripts/run_dev_server.sh`：

```bash
OJ_JWT_SECRET="$(openssl rand -hex 32)" OJ_ADMIN_PASSWORD='<强密码>' \
  bash scripts/run_dev_server.sh
```

- 脚本会定位项目与 `build/oj_server`、读取 `OJ_DB`/`OJ_HOST`/`OJ_PORT`/`OJ_WEB`、
  检查必要前置条件（可执行文件、密钥、首次初始化密码）、优先选择可用的 tmpfs
  判题目录（`/opt/oj-tmpfs` → `/dev/shm`），然后 `exec` 启动服务，**正确透传停止
  信号与退出码**。
- 它**不会**自动安装依赖、修改系统配置、挂载文件系统、重置密码或恢复数据库；
  缺少必要前置条件（可执行文件、JWT 密钥、首次初始化密码）时以非零码清晰退出。
- **安全边界**：若既无 `/opt/oj-tmpfs` 也无 `/dev/shm`，脚本会**显式告警**并仅作为
  **开发例外**回退到非 tmpfs；正式部署必须先按第 4 步挂载 tmpfs（脚本会直接使用），
  或改用手动命令显式指定 `OJ_JUDGE_WORKSPACE`。**不要**用 `OJ_JUDGE_ALLOW_NON_TMPFS`
  在正式环境掩盖缺少 tmpfs 的问题。
- 默认监听 `0.0.0.0:8080`。也可手动启动：

  ```bash
  OJ_JWT_SECRET="$OJ_JWT_SECRET" OJ_ADMIN_PASSWORD="$OJ_ADMIN_PASSWORD" \
    ./build/oj_server --host 0.0.0.0 --port 8080 --db data/oj.db --web web
  ```

- 启动自检：

  ```bash
  curl -sS --max-time 5 http://127.0.0.1:8080/api/health   # 期望 {"status":"ok"}
  ```

### 9. 访问：本机与远程（含 Windows 端口转发）

- 本机访问：`http://127.0.0.1:<端口>/`（默认 `8080`）。
- 远程服务器：直接浏览器访问需使用服务器真实地址且防火墙/安全组放行端口；
  否则用 SSH 端口转发。
- **Windows + VS Code Remote-SSH**：在 VS Code「端口 / PORTS」面板转发 `8080`，
  再点击「在浏览器中打开」，使用面板给出的**实际本地地址**（通常是
  `http://localhost:<VS Code 分配的本地端口>`）。
- **不要默认 Windows 的 `127.0.0.1:8080` 就是云服务器的 `8080`**：未在转发面板
  暴露/未做端口转发时，Windows 本机该端口并无监听。页面空白或一直加载通常表示
  服务未启动或启动失败（见「故障排查」），而非浏览器问题。

### 10. 正常停止与再次启动

- 前台运行按 `Ctrl+C`（或 `SIGTERM`）优雅停止，见「停止服务」。
- 停止后确认无残留：`pgrep -a oj_server`；再次执行第 8 步即可重启，已有数据不重置。

### 11. 管理员与账号安全要点

- 默认管理员账号为 **`admin`**；初始密码由 `OJ_ADMIN_PASSWORD` 提供，不写死、
  不在文档或日志中出现。首次登录 `reset_pwd_flag=1`，**必须改密**后才能使用管理功能。
- 已有管理员**不会**因正常重启被重置；`--seed` 也不涉及 admin。
- 重置密码/改角色后旧 JWT 仍有效至 `exp`（**无会话撤销机制**），但每次鉴权按数据库
  最新角色与首改标记判定，权限变化实时生效；退出登录只清理前端凭证，不撤销后端 token。
- 真实密钥与口令不进入版本控制、日志或静态托管目录（`web/`）。

### 12. 其它入口

- 配置项明细见「配置方式」；数据位置见「数据位置与持久化」。
- 备份/恢复见「备份与恢复（M6.2）」；cron 需单独配置，本轮不安装。
- 故障处理见「故障排查」。

## 构建

```bash
cmake -S . -B build
cmake --build build --parallel 1
```

生成可执行程序 `build/oj_server`。

> 开发验证构建必须固定并发为 1：本项目目标机内存较小（约 3.3 GiB、无 Swap，与
> VS Code Server / OpenCode 共用资源）。裸 `-j`（无数量的 -j 会使 GNU Make 无限
> 并发）或 `-j$(nproc)` 会耗尽内存、引发严重 I/O 等待甚至 SSH 断连，故统一使用
> `cmake --build build --parallel 1`（或 `-j 1`），不要改回无数量的 `-j`。

## 测试

数据库集成测试（使用 `/tmp` 下的隔离临时库，不触碰 `data/oj.db`）：

```bash
ctest --test-dir build -R db_integration --output-on-failure
```

注册单元测试（gtest）与接口集成测试（同样使用隔离临时库与随机端口，不触碰正式数据库）：

```bash
ctest --test-dir build -R register_gtest --output-on-failure
ctest --test-dir build -R register_api --output-on-failure
# 或一次性运行全部测试
ctest --test-dir build --output-on-failure
```

覆盖建表与预置 admin、WAL / 外键、唯一性约束、非法外键、密码哈希、重复初始化幂等、
持久化及失败路径；以及注册成功、重复昵称、账号碰撞重试/耗尽、非法输入、越权字段、
并发同昵称、内部故障不泄露、持久化等。

改密与权限检查测试（单元 + 集成，同样使用隔离临时库与随机端口，不触碰正式数据库）：

```bash
ctest --test-dir build -R password_unit --output-on-failure
ctest --test-dir build -R password_api --output-on-failure
```

覆盖改密校验、首次改密检查、管理员权限检查组合、改密服务原子更新与并发；以及普通
用户改密、错误/非法输入拒绝、越权字段、admin 首改限制与绕过、角色变更后权限实时
生效、并发改密、内部故障不泄露、改密后旧 token 行为等。

题目数据与查询测试（单元 + 集成，同样使用隔离临时库与随机端口，不触碰正式数据库）：

```bash
ctest --test-dir build -R problems_unit --output-on-failure
ctest --test-dir build -R problems_api --output-on-failure
```

覆盖标签解析；以及种子导入内容与用例顺序、重复导入幂等不覆盖、游客/普通用户/管理员的
可见范围、首改限制、隐藏用例不泄露、空库、不存在/非法 ID、数据库故障与重启持久化。

判题器测试（M1.5，单元 + 编译/进程集成，使用隔离临时工作目录，不触数据库）：

```bash
ctest --test-dir build -R judge_unit --output-on-failure
ctest --test-dir build -R judge_integration --output-on-failure
```

`judge_unit` 用 gtest + FakeExecutor 覆盖输出归一化与比对（行尾空白、文末空行、
行首/行内空白、中间空行、空输出）、汇总规则（AC/WA/RE/TLE/MLE/SYSERR 与混合结果）、
语言解析，以及编译一次、顺序执行、WA 不阻断后续点、CE/SYSERR 分类、非法输入不返回
AC 等编排逻辑。`judge_integration` 用真实 g++/gcc 与 fork/exec 覆盖 C++17/C11 的
AC/WA、CE 诊断、每测试点新进程、超时终止与回收、超大输出有界采集、程序提前退出、
编译器不可用返回 SYSERR、临时目录与子进程清理。

提交接口与持久化测试（M1.6，单元 + 集成，隔离临时库 + 随机端口 + 可注入执行器）：

```bash
ctest --test-dir build -R submit_unit --output-on-failure
ctest --test-dir build -R submit_api --output-on-failure
```

`submit_unit` 覆盖语言取值解析、源码校验（空/纯空白/超长/边界）与做题状态计算
（首次失败、首次 AC、重复 AC 不覆盖、AC 后失败不清除）。`submit_api` 通过真实 HTTP
覆盖：真实 g++/gcc 的 C++17/C11 AC、WA/CE 返回与入库、源码完整保存、认证/首改/
可见性/参数校验被拒且不产生记录、额外字段无法改变归属或结果、首次失败与首次 AC 的
状态与计数、多用户多题独立、8 路并发不丢计数且只有一条状态记录、SYSERR 记录与计数、
持久化事务中途失败整体回滚、通过点不泄露隐藏用例、重启后源码/结果/次数/AC 状态/
首次 AC 时间保留。

判题任务调度测试（M3.1，调度单元 + HTTP 集成 + 真实并发判题）：

```bash
ctest --test-dir build -R judge_manager_unit --output-on-failure
ctest --test-dir build -R submit_scheduling_api --output-on-failure
```

`judge_manager_unit` 使用可控 handler 与同步屏障（不依赖长时间 sleep）覆盖：worker
数量规则（min(CPU,8)、CPU 无法获取时至少 1）、多个任务真正并发执行且活动数不超过
worker 上限、有界等待队列满载立即拒绝且并发入队不突破容量、每任务结果通道独立、
任务异常被转换且 worker 继续、停止排空已接收任务并拒绝新任务。`submit_scheduling_api`
通过真实 HTTP 与受控/真实执行器覆盖：队列满载 503 + 稳定 code + Retry-After 且未接收
不落库不计次、繁忙/满载时健康检查与题目查询仍可响应、执行器异常转 SYSERR 且 worker
恢复、并发同题计数与首次 AC 取最早原提交时间、排队时间不计入运行耗时且不误判 TLE、
真实 C++17/C11 并发判题结果正确且子进程无遗留、优雅停止取消已接收任务并交付明确结果、
5 用户并发持续提交（3 轮共 15 次真实判题）结果互不混用且计数一致。

判题子进程与超时控制测试（M3.2，截止时间单元 + 判题/调度集成）：

```bash
ctest --test-dir build -R judge_deadline_unit --output-on-failure
```

`judge_deadline_unit` 覆盖：单调时钟剩余时间（向上取整、到期为 0）、有效单点时限
=`min(题目时限, 剩余全局预算)`、默认全局 60s 上限、全局硬上限跨点累计并保留已有结果、
编译保护超时（CE）与全局裁剪超时（SYSERR）区分、预取消不启动进程与取消终止后续点。
`judge_integration`（M3.2 扩展）另以真实 g++/gcc 覆盖：全局硬上限、单点超时后继续执行、
编译保护超时、后代进程组清理、协作式取消终止、子进程不继承无关 fd、关闭标准输出、
输出匹配但异常退出判 `RE`。`submit_scheduling_api`（M3.2 扩展）覆盖停止服务时取消真实
运行与排队任务、取消结果先落库且无遗留进程。

运行隔离与资源限制测试（M3.3，单元 + 真实 Linux 进程集成）：

```bash
ctest --test-dir build -R sandbox_unit --output-on-failure
ctest --test-dir build -R sandbox_integration --output-on-failure
```

`sandbox_unit` 覆盖编译并发门限（上限/超时/取消/释放）、资源上限换算、seccomp 过滤器
构建、命名空间与 tmpfs 能力探测、`Workspace` 清理安全（不越界、不跟随符号链接）。
`sandbox_integration` 以真实 g++/gcc 与命名空间/chroot/seccomp 验证：正常 C++17/C11、
目录与网络与进程隔离、CPU/内存/输出上限、ASan/UBSan 默认接入与越界/UB 诊断、环境不泄露、
失败路径与遗留进程/目录清理。

编译与结果分类测试（M3.4，纯函数单元 + 真实进程集成）：

```bash
ctest --test-dir build -R judge_classification_unit --output-on-failure
ctest --test-dir build -R m34_classification --output-on-failure
```

`judge_classification_unit` 覆盖单点分类的确定性优先级（取消/启动失败/内存/全局裁剪/
单点超时/信号/非零退出/输出超限/AC/WA）、多迹象并存、Sanitizer 文本不单独判失败、
总体汇总的严重度与顺序无关性、编译诊断内部路径清洗。`m34_classification` 以真实
g++/gcc 与沙箱验证：两套语言默认 ASan/UBSan 模板的 AC/WA/CE、行尾空白规则、越界与
UB 判失败并保留诊断、伪 Sanitizer 文本不误判、TLE/RE/MLE（可靠 RSS 证据）、超大输出
失败与截断、编译诊断有界、编译器缺失/不可执行 `SYSERR` 后恢复、用户链接错误判 `CE`、
真实诊断不泄露内部路径、失败点后继续执行与混合优先级、逐点耗时/内存与编译耗时口径、
提交级内存取逐点峰值最大值。

持久化与停止清理测试（M3.5，清理单元 + 端到端集成，隔离临时库 + 受控执行器/同步门 +
故障注入）：

```bash
ctest --test-dir build -R m35_cleanup_unit --output-on-failure
ctest --test-dir build -R m35_persistence_shutdown --output-on-failure
```

`m35_cleanup_unit` 覆盖：真实清理失败时日志包含具体资源路径、保留现场且不误删其它
任务目录；资源已不存在时重复收尾不崩溃。`m35_persistence_shutdown` 覆盖：完整
AC/WA/CE/SYSERR 结果关闭重开数据库仍可读取（源码、逐点详情、编译信息、耗时/内存、
未采集指标以 null 表示）；提交与做题状态单事务失败整体回滚；数据库写锁短期竞争有界
等待成功、长期竞争明确失败且不永久挂起；同一任务正常完成与取消竞争下只保存一次、
计数精确；客户端在任务被接收后断开仍完成并保存、结果不被删除；空闲/编译中/结果保存
阶段停止与并发重复停止均安全收尾、停止中与停止后新提交返回 503 且不落库；停止后无
遗留子进程与判题目录；失败路径反复执行后文件描述符/线程/内存无持续增长；停止重启后
数据一致且可继续判题。

5 用户并发持续提交验证（真实 ASan 判题，编译门限 2，worker=5）位于
`submit_scheduling_api`：

```bash
ctest --test-dir build -R submit_scheduling_api --output-on-failure
```

`test_five_users_sustained_concurrency` 由 5 名用户并发提交、连续 3 轮（共 15 次真实
判题，AC/WA 交替），验证结果互不混用、每用户计数一致、无遗留子进程；本机实测最低可用
内存约 737 MiB，无 OOM。该场景据此前的「长时间压测」范围收敛为产品目标规模（5 人同时），
超出该目标的更高并发尚未验证。

Rejudge 测试（M3.6，集成，隔离临时库 + 随机端口 + 受控/真实执行器）：

```bash
ctest --test-dir build -R rejudge_api --output-on-failure
```

`rejudge_api` 覆盖：管理员权限与首次改密限制、非法/不存在提交 ID（401/403/400/404 且
不修改数据）、客户端夹带 `user_id`/`language`/`code`/`status`/`testcases` 不能替换原
提交；真实 g++/gcc 下 C++17 的 AC→WA→AC 与 C11 的 WA→AC→WA（修改当前用例后重判生效）；
原记录更新而保留 ID/归属/源码/语言/`created_at`，不新增记录、不增加 `submit_count`；
状态重算（最早 AC 被重判失效后 `first_ac_at` 更新为其余 AC 的最早原提交时间、唯一 AC
失效后清空为 `none`、`pass_count` 同步变化）；同一提交重复重判 `409 REJUDGE_IN_PROGRESS`、
不同提交排队、队列满载 `503 JUDGE_QUEUE_FULL`、完成后去重释放；SYSERR 与服务取消按既定
策略保留原结果与统计（返回 500）；客户端在重判执行中断开后，任务仍完成并更新原记录、
去重占用可靠释放（原始 socket 用例）；管理员可重判隐藏题目的提交；重启后重判结果与统计
保持一致（11 场景 / 116 项断言）。前端 `tests/frontend/admin_pages_dom.mjs` 的 S22 重判
场景已用 jsdom 完成 DOM 级执行，全量 **127/127** 通过（入口、确认与不增次数说明、等待态
禁用、结果与诊断展示、记录不存在与网络失败提示、不泄露无权信息）；执行中发现并修复
`web/js/pages/admin-rejudge.js` 成功提示被 `renderJudgeResult` 清除的缺陷（改为独立状态区），
并修正 S22 与 S21 的场景顺序（不得在自我降级后使用已降级的 admin token）。

管理员题目接口测试（M2.1，单元 + 集成，隔离临时库 + 随机端口 + 可注入执行器）：

```bash
ctest --test-dir build -R problem_admin_unit --output-on-failure
ctest --test-dir build -R admin_problems_api --output-on-failure
```

`problem_admin_unit` 覆盖创建/部分更新请求体的字段校验（必填、类型、长度、难度枚举、
标签结构、数值范围与默认值）及标签拼接。`admin_problems_api` 通过真实 HTTP 覆盖：
建题返回有效 ID 与默认限制、创建后可查询且不泄露隐藏用例、改题后查询与数据库一致且
`created_at` 保留/`updated_at` 更新、非法参数不产生记录与部分更新、游客/普通用户/
未改密管理员被拒且数据库不变、角色撤销后旧 token 失效、隐藏题对游客/普通用户不可见且
不可提交而管理员可见、重新公开恢复可见且不清除历史提交与状态、删除规则（无提交可删
无孤立、有提交 409）、删除与提交并发时数据一致、不存在/非法 ID、数据库故障不泄露、
重启后数据保留。

题目列表查询测试（M2.3，单元 + 集成，隔离临时库 + 随机端口 + 可注入执行器）：

```bash
ctest --test-dir build -R problem_list_query_unit --output-on-failure
ctest --test-dir build -R problems_list_api --output-on-failure
```

`problem_list_query_unit` 覆盖参数纯函数：空白裁剪、LIKE 特殊字符转义、标题/标签匹配
模式、难度取值，以及 `q`/`difficulty`/`tag`/`page`/`visible` 的归一化与非法值拒绝。
`problems_list_api` 通过真实 HTTP 覆盖：无条件查询与分页稳定性（45 道题跨页不重复、
不遗漏、末页与超出末页）、关键词（中文/大小写/引号/`%`/`_` 字面匹配）、难度与标签
精确筛选（图 ≠ 图论）、组合 AND、`total` 与可见范围一致不泄露隐藏题、通过人数口径
（重复 AC 不增加、仅失败不计入）、本人 `solved` 状态、游客不返回该字段、客户端不能
指定他人身份、游客/普通/已改密管理员/未改密管理员可见范围、管理员 `visible` 筛选、
列表不含隐藏用例与用户源码、非法参数 400、无效 token 401、数据库故障 500。

管理员用户接口测试（M2.4，单元 + 集成，隔离临时库 + 随机端口 + 可注入执行器）：

```bash
ctest --test-dir build -R user_admin_unit --output-on-failure
ctest --test-dir build -R admin_users_api --output-on-failure
```

`user_admin_unit` 覆盖请求参数纯函数：`action`（`reset_password`/`change_role`）、正整数
`user_id`、`new_password` 密码规则复用且不裁剪、`role` 枚举，以及未知操作/缺失字段/
错误类型/超长数字与 `page` 分页边界。`admin_users_api` 通过真实 HTTP 覆盖：用户列表
字段完整、按 `id` 升序稳定排序、分页与非法分页参数、响应不含密码哈希等敏感信息；未登录
`401`、普通用户 `403`、未完成首改管理员 `403 PASSWORD_CHANGE_REQUIRED` 且数据库不变；
重置密码后旧密码失效、新密码可登录、库内保存有效 argon2id 哈希并置 `reset_pwd_flag=1`、
普通用户提交被拦截且改密后恢复、旧 token 仍有效但权限按数据库最新值；提升后新角色立即
生效、降级后原 token 失去管理员权限；非法请求与不存在用户被拒且原数据不变、夹带
`account`/`nickname`/`role` 等字段不能越权；自我降级、最后管理员 `409` 与并发互降始终
保留一名管理员；修改密码/角色不改变账号、历史提交与做题状态；数据库故障 `500` 不泄露；
重启后密码与角色保留。

配置管理单元测试（基于 gtest，无外部依赖，不触碰数据库与网络）：

```bash
ctest --test-dir build -R config_unit --output-on-failure
```

覆盖命令行参数解析（`--host`/`--port`/`--db`/`--web`/`--help`）、端口校验、默认值与
组合参数、非法/未知参数、初始管理员密码环境变量读取，以及 JWT 配置
（`OJ_JWT_SECRET` / `OJ_JWT_EXPIRES_SECONDS`）的读取与边界校验。

静态资源托管测试（M1.7，基于 gtest，隔离临时库 + 临时 web 目录 + 随机端口）：

```bash
ctest --test-dir build -R static_files_unit --output-on-failure
```

`static_files_unit` 覆盖：`/` → `index.html` 及 HTML/CSS/JS 的 MIME；未知静态路径
返回 404；托管目录之外的同级文件不可访问；目录穿越（`..`、`%2e%2e`、`..%2f`、
`.git/config`、越界系统文件）被拒绝且不回显内容；空/不存在的 `web_root` 只跳过静态
托管并保持 `/api` 可用；静态托管不影响健康检查、题目列表与注册 POST。

### 端到端冒烟回归脚本（M6.1）

`scripts/regression.sh` 是端到端冒烟回归入口（**不是全量测试入口**）：在已有构建产物
基础上，一次完成隔离数据准备（临时库 + 内置种子题）、测试服务启动与健康检查、真实
HTTP 接口断言、结果汇总与统一清理。它验证的链路为：注册 → 登录 → 鉴权（`/api/me`）→
取题 → C++17/C11 已知 AC/WA 提交并按 JSON 判题状态断言 → 重启后本人历史持久化。

```bash
bash scripts/regression.sh                          # 自动挑选空闲端口与 tmpfs 判题目录
OJ_REGRESSION_PORT=18080 bash scripts/regression.sh
OJ_REGRESSION_KEEP=1 bash scripts/regression.sh     # 排查失败时保留临时目录
```

- 前置条件：先构建 `cmake --build build --parallel 1`；依赖 `curl`、`python3`（可靠
  JSON 解析与空闲端口选择），`openssl` 可选。脚本**不会自动构建或下载工具**。
- 隔离：独立 `mktemp` 临时库、随机测试 JWT 密钥与测试管理员密码、独立空闲端口、优先
  `/opt/oj-tmpfs` → `/dev/shm` 的 tmpfs 判题目录；不连接/修改 `data/oj.db`，不使用真实
  管理员密码，日志不写入密钥/token。
- 失败语义：任一关键断言失败、服务启动失败或健康检查超时均返回非零退出码，并打印
  场景、期望与实际；服务启动失败会保留诊断日志。`OJ_REGRESSION_INJECT_FAILURE=1`
  仅用于验证脚本自身失败路径（正常运行勿设）。
- 清理：`INT/TERM/EXIT` 统一清理，只终止本轮以记录 PID 启动的服务，先停服务再删除
  临时数据；完整服务日志保留在 `build/regression-logs/<时间戳>/server.log`（`build/`
  已被忽略）。
- 通过退出码 0；交互终端以绿色显示通过，重定向时为纯文本，颜色不代替断言与退出码。

### 全量常规回归与特殊测试

全量常规回归（已注册的常规单元 + 集成测试，固定单并发、串行）：

```bash
ctest --test-dir build --parallel 1 --output-on-failure --timeout 120
```

- 构建固定 `cmake --build build --parallel 1`；构建结束后再串行执行，遵守约 3.3 GiB
  服务器资源约束。
- 压力/超内存/死循环等**破坏性**测试有意不注册到 CTest：`oj_m53_special` 需通过
  `cmake --build build --target run_m53_special` 显式执行（自带内存自检）；常规回归
  `ctest -N` 不包含它。
- 前端逻辑/DOM 与真实浏览器验证是可选项，位于 `tests/frontend/`，不经 CTest 注册，
  用法见 `tests/frontend/README.md`。
- 失败排查入口：CTest 的 `--output-on-failure` 输出；单项直接执行对应
  `build/oj_<name>`；冒烟回归看 `build/regression-logs/<时间戳>/server.log`。

### 可选 C++ 覆盖率（非 M6.1 要求，不注册 CTest）

在独立目录 `build-cov/` 采集 gcov 覆盖率，生成 gcovr（行/分支）与 lcov（含函数）HTML 报告；
不修改 `build/` 与 `CMakeLists.txt`，不进入常规回归。

```bash
bash tests/coverage/install_tools_local.sh   # 免 sudo 安装 gcovr/lcov/genhtml
bash tests/coverage/run_coverage.sh          # 配置→单并发构建→串行 CTest→生成报告
# 报告：build-cov/coverage/index.html（gcovr）、build-cov/coverage/html-lcov/index.html（lcov）
```

说明与最近一次实测数据见 `tests/coverage/README.md` 与 `tests/M6.1-test-report.md`
第 10 节。覆盖率是可选 QA 增强，未采集时不得声称“全部函数/分支已覆盖”。

## 运行

```bash
./build/oj_server                     # 默认监听 0.0.0.0:8080
./build/oj_server --host 127.0.0.1 --port 9000
./build/oj_server --help
```

### 配置方式

命令行参数与环境变量。下表来自实际代码（`src/config.{h,cpp}`、`src/auth/jwt.cpp`）：

| 配置名称 | 用途 | 默认值 | 必填条件 | 取值范围 | 敏感性 |
|---|---|---|---|---|---|
| `--host` | HTTP 监听地址 | `0.0.0.0` | 可选 | 非空字符串 | 低（暴露面相关） |
| `--port` | HTTP 监听端口 | `8080` | 可选 | 整数 `1..65535` | 低 |
| `--db` | SQLite 数据库路径 | `data/oj.db` | 可选 | 非空路径 | 中（数据） |
| `--web` | 前端静态资源目录（仅该目录对外只读托管） | `web` | 可选 | 非空目录 | 中（勿指向项目根/敏感目录） |
| `--seed` | 仅导入内置种子题后退出，不启动服务（幂等） | 关闭 | 可选 | 布尔开关 | 低 |
| `OJ_ADMIN_PASSWORD` | 首次初始化（尚无 `admin`）时预置管理员初始密码 | 无 | **首次初始化时必需**；已有 admin 可省略 | 非空字符串 | **高**（口令，勿记录/入库明文） |
| `OJ_JWT_SECRET` | JWT HS256 签名密钥 | 无 | **必需** | ≥ 16 字节 | **高**（泄露可伪造 token） |
| `OJ_JWT_EXPIRES_SECONDS` | JWT 有效期 | `3600` | 可选 | 整数 `1..31536000` | 低 |
| `OJ_JUDGE_QUEUE_CAPACITY` | 判题等待队列容量（等待执行的任务数，非正在执行数） | `32` | 可选 | 整数 `1..256` | 低 |
| `OJ_JUDGE_WORKSPACE` | 判题工作目录根（应为 tmpfs） | `/opt/oj-tmpfs` | 可选 | 可写目录路径 | 中（隔离） |
| `OJ_JUDGE_ALLOW_NON_TMPFS` | 允许工作目录非 tmpfs（仅开发/测试，显著告警） | 未设置 | 可选 | `1/true/yes` 或 `0/false/no` | 中（降低隔离，正式部署勿设） |
| `OJ_JUDGE_COMPILE_CONCURRENCY` | 编译阶段并发门限（运行阶段并发仍为 `min(CPU 核数, 8)`） | `2` | 可选 | 整数 `1..64` | 低 |

- 备份脚本另有独立配置（`OJ_DB`/`OJ_BACKUP_DIR`/`OJ_BACKUP_TZ` 等），见「备份与恢复（M6.2）」。
- 上表仅列实际存在的配置，不包含未实现的参数。示例中的值均为**占位符**，不能直接
  当作安全生产配置；真实密钥/口令只放环境变量。
- 非法输入（如 `--port abc`、`--port 0`、未知参数、`OJ_JUDGE_QUEUE_CAPACITY=0`、
  `OJ_JUDGE_COMPILE_CONCURRENCY=0`、缺失或过短的 `OJ_JWT_SECRET`）会打印错误信息并
  以非零返回码退出；端口被占用或地址不可用时同样报错退出。

### 数据库与初始管理员

- 首次启动自动创建数据目录、`oj.db` 及六张业务表（`users`/`problems`/`testcases`/`submissions`/`user_problem_status`，以及 M3.7 的 `in_flight_tasks`），并预置管理员 `admin`（角色 `admin`，`reset_pwd_flag=1`，首次登录强制改密）。
- 初始管理员密码通过环境变量 `OJ_ADMIN_PASSWORD` 提供，仅以 argon2id 哈希落库，不写入源码、版本控制或日志。示例：

  ```bash
  OJ_ADMIN_PASSWORD='请改为强密码' ./build/oj_server
  ```

- 数据库中已有 `admin` 时无需再设置该变量；重复启动不会重复创建 admin，也不会覆盖密码或重置首次改密标记。
- 数据库未就绪（缺少初始密码、路径不可写等）时，服务在开始监听前报错并以非零返回码退出。

### JWT 密钥配置

- `OJ_JWT_SECRET` 为**必填**，长度不少于 16 字节，无默认值。缺失、为空或过短时服务在开始监听前报错并以非零返回码退出，绝不使用公开默认密钥启动。
- 建议使用 `openssl rand -hex 32` 生成强随机密钥；密钥只从环境变量读取，不写入源码、版本控制或日志。
- 保持同一密钥重启服务后，重启前签发且未过期的 token 仍可继续验证；更换密钥会使已有 token 立即失效。
- 生成示例：

  ```bash
  OJ_JWT_SECRET="$(openssl rand -hex 32)" \
  OJ_ADMIN_PASSWORD='请改为强密码' ./build/oj_server
  ```

### 访问验证接口

```bash
curl -i http://127.0.0.1:8080/api/health
```

正常响应：

```
HTTP/1.1 200 OK
Content-Type: application/json

{"status":"ok"}
```

### 前端页面（M1.7，最小实现）

服务启动后，浏览器直接访问根路径即可打开前端（默认 <http://127.0.0.1:8080/>）：

```bash
# 推荐：辅助脚本自动选择可用的判题 tmpfs 并在首次初始化时校验 admin 密码
OJ_JWT_SECRET="$(openssl rand -hex 32)" OJ_ADMIN_PASSWORD='请改为强密码' \
  bash scripts/run_dev_server.sh
# 浏览器打开 http://127.0.0.1:8080/
```

也可手动启动（**注意**：判题工作目录必须为 tmpfs，否则服务启动即退出，退出码 1，
端口转发后页面会空白/一直加载；本机 `/opt/oj-tmpfs` 未挂载时可设
`OJ_JUDGE_WORKSPACE=/dev/shm` 或开发用 `OJ_JUDGE_ALLOW_NON_TMPFS=1`）：

```bash
OJ_JWT_SECRET="$(openssl rand -hex 32)" OJ_ADMIN_PASSWORD='请改为强密码' \
  OJ_JUDGE_WORKSPACE=/dev/shm ./build/oj_server --db data/oj.db --web web
# 启动后先自检：curl -sS --max-time 5 http://127.0.0.1:8080/api/health
```

- **静态托管**：cpp-httplib 仅把 `--web`（默认 `web/`）目录只读挂载到 URL 根路径 `/`，并处理目录下的 `index.html`。项目根目录、`data/oj.db`、`src/`、配置文件与判题临时目录都不在托管范围内；`..` 与 URL 编码的越界路径由路径校验拦截并返回 `404`（可自行验证：`curl -i --path-as-is http://127.0.0.1:8080/SPEC.md`、`/data/oj.db`、`/../SPEC.md`、`/%2e%2e/SPEC.md` 均为 `404`）。`/api/*` 路由与 `/api/health` 行为保持不变。
- **启动排错**：若端口转发后页面空白/一直加载，先确认服务确在监听（`ss -ltnp | grep 8080`）且 `curl --max-time 5 http://127.0.0.1:8080/api/health` 返回 `{"status":"ok"}`。常见原因是服务未启动或启动失败：① 判题目录非 tmpfs（加 `OJ_JUDGE_WORKSPACE`）；② 首次初始化未设 `OJ_ADMIN_PASSWORD`。两者都会让 `oj_server` 以非零码退出。`scripts/run_dev_server.sh` 会提前报错并给出提示。
- **请求超时**：`web/js/api.js` 对所有请求设 15s 上限（`DEFAULT_TIMEOUT_MS`，可用 `timeoutMs` 覆盖）。后端不可达或连接被接受却无响应时，界面会显示「请求超时/网络连接失败」并可重试，而不是永久停留在「加载中」；超时不会清除本地 token。
- **无构建流程**：纯原生 HTML/CSS/ES Module，无打包器、无 React/Vue 等框架。本阶段源码编辑器为 `textarea`，CodeMirror 属 M4.3。
- **hash 路由**：`#/problems`（列表）、`#/problems/{id}`（题目页）、`#/login`、`#/register`、`#/password`。游客可浏览公开题目；`#/password` 为受保护路由，未登录时重定向到登录页并携带 `redirect` 参数。返回目标的完整规则见「前端基础设施（M4.1）」——登录/注册/改密等流程页不作为返回目标，避免流程互相回跳。
- **统一 API 封装**（`web/js/api.js`）：负责 JSON 序列化/解析、`Authorization: Bearer <token>`、HTTP 错误与网络异常归一化。token 保存在浏览器 `localStorage`，仅放入请求头，不进入 URL 或日志。
- **认证行为**：身份失效（`401`）会清理本地凭证并跳转登录页（内部去重，避免并发请求重复跳转）；登录失败只显示表单错误；改密接口的「旧密码错误」按表单错误处理，不会误退出登录；后端返回 `code:"PASSWORD_CHANGE_REQUIRED"` 时引导到改密页。
- **功能范围**：注册（成功显著展示系统分配的 10 位账号并引导用该账号登录，不依赖未实现的自动登录）、登录（保存 token 与用户状态、导航显示昵称、退出登录）、改密（沿用后端字段与密码规则，admin 首登强制引导）、题目列表（题目 ID/标题/难度/标签，含加载中、空列表、加载失败状态）、题目页（左侧题面+公开样例+难度标签+时空限制，右侧语言选择+`textarea`+提交+逐点结果）。退出登录只清理前端凭证，不声称已撤销后端 JWT。
- **结果展示**：展示提交 ID、总体状态、逐点结果、编译信息、诊断，以及 WA 失败点的输入/期望输出/实际输出；后端未采集的内存显示为「未采集」而非真实的 `0`；不为展示结果额外获取隐藏用例。
- **提交行为**：请求期间按钮禁用并显示「判题中」，避免重复提交；失败后保留编辑器源码、恢复可操作状态且不自动重试；网络中断时说明「结果无法确认」，不断言后端未保存提交。
- **输出安全**：题面、样例、昵称、编译信息与程序输出一律通过 `textContent`/`<pre>` 作为纯文本渲染，HTML 特殊字符不会被解释执行。
- **响应式**：题目页左右两栏，窗口宽度 ≤ 900px 时改为上下排列；长题面与长输出可滚动阅读。

> 安全边界：本阶段前端可提交并通过后端沙箱判题（M3.3），且自 M3.4 起默认全开
> ASan/UBSan 并完成统一异常分类，但仍不代表可安全公开运行任意不可信代码。

#### M1.7 前端验证

内网环境未安装可用的图形/无头浏览器，且浏览器二进制下载受限，故未使用真实
Chrome/Firefox 渲染；页面验证由两部分完成：真实启动服务后的 HTTP 级检查，以及在
jsdom 中真实执行 `web/js` 模块（连接同一服务）的 DOM 级验证。已验证：

- HTTP 级 54 项全部通过：页面/CSS/JS 均 `200` 且 MIME 正确；`/api/health` 与现有
  API 正常；`/SPEC.md`、`/data/oj.db`、`/src/main.cpp`、`/../SPEC.md`、`/%2e%2e/SPEC.md`
  等静态路径均 `404`；注册/重复昵称/错误密码、公开与隐藏题可见性、C++17/C11 的
  AC/WA/CE、admin 首改限制与改密、无效 token 等行为符合约定。
- DOM 级 55 项全部通过：真实跑通注册（展示 10 位账号）→ 登录（保存 token、导航显示
  昵称）→ 题目列表/详情 → C++17 与 C11 提交 AC → WA（含输入/期望/实际输出）→ CE
  （编译信息）→ 查看结果；以及改密（错误旧密码提示、正确改密继续操作）、退出登录清
  理凭证、token 失效跳转、游客提交受控、提交期间禁用按钮、失败保留源码且不自动重试、
  HTML 特殊字符按文本显示、无脚本错误。

**手动验证步骤**（可在有图形浏览器的机器上复现）：

```bash
# 1) 隔离启动（勿使用正式 data/oj.db）
./build/oj_server --db /tmp/oj-web.db --seed
OJ_JWT_SECRET="$(openssl rand -hex 32)" OJ_ADMIN_PASSWORD='请改为强密码' \
  ./build/oj_server --db /tmp/oj-web.db --host 127.0.0.1 --port 8080

# 2) 浏览器打开 http://127.0.0.1:8080/
#    - 打开 DevTools Console/Network，确认无脚本错误与资源 404
#    - #/register 注册，记下 10 位账号 → #/login 用该账号登录
#    - #/problems 进入题目 → 右侧输入 C++17/C11 代码 → 提交 → 查看 AC/WA/CE 结果
#    - 先用 admin/OJ_ADMIN_PASSWORD 登录：应进入 #/password 强制改密
#    - 退出登录后访问 #/problems/{id}，提交按钮应禁用并提示登录
#    - 缩小窗口至 <900px，确认题目页改为上下排列且控件可操作
#    - 访问 /SPEC.md、/data/oj.db、/../SPEC.md 确认返回 404
```

> jsdom 仅用于本次验证、不属于项目运行依赖，未纳入仓库（保持前端无构建、无测试框架）。
> 真实浏览器渲染、真机窄屏适配与手动点击验证为未验证项。

### 后台管理页面（M2.5）

沿用 M1.7 的原生 HTML/CSS/ES Module 与 hash 路由，未引入任何前端框架或构建流程。
后台入口仅对「已登录 + 已完成首次改密 + 当前角色为 `admin`」的账号显示（导航栏与页脚），
直接访问后台路由时前端同样检查身份、角色与首改状态；**前端检查只用于页面体验，真正的
权限仍由后端接口按数据库最新状态执行**。

- **路由**：`#/admin`（总览）、`#/admin/problems`（题目管理）、`#/admin/problems/new`
  （新建）、`#/admin/problems/{id}/edit`（编辑）、`#/admin/problems/{id}/testcases`
  （测试用例）、`#/admin/users`（用户管理）。
- **访问行为**：游客访问后台会重定向到登录页并携带 `redirect`；已登录的普通用户进入
  后台显示「无权访问后台」；未完成首次改密的管理员被引导到 `#/password`；在使用过程中
  权限被撤销（后端返回 `403`）时，前端刷新本人状态并退出后台页面。`401` 清理本地凭证
  并跳转登录，不把所有 `403` 都当作 token 失效。
- **题目管理**：列表接入 `GET /api/problems` 的分页与可见性筛选（`page`/`visible`，另附
  标题搜索与难度筛选），展示题目 ID、标题、难度、标签、可见性与通过人数；提供新建、
  编辑、用例管理与删除入口。
- **创建 / 编辑表单**：覆盖标题、纯文本题面、公开样例、难度、标签、时间限制（**毫秒
  ms**，默认 2000）、内存上限（**千字节 KB**，默认 65536）与可见性；字段与单位严格沿用
  后端约定，不做 ms/秒、KB/MB 混用；前端做基本校验，最终以服务端校验为准。公开样例由
  题目表单整体维护。
- **公开 / 隐藏**：通过 `PUT /api/admin/problems/{id}` 修改 `visible`，成功后重新拉取列表；
  隐藏题目对普通用户不可见，管理员仍可见。失败时保留真实状态并展示原因，不显示假成功。
- **删除**：确认对话框展示题目身份与既定删除策略（已有提交则拒绝；无提交则连同用例、
  做题状态一并删除）。后端返回 `409` 时解释「已有提交记录，不能删除」，不自动级联、
  也不用隐藏操作替代删除。
- **测试用例**：读取 `GET /api/admin/problems/{id}/testcases`，按 `(ord ASC, id ASC)` 展示；
  公开样例只读（由题目表单维护），隐藏用例支持新增、编辑、删除与 `ord` 排序。输入/输出
  原样保存，不做 trim 或换行归一化；所有写请求同时携带题目 ID 与用例 ID，避免串题。
- **用户管理**：`GET /api/admin/users` 分页列表（账号、昵称、角色、首改标记、注册时间），
  提供密码重置与角色修改，不提供删除用户。重置密码使用密码输入控件，不显示明文、不写
  日志、不做浏览器持久化，成功后清空并提示「下次登录后需改密」；角色修改展示目标用户、
  原角色与新角色并确认，正确展示「最后一个管理员」保护与自我降级，自我降级成功后立即
  更新本机状态并退出后台。
- **状态与安全**：所有页面覆盖加载中、空列表、保存中、成功与失败状态；写入请求进行中
  禁用重复提交，失败后恢复操作并尽量保留非敏感编辑内容；网络失败明确说明「无法确认后
  端是否已执行」，不自动重试写入。题面、标签、昵称、用例与错误信息全部按纯文本渲染，
  HTML 特殊字符不会被解释执行；隐藏用例与用户列表不写入浏览器持久化缓存。

**手动验证步骤**（可在有图形浏览器的机器上复现）：

```bash
# 1) 隔离启动（勿使用正式 data/oj.db）
./build/oj_server --db /tmp/oj-admin.db --seed
OJ_JWT_SECRET="$(openssl rand -hex 32)" OJ_ADMIN_PASSWORD='请改为强密码' \
  ./build/oj_server --db /tmp/oj-admin.db --host 127.0.0.1 --port 8080

# 2) 浏览器打开 http://127.0.0.1:8080/
#    - 先用 admin/OJ_ADMIN_PASSWORD 登录：应进入 #/password 强制改密
#    - 改密后导航出现「管理后台」→ #/admin/problems 新建题目（含公开样例与时空限制）
#    - 设为隐藏后，用普通用户访问 #/problems 确认不可见；管理员仍可在后台看到
#    - 进入某题的「用例」页新增隐藏用例（含空输出/空格/换行），确认原样保存
#    - 删除无提交的题目按既定策略执行；删除已有提交的题目应提示冲突
#    - #/admin/users 重置普通用户密码：旧密码不能登录、新密码可登录并需改密
#    - 修改角色（提升/降级）；仅剩一名管理员时降级应被拒绝
#    - 打开 DevTools Console/Network，确认无脚本错误与接口异常
```

> 页面验证使用真实服务 + jsdom 执行 `web/js` 模块，可重复运行脚本见 `tests/frontend/`
> （可选、不注册 CTest；需外部安装 jsdom）；结果与回归见 `tests/M2.5-test-report.md`。
> 真实图形浏览器渲染与手动点击仍属未验证项。

### 前端基础设施（M4.1）

在 M1.7/M2.5 已有前端基础上统一路由、身份状态、错误处理与登录回跳，未引入新框架或
构建流程，也不再新增重复的路由或认证体系。相关模块：`web/js/router.js`（路由与访问
条件）、`web/js/auth.js`（身份状态机与凭证）、`web/js/session.js`（`/api/me` 核实）、
`web/js/api.js`（请求封装与错误分类）、`web/js/lifecycle.js`（页面生命周期）、
`web/js/storage.js`（登录后返回目标）、`web/js/nav.js`（导航）。

- **路由接入与访问条件**：全部已实现页面接入同一 hash 路由表——`#/problems`、
  `#/problems/{id}`、`#/leaderboard`、`#/submissions`、`#/submissions/{id}`、
  `#/login`、`#/register`、`#/password`、`#/admin`、`#/admin/problems`、
  `#/admin/problems/new`、`#/admin/problems/{id}/edit`、
  `#/admin/problems/{id}/testcases`、`#/admin/users`、`#/admin/rejudge`（已保留 Rejudge
  入口）。路由以 `access`（`public`/`auth`/`admin`）声明访问条件，由 `router.js` 统一判定，
  页面不再各自分散判断。默认空 hash → `#/problems`；未知路由显示「页面不存在」；非法路由
  参数/编码异常显示「地址参数无效」；直接打开带 hash 的地址、刷新与前进后退均走同一套规则。
  提交历史页（M4.4）与排行榜页（M4.5）已在后续阶段接入导航与路由（见「提交历史与详情页
  （M4.4）」「排行榜页（M4.5）」）；导航与页脚随对应页面实现提供入口。
- **身份状态**：`auth.js` 区分 `unknown`（有 token 待核实）、`guest`、`authenticated`
  三种状态。恢复会话时由 `session.js` 通过 `GET /api/me` 核实当前用户，**后台访问
  权限依据服务端最新角色与 `reset_pwd_flag`，不凭本地保存的角色**；核实前导航不显示
  后台入口或用户昵称，避免短暂错误展示。角色/首改状态经 `setUser` 更新后自动刷新导航
  与路由判断。
- **统一错误处理**：`api.js` 携带 `Authorization: Bearer <token>`、解析 JSON、归类
  HTTP 与网络异常，并保留 `status`/`code`/`retryable` 供页面按接口约定分支（不依赖中文
  文案匹配）。受保护请求 `401` 清理失效凭证并引导登录（去重）；登录失败 `401` 只显示
  表单错误；改密旧密码错误 `401` 按表单错误处理、不退出登录；`403
  PASSWORD_CHANGE_REQUIRED` 进入改密流程；普通 `403` 显示权限不足并在必要时刷新身份；
  `404/409/429/503` 显示对应业务提示（如 `JUDGE_QUEUE_FULL` 不自动重试）；网络中断或
  非预期响应恢复可操作状态并准确提示。并发 401 或改密要求只触发一次状态转换与跳转。
- **登录后返回原目标**：未登录访问受保护页面时，目标路径经 `sanitizeTarget` 校验后存入
  sessionStorage，并在登录页 `redirect` 参数中携带；登录成功后 `completePostAuthRedirect`
  重新校验目标合法性**与当前用户权限**再跳转——只接受站内、可匹配路由且非流程页的地址，
  拒绝外部 URL、`//` 协议地址与含反斜杠/控制字符/恶意编码的地址，避免开放重定向；普通
  用户不会因目标指向后台而越权，无效/无权目标回退到 `#/problems` 并说明原因。登录/注册/
  改密流程页不互相回跳。注册成功仍展示分配的 10 位账号，不改为自动登录。
- **首次强制改密流程**：登录响应、`GET /api/me` 与后端 `403 PASSWORD_CHANGE_REQUIRED`
  任一触发都进入同一改密流程。登录后若 `reset_pwd_flag=1`，保留原目标先到 `#/password`；
  改密页本身允许该用户访问，不被路由守卫反复拦截；未完成改密的账号进入后台会被引导到
  改密页。改密成功后回查 `/api/me` 刷新状态，再按当前权限返回原目标。沿用既有 JWT 策略：
  改密不清除旧 token，界面不声称已撤销服务端 token。
- **页面生命周期**：`router.js` 为每次渲染创建 `lifecycle`，切换页面时统一清理并中止
  未完成的读取请求（`AbortController` / 过期响应丢弃），避免旧响应覆盖新页面、重复进入
  累积监听或重复触发。**写请求（提交、Rejudge、增删改）不随页面切换取消**：前端停止
  等待不等于后端已取消，继续沿用「网络失败时结果无法确认、不自动重试」的规则。退出
  登录清理本地凭证、用户状态、待返回目标与受保护页面数据，并通过 `epoch` 丢弃退出前
  发起、退出后才返回的旧请求，避免其恢复已退出的身份。
- **范围与边界**：本轮仅做基础设施与已有页面的必要适配，未重做全站视觉，未提前实现
  M4.2 完整题目列表、M4.3 CodeMirror、M4.4 提交历史与 M4.5 排行榜（其中 M4.2～M4.4
  已在后续阶段分别实现）；对应导航入口在不具备时保持不提供。所有敏感操作仍依赖后端
  鉴权，前端检查仅用于页面体验。昵称、错误提示与接口文本一律经 `textContent`/`<pre>`
  纯文本渲染；token 不进入 URL、日志或错误提示；后台敏感数据不新增浏览器持久化缓存。

> 验证：逻辑层 `tests/frontend/m41_logic_test.mjs`（73 项）与页面级
> `tests/frontend/m41_infrastructure_dom.mjs`（45 项，`run_m41.sh`）通过；真实浏览器
> `tests/frontend/browser/`（`playwright-cli` + 真实 Chromium，桌面与 360×640/390×844/
> 768×1024 窄视口，37 项）通过；M2.5 后台页面 DOM 回归 128 项通过。完整逐项结果见
> `tests/M4.1-test-report.md`。Windows 有头复核脚本已就绪（本环境未执行）。

### 题目列表页（M4.2）

在 M1.7 基础列表上完善为 SPEC 2.6.1 主页结构，复用 M4.1 的路由、身份、错误处理与
生命周期，未新增框架。页面实现位于 `web/js/pages/problems.js`，标签选项接口见
「题目标签选项接口」，返回目标存储见 `web/js/storage.js`，通用紧凑分页见
`web/js/util.js`。

- **列结构**：`#`、本人状态（仅登录用户；已 AC 显示 `AC`，否则 `未AC`，不把未 AC 细分
  为后端未提供的状态）、标题、难度（`easy/medium/hard` → 易/中/难）、标签、通过人数。
  管理员额外显示可见性列（公开/隐藏）。昵称、标题、标签等一律经 `textContent` 纯文本
  渲染，接口文本不作为 HTML 执行。
- **组合筛选**：搜索关键字（`q`，标题子串）、难度下拉（`difficulty`）、标签下拉
  （`tag`）可组合；管理员的「可见性」下拉（`visible`，全部/公开/隐藏）仅对通过权限检查
  的管理员显示与发送。搜索通过「搜索」按钮或输入框回车触发（不做逐键请求），难度/
  标签/可见性在变更时立即应用；任一条件变化回到第 1 页。空值不写入请求，界面「全部」
  不是实际难度或标签；「清空条件」恢复不限条件。
- **标签来源**：标签选项来自 `GET /api/problem-tags`，按当前身份可见范围返回去重、
  稳定排序的完整标签集合，不从前端分页结果拼凑；每会话按模块级缓存复用并在进入页面
  时刷新，已选标签即使不在选项中也会保留，翻页或一次请求失败不会使其消失。
- **分页**：使用后端 `page`/`page_size`（固定 20）/`total`/`total_pages`，展示总题数与
  当前页，提供上一页/下一页与紧凑页码范围（页数多时用省略号，不生成海量按钮）。超出
  末页或筛选后为空但页码 > 1 时，前端修正到合法页并重取，避免空白或请求循环。不在前端
  对当前页数据二次分页。
- **路由状态**：搜索、难度、标签、页码（管理员另含可见性）全部承载于 hash 查询串
  `#/problems?q=&difficulty=&tag=&page=&visible=`，通过 `URLSearchParams` 编码中文、
  空格与特殊字符。刷新、浏览器前进后退由 URL 恢复；从详情页返回时使用列表页记录的地址
  （`sessionStorage`）恢复原条件与页码并重新拉取，使刚完成的 AC 与通过人数得以更新。
  URL 中非法难度/可见性归一为「不限」、非法页码归一为 1，避免反复请求错误或一直加载。
- **行点击与可访问性**：标题为真实链接（支持键盘、中键/新标签打开）；整行可点击进入
  详情；点击行内链接/按钮等交互控件不会误触发跳转。
- **身份变化与管理员范围**：身份、角色或首次改密状态变化时按最新状态重建列表；退出或
  被降级后清除本人状态列与管理员内容，原先的隐藏题目筛选回到合法普通列表条件。后端
  仍执行最终可见性检查，普通用户/游客即使残留 `visible` 参数也只看公开题。
- **请求状态**：区分首次加载、条件更新、空题库（无筛选时）、无匹配结果（有筛选时，提供
  清空入口）与请求失败（提供重试）；读取请求可被取消或按代次丢弃，只有最新结果更新
  列表与分页；页面销毁时取消未完成读取并解除身份订阅，避免重复进入累积请求。
- **范围**：本轮仅实现题目列表，不进入 M4.3（CodeMirror/做题页）、M4.4 提交历史与
  M4.5 排行榜；不重做全站视觉，不提供会进入空白页的导航入口。

### 题目与做题页面（M4.3）

在 M1.7 题目页基础上完善左侧题面/样例/限制/本人状态，并用 CodeMirror 替换
`textarea`。页面实现位于 `web/js/pages/problem.js`，编辑器接入位于
`web/js/editor.js`，结果渲染位于 `web/js/judge.js`，复用 M4.1 的路由、身份、
错误处理与生命周期，未引入构建流程或前端框架。

- **编辑器（CodeMirror 5）**：通过 CDN 引入 **固定版本 5.65.21**（cdnjs，
  `https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.21/`），加载
  `codemirror.min.css`、`codemirror.min.js`、`mode/clike/clike.min.js` 及
  `matchbrackets`/`closebrackets`/`active-line`/`placeholder` 四个 addon；只使用
  5.x API，不使用 `latest`。C/C++ 高亮模式按语言选择映射为
  `text/x-c++src`（`cpp17`）与 `text/x-csrc`（`c11`）。切换语言只更新模式，
  **绝不清空源码**，也不使用会覆盖已编辑内容的语言模板。
- **加载失败与降级**：脚本/样式以动态 `<link>`/`<script>` 注入并设 8 秒超时，
  CDN 不可达或超时不会阻塞首屏（不会让整个应用一直加载）。加载或初始化失败时
  **保留可编辑的 `textarea#source-code`** 并给出明确提示，源码仍可正常提交；
  编辑器就绪前，提交逻辑通过 `getValue()` 读取 `textarea`，始终有确定的数据来源。
- **快捷键与提交**：提交按钮与 `Ctrl+Enter` 共用同一提交入口，执行相同的登录、
  必要改密、语言、源码与提交中检查；CodeMirror 快捷键与降级文本框监听不会同时
  触发，且 `submitting` 标志去重，不会重复提交。提交瞬间保存题目 ID、语言与源码
  快照；等待期间继续编辑不改变已发出的提交，结果也只描述本次快照。
- **提交状态**：请求期间禁用按钮并显示「判题中」，不伪造排队位置/百分比/进度；
  `WA/CE/TLE/RE/MLE/SYSERR` 按后端响应作为判题结果展示；队列满载（503）、身份
  失效（401）、权限不足（403/404）与网络异常沿用 M4.1 的处理。网络中断说明
  「结果无法确认」、保留源码与语言选择、不自动重试；页面切换后旧提交响应不写入
  新页面（前端停止等待不代表后端取消，后端结果仍会落库）。
- **结果展示**：展示提交 ID、总体状态、运行耗时（程序执行，不含排队与编译）、
  编译耗时、峰值内存与提交时间；未采集指标显示「未采集」而非 `0`。逐测试点按
  后端顺序展示状态、耗时、内存与结构化原因；全局硬上限/服务取消/内部故障导致的
  部分结果明确说明未全部执行，未执行点不伪造成通过，后端未提供总数时不猜测剩余。
  WA 点展示输入、期望输出与实际输出（保留空白换行，区分「空字符串」与「后端未
  提供」，输出截断明确标识）；编译诊断、程序标准错误与标准输出分开展示，截断信息
  明确标识；长输出与多测试点可滚动查看。所有内容按纯文本渲染，不执行其中的 HTML。
- **本人状态**：左侧显示登录用户本人该题 AC/未 AC 状态，游客不显示；数据来自
  详情接口的 `solved`（后端 `user_problem_status`）。判题结果更新后重新从该接口
  刷新本人状态——当前提交 WA 不代表历史 AC 失效，不把本次总体结果直接映射为
  永久状态；这与 M4.2 返回列表时重新拉取状态的行为一致。
- **释放与布局**：页面销毁时通过 `lifecycle.onDispose` 调用 `toTextArea()` 还原、
  断开 `ResizeObserver` 与窗口监听，重复进入不会产生重复编辑器或重复提交事件；
  布局变化时刷新编辑器尺寸。桌面保持左题面右编辑，窄视口（≤900px）改为上下排列。
- **接口补充**：详情接口新增 `solved`（见「题目详情接口」，仅登录返回），提交响应
  在 M3.4 字段基础上补充逐点 `output_truncated` 与提交级 `global_deadline_hit`/
  `cancelled`，用于明确展示截断与「未全部执行」的原因；未新增隐藏用例下发路径。
- **范围**：本轮不实现 M4.4 完整提交历史/`GET /api/status` 与 M4.5 排行榜，不改写
  判题分类/统计/崩溃恢复规则；未额外持久化草稿、隐藏用例或诊断数据。本页不预加载
  隐藏用例、不请求管理员用例接口。

### 提交历史与详情页（M4.4）

在 M4.1～M4.3 基础上新增本人提交历史页 `#/submissions` 与提交详情页
`#/submissions/{id}`，并接入顶部导航（登录后显示「提交历史」）。页面实现位于
`web/js/pages/submissions.js`（历史）与 `web/js/pages/submission-detail.js`（详情），
复用 M4.1 的路由/身份/错误处理/生命周期与 M4.3 的 `renderJudgeResult` 结果组件，
未引入构建流程或前端框架。

- **历史列表**：调用 `GET /api/submissions?mine`，展示提交 ID、题目、语言、状态、
  耗时、内存与提交时间；不含源码、逐点结果或 WA 用例详情。分页沿用「每页 20 条」
  与紧凑页码，查询条件（`page`、`problem_id`）承载于 hash 路由查询串，刷新与前进
  后退可恢复；超出末页自动修正。点击提交 ID 进入详情，点击标题进入题目页（题目页
  仍由题目接口独立执行可见性检查，隐藏题目不会因历史链接而绕过）。
- **本题提交入口**：题目页（登录用户）提供「查看本题提交记录 →」，跳转到
  `#/submissions?problem_id=N`；后端按该题目筛选本人全部提交（不是仅筛选当前页），
  空结果给出明确提示与返回入口。
- **详情**：调用 `GET /api/submissions/{id}`，展示数据库保存的完整源码、语言、
  总体状态、编译信息、逐点结果、运行指标与原提交时间。源码使用**只读文本区域**
  （`textarea[readonly]`，纯文本、保留换行、可滚动），本页不提供提交入口，浏览历史
  不会误提交代码。未持久化的指标（如编译耗时）显示「未采集」，不把 `null` 显示为
  `0`。逐点结果 JSON 损坏时以 `per_case_parse_error` 明确标记，不伪装成 AC 或正常
  空结果。管理员额外显示「重判此提交」，成功后重新读取详情与该题状态，失败时按既有
  策略保留原结果展示。
- **本人题目状态**：详情页与 `GET /api/status?problem_id=N` 合并展示本人该题当前
  状态（已 AC/未 AC、提交次数、首次 AC 时间）；无状态记录表示从未提交，按未 AC
  显示，不由前端创造状态行。已有页面（题目列表/题目页）继续使用各自响应内的
  `solved`，不额外重复发起状态查询，保持 M4.2/M4.3 兼容。
- **状态与一致性**：提供加载中、无历史、无权访问/不存在、读取失败状态；快速翻页或
  切换详情时按代次丢弃旧响应，旧结果不覆盖新页面；退出登录/身份变化时清除受保护
  内容（`subscribeAuth` + 路由重渲染），避免用户切换后残留他人数据。
- **身份与可见性规则（本次确定）**：本人提交历史、提交详情（含当时返回并保存的
  WA 对比）与本人题目状态属于用户自身数据，**不因题目后来隐藏而收回**；但题目本身
  （题面、公开样例、当前隐藏用例）仍完全按题目可见性规则控制，历史/状态接口不下发
  题面或隐藏用例，历史中的题目链接仍走题目接口的可见性检查。管理员按现有管理员
  权限（已登录 + 已完成首次改密 + 当前数据库角色为 admin）可查看任意提交详情。
- **范围**：不进入 M4.5 排行榜，不新增源码编辑后重提、批量重判、结果版本历史或导出；
  在途任务不进入提交历史（沿用 M3.7：独立 `in_flight_tasks` 表，结算后才写入
  `submissions`），未新增状态枚举、未改变恢复结算规则。

> **已通过独立测试验证**：后端单元 `tests/unit/test_m44_submission_history_unit.cpp`
> （7 用例）与集成 `tests/integration/test_m44_history_api.cpp`（90 项断言）、页面级
> `tests/frontend/m44_history_dom.mjs` + `run_m44.sh`（jsdom，31 项）；全量常规回归
> `ctest --parallel 1` 43/43，旧前端回归 M4.1 78/78 + 53/53、M2.5 128/128、M4.3 54/54。
> 完整逐项结果见 `tests/M4.4-test-report.md`。

### 排行榜页（M4.5）

新增公开排行榜页 `#/leaderboard`，游客与登录用户均可访问，并接入顶部导航。页面实现
位于 `web/js/pages/leaderboard.js`，复用 M4.1 的路由/身份/错误处理/生命周期，未引入
构建流程或前端框架。统计与排序由后端完成，前端只负责一致展示，不根据单次提交结果
自行加减排名。

- **展示列**：名次（后端返回的全局名次，不在各页从 1 重置）、昵称、AC 题目数、总提交
  次数与首次 AC 时间。没有 AC 的用户首次 AC 时间显示「—」，不显示无意义日期；昵称等
  文本一律经 `textContent` 纯文本渲染。
- **分页**：使用后端 `page`/`page_size`（固定 20）/`total`/`total_pages`，展示当前页
  与总人数，提供上一页/下一页与紧凑页码范围；页码承载于 `#/leaderboard?page=`，刷新与
  前进后退可恢复，超出末页自动修正并重取，不在前端对当前页二次排序或分页。
- **状态**：提供加载中、无数据（暂无可展示排名）与请求失败（提供重试）状态；「刷新」
  按钮主动重新获取当前排名。进入页面或主动刷新时获取，不新增高频轮询或实时推送。
- **当前用户高亮**：仅当已登录且响应中 `user_id` 与当前身份可靠匹配时高亮该行，**不通过
  昵称猜测身份**；登录/退出或切换账号后按最新身份重绘高亮，退出登录不残留过时的个人状态。
- **旧响应处理**：读取请求可被取消或按代次丢弃，快速翻页或离开页面时旧响应不会覆盖
  当前页面。
- **范围**：不实现竞赛榜、积分系统、时间段榜单、奖章或批量重算平台，不改变已确认的
  提交计数、Rejudge 与崩溃恢复规则。

> **已通过独立测试验证**：后端单元 `tests/unit/test_m45_leaderboard_unit.cpp`（9 用例）
> 与集成 `tests/integration/test_m45_leaderboard_api.cpp`（4 场景 / 52 项断言）、页面级
> `tests/frontend/m45_leaderboard_dom.mjs` + `run_m45.sh`（jsdom，23 项）；全量常规回归
> `ctest --parallel 1` 45/45 通过。完整逐项结果见 `tests/M4.5-test-report.md`。

### 注册接口

`POST /api/register`（公开，无需登录），请求体为 JSON，仅读取 `nickname` 与 `password`：

```bash
curl -i -X POST http://127.0.0.1:8080/api/register \
  -H 'Content-Type: application/json' \
  -d '{"nickname":"alice","password":"Secret123"}'
```

成功响应（`201`）：

```
HTTP/1.1 201 Created
Content-Type: application/json

{"account":"3084523017","id":2,"nickname":"alice","role":"user"}
```

- `account`：后端随机分配的 10 位纯数字账号（字符串，首位允许为 0），一次性分配、永久不复用。
- `role` 固定为 `user`；`account`/`role` 由后端控制，客户端传入的 `role`、`account` 等字段一律被忽略，无法提权或指定账号。
- 响应不包含密码或其哈希；服务端仅保存 argon2id 哈希。

输入规则：

| 字段 | 类型 | 规则 |
|---|---|---|
| `nickname` | string | 去除首尾空白后非空、长度 ≤ 30（去空白后计）；内部空白保留；全局唯一 |
| `password` | string | 非空、长度 ≤ 128；不裁剪不截断，空白视为有效内容 |

错误约定（响应体统一为 `{"error":"..."}`，内部故障返回通用文案，不泄露数据库细节）：

| 状态码 | 含义 |
|---|---|
| `201` | 注册成功 |
| `400` | 非法输入：JSON 解析失败、字段缺失/类型错误、非法昵称或密码 |
| `409` | 昵称已被使用（含并发冲突） |
| `500` | 内部故障 |

### 登录接口

`POST /api/login`（公开，无需登录），请求体为 JSON，仅读取 `account` 与 `password`。
普通用户使用系统分配的 10 位数字账号，预置管理员使用 `admin`：

```bash
curl -i -X POST http://127.0.0.1:8080/api/login \
  -H 'Content-Type: application/json' \
  -d '{"account":"3084523017","password":"Secret123"}'
```

成功响应（`200`）：

```
HTTP/1.1 200 OK
Content-Type: application/json

{"expires_in":3600,"token":"<jwt>","token_type":"Bearer",
 "user":{"account":"3084523017","id":2,"nickname":"alice","reset_pwd_flag":0,"role":"user"}}
```

- `token`：JWT（HS256），后续请求通过 `Authorization: Bearer <token>` 携带。
- `token_type`：固定 `Bearer`；`expires_in`：有效期（秒）。
- `user`：当前用户信息，含 `reset_pwd_flag`（预置 admin 为 `1`，供后续首次改密流程使用）。
- 响应不包含密码、密码哈希或密钥；错误账号与错误密码返回一致的失败提示，不泄露账号是否存在。

输入规则：

| 字段 | 类型 | 规则 |
|---|---|---|
| `account` | string | 非空；普通用户为 10 位数字账号，预置管理员为 `admin` |
| `password` | string | 非空；不裁剪不截断，空白视为有效内容 |

### 当前用户信息

`GET /api/me`（需登录），通过 Bearer token 鉴权：

```bash
curl -i http://127.0.0.1:8080/api/me \
  -H 'Authorization: Bearer <token>'
```

成功响应（`200`）：

```
{"account":"3084523017","id":2,"nickname":"alice","reset_pwd_flag":0,"role":"user"}
```

服务验证 token 签名、算法、过期时间与身份字段后，会**重新查询数据库**返回当前
昵称、角色与首次改密标记，因此数据库中的最新信息会实时反映，不返回密码哈希等敏感字段。

### 修改密码

`POST /api/me/password`（需登录），仅允许已登录用户修改**本人**密码。请求体仅读取
`old_password` 与 `new_password`（均为非空字符串）：

```bash
curl -i -X POST http://127.0.0.1:8080/api/me/password \
  -H 'Authorization: Bearer <token>' \
  -H 'Content-Type: application/json' \
  -d '{"old_password":"旧密码","new_password":"新密码"}'
```

成功响应（`200`）：

```
{"status":"ok"}
```

- 目标用户来自**已验证的当前用户上下文**（token 验证 + 按 `sub` 回查数据库），
  请求体中的 `id` / `account` / `role` 等字段一律被忽略，无法修改他人密码或提权。
- 新密码复用注册密码规则（非空、长度 ≤ 128、不裁剪不截断），并要求与旧密码不同；
  只保存新密码的 argon2id 哈希。
- 改密在单个事务内原子完成「校验旧密码 → 更新哈希 + 清除 `reset_pwd_flag`」，
  失败不产生部分更新；并发改密时已失效的旧密码不会覆盖新密码。
- **改密不撤销已有 token**：当前无会话撤销机制，已签发的旧 token 持续有效至过期
  （`exp`）。鉴权每次按 `sub` 回查数据库，故旧 token 的权限随数据库中最新角色与
  首次改密标记实时生效。

### 首次强制改密与权限

预置 admin 首次登录时 `reset_pwd_flag=1`，其后端限制为：

- **允许**：登录、`GET /api/me`、`POST /api/me/password`。
- **受限**：其他需要登录的业务操作（管理员业务入口）必须先改密，否则返回
  `403` + `{"error":"请先修改密码","code":"PASSWORD_CHANGE_REQUIRED"}`，供前端
  识别并跳转改密。公开接口（注册、登录等）不受影响。

管理员权限检查基于**数据库中的当前用户角色与首次改密标记**（不信客户端传入角色，
也不依赖 JWT 中可能过时的角色），组合满足「已登录 + 已完成必要改密 + 具备 admin
角色」才放行；所有管理员入口统一复用该检查。改密成功后按当前数据库状态判定权限，
无需修改 token 内容。

### 种子数据导入

内置 41 道种子题，其中 3 道为入门基础题（A+B Problem、整数求和、求最大值），其余
38 道改编自 LeetCode 经典题目（如两数之和、回文数、有效的括号、爬楼梯、打家劫舍等），
统一为 ACM 标准输入输出模式，包含标题、纯文本题面、输入输出说明、公开样例、隐藏测试
用例、难度、标签、时限与内存限制。

导入方式（`--seed`，显式执行一次，**不在服务启动时自动运行**）：

```bash
# 先导入种子题目（只创建/迁移表结构，不涉及 admin，也不需要 JWT 密钥）
./build/oj_server --db data/oj.db --seed
```

输出示例：

```
种子数据导入完成：新建题目 41 道（已存在的题目已跳过）
```

- **幂等**：种子题以 `problems.seed_key` 稳定标识，配合唯一索引保证同一道题只导入一次。
  重复执行 `--seed` 只会处理缺失的种子题，不会重复创建题目，也不会覆盖管理员后续的
  任何修改（题面、标题、用例等），更不会清空其它业务数据。
- 导入后正常启动服务即可看到题目：

  ```bash
  OJ_JWT_SECRET="$(openssl rand -hex 32)" OJ_ADMIN_PASSWORD='请改为强密码' \
    ./build/oj_server --db data/oj.db
  ```

- 种子题在 `testcases` 表中用 `is_sample` 显式区分：`1` 为公开样例（随题面下发），
  `0` 为隐藏用例（仅判题读取），不依赖「前 N 个默认公开」等隐含规则。

### 题目列表接口

`GET /api/problems`（公开，无需登录）。未携带 token 时按游客处理；携带 token 时
复用已有身份验证。按 `id` 升序稳定排序（`id` 为最终排序键），分页结果稳定，静态数据
下不会重复或遗漏。

查询参数（均可省略，省略/为空表示不施加该条件）：

| 参数 | 取值 | 说明 |
|---|---|---|
| `q` | 任意字符串 | 题目标题子串搜索；去首尾空白后为空则不限 |
| `difficulty` | `easy` / `medium` / `hard` | 难度筛选；为空则不限；其它取值 `400` |
| `tag` | 单个完整标签 | 按完整标签匹配；为空则不限 |
| `page` | 正整数，`1..1000000` | 页码，默认 `1`；非法格式或越界 `400` |
| `visible` | `all` / `1` / `true` / `0` / `false` | 可见性筛选；取值为非法时 `400`；仅管理员据此筛选，其它身份忽略其效果 |

搜索与筛选规则：

- **关键词**：对 `title` 做子串匹配，ASCII 字母大小写不敏感，中文按字节精确匹配。
  使用参数绑定；输入中的 `%`、`_`、`\` 按普通字符处理（转义后配合 `ESCAPE`），
  不会扩大匹配范围。关键词不含 `description` 等其它字段。
- **标签**：以逗号分隔补齐首尾后做**完整标签**匹配，不会把「图」当作「图论」的子串命中。
  沿用现有 `problems.tags` 存储（逗号分隔文本），不重构数据模型。
- **组合**：`q`、`difficulty`、`tag`、可见性之间为 **AND** 关系；单独、组合或全部省略
  均正确工作。无匹配时返回空列表且 `total=0`、`total_pages=0`。
- **分页**：每页固定 20 条。超出末页返回正常空列表（`200`），`total`/`total_pages`
  仍为筛选后的总数。`page` 为 `0`、负数、非整数或超出上限返回 `400`。
- **通过人数**：`pass_count` 为已 AC 的**不同用户数**（基于 `user_problem_status`），
  同一用户重复 AC 只计一人，仅失败提交不计入。
- **本人状态**：携带有效 token 时，每个条目附带 `solved`（本人是否已 AC；无状态记录
  视为 `false`）。身份只来自已验证的 token，客户端参数不能指定他人；游客不返回
  `solved` 字段（不伪装成本人状态）。
- **可见性**：游客与普通用户只能看到 `visible=1` 的题目，`total` 与实际可见数量一致，
  不会通过总数泄露隐藏题目。`visible` 参数仅对通过管理员检查的账号生效；普通用户即使
  传入 `visible=0` 也只看公开题。详见「题目可见性」。

```bash
# 第 2 页，搜索标题含 A+B、难度 easy、标签为「入门」的题目
curl -i 'http://127.0.0.1:8080/api/problems?q=A%2BB&difficulty=easy&tag=%E5%85%A5%E9%97%A8&page=2'
```

成功响应（`200`）：

```
{"problems":[
  {"id":1,"title":"A+B Problem","difficulty":"easy","tags":["入门","数学"],
   "visible":true,"pass_count":42,"solved":false}
 ],"page":2,"page_size":20,"total":21,"total_pages":2}
```

- `page_size` 固定为 `20`；`total_pages` 为 `ceil(total / 20)`，`total=0` 时为 `0`。
- 游客响应中的条目不含 `solved` 字段；`pass_count` 对所有访问者公开。
- 条目只含列表展示字段，不含题面、隐藏用例或任何用户源码。

### 题目标签选项接口

`GET /api/problem-tags`（公开，无需登录，M4.2 为列表页标签筛选新增的最小读取能力）。
未携带 token 时按游客处理；携带 token 时复用已有身份验证。

```bash
curl -i http://127.0.0.1:8080/api/problem-tags
```

成功响应（`200`）：

```
{"tags":["入门","图论","数学","排序"]}
```

- **来源与范围**：标签来自 `problems.tags`（逗号分隔存储），在**完整可见范围**的题目上
  去重、按字节升序稳定排序；不基于当前分页结果，也不通过抓取全部分页拼凑。游客与普通
  用户仅统计 `visible=1` 的题目，不会泄露仅隐藏题目使用的标签；管理员（已登录 +
  已完成首次改密 + admin 角色）可取得含隐藏题目的全部标签。
- 仅返回标签字符串，不含题目内容、题面或隐藏用例；非法/失效 token 仍按既有约定 `401`。

> 列表页不会把当前页标签当作全题库标签：标签选项独立于列表请求、按会话缓存并在每次
> 进入页面时刷新，已选标签即使在选项中缺失也会保留，翻页不会使其消失。

### 题目详情接口

`GET /api/problems/{id}`（公开，无需登录），只返回元数据与**公开样例**，
绝不包含隐藏用例。携带有效 token 时额外返回本人该题的 AC 状态 `solved`
（数据来源 `user_problem_status`，用户 ID 取自后端鉴权，不接受客户端指定）；
游客响应不含该字段：

```bash
curl -i http://127.0.0.1:8080/api/problems/1
```

成功响应（`200`）：

```
{"id":1,"title":"A+B Problem","description":"给定两个整数 a 和 b……",
 "difficulty":"easy","tags":["入门","数学"],"time_limit_ms":2000,
 "memory_limit_kb":65536,"visible":true,
 "samples":[{"input":"1 2\n","output":"3\n"},{"input":"100 -50\n","output":"50\n"}]}
```

携带有效 token 时（`solved` 表示本人是否已 AC；无状态记录为 `false`）：

```
{"id":1,...,"samples":[...],"solved":false}
```

> `solved` 是 M4.3 详情页本人状态的最小读取能力，与列表接口的 `solved` 同源，
> 不使用「列表第一页是否包含该题」等推断方式；游客不返回该字段。完整做题状态
> 接口 `GET /api/status` 已随 M4.4 提供（见「本人题目状态接口（M4.4）」）。

### 题目可见性

| 身份 | 列表可见范围 | 详情可见范围 |
|---|---|---|
| 游客（无 token） | `visible=1` 的题目 | `visible=1` 的题目 |
| 普通用户 | `visible=1` 的题目 | `visible=1` 的题目 |
| 管理员（已登录 + 已完成首次改密） | 全部题目（含隐藏） | 全部题目（含隐藏） |
| 管理员（`reset_pwd_flag=1`，未完成首次改密） | 同普通用户 | 同普通用户 |

- 列表与详情使用**一致的可见性规则**，无法通过直接请求隐藏题 ID 绕过。
- 管理员可在列表用 `visible` 参数筛选：`all`（默认，全部）、`1`/`true`（仅公开）、
  `0`/`false`（仅隐藏）。该参数只对管理员生效；普通用户与游客传入时被忽略，仍只看
  公开题，不会因筛选条件扩大访问范围。
- 对游客和普通用户，不存在的题目与无权查看的隐藏题目统一返回 `404`，不通过状态码
  差异泄露隐藏题目是否存在。
- 无效/伪造/过期 token 不会被当作游客或管理员：沿用既有约定返回 `401`。
- 非法 ID（非数字、负数、0、溢出）返回 `400`；数据库故障返回 `500` 通用文案，
  不泄露 SQL、文件路径或隐藏用例。

### 管理员题目接口（M2.1）

三个接口均需管理员权限（已登录 + 已完成首次改密 + 当前数据库角色为 `admin`）。
角色与首次改密标记每次按 `sub` 回查数据库，客户端提交的角色字段无效，角色被撤销后
原 token 立即失去管理权限。隐藏测试用例的增删改属 M2.2，不在这三个接口范围内。

#### 创建题目

`POST /api/admin/problems`，请求体为 JSON，仅读取下表字段；`id`/`created_at`/
`updated_at`/`seed_key` 由服务端管理，客户端传入一律忽略。

```bash
curl -i -X POST http://127.0.0.1:8080/api/admin/problems \
  -H 'Authorization: Bearer <admin-token>' \
  -H 'Content-Type: application/json' \
  -d '{"title":"A+B","difficulty":"easy","description":"输入两个整数，输出和。",
       "tags":["入门","数学"],"time_limit_ms":2000,"memory_limit_kb":65536,
       "visible":true,"samples":[{"input":"1 2\n","output":"3\n"}]}'
```

成功响应（`201`）：

```
{"id":4}
```

#### 修改题目

`PUT /api/admin/problems/{id}`，采用**部分更新**：只更新请求体中出现的字段，未出现的
字段保持原值（避免遗漏字段被意外清空）；需要清空时显式传空值，如 `"description":""`、
`"tags":[]`、`"samples":[]`。`samples` 出现时整体替换公开样例，隐藏用例不受影响。
`created_at` 保留，`updated_at` 更新为当前时间。

```bash
curl -i -X PUT http://127.0.0.1:8080/api/admin/problems/4 \
  -H 'Authorization: Bearer <admin-token>' \
  -H 'Content-Type: application/json' \
  -d '{"difficulty":"medium","visible":false,"samples":[]}'
```

成功响应（`200`）：

```
{"id":4,"status":"ok"}
```

#### 删除题目

`DELETE /api/admin/problems/{id}`。删除策略：

- **题目已有提交记录、或存在未结算在途任务（判题进行中）时拒绝删除**，返回 `409`
  （M3.7 扩展），保留学生提交历史与做题状态，避免已接收任务因删题无法保存终态；
- 两者都没有时在同一事务内删除该题的全部测试用例（公开样例与隐藏用例）、
  `user_problem_status` 记录、残留的中断在途记录与题目本身，不产生孤立记录；
- 不级联清空提交记录，也不新增软删除字段。

```bash
curl -i -X DELETE http://127.0.0.1:8080/api/admin/problems/4 \
  -H 'Authorization: Bearer <admin-token>'
```

成功响应（`200`）：

```
{"status":"ok"}
```

字段规则：

| 字段 | 类型 | 必填 | 规则 |
|---|---|---|---|
| `title` | string | 是 | 去首尾空白后非空，≤ 200 字节；入库为去空白后的值 |
| `difficulty` | string | 是 | 仅 `easy` / `medium` / `hard` |
| `description` | string | 否 | 纯文本，≤ 64 KiB，默认 `""` |
| `tags` | string[] | 否 | 去空白后非空、不含逗号、每个 ≤ 30 字节、≤ 20 个、不可重复；默认 `[]` |
| `time_limit_ms` | integer | 否 | `1..60000`，默认 `2000`；不接受 0/负数（项目未定义「无限」取值） |
| `memory_limit_kb` | integer | 否 | `1..1048576`，默认 `65536`；不接受 0/负数 |
| `visible` | boolean | 否 | 默认 `true`（创建时） |
| `samples` | object[] | 否 | 每项含字符串 `input`/`output`（允许空串），≤ 50 组，默认 `[]` |

错误约定：

| 状态码 | 含义 |
|---|---|
| `201` / `200` | 创建成功 / 修改、删除成功 |
| `400` | 非法参数：非法 JSON、缺少必填字段、类型错误、非法难度、标签结构错误、限制值越界、无可更新字段、非法题目 ID |
| `401` | 未登录 / 无效 token |
| `403` | 非管理员；或管理员未完成首次改密（含 `code:"PASSWORD_CHANGE_REQUIRED"`） |
| `404` | 题目不存在（修改/删除） |
| `409` | 删除冲突：题目已有提交记录或未结算在途任务 |
| `500` | 内部故障（不泄露 SQL/路径） |

> 可见性说明：管理员可将其设为隐藏并仍能在列表/详情中查看；游客与普通用户列表不含
> 隐藏题、直接访问详情或发起新提交均被拒（`404`）。可见性变化不会清除已有提交与做题
> 状态，也不重判历史提交（Rejudge 见 M3.6）。

### 管理员测试用例接口（M2.2）

四个接口均需管理员权限（已登录 + 已完成首次改密 + 当前数据库角色为 `admin`），角色与
首次改密标记每次按 `sub` 回查数据库。游客/无效 token 返回 `401`；普通用户返回 `403`；
未完成首次改密的管理员返回 `403` + `code:"PASSWORD_CHANGE_REQUIRED"`。普通用户即使知道
题目 ID 或用例 ID，也无法读取完整用例或执行修改。

**存储模型（公开样例 vs 隐藏用例）**：公开样例与隐藏用例共用 `testcases` 表，用
`is_sample` 显式区分：`1` = 公开样例，`0` = 隐藏用例。

- 公开样例由 M2.1 的题目接口通过 `samples` 字段整体维护；本组接口**只读写
  `is_sample=0`**，从不读取或改写 `is_sample`，因此新增、修改、排序都不可能把隐藏
  用例变成公开样例，也不会破坏公开样例。
- 通过用例接口修改/删除公开样例（或不属于该题的用例）一律返回 `404`，公开样例保持
  不变。
- 公开题目列表与详情仍只返回公开样例；管理员读取接口是唯一返回完整用例的入口，
  且受管理员权限保护，不复用于公开响应。

**管理员读取用例**：`GET /api/admin/problems/{id}/testcases` 返回该题**完整用例列表**
（含公开样例与隐藏用例，带 `is_sample` 标记），按 `(ord ASC, id ASC)` 排序，与判题器
执行顺序一致。题目不存在返回 `404`。

```bash
curl -i http://127.0.0.1:8080/api/admin/problems/4/testcases \
  -H 'Authorization: Bearer <admin-token>'
```

成功响应（`200`）：

```
{"problem_id":4,"total":3,"testcases":[
  {"id":9,"problem_id":4,"ord":0,"input":"1 2\n","output":"3\n","is_sample":true},
  {"id":10,"problem_id":4,"ord":1,"input":"5 7\n","output":"12\n","is_sample":false}
]}
```

**新增用例**：`POST /api/admin/problems/{id}/testcases`。用例归属只由 URL 中的题目 ID
决定，请求体中的 `id`/`problem_id`/`is_sample` 等字段一律忽略；新增记录固定为隐藏用例
（`is_sample=0`）。成功返回 `201` + 新用例 ID 与实际保存的 `ord`。

```bash
curl -i -X POST http://127.0.0.1:8080/api/admin/problems/4/testcases \
  -H 'Authorization: Bearer <admin-token>' \
  -H 'Content-Type: application/json' \
  -d '{"input":"111 222\n","output":"333\n","ord":5}'
```

成功响应（`201`）：

```
{"id":11,"problem_id":4,"ord":5}
```

**修改用例**：`PUT /api/admin/problems/{id}/testcases/{tid}`。采用**部分更新**：只更新
请求体中出现的字段，未出现的字段保持原值；需要清空时显式传空串（如 `"input":""`）——
空串是合法内容，和「字段缺失」语义不同。用例按 `problem_id` 与用例 ID 同时定位；
不存在、不属于该题或属于公开样例时统一返回 `404`，不会修改其它题目的用例。

```bash
curl -i -X PUT http://127.0.0.1:8080/api/admin/problems/4/testcases/11 \
  -H 'Authorization: Bearer <admin-token>' \
  -H 'Content-Type: application/json' \
  -d '{"output":"334\n"}'
```

成功响应（`200`）：

```
{"id":11,"problem_id":4,"ord":5,"status":"ok"}
```

**删除用例**：`DELETE /api/admin/problems/{id}/testcases/{tid}`。同样以题目 ID 与用例 ID
同时定位；不存在/不属于该题/公开样例返回 `404`。删除**不重排、不回收空号**，其余用例
`ord` 保持不变。

```bash
curl -i -X DELETE http://127.0.0.1:8080/api/admin/problems/4/testcases/11 \
  -H 'Authorization: Bearer <admin-token>'
```

成功响应（`200`）：

```
{"status":"ok"}
```

字段规则：

| 字段 | 类型 | 必填 | 规则 |
|---|---|---|---|
| `input` | string | 是（新增）/ 否（修改） | 纯文本，≤ 64 KiB；允许空串；不 trim、不归一化，原样保存 |
| `output` | string | 是（新增）/ 否（修改） | 同上 |
| `ord` | integer | 否 | `0..1000000`，默认见下 |

`ord` 规则（沿用既有约定）：

- 起始值 `0`；允许重复；同一题内顺序由 `(ord ASC, id ASC)` 唯一确定，`id` 为稳定
  第二排序键，不依赖数据库默认行顺序。数据库读取、管理员列表与判题执行使用同一排序。
- 新增时缺省 `ord` = 该题全部用例（含公开样例）当前最大 `ord` + 1，无用例时为 `0`
  （即追加到末尾）；若该题 `ord` 已达上限（`1000000`），自动分配无法满足范围时返回
  `409`，需显式指定未占用的 `ord`。
- 删除不重排、不回收空号；修改 `ord` 仅改变该条记录的排序位置。

错误约定：

| 状态码 | 含义 |
|---|---|
| `201` / `200` | 新增成功 / 读取、修改、删除成功 |
| `400` | 非法参数：非法 JSON、缺少必填字段、类型错误、文本超长、`ord` 越界、无可更新字段、非法题目/用例 ID |
| `401` | 未登录 / 无效 token |
| `403` | 非管理员；或管理员未完成首次改密（含 `code:"PASSWORD_CHANGE_REQUIRED"`） |
| `404` | 题目不存在；或用例不存在 / 不属于该题 / 属于公开样例 |
| `409` | 约束冲突：该题 `ord` 已达上限，缺省追加无法自动分配 |
| `413` | 请求体超过 1 MiB（由服务器在解析前拒绝） |
| `500` | 内部故障（不泄露 SQL/路径/隐藏用例内容） |

对判题与历史数据的影响：

- 一次提交在判题前读取完整用例并构造内存快照，判题全程使用该快照；判题期间修改用例
  不会让同一次判题混用修改前后的版本。后续提交使用修改后的用例。
- 新增/修改/删除用例**不重判历史提交**，不更改已保存的逐点结果、AC 状态与提交次数
  （Rejudge 见 M3.6）。
- 管理员可暂时删空某题用例以便编辑；空测试集**不判 AC**，提交时返回约定的 `SYSERR`
  （不可判题）并正常持久化为提交记录。删题规则仍沿用 M2.1（有提交记录时拒绝删除）。

### 管理员用户接口（M2.4）

两个接口均需管理员权限（已登录 + 已完成首次改密 + 当前数据库角色为 `admin`），角色与
首次改密标记每次按 `sub` 回查数据库。游客/无效 token 返回 `401`；普通用户返回 `403`；
未完成首次改密的管理员返回 `403` + `code:"PASSWORD_CHANGE_REQUIRED"`；数据库故障返回
`500`。请求体只读取下列明确列出的字段，其余字段（`account`/`nickname`/`password_hash`/
`reset_pwd_flag` 等）一律忽略，不同操作只修改对应字段，绝不把请求体任意映射到 `users` 表。

**用户列表**：`GET /api/admin/users`。分页沿用 `GET /api/problems` 的既有约定：`page`
默认 `1`，仅接受正整数 `1..1000000`（`0`、负数、非整数、超上限返回 `400`），每页固定
`20` 条；超出末页返回 `200` 空列表。排序为 `id ASC`（主键，跨页不重不漏）。响应只返回
管理所需字段，**不含 `password_hash` 等任何敏感字段**。

```bash
curl -i 'http://127.0.0.1:8080/api/admin/users?page=1' \
  -H 'Authorization: Bearer <admin-token>'
```

成功响应（`200`）：

```
{"page":1,"page_size":20,"total":23,"total_pages":2,
 "users":[
   {"id":1,"account":"admin","nickname":"admin","role":"admin","reset_pwd_flag":0,"created_at":"2026-09-22 08:00:00"},
   {"id":2,"account":"0123456789","nickname":"alice","role":"user","reset_pwd_flag":0,"created_at":"2026-09-22 08:05:11"}
 ]}
```

**重置密码 / 修改角色**：`PUT /api/admin/users`。请求体为 JSON，必须包含：
`action`（字符串，严格取值 `reset_password` 或 `change_role`）和 `user_id`（目标用户 ID，
正整数）。不同 `action` 读取的字段不同，**只允许修改对应字段**：

- `reset_password`：必填 `new_password`（字符串，复用注册密码规则：非空、≤128 字符、
  不裁剪不截断）。**不需要也不读取目标用户的旧密码**。
- `change_role`：必填 `role`（字符串，严格取值 `admin` 或 `user`）。

```bash
# 重置用户 2 的密码
curl -i -X PUT http://127.0.0.1:8080/api/admin/users \
  -H 'Authorization: Bearer <admin-token>' -H 'Content-Type: application/json' \
  -d '{"action":"reset_password","user_id":2,"new_password":"NewSecret1"}'

# 将用户 2 的角色改为 admin
curl -i -X PUT http://127.0.0.1:8080/api/admin/users \
  -H 'Authorization: Bearer <admin-token>' -H 'Content-Type: application/json' \
  -d '{"action":"change_role","user_id":2,"role":"admin"}'
```

成功响应（`200`，只返回必要确认信息，不回显密码）：

```
{"user_id":2,"status":"ok"}
{"user_id":2,"role":"admin","status":"ok"}
```

字段规则：

| 字段 | 类型 | 必填 | 规则 |
|---|---|---|---|
| `action` | string | 是 | 严格取值 `reset_password` / `change_role`，其它值 `400` |
| `user_id` | integer | 是 | 正整数；`0`/负数/小数/字符串/布尔/null 及超大值均 `400` |
| `new_password` | string | `reset_password` 时必填 | 非空、≤128 字符；不裁剪、不截断；空白视为有效内容 |
| `role` | string | `change_role` 时必填 | 严格取值 `admin` / `user`，其它值 `400` |

**重置后的首次改密策略（本次确定）**：管理员重置密码时，目标用户的 `reset_pwd_flag`
置为 `1`（与密码哈希在同一条 `UPDATE` 内原子生效）。该标记**对所有目标用户生效**
（含普通用户），复用 M1.3 的既有强制检查：

- 普通用户标记为 `1` 时，`POST /api/problems/{id}/submit` 返回 `403` +
  `code:"PASSWORD_CHANGE_REQUIRED"`；管理员标记为 `1` 时，所有管理员接口返回同样的
  `403`。登录、`GET /api/me`、`POST /api/me/password` 不受限制。
- 目标用户通过 `POST /api/me/password` 修改本人密码后，标记清除，受限业务恢复。
- 这说明本阶段把 M1.3 的强制改密适用范围从「管理员业务入口」扩展到「普通用户的提交
  业务」；后端确有对应限制，不只是设置标记。

**角色保护规则（本次确定）**：

- 仅允许现有角色枚举 `admin` 与 `user`。
- **禁止取消最后一个管理员的权限**：把当前唯一的管理员降级为 `user` 返回 `409`
  （`{"error":"不能取消最后一个管理员的权限"}`）。该检查与角色更新在同一个
  `BEGIN IMMEDIATE` 事务内完成并复用连接级事务互斥锁，**并发降级不会把管理员清零**。
- **允许管理员取消自己的权限**（自我降级），只要不是最后一名管理员；**预置 `admin`
  没有额外豁免**，其角色同样可被修改。降级后其原有 token 在下一次请求时即按数据库
  最新角色判定为普通用户，不能再调用管理员接口。
- 角色修改不改变账号、昵称、密码哈希、历史提交记录与 `user_problem_status`（AC 状态、
  首次 AC 时间、提交次数）。

**JWT 行为（沿用现有会话策略）**：本阶段**没有**会话撤销机制。管理员重置密码或修改
角色后，已签发的旧 token 仍然有效至其 `exp` 过期，**不声称「改了密码哈希旧 token 就
失效」**。由于每次鉴权都按 `sub` 回查数据库，旧 token 的**权限与首次改密标记始终按数据库
最新值实时生效**：被降级的用户即使持有旧 token 也无法访问管理员接口；被重置密码的用户
的提交/管理员访问会因 `reset_pwd_flag=1` 被拦截。

错误约定：

| 状态码 | 含义 |
|---|---|
| `200` | 查询、重置、改角色成功 |
| `400` | 非法参数：非法 JSON、未知 `action`、缺少字段、类型错误、`user_id` 非正整数、非法/过长 `new_password`、非法 `role`、非法 `page` |
| `401` | 未登录 / 无效 token |
| `403` | 非管理员；或管理员未完成首次改密（含 `code:"PASSWORD_CHANGE_REQUIRED"`） |
| `404` | 目标用户不存在 |
| `409` | 管理员保护冲突：不能取消最后一个管理员的权限 |
| `500` | 内部故障（不泄露 SQL/路径/哈希/密码） |

> 运行日志记录操作者 ID、目标用户 ID、操作类型与结果，**不记录密码、哈希或完整 token**；
> 目标用户不存在、请求非法或写入失败时不产生部分更新。

### 管理员重判接口（M3.6）

`POST /api/admin/submissions/{id}/rejudge`（需管理员权限），使用数据库中保存的原提交
源码、语言与当前题目配置（含完整测试用例）重新判题，更新原提交记录，并联动重算该用户
该题的做题状态；不新增提交记录，不增加 `submit_count`。

```bash
curl -i -X POST http://127.0.0.1:8080/api/admin/submissions/42/rejudge \
  -H 'Authorization: Bearer <admin-token>' \
  -H 'Content-Type: application/json' \
  -d '{}'
```

成功响应（`200`）：返回更新后的原提交结果，字段与 `POST /api/problems/{id}/submit` 的
成功响应一致（`id` 为原提交 ID，`created_at` 保持不变，`status`/`results`/`runtime_ms`/
`memory_kb`/`compile_output` 等为本次重判结果）。

```json
{"id":42,"problem_id":1,"language":"cpp17","status":"AC","passed":5,"total":5,
 "runtime_ms":18,"memory_kb":8420,"compile_time_ms":640,"compile_ok":true,
 "compile_output":"","compile_output_truncated":false,
 "message":"全部测试点通过","created_at":"2026-09-21 12:00:00",
 "results":[{"index":0,"status":"AC","time_ms":3,"memory_kb":8000}]}
```

权限与错误约定：

| 检查 | 结果 |
|---|---|
| 未登录 / 无效 token | `401` |
| 非管理员；或管理员未完成首次改密 | `403`（含 `code:"PASSWORD_CHANGE_REQUIRED"`） |
| 非法提交 ID（非数字 / `0` / 负数 / 溢出） | `400` |
| 提交记录不存在 | `404` |
| 该提交 ID 已有正在进行的重判 | `409` + `code:"REJUDGE_IN_PROGRESS"` |
| 判题队列已满 | `503` + `code:"JUDGE_QUEUE_FULL"` + `Retry-After` |
| 判题服务正在停止 | `503` + `code:"JUDGE_UNAVAILABLE"` |
| 重判过程中产生 SYSERR（服务取消 / 全局硬上限 / 环境故障等） | `500`（按当前策略保留原结果与统计，见下文） |
| 数据库写入等内部故障 | `500`（事务回滚，不覆盖原结果） |

重判规则：

- **源码与语言**：从 `submissions` 原记录读取，客户端请求体不得替换源码、语言、用户归属或
  指定结果；请求体中任何额外字段均被忽略。
- **题目配置**：使用当前 `problems.time_limit_ms` / `memory_limit_kb` 与当前完整测试用例
  （含隐藏用例），判题前构造内存快照，判题期间修改用例不会让同一次重判混用不同版本。
- **调度**：重判与普通提交共用 `JudgeManager` 的有界队列、worker、编译并发门限、取消机制与
  结果通道；`HTTP` 同步等待结果，不另开绕过调度器的执行路径。
- **持久化**：在原 `submissions` 记录上更新 `status` / `per_case` / `compile_msg` /
  `runtime_ms` / `memory_kb`；保留 `id` / `user_id` / `problem_id` / `language` /
  `source_code` / `created_at`；不新增提交记录，不增加提交次数。
- **状态重算**：更新原记录与 `user_problem_status` 在同一个短事务内完成：
  - 该用户该题仍有状态为 `AC` 的提交：`status='accepted'`，`first_ac_at` 为其中最早的
    `created_at`；
  - 已无 `AC` 提交：`status='none'`，`first_ac_at` 清空；
  - `submit_count` 不因重判增加或减少。
- **并发去重**：同一 `submissions.id` 同时只能有一个待执行或正在执行的重判；冲突请求返回
  `409 REJUDGE_IN_PROGRESS`。完成、失败或取消后从去重集合移除。
- **失败处理策略（本阶段明确）**：当本次重判最终状态为 `SYSERR`（含服务取消、全局硬上限、
  编译/运行环境故障等）时，视为“无法完成本次重判”，保留原提交结果与统计，向管理员返回
  `500` 及失败原因；待后续需求最终确认后可调整是否覆盖。持久化事务失败同样回滚，原结果
  与统计保持不变。

> 日志记录操作者 ID、提交 ID、任务标识与最终结果，不记录 token、完整源码或隐藏用例。

### 提交接口（M1.6）

`POST /api/problems/{id}/submit`（需登录），同步判题并返回本次提交的结果。请求体为
JSON，仅读取 `language` 与 `code`；用户归属取自已验证的当前用户上下文，客户端传入的
`user_id`/`id`/`status`/`testcases` 等字段一律忽略。

```bash
curl -i -X POST http://127.0.0.1:8080/api/problems/1/submit \
  -H 'Authorization: Bearer <token>' \
  -H 'Content-Type: application/json' \
  -d '{"language":"cpp17","code":"#include <iostream>\nint main(){long long a,b;std::cin>>a>>b;std::cout<<a+b<<\"\\n\";}"}'
```

成功响应（`200`，判题结果本身不是 HTTP 故障，CE/WA/TLE/RE/SYSERR 同样返回 `200`）：

```
{"id":12,"problem_id":1,"language":"cpp17","status":"AC","passed":5,"total":5,
 "runtime_ms":18,"memory_kb":8420,"compile_time_ms":640,"compile_ok":true,
 "compile_output":"","compile_output_truncated":false,
 "global_deadline_hit":false,"cancelled":false,
 "message":"全部测试点通过","created_at":"2026-09-21 12:00:00",
 "results":[{"index":0,"status":"AC","time_ms":3,"memory_kb":8000},
            {"index":1,"status":"AC","time_ms":4,"memory_kb":8420}]}
```

- `language`：仅接受 `cpp17`（C++17）与 `c11`（C11），大小写不敏感；其它取值返回
  `400`。入库保存规范小写值。
- `code`：完整用户源码，原样送入编译与入库，不做裁剪或修改。
- `status`：`AC/WA/CE/TLE/RE/MLE/SYSERR`。
- 指标口径（单位分别为毫秒 ms 与千字节 KB，均为整数）：
  - `runtime_ms` = 各测试点**程序执行**墙钟耗时之和，**不含排队等待与编译时间**；
  - `memory_kb` = 各测试点观测峰值 RSS 的最大值（非求和）；未采集到时为 `null`，
    绝不伪造为 0；
  - `compile_time_ms` = 编译阶段墙钟耗时，单独展示，不混入 `runtime_ms`。
- `memory_kb`（提交级）：已由 RSS 采样采集时返回数值，未采集到时返回 `null`。
  逐点结果的 `memory_kb` 同理。
- `results`：逐测试点结果，按执行顺序（`index` 从 0 起）返回。通过（AC）测试点只含
  `index/status/time_ms/memory_kb`，**绝不附带隐藏测试输入或标准答案**；非 AC 点含
  `reason`（结构化终止原因，如 `non_zero_exit`/`signaled`/`timed_out`/
  `memory_exceeded`/`cancelled`/`launch_failure`）、`exit_code`、`term_signal`、
  `message`、`stderr_output`、`actual_output`（**始终返回，允许空字符串**，便于区分
  「输出为空」与「未提供」）与 `output_truncated`（标准输出超限被截断）等诊断；
  `WA` 点另按 SPEC PRB-05/JUDGE-07 附上该失败点的 `input`/`expected_output`。
  仅本次提交者可见。
- 编译失败返回 `status:"CE"` 并在 `compile_output` 给出编译器诊断（已清洗内部路径；
  超限时 `compile_output_truncated` 为 `true`），`results` 为空。
- `global_deadline_hit`：单次判题全局硬上限耗尽时为 `true`，未执行的测试点不会出现在
  `results` 中；`cancelled`：服务停止导致判题被取消时为 `true`（按内部错误记为
  `SYSERR`）。两者供调用方明确区分「未执行全部测试点」的原因，未执行点不会伪造成通过。
- 运行阶段全开 ASan/UBSan：越界、非法内存访问或不可恢复的未定义行为会导致非正常
  退出，按 `RE` 处理并保留诊断，绝不因输出碰巧匹配而判 `AC`；用户自行打印的类似
  文本不会单独导致失败。

参数与权限规则：

| 检查 | 结果 |
|---|---|
| `language`/`code` 缺失、类型错误、非法 JSON、非对象请求体 | `400` |
| 不支持的语言、纯空白源码 | `400` |
| 源码超过 64 KiB（字节） | `400` |
| 请求体超过 1 MiB | `413`（由服务器在解析前拒绝） |
| 非法题目 ID（非数字/0/负数/溢出） | `400` |
| 题目不存在，或普通用户向隐藏题提交 | `404`（统一，不泄露存在性） |
| 未登录 / 无效 token | `401` |
| 尚未完成首次强制改密 | `403` + `code:"PASSWORD_CHANGE_REQUIRED"` |
| 数据库写入等内部故障 | `500`（不声称保存成功，不泄露 SQL/路径） |

提交与统计口径：

- 用户身份只来自已验证的当前用户上下文（token + 按 `sub` 回查数据库）；客户端无法
  指定或伪造提交归属。
- 判题用例、顺序与时限全部从后端数据库读取（含隐藏用例），不接受客户端提供的标准
  答案、用例或资源限制覆盖值；判题期间不持有数据库事务。
- **参数或权限检查失败不创建提交、不增加次数**；产生并保存的判题结果（含 CE、WA、
  TLE、RE、MLE）以及内部判题故障产生的 `SYSERR` 都是提交记录，均使 `submit_count`
  加一（SYSERR 也保存并计数，与「检查失败无记录」区分，保持记录与统计一致）。
- `user_problem_status`：首次有效提交创建记录；首次 AC 设置 `accepted` 与
  `first_ac_at`；重复 AC 保留首次 AC 时间；AC 之后的失败提交不清除已通过状态。
- 提交记录写入与做题状态更新在**同一个短事务**内完成，任一失败整体回滚；并发提交
  由连接级事务互斥 + `BEGIN IMMEDIATE` 串行化，并依靠 `UNIQUE(user_id, problem_id)`
  约束保证不重复创建状态记录、不丢失计数。`submissions.created_at` 与
  `user_problem_status.first_ac_at` 采用同一时间口径，便于后续 Rejudge 按原提交时间重算。
- 判题阶段（编译 + 逐点运行）自 M3.1 起由 `JudgeManager` 的 worker 线程池并发调度
  （worker 数 = `min(CPU 核数, 8)`），每个任务独立执行、结果独立交付；并发提交不再
  全局串行（详见「判题任务调度（M3.1）」）。
- **接收边界与崩溃恢复（M3.7）**：接收顺序为「容量预留 → 写入独立在途记录
  `in_flight_tasks` → 入队」。未通过鉴权/参数/可见性/容量检查不产生在途任务；写库
  失败返回 `500` 且不声称已接收；写库成功后才入队，故崩溃后仍能找到。任务完成时在
  同一短事务内删除在途记录并写入 `submissions`/更新状态，保证只结算一次；在途记录
  在结算前不计入 `submit_count`、不设置 AC。服务启动时扫描并重新入队未结算任务（失败
  则以 `interrupted` 保留信息），详见「崩溃恢复与在途任务持久化（M3.7）」。

> **安全边界（重要）**：自 M3.3 起判题在进程级沙箱中执行（tmpfs 随机目录 +
> 命名空间/chroot + setrlimit/RSS 限制 + seccomp），自 M3.4 起默认全开 ASan/UBSan
> 并完成统一异常分类；Rejudge 属 M3.6。受控样例通过不代表可安全公开运行任意不可信代码。

### 本人提交历史接口（M4.4）

`GET /api/submissions?mine`（需登录），返回当前登录用户的提交历史摘要，按
`created_at DESC, id DESC` 稳定排序（最新优先）。支持可选参数 `page`（默认 1）、
`problem_id`（仅返回该题目的提交）。

```bash
curl -i 'http://127.0.0.1:8080/api/submissions?mine' \
  -H 'Authorization: Bearer <token>'
```

成功响应（`200`）：

```json
{"submissions":[{"id":12,"problem_id":1,"problem_title":"A+B Problem",
  "language":"cpp17","status":"AC","runtime_ms":18,"memory_kb":8420,
  "created_at":"2026-09-21 12:00:00"}],
 "page":1,"page_size":20,"total":1,"total_pages":1,"mine":true}
```

- **身份**：只来自后端验证后的当前用户（token + 按 `sub` 回查），客户端传入的
  `user_id` 等参数一律忽略；无论是否携带 `mine`，都只返回本人记录，**绝不返回全体
  用户提交**。`mine` 以「参数是否出现」判断（`?mine` 空值按本人历史处理），未提供时
  默认返回本人历史并记录运行日志；显式传入非法 `mine` 取值（如 `mine=0`）返回 `400`。
- **摘要范围**：仅提交 ID、题目 ID、题目标题、语言、状态、耗时、内存与原提交时间；
  不含源码、逐点结果 JSON、编译信息或 WA 用例详情。
- `runtime_ms`/`memory_kb` 口径与提交接口一致；`memory_kb` 未采集时为 `null`。
- **分页**：列表与总数使用完全相同的身份与筛选条件；每页固定 20，`page` 非法返回
  `400`，超出上限明确拒绝；按 `(user_id, created_at DESC, id DESC)` 建索引。
- **只读**：读取接口不修改提交、计数或做题状态。在途任务不进入历史（沿用 M3.7：
  结算后才写入 `submissions`），未新增状态枚举。
- 未登录 / 无效 token `401`；内部故障 `500`。查询参数错误 `400`。

### 提交详情接口（M4.4）

`GET /api/submissions/{id}`（需登录），返回数据库保存的完整源码与判题结果。

```bash
curl -i http://127.0.0.1:8080/api/submissions/12 \
  -H 'Authorization: Bearer <token>'
```

成功响应（`200`）：

```json
{"id":12,"problem_id":1,"problem_title":"A+B Problem","language":"cpp17",
 "status":"AC","source_code":"...","runtime_ms":18,"memory_kb":8420,
 "compile_output":"","created_at":"2026-09-21 12:00:00",
 "results":[{"index":0,"status":"AC","time_ms":3,"memory_kb":8000}],
 "per_case_parse_error":false}
```

- **权限**：普通用户只允许查看本人记录；管理员（已登录 + 已完成首次改密 + 当前
  数据库角色为 `admin`，每次按数据库最新值判定）可查看任意记录。不存在的记录与
  无权访问统一返回 `404`，不通过错误响应泄露他人源码或提交信息。
- **数据来源**：全部来自 `submissions` 保存结果，**不重新判题、不按当前 testcases
  重拼历史 WA**。Rejudge 后展示原记录当前保存的结果；不虚构未持久化的编译耗时或旧
  版本结果。未持久化的指标（如 `compile_time_ms`）在响应中省略，前端显示「未采集」，
  不将 `null` 显示为真实的 `0`。
- **逐点结果**：复用提交响应的逐点结构（`results`）；`WA` 点附保存的输入/期望输出/
  实际输出，通过点不额外附带隐藏输入或标准答案。`per_case` 为空按「无逐点结果」
  处理；JSON 损坏时 `per_case_parse_error` 为 `true` 并返回空 `results`，明确标记
  不可解析，**不把异常记录显示为 AC 或正常空结果**。
- **题目标识**：返回 `problem_id`/`problem_title` 供展示与跳转；题目接口仍独立执行
  可见性检查，本接口不下发题面或隐藏用例。
- 非法 ID `400`；未登录 / 无效 token `401`；不存在 / 无权访问 `404`；内部故障 `500`。

### 本人题目状态接口（M4.4）

`GET /api/status`（需登录），返回当前用户的 `user_problem_status` 记录，按
`problem_id` 升序。支持可选参数 `problem_id`（仅返回该题状态）。

```bash
curl -i 'http://127.0.0.1:8080/api/status?problem_id=1' \
  -H 'Authorization: Bearer <token>'
```

成功响应（`200`）：

```json
{"statuses":[{"problem_id":1,"status":"accepted",
  "first_ac_at":"2026-09-21 12:00:00","submit_count":2}]}
```

- 字段沿用既有约定：`status` 为 `accepted`/`none`，另含 `first_ac_at`（无则为
  `null`）与 `submit_count`。仅返回当前用户已有记录，客户端不能指定他人身份。
- **无记录不等于接口失败**：未提交的题目不出现在 `statuses` 中，表示该题从未提交、
  按未 AC 显示；接口不会为未提交题目创造状态行。
- 范围依据已确认的历史访问规则：状态属用户自身数据，本人对已提交题目的状态不因题目
  后来隐藏而收回；本接口不返回题目标题/题面等隐藏题目资料。
- 非法 `problem_id` `400`；未登录 / 无效 token `401`；内部故障 `500`。

### 排行榜接口（M4.5）

`GET /api/leaderboard`（公开，无需登录），返回分页排行榜。支持可选参数 `page`
（默认 1，正整数 `1..1000000`，非法 `400`），每页固定 20 条。

```bash
curl -i 'http://127.0.0.1:8080/api/leaderboard?page=1'
```

成功响应（`200`）：

```json
{"leaderboard":[
   {"rank":1,"user_id":2,"nickname":"alice","ac_count":5,"submit_count":8,
    "first_ac_at":"2026-09-21 12:00:00","created_at":"2026-09-20 09:00:00"},
   {"rank":2,"user_id":3,"nickname":"bob","ac_count":0,"submit_count":2,
    "first_ac_at":null,"created_at":"2026-09-20 09:05:00"}],
 "page":1,"page_size":20,"total":2,"total_pages":1}
```

- **排名对象（本次确定）**：仅统计至少有 1 次已结算提交的用户，从未提交的用户不出现；
  管理员按普通用户口径参与排名（与「管理员也可做题」一致）；隐藏题目
  （`problems.visible=0`）的 AC 与提交均**不计入**公开榜。未接收请求与未结算在途
  任务不参与统计。
- **统计来源**：复用既有持久化状态 `user_problem_status`（`status`、`first_ac_at`、
  `submit_count`）与 `users.created_at`，不为排行榜重复存储整套统计。AC 数按 accepted
  的**不同题目**计数（重复 AC 不重复增加）；总提交次数为该用户可见题目的
  `submit_count` 合计，沿用既有结算规则（含失败提交，Rejudge 不增加）；首次 AC 时间
  取当前仍有效 accepted 题目中最早的 `first_ac_at`，无 AC 为 `null`（不用注册时间/
  当前时间/0 伪造）。
- **排序**：AC 数降序 → 总提交次数升序 → 首次 AC 时间升序 → 注册时间升序 →
  用户 ID 升序（稳定的最终排序键）。首次 AC 时间为 `null` 的用户排在有值者之后
  （显式判定，不依赖数据库默认行为）；无 AC 用户之间继续按规定比较提交次数、注册时间
  与 ID。分页在全局排序后进行，`rank` 为全局名次，不按页从 1 重置。
- **只读与一致性**：读取不修改记录、不重新判题、不触发补计数；使用短只读事务，使
  `total` 与当前页来自同一数据视图，不长期持有覆盖判题执行的事务。每个用户行来自
  单个 GROUP BY 聚合子查询后再与 `users` 一对一连结，避免多表连接放大统计；全部
  参数绑定，沿用既有索引。
- **字段范围**：只返回名次、昵称、AC 数、总提交次数、首次 AC 时间与注册时间，以及供
  前端可靠高亮当前用户的 `user_id`；**不返回账号、密码哈希、token、源码或逐点结果**。
- 非法 `page` `400`；内部故障 `500`。

### 判题任务调度（M3.1）

`JudgeManager`（`src/judge/manager.h`）统一负责判题任务的接收、入队、worker 调度与
结果交付，把「任务调度」与「单次判题执行 + 持久化」分离；单次编译/运行/比对/落库仍
复用 `JudgeEngine` + `LocalExecutor` + `SubmitService`，不复制第二套逻辑。

- **worker 数量**：`min(CPU 核数, 8)`，至少 1 个。CPU 核数经 `sysconf(_SC_NPROCESSORS_ONLN)`
  获取，失败时退回 `std::thread::hardware_concurrency()`；仍为 0 或无法获取时按 1
  处理，绝不出现没有线程处理任务的情况。每个 worker 一次处理一个完整提交，同一提交
  的测试点不额外并行化。
- **有界等待队列**：容量只计算「已接收但尚未开始执行」的任务数，与正在执行的任务数
  分开（正在执行数由 worker 数量限制）。容量默认 **32**，通过环境变量
  `OJ_JUDGE_QUEUE_CAPACITY`（1..256 的整数）配置，非法值启动即报错退出，队列不会
  无限增长。入队判断与入队操作在同一把锁内原子完成，并发提交也无法突破容量。
- **满载行为**：等待队列已满时**立即拒绝**，不阻塞、不积压；提交接口返回 `503` +
  `{"error":"判题队列已满，请稍后重试","code":"JUDGE_QUEUE_FULL","retryable":true}`，
  并带 `Retry-After` 头；**前端不自动重试**（沿用通用错误展示，显示后端文案）。被拒
  请求在入队前即返回，不创建提交记录、不增加提交次数。
- **结果交付**：每个被成功接收的任务拥有独立 `std::promise/future` 结果通道，结果
  或异常必交付给对应请求，不串用、不重复完成、不永久等待；单个任务抛异常会被转换为
  内部错误（执行器异常在 `SubmitService` 内转为 `SYSERR` 记录，调度层兜底为内部错误），
  worker 不受影响，继续处理后续任务。
- **同步返回**：`POST /api/problems/{id}/submit` 仍同步等待该提交的判题与持久化结果，
  不是轮询接口；身份验证、首次改密检查、题目权限与参数检查及 JSON 响应约定保持不变。
- **排队时间口径**：原始提交时间在任务接受入队时采集，`runtime_ms` 仅为各测试点程序
  执行耗时之和；排队等待既不计入程序耗时，也不会误判 `TLE`（超时只在子进程执行阶段
  由 watchdog 判定）。并发任务乱序完成时，首次 AC 时间收敛为最早符合条件的原提交时间。
- **HTTP 并发协调**：cpp-httplib 请求处理线程数显式设为
  `worker 数 + 等待队列容量 + 8`。最坏情况下被同步等待占用的 HTTP 线程数不超过
  `worker 数 + 队列容量`，额外 8 个线程保证健康检查与题目查询在判题繁忙/队列满载时
  仍可响应，不把阻塞从判题器整体转移到 HTTP 层。
- **停止策略**：`shutdown()` 先停止接收新任务，并按 M3.2 的协作式取消通知正在执行与等待
  中的任务（不再启动新进程、终止正在运行的进程组），已接收任务各自得到明确结果并持久化后
  再回收 worker；`HttpServer::stop()` 先 `cancel_all()` 再停止 HTTP（含正在同步等待的请求），
  然后 `shutdown()` 回收 worker，最后才关闭数据库，保证取消结果先落库、无永久等待、无数据库
  关闭顺序错误。
- **配置与限制**：`OJ_JUDGE_QUEUE_CAPACITY` 见「配置方式」。全局 60s 硬上限、强制终止与
  服务停止取消见 M3.2；运行隔离与资源限制（tmpfs/随机目录/chroot/setrlimit/seccomp/
  输出上限/编译并发门限）见 M3.3 与下文「运行隔离与资源限制（M3.3）」。M3.4 完整分类与
  Sanitizer 默认接入已完成；M3.6 Rejudge 尚未实现。

### 持久化与停止清理（M3.5）

**结果保存时机与内容**：判题在 `SubmitService` 内完成后，于**单个短事务**中写入
`submissions`（用户、题目、语言、完整源码、最终状态、逐点结果 JSON、编译诊断、各点
耗时之和、峰值 RSS、提交时间）并更新 `user_problem_status`（提交次数、AC 状态与首次
AC 时间）。事务不覆盖排队、编译或程序运行阶段；两者任一失败整体回滚，提交接口返回
通用 `500`，绝不声称已保存。逐点 JSON 保留 WA 点的输入/期望/实际输出、结构化终止
原因、编译诊断截断标识与全局硬上限/取消等已有信息；**未采集到的内存以 `null` 表示，
未执行的测试点不写入结果**，绝不用 0 伪装测量值、绝不把未运行点记为通过。

**取消任务的状态与统计规则**：服务停止时已接收但尚未执行的任务被取消、不再启动新
进程；正在编译/运行的任务终止其进程组。取消属内部原因，按既有约定以 `SYSERR` 记录并
附带明确取消语义（不归咎为学生的 TLE/RE），其逐点结果与指标照常保存并计入提交次数。
未接收的请求（队列满载/调度器已停止）返回 `503`，不创建提交记录、不增加提交次数。

**责任归属与终态保护**：每个被接收任务由 `JudgeManager` 恰好执行一次 handler，结果
通过独立 future 交付，不重复处理、不重复计数；正常完成与取消竞争时以先到的终态为准，
只产生一条提交记录与一次计数。每条任务在接收时分配任务标识，接收、开始、完成、取消
以及清理失败日志均带该标识（或提交 ID），便于停止排障。

**客户端断开 vs 服务器取消**：已接收的提交在客户端断开后仍继续执行并保存（断开不
导致结果丢弃）；网络响应失败不会回滚已成功提交的数据库事务。

**停止顺序与超时预算**：`Ctrl+C`/`SIGTERM` 的信号入口只置位标志，复杂清理在主循环
之外执行。停止时：① `cancel_all()` 通知判题调度器取消（非阻塞）；② `svr_.stop()`
停止监听并等待 HTTP 处理线程（含同步等待判题的请求）结束；③ `shutdown()` 回收 worker
并等待所有已接收任务的结果先写库；④ 最后关闭数据库。等待预算为单次判题全局 60s 硬
上限，取消以约 20ms 周期被观察，正常情况下数秒内完成；停止耗时会记入日志，超出
30s 预算仅告警（不强制杀死线程、不宣称已保存全部结果），并核对「已接收/已完成」计数。

**清理边界与崩溃保障**：清理仅针对本任务拥有的工作目录与进程组——不按进程名批量杀
进程、不递归清理未确认路径、不卸载共享 tmpfs。清理可重复执行，资源已不存在时静默
收尾；真正清理失败会记录具体资源路径并保留现场，不吞掉异常后报告成功。已提交的数据
在重启后可从 SQLite（WAL）读取；`cron .dump` 备份见 M6。退出清理只保证正常停止流程，
不承诺在强制杀死时执行；进程被 `SIGKILL` 或整机断电时，进行中的任务由 M3.7 的在途
持久化与启动恢复兜底（见下节）。

### 崩溃恢复与在途任务持久化（M3.7）

提交被接收时，先经「容量预留 → 写入独立在途表 `in_flight_tasks` → 入队」，
在途记录包含用户、题目、语言、完整源码、原提交时间与持久化任务标识 `task_id`。
在途记录在结算前**不计入** `submit_count`、不设置 AC 状态，也不出现在题目通过人数与
排行榜统计中（独立表与 `submissions` 完全隔离，既有读取/统计接口无需改动）。

- **接收边界**：未通过鉴权/首次改密/参数/可见性/容量检查的请求不产生在途任务；
  在途记录写库失败返回 `500` 且不声称已接收；写库成功后才入队，故崩溃后仍能在库中找到。
- **原子结算**：判题完成后在同一个短事务内**先删除该在途记录（`changes==1` 才继续），
  再写入 `submissions` 并更新 `user_problem_status`**；删除未命中即回滚，保证同一任务只
  最终结算一次。统计沿用既有规则：每个最终保存的新提交计数一次、重复 AC 不重复增加
  通过人数、失败不清除 AC、首次 AC 时间取符合条件的最早**原提交时间**。
- **启动恢复**：服务启动、数据库迁移与判题环境检查完成后，扫描 `state='pending'` 的
  在途任务，将上次崩溃遗留的 `claimed` 重置为 `pending`，按有界队列容量分批（默认每批
  16 条）重新入队判题；使用**当前**题目配置与测试用例、保留原提交时间。题目不存在等
  无法判题时置 `interrupted` 并记录原因（保留任务信息、不再恢复）。恢复与普通提交共用
  同一 worker 池、有界队列、编译门限与沙箱，队列暂满时等待重试、绝不丢弃。
- **多实例互斥**：正式服务启动时对 `<db_path>.lock` 取 `flock` 排他锁；进程崩溃由操作
  系统自动释放，无需人工清理。任务级原子认领与结算唯一性约束作为最后防线，不引入分布式
  平台。
- **Rejudge 兼容**：重判不写、不读在途表，仍以原提交记录为数据源单事务更新；重判崩溃时
  原记录保持原样，不改变 M3.6 的失败处理策略。启动恢复仅覆盖新提交的在途任务。
- **保障边界**：SQLite 仍为 WAL + `synchronous=NORMAL`（未擅自调整）。`INSERT` 成功仅
  表示已提交到 WAL；`SIGKILL` 一般可恢复，整机断电的持久性受底层存储与同步级别限制，
  不承诺绝对保障。崩溃遗留的旧判题进程/工作目录沿用现有进程组与 `Workspace` 清理机制，
  不依据数据库中的旧 PID 杀进程、不按目录名批量删除。

### 运行隔离与资源限制（M3.3）

判题子进程在隔离环境中执行（`src/judge/sandbox.{h,cpp}` + `local_executor.cpp`）：

- **工作目录**：默认 `/opt/oj-tmpfs`（`OJ_JUDGE_WORKSPACE` 覆盖），每次判题以 `mkdtemp`
  原子创建随机目录（0700），用后仅删除本任务目录。启动时校验其为 tmpfs 并做一次真实
  沙箱自检，失败即拒绝启动（不会降级为无保护执行）；无挂载权限的开发环境需显式设置
  `OJ_JUDGE_ALLOW_NON_TMPFS=1`。挂载与容量见 `dependence.md` 3.9/8 节。
- **命名空间与最小根目录**：`user/mount/net/pid/ipc/uts` 命名空间 + tmpfs 根 + 只读
  bind `/usr`，用户程序无法读取沙箱外文件/受保护目录、无法越权写文件；工作目录在运行
  阶段只读。`/proc` 由 PID 命名空间隔离为仅本任务进程。
- **seccomp-bpf**：拒绝网络、挂载/逃逸、调试/内核接口、进程创建（运行阶段）、时间/
  主机名修改及 x32 ABI 等；每个任务独立进程组，成功/失败/超时/取消后清理进程与目录。
- **资源限制**：`RLIMIT_CPU/FSIZE/STACK/NOFILE/CORE`；内存**不使用 `RLIMIT_AS`**（与
  ASan/UBSan 不兼容），改以 RSS 采样超限强杀并标记 `MLE`，峰值写入逐点结果。
- **输出上限**：stdout 64 KiB、stderr 16 KiB、编译诊断 64 KiB，采集时截断且不挂死。
- **编译并发门限**：`OJ_JUDGE_COMPILE_CONCURRENCY`（默认 2）单独约束高内存的编译阶段；
  等待计入全局预算并可被取消。
- **环境隔离**：子进程仅获受控最小环境，绝不继承 `OJ_JWT_SECRET`/`OJ_ADMIN_PASSWORD`。
- **兼容性**：已验证 ASan/UBSan 程序在沙箱内正常启动运行，受控越界样例产生
  AddressSanitizer 诊断（ASan/UBSan 的默认接入与完整分类属 M3.4）。

### 判题器（`IExecutor` / `JudgeEngine`）

判题核心位于 `src/judge/`，不依赖 HTTP 与数据库，可独立调用和测试：

- `IExecutor`（`src/judge/executor.h`）：进程执行抽象，把「如何编译/运行子进程」与
  「如何比对、汇总」解耦；`LocalExecutor`（`src/judge/local_executor.h`）为本机实现，
  M3.3 起在进程级沙箱中执行。
- `JudgeEngine`（`src/judge/judge.h`）：验证输入 → 创建独立临时工作目录 → 编译一次 →
  按顺序逐测试点运行 → 归一化比对 → 汇总，返回 `JudgeResult`（含逐点结果）。
- `normalize_output` / `outputs_match`（`src/judge/comparator.h`）：输出归一化与比对，
  纯函数，可单测。

**调用方式**：M1.6 起已通过 `POST /api/problems/{id}/submit` 接入 HTTP 与持久化
（见「提交接口」一节）；判题核心仍可独立调用，用于开发环境不经过 HTTP 与数据库地
验证判题流程（见「测试」一节的 `judge_unit` / `judge_integration`）。库调用示例：

```cpp
oj::judge::LocalExecutor executor;
oj::judge::JudgeOptions options;            // 可选：workspace_root、超时、输出上限等
oj::judge::JudgeEngine engine(executor, options);

oj::judge::JudgeTask task;
task.language   = "cpp17";                  // 或 "c11"
task.source_code = source;
task.testcases  = {{"1 2\n", "3\n"}};       // input / expected output
task.time_limit_ms = 2000;
oj::judge::JudgeResult result = engine.judge(task);
result.status;                              // AC/WA/CE/TLE/RE/MLE/SYSERR
```

当前支持的判题行为：

- **语言与编译**：C++17 使用 `g++ -O2 -std=c++17 <src> -o program -lm`，C11 使用
  `gcc -O2 -std=c11 <src> -o program -lm`；可执行文件路径在父进程解析（含 `PATH`
  搜索）后由子进程以参数数组 `execv` 启动，不拼接 shell 命令，也不在 `fork` 后调用
  非异步信号安全的复杂逻辑（M3.1 多线程 fork 安全）。ASan/UBSan 的默认接入属 M3.4；
  M3.3 已通过 `JudgeOptions::extra_compile_flags` 验证 `-fsanitize` 程序与沙箱兼容。
- **工作目录**：每次判题在 `OJ_JUDGE_WORKSPACE`（默认 tmpfs `/opt/oj-tmpfs`）下经
  `mkdtemp` 创建随机目录，保存源码与编译产物，结束后只删除本次目录（M3.3）。
- **编译结果**：编译进程正常且退出码为 0 视为成功；非零退出码返回 `CE`，并附有长度
  上限的编译诊断；编译器不存在、无法创建目录等环境故障返回 `SYSERR`，不伪装为 `CE`；
  编译过程有保护超时。
- **逐点运行**：每个测试点启动**新的**进程，输入经标准输入传入，标准输出与标准错误
  分别采集；输出有界（标准输出 64KB、标准错误 16KB，超限只标记截断并继续排空管道），
  标准输出超限不会被当作正常输出判为 `AC`。
- **超时与全局硬上限（M3.2）**：单测试点采用**墙钟经过时间**限制（非精确 CPU 时间），
  从子进程启动到回收计时；超时 `SIGKILL` 强杀整个进程组并通过 `waitpid` 回收，计为 `TLE`。
  另有单次判题**全局 60s 硬上限**（`JudgeOptions::global_time_limit_ms`，默认 60000ms，
  覆盖编译与全部测试点，不含排队等待，进入新点不重置）；每个测试点有效时限 =
  `min(题目时限, 剩余全局预算)`，全局耗尽后不再启动新点并保留已有结果（总体 `SYSERR`），
  未执行点不伪造。程序无任何输出时也能发现超时（`steady_clock` + 周期轮询）。
- **进程组与 fd 卫生（M3.2）**：每个编译/运行进程在子进程中 `setpgid(0,0)` 建立独立进程组，
  超时/取消/后代占用管道时以 `kill(-pid, SIGKILL)` 清理编译器与用户程序的**后代进程**；
  子进程 `exec` 前关闭继承的无关 fd（监听套接字、数据库连接、其它任务管道）。M3.3 起运行
  阶段由 seccomp 禁止创建进程，恶意进程逃逸与干扰进一步由 M3.3 沙箱约束。
- **服务取消（M3.2）**：停止服务时 `JudgeManager` 取消全部已接收任务——正在运行的进程组被
  终止、等待队列中的任务不再启动新进程，取消结果按内部错误 `SYSERR` 持久化。
- **比对**：去除每行行尾空白与文末空行后逐字符比较；保留行首空白、行内空白与中间
  空行的意义，不做分词比较。程序非正常退出（信号或非零退出码）计为 `RE`，即使输出
  碰巧与期望一致也不判 `AC`。
- **汇总**：按顺序执行全部测试点，普通 `WA` 不阻止后续测试点；全部 `AC` 才返回 `AC`，
  否则按 `SYSERR > TLE > MLE > RE > WA > AC` 取最严重状态。全局硬上限或服务取消会终止
  剩余执行并覆盖为 `SYSERR`。空测试集、非法语言、无效时间配置均返回明确的 `SYSERR`，
  绝不因未执行任何测试点而返回 `AC`。
- **清理**：成功、`CE`、`RE`、`TLE`、全局上限、取消等所有路径都会关闭文件描述符、回收
  子进程（含后代进程组）并删除本次临时目录；不以系统范围的进程名匹配清理。

> **安全边界**：M3.3 已提供进程级隔离与资源限制（命名空间 + chroot + seccomp +
> setrlimit/RSS 限制 + tmpfs 随机目录 + 输出上限），M3.4 已默认全开 ASan/UBSan 并
> 完成统一异常分类，均以真实 Linux 进程受控样例验证。但受控样例通过不代表可安全公开
> 运行任意不可信代码；仍需 M3.5/M5 的完整回归与验收。

### 登录限速

- 维度：来源 IP（`remote_addr`，不信任 `X-Forwarded-For` 等客户端提供的转发头）。
- 阈值：同一 IP 在 5 分钟窗口内连续 **5 次** 登录失败后，第 6 次起返回 `429`。
- 解除：窗口自最早失败时刻起 5 分钟后自动解除；登录成功会清空该 IP 的失败计数。
- 触发限速时响应含 `Retry-After`（秒）头；登录页据此提示「登录过于频繁，在 N 分钟后
  就会消除，届时可以正常进行登录」，让用户知道无需反复尝试。
- 实现为进程内状态（线程安全），不跨重启持久化；重启后计数清零。

### 错误约定

错误响应体统一为 `{"error":"..."}`，内部故障返回通用文案，不泄露数据库/哈希/密钥/token 细节：

| 状态码 | 含义 |
|---|---|
| `200` | 登录成功 / 获取用户信息成功 / 修改密码成功 / 提交并返回判题结果（含 CE/WA/TLE/RE/SYSERR） |
| `201` | 注册成功 |
| `400` | 非法输入：JSON 解析失败、字段缺失/类型错误、非法昵称/密码/账号/密码为空、非法新密码、新密码与旧密码相同、非法题目 ID、不支持的语言、空/纯空白/超长源码 |
| `401` | 登录失败（账号或密码错误）/ 认证无效（缺失、损坏、伪造、篡改、过期、无签名、算法不匹配或引用不存在用户等 token）/ 旧密码错误 |
| `403` | 权限不足（非管理员访问管理员功能）/ 必须先改密（含 `code:"PASSWORD_CHANGE_REQUIRED"`） |
| `404` | 题目不存在，或当前用户无权查看/提交的隐藏题目（统一返回，不区分）；管理员用户接口的目标用户不存在 |
| `409` | 昵称已被使用（含并发冲突）/ 不能取消最后一个管理员的权限 |
| `413` | 请求体超过服务器上限（1 MiB，在解析前拒绝） |
| `429` | 登录尝试过于频繁（触发限速） |
| `503` | 判题等待队列已满（`code:"JUDGE_QUEUE_FULL"` + `Retry-After`）或调度器已停止（`code:"JUDGE_UNAVAILABLE"`）；未接收，不落库、不计次 |
| `500` | 内部故障（含提交持久化失败，不声称已保存，不泄露 SQL/路径） |

### 停止服务

前台运行时按 `Ctrl+C`（或发送 `SIGTERM`）即可优雅停止：服务会先停止监听并通知判题调度器
取消（不再接收新任务，终止正在运行的判题进程组，等待队列中的任务不再启动新进程），等待
同步等待判题的 HTTP 请求结束，再回收 worker，最后关闭数据库——取消结果先于数据库关闭写入，
无永久等待、无未回收 worker、无数据库关闭顺序错误。停止流程会记录耗时与「已接收/已完成」
任务数；重复（含并发）停止通知幂等安全。若停止耗时超过 30s 预算仅记录告警，不会强制杀死
线程、也不宣称已保存全部结果（详见「持久化与停止清理（M3.5）」）。验证方式：

```bash
# 前台启动后按 Ctrl+C，随后确认无残留进程
pgrep -a oj_server
```

停止后可在同一端口重新启动。

## 数据位置与持久化

| 位置 | 内容/用途 | 权限 | 持久性 |
|---|---|---|---|
| `data/oj.db` | SQLite 主库（用户、题目、用例、提交、状态、在途任务） | 服务账号可读写 | **持久化**（业务数据唯一真源） |
| `data/oj.db-wal` | WAL 日志（已提交但未检查点的数据） | 服务账号可读写 | **运行辅助**，随检查点收敛；**运行中勿删除/移动/单独备份** |
| `data/oj.db-shm` | WAL 共享内存索引 | 服务账号可读写 | 运行辅助，运行中勿删 |
| `data/oj.db.lock` | 单实例 `flock` 锁文件（M3.7） | 服务账号可读写 | 运行辅助，进程退出即释放 |
| `/opt/oj-tmpfs`（`OJ_JUDGE_WORKSPACE`） | 每次判题的 `oj_judge_XXXXXX` 随机目录（源码、编译产物、运行目录） | 根目录 `1777`，子目录 0700 | **临时**（tmpfs；用后即删，重启消失） |
| `backup/oj-YYYYMMDD.sql` | `sqlite3 .dump` 逻辑备份（含密码哈希/源码/隐藏用例） | `0600`，仅服务账号 | **持久化**（外部备份，已被 `.gitignore` 忽略） |
| 服务日志 | 启动/停止/判题等运行日志；无内置日志文件，输出到 stdout/stderr | 由运行方式决定 | 临时/由外部收集 |
| `build/regression-logs/<时间戳>/server.log` | 冒烟回归的服务日志 | 可重建 | 临时（`build/` 已忽略） |

- **持久化 vs 临时**：只有 `data/oj.db` 与 `backup/*.sql` 是需要保留的业务数据；
  `-wal`/`-shm`/`-lock` 是运行辅助，tmpfs 判题目录是临时产物，构建与回归日志可重建。
- `data/oj.db`、`data/oj.db-wal`、`data/oj.db-shm`、`data/*.lock`、`backup/*`、
  `build/`、`*.log` 均已被 `.gitignore` 忽略，不进入版本控制。
- 真实密钥（`OJ_JWT_SECRET`）与初始口令（`OJ_ADMIN_PASSWORD`）**不在数据库内**，
  也不在项目中，只由运行环境的环境变量提供；备份文件不含它们，恢复后需另行准备。
- 判题临时文件不在 `data/` 或备份范围，服务停止/判题结束会按任务清理；崩溃遗留的
  旧目录不会依据数据库旧 PID 被接管。

## 故障排查

> 基本原则：先看**服务日志**与 `/api/health`，再对照配置。**不要**以关闭沙箱/隔离、
> 删除数据库或反复重启作为默认手段。以下均对应实际实现或启动检查。

| 现象 | 常见原因 | 处理 |
|---|---|---|
| 启动即退出，报 JWT/密码配置错误 | `OJ_JWT_SECRET` 缺失/过短；首次初始化未设 `OJ_ADMIN_PASSWORD` | 按第 5 步设置环境变量；确认长度 ≥ 16 字节。日志会给出明确原因 |
| `bind` 失败 / 端口被占用 | `--port` 已被其它进程占用 | `ss -ltnp | grep <端口>` 找到占用者；改用其它端口或停止占用进程。不要用自动重启掩盖 |
| 数据库不可写 / 锁等待失败 | `data/` 无写权限；另有实例持锁（`.lock`）；磁盘满 | 检查 `data/` 属主权限、用 `ss`/`pgrep -a oj_server` 确认无重复实例；SQLite `busy_timeout` 为 5s，长期竞争会返回 `500` 而非假成功 |
| 静态资源目录错误 / 页面 404 或空白 | `--web` 指向错误；`web/` 不存在 | 确认 `--web web` 存在且含 `index.html`；静态托管仅覆盖该目录。用 `curl -i http://127.0.0.1:<端口>/api/health` 先确认服务在跑 |
| 判题启动失败：非 tmpfs / 沙箱自检失败 | `/opt/oj-tmpfs` 未挂载或非 tmpfs；内核禁止非特权用户命名空间/AppArmor 限制 | 按第 4 步挂载 tmpfs；按 `dependence.md` 1.2 检查内核能力。**不要**用 `OJ_JUDGE_ALLOW_NON_TMPFS` 或关闭隔离来「修复」正式部署 |
| 提交返回 `CE`，但代码正确 | 编译器/依赖缺失（`g++`/`gcc` 不在 PATH）、编译超时 | 检查 `g++ --version`/`gcc --version`；查看返回的编译诊断（已清洗内部路径）。环境故障会记为 `SYSERR` 而非 `CE` |
| 提交返回 `503 JUDGE_QUEUE_FULL` | 等待队列（默认 32）已满 | 稍后重试；前端不自动重试。可评估 `OJ_JUDGE_QUEUE_CAPACITY` 与 `OJ_JUDGE_COMPILE_CONCURRENCY`，但受 3.3 GiB 内存约束，勿盲目调高 |
| 资源不足 / 判题被 `SIGKILL` 或整体变慢 | 内存紧张（无 Swap）、并发过高 | `free -h` 确认可用内存；下调编译门限/减少并发；构建坚持 `--parallel 1`。恢复内存后再启动 |
| Windows 转发后空白/一直加载 | 端口未转发；服务未启动或启动失败 | 在 VS Code「端口」面板确认 `8080` 已转发并使用其实际本地地址；先在服务器本机 `curl` 健康检查。见第 9 步 |
| 重启后出现异常判题/统计变化 | M3.7 启动恢复按当前题目配置重新入队上次崩溃遗留的在途任务；或恢复了旧备份 | 查启动日志是否有「启动恢复」提示；见「崩溃恢复与在途任务持久化（M3.7）」与「从备份恢复数据库」的在途说明 |

- 排查入口：服务 stdout/stderr 日志；`curl -sS --max-time 5 http://127.0.0.1:<端口>/api/health`；
  `build/regression-logs/<时间戳>/server.log`（冒烟回归）；`ctest -R <name> --output-on-failure`（测试）。
- 停止服务见「停止服务」；备份/恢复见下一节。

## 备份与恢复（M6.2）

> 部署视角的简明入口：备份脚本 `scripts/backup.sh`，恢复步骤见本节。
> 数据库备份与**密钥/服务配置等外部文件分开**：备份只含数据库逻辑内容，
> **不含** `OJ_JWT_SECRET`、`OJ_ADMIN_PASSWORD`、`web/` 或 tmpfs 文件。
> 恢复必须**先在隔离路径验证**（新建库 + `integrity_check`/`foreign_key_check` +
> 关键数据核对），确认后再按本节「正式恢复的操作顺序」切换；**不要**在服务运行时
> 直接覆盖正式库。cron 需单独配置，**本轮不安装**。

> 本节说明 `scripts/backup.sh` 的用法、cron 定时配置与数据库恢复步骤。
> **状态**：备份脚本与恢复说明已实现，并已通过独立测试验证（备份可在全新隔离库恢复，
> 完整性与关键数据核对通过）。逐项结果见 `tests/M6.2-test-report.md`。

### 备份脚本 `scripts/backup.sh`

使用 `sqlite3 .dump` 将运行中的数据库导出为 SQL 文本，默认写入 `backup/oj-YYYYMMDD.sql`。
脚本读取数据库时包含 WAL 中已提交的数据，**不**直接复制主库文件，**不**删除或移动
运行中的 `-wal`/`-shm`；在单个读事务内导出，多张表来自同一数据库快照。

| 项目 | 值 |
|---|---|
| 源数据库 | `--db <路径>`，或 `OJ_DB`，默认 `<仓库>/data/oj.db` |
| 备份目录 | `--out-dir <目录>`，或 `OJ_BACKUP_DIR`，默认 `<仓库>/backup` |
| 文件名 | `oj-YYYYMMDD.sql`（日期时区见下） |
| 文件权限 | `0600`（含密码哈希、源码与隐藏用例，限制为仅属主可读写） |
| 退出码 | `0` 成功（或 `--no-replace` 跳过）；`1` 备份执行失败；`2` 用法/前置条件错误 |

```bash
bash scripts/backup.sh --help
bash scripts/backup.sh
bash scripts/backup.sh --db /srv/oj/app/data/oj.db --out-dir /srv/oj/backup
bash scripts/backup.sh --no-replace          # 当天文件已存在则不覆盖
echo "退出码：$?"
```

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `OJ_DB` | `<仓库>/data/oj.db` | 源数据库路径 |
| `OJ_BACKUP_DIR` | `<仓库>/backup` | 备份目录 |
| `OJ_BACKUP_TZ` | 系统本地时区 | **日期所用时区**（如 `Asia/Shanghai`、`UTC`） |
| `OJ_BACKUP_BUSY_TIMEOUT_MS` | `5000` | SQLite 锁等待毫秒数 |
| `OJ_BACKUP_TIMEOUT` | `600` | 单次导出总执行超时（秒），超时终止并失败 |
| `OJ_BACKUP_LOCK_WAIT` | `30` | 等待备份互斥锁的秒数，超时失败 |
| `OJ_BACKUP_NO_REPLACE` | `0` | 取 `1` 时当天已存在则跳过，不覆盖 |
| `OJ_BACKUP_SKIP_SPACE_CHECK` | `0` | 取 `1` 时跳过磁盘空间预检 |

**日期时区**：默认使用**服务器本地时区**（由系统时区决定）；可用 `OJ_BACKUP_TZ` 显式指定，
脚本会在开始日志中打印实际生效的时区与偏移。注意数据库内 `created_at` 等时间戳为 UTC，
文件命名时区与库内时间口径不必相同。cron 调度时间为系统本地时间，建议与 `OJ_BACKUP_TZ` 保持一致。

**一致性与成功判定**：脚本先确认源库存在且可读（避免路径写错时 SQLite 自动创建空库后误报成功），
再在单连接中执行 `BEGIN;` → `.dump` → `COMMIT;`，并设置锁等待与总执行超时。导出先写入备份
目录中的临时文件（`0600`），随后校验「sqlite3 退出码为 0」「文件非空」「含表结构」「以 `COMMIT;`
正常收尾」，全部满足才原子替换为正式文件。**文件非空本身不代表成功**；失败时不会留下看似可用的
正式文件，也不会预先删除当天已有的成功备份。

**并发与同日重跑**：同一备份目录用 `flock` 目录级互斥，避免并发任务同时写入；等待超过
`OJ_BACKUP_LOCK_WAIT` 秒即失败。同一天重复执行默认在新备份**完整产出后原子替换**已有
`oj-YYYYMMDD.sql`；加 `--no-replace`/`OJ_BACKUP_NO_REPLACE=1` 则保留已有文件并跳过。临时文件
名含日期与随机后缀，避免日期变化或冲突造成混乱。

**依赖**：`sqlite3`（见 `dependence.md` 3.2 节），以及 `flock`/`timeout`/`mktemp`/`stat`
（util-linux 与 coreutils，Ubuntu 默认已装）。缺失任一依赖即明确失败，不静默降级。

**日志与安全**：脚本只输出开始/结束、目标路径、大小、表数、耗时与失败原因，**不**打印 SQL 全文、
密码哈希、源码或隐藏用例。备份目录已被 `.gitignore` 忽略（仅保留 `backup/.gitkeep`），不在 `web/`
静态托管范围，也不提交到 Git。本阶段**不**自动删除历史备份、不设置保留天数、不上传云端。
磁盘不足或写入失败时明确失败，**不**自动清理其它数据腾空间。

### cron 定时备份配置说明

cron 环境不读取交互终端的当前目录与配置：请使用**绝对路径**、显式设置 `PATH`，并以运行服务的
同一账号执行（见下「执行账号权限」）。以下示例**仅作说明，本轮不修改任何 crontab、不启用定时任务**。

以服务账号（示例 `oj`）编辑用户 crontab：`sudo -u oj crontab -e`

```cron
SHELL=/bin/bash
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
OJ_BACKUP_TZ=Asia/Shanghai
# 每天 03:30（系统本地时区）备份，日志追加写入独立日志文件
30 3 * * * /srv/oj/app/scripts/backup.sh --db /srv/oj/app/data/oj.db --out-dir /srv/oj/backup >> /srv/oj/log/backup.log 2>&1
```

若使用 `/etc/cron.d/oj-backup`（系统级，多一个用户名字段）：

```cron
SHELL=/bin/bash
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
30 3 * * * oj /srv/oj/app/scripts/backup.sh --db /srv/oj/app/data/oj.db --out-dir /srv/oj/backup >> /srv/oj/log/backup.log 2>&1
```

- **执行时间**：示例为每天 03:30。cron 的调度时间使用系统本地时区；如需特定时区，请设置服务器
  时区或使用 cron 实现支持的 `CRON_TZ`（取决于 cron 版本），并让 `OJ_BACKUP_TZ` 与之保持一致。
- **运行账号**：使用运行 OJ 服务的账号，**不要**用 root 运行整个服务或备份。
- **绝对路径**：`backup.sh`、`--db`、`--out-dir`、日志文件均用绝对路径；脚本自身虽会解析仓库根，
  但 cron 环境不应依赖当前目录。
- **必要环境**：至少设置 `PATH`；如需固定日期时区设置 `OJ_BACKUP_TZ`。日志文件所在目录须预先
  存在且运行账号可写。
- **日志位置**：将 stdout/stderr 追加到独立日志，便于事后核对；脚本不会自建日志目录。

**手动调用与结果核对**：

```bash
# 以与 cron 相同的账号执行同样的命令
bash scripts/backup.sh --db /srv/oj/app/data/oj.db --out-dir /srv/oj/backup; echo "exit=$?"
ls -l /srv/oj/backup/oj-*.sql
tail -n 20 /srv/oj/log/backup.log          # 应含“[backup] 备份完成”或明确的失败原因
```

**确认定时任务实际运行**：

```bash
sudo -u oj crontab -l                       # 查看已配置的条目
journalctl -u cron --since today | grep -i backup   # 或 grep CRON /var/log/syslog
ls -l /srv/oj/backup/oj-$(date +%Y%m%d).sql # 当天应产生新文件（按脚本时区）
```

**执行账号权限（最小化）**：

- 推荐由**运行服务的同一账号**执行备份：WAL 模式下读取数据库需要访问 `-shm`，同账号最省心。
- 需要：对 `oj.db` 及 `oj.db-wal` 的**读**权限；对数据库所在**目录**的读+写权限（SQLite 使用
  `-shm`/锁文件）；对备份目录的**读+写+创建**权限；对 `scripts/backup.sh` 与 `sqlite3` 的执行权限。
- 首次创建备份目录、设置属主等管理操作可由 root 完成，但**定时执行本身不要求 root**。
- 定时执行同样遵守脚本的 `flock` 并发锁与磁盘空间预检；避免与其它重任务重叠导致资源紧张。

### 从备份恢复数据库

> M6.4 最终验收已在隔离环境实际执行本节流程：`scripts/backup.sh` 生成备份 → 导入全新
> 隔离库 → `integrity_check`/`foreign_key_check` 与关键数据核对 → 启动恢复库并观察
> M3.7 在途任务恢复结算（不重复计数）。逐项结果见 `tests/M6.4-acceptance-report.md`。
> 以下步骤为正式恢复流程与注意事项；正式切换仍须按第六步在维护窗口执行。

**总原则**：SQL 备份先恢复到**新建的隔离数据库路径**，完成核对后再按正式流程切换；不要在服务
仍连接数据库时直接覆盖主库，也不要混用旧库的 `-wal`/`-shm` 与恢复出的新库。

**第一步：隔离恢复（新库，不影响正式库）**

```bash
RESTORE_DB=/srv/oj-restore/oj-restore-$(date +%Y%m%d).db
mkdir -p "$(dirname "$RESTORE_DB")"
# 从 SQL 文本导入到全新数据库文件（SQL 内含 BEGIN TRANSACTION; ... COMMIT;）
# -bail：任一语句出错即停止并返回非零，避免“出错但退出码仍为 0”的假成功
set -o pipefail
sqlite3 -bail "$RESTORE_DB" < /srv/oj/backup/oj-YYYYMMDD.sql; echo "import exit=$?"
```

- 检查导入退出码：非 0 表示导入中断，应排查日志后重做，不要使用不完整的库。
- `.dump` 输出开头含 `PRAGMA foreign_keys=OFF;`，因此导入期间不校验外键，导入后再独立校验。

**第二步：完整性与关键业务数据核对（只读检查）**

```bash
sqlite3 --readonly "$RESTORE_DB" "PRAGMA integrity_check;"          # 期望 ok
sqlite3 --readonly "$RESTORE_DB" "PRAGMA foreign_key_check;"        # 期望无输出
# 关键数据存在性与数量（不打印密码哈希/源码/隐藏用例内容）
sqlite3 --readonly "$RESTORE_DB" "SELECT account, role, reset_pwd_flag FROM users WHERE account='admin';"
sqlite3 --readonly "$RESTORE_DB" "SELECT (SELECT count(*) FROM users) AS users,
  (SELECT count(*) FROM problems) AS problems,
  (SELECT count(*) FROM testcases) AS testcases,
  (SELECT count(*) FROM submissions) AS submissions,
  (SELECT count(*) FROM user_problem_status) AS status_rows,
  (SELECT count(*) FROM in_flight_tasks) AS in_flight;"
sqlite3 --readonly "$RESTORE_DB" "SELECT is_sample, count(*) FROM testcases GROUP BY is_sample;"   # 0=隐藏用例 1=公开样例
sqlite3 --readonly "$RESTORE_DB" "SELECT status, count(*) FROM submissions GROUP BY status;"      # 判题结果分布
```

恢复说明覆盖的数据范围（均存于逻辑备份）：`users`（用户与管理员，含角色与首改标记）、`problems`
（题目）、`testcases`（公开样例 `is_sample=1` 与隐藏用例 `is_sample=0`，**含隐藏用例**）、
`submissions`（完整提交源码、状态、逐点结果、编译信息、耗时/内存、原提交时间）、
`user_problem_status`（用户题目状态、首次 AC 时间、提交次数），以及 M3.7 的 `in_flight_tasks`
（在途记录与结算元数据：`task_id`/原提交时间/状态 `pending`/`claimed`/`interrupted`）。

**第三步：SQL 逻辑备份内容 vs 运行配置（边界）**

- 会保留：表结构与列定义、`CHECK` 约束、`UNIQUE`/主外键与索引（以 `CREATE` 语句写入）、
  触发器/视图（若有）、以及 `AUTOINCREMENT` 计数（`sqlite_sequence` 同步转储）。
- **不会**包含：连接级与会话级 PRAGMA（如 `journal_mode=WAL`、`synchronous`、`foreign_keys`、
  `busy_timeout`）。恢复后数据库为默认回滚日志模式，需由现有初始化流程重新开启 WAL 与外键；
  正式服务启动时会自动执行（见下）。
- **不会**备份：`OJ_JWT_SECRET`、`OJ_ADMIN_PASSWORD` 等运行配置、服务端环境变量、`web/` 静态
  前端、判题 tmpfs 外部文件或其它非数据库内容。恢复时需另行准备这些配置。

**第四步：首次打开恢复库时的初始化与迁移（幂等、不重置）**

用现有服务打开恢复库（`--db <恢复库路径>`）时，`initialize_schema` 只会**新增**缺失的表/列与
索引（`CREATE TABLE IF NOT EXISTS` + 受检的 `ALTER TABLE ADD COLUMN`），并重新启用 WAL 与外键；
它**不会**重置已有管理员密码、**不会**覆盖角色或首改标记、**不会**自动写入种子数据（种子题仅在
显式 `--seed` 时导入）。恢复验证必须使用**独立配置**：独立的 `--db`、独立的 `--port`、独立的
`OJ_JUDGE_WORKSPACE`，不得与正式服务共用端口或判题目录。

**第五步：M3.7 在途任务的恢复行为（重点）**

- 备份快照可能包含**尚未结算**的在途任务（`state='pending'` 或 `claimed`）。启动恢复后的服务时，
  `RecoveryService` 会把失效的 `claimed` 重置为 `pending`，并**按当前题目配置与用例重新入队判题**。
  因此：**仅为查看数据时不要启动服务**，请用 `sqlite3 --readonly` 查询，避免无提示触发判题。
- 快照之后产生的新提交或修改**不在**备份中，不能视为已包含；已标记 `interrupted` 的记录会保留
  但不再恢复。
- 恢复库中不存在旧的 `-wal`/`-shm`（由 SQL 文本新建），可避免混用旧 WAL 导致不一致。

**第六步：正式恢复的操作顺序（先验证，再切换）**

1. 先完成上述**隔离恢复与核对**，确认完整性与关键数据无误。
2. 安排维护窗口，**优雅停止**相关服务（`SIGTERM`/`Ctrl+C`），确认进程已退出、无写入方；此时快照
   之后未保存的提交无法恢复。
3. **保留旧数据与回退路径**：将旧的 `oj.db` 连同 `oj.db-wal`、`oj.db-shm`、`oj.db.lock` 整体
   改名备份到带时间戳的位置，**不要删除**，以便回退。
4. 将恢复库放到确认的路径（或通过 `--db` 指向新路径），设置属主/权限为服务账号，确保其同级目录
   **没有**旧库的 `-wal`/`-shm` 残留；以 SQL 导入方式新建的恢复库本身没有 WAL。
5. 按确认的方案启动服务，检查健康检查、管理员登录、题目列表与只读数据一致性；如需验证判题，
   务必在隔离环境进行。
6. 如出现异常，用步骤 3 保留的旧数据回退。

> 本阶段只交付 `scripts/backup.sh` 与恢复说明，**不**提供自动覆盖正式数据库的一键恢复命令；
> 如后续需要，须先完成独立恢复验证并明确切换方案。
