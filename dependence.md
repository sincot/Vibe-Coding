# 依赖清单与安装指南

> 适用环境：Ubuntu 22.04 LTS（Jammy）x86_64
> 依据 `SPEC.md` 的实现方案制定（后端 C++ / cpp-httplib，判题 g++/gcc + ASan/UBSan，存储 SQLite，沙箱 seccomp-bpf）
>
> 本文件记录依赖的来源（系统包 / 源码）、用途与安装方式。版本为在 Ubuntu 22.04 软件源中核实到的候选版本（`apt-cache policy`），随系统更新可能小幅变化。

---

## 1. 操作系统

- 发行版：**Ubuntu 22.04 LTS（Jammy Jellyfish）**
- 架构：x86_64（内核 5.15.0-*）
- 说明：以下安装命令以「空白系统 + 拥有 sudo 权限的普通用户」为前提。

---

## 2. 依赖总览

### 2.1 系统包（`apt` 安装）

| 软件/库 | 版本（Jammy 候选） | 用途 |
|---|---|---|
| `build-essential` | 12.9（gcc/g++ 11.2.0） | 编译服务端；同时作为判题语言 C++17 / C11 的编译器（ASan/UBSan 随 gcc 自带） |
| `cmake` | 3.22.1 | 构建工程（M0.2 起使用） |
| `sqlite3` | 3.37.2 | SQLite 命令行工具，备份脚本 `sqlite3 .dump` 使用 |
| `libsqlite3-dev` | 3.37.2 | SQLite 的 C 开发头文件/库，服务端数据层接入 |
| `libseccomp-dev` | 2.5.3 | seccomp-bpf 沙箱（禁网络 / 文件读写 / 读 /proc 等危险系统调用） |
| `libargon2-dev` | 0~20171227 | argon2id 密码哈希（AUTH-03） |
| `libssl-dev` | 3.0.2 | OpenSSL 3.0，JWT HS256 签名所需 libcrypto |
| `nlohmann-json3-dev` | 3.10.5 | JSON 序列化 / 解析（API 与判题逐点结果） |
| `libcpp-httplib-dev` | 0.10.3 | 后端 HTTP 服务（静态资源托管 + JSON API） |
| `cron` | 3.0pl1 | 定期 `.dump` 备份（PERS-04） |
| `curl` | 7.81.0 | 回归脚本 `scripts/regression.sh` 发起 HTTP 请求 |

### 2.2 源码安装（APT 无包）

| 软件/库 | 来源 | 用途 |
|---|---|---|
| `jwt-cpp` | GitHub `Thalhammer/jwt-cpp`（header-only） | JWT 生成 / 校验（AUTH-04） |

> 说明：jwt-cpp 为 header-only 库，Ubuntu 22.04 未收录（仅有 C 语言版 `libjwt`，非本项目所用 C++ 库），故采用源码安装。

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

### 3.3 判题沙箱（seccomp）

```bash
sudo apt install -y libseccomp-dev
```

### 3.4 认证与安全

```bash
sudo apt install -y libargon2-dev libssl-dev
```

### 3.5 JSON 与 HTTP 服务

```bash
sudo apt install -y nlohmann-json3-dev libcpp-httplib-dev
```

### 3.6 jwt-cpp（header-only，源码安装）

```bash
cd /tmp
git clone https://github.com/Thalhammer/jwt-cpp.git
# 建议固定版本：cd jwt-cpp && git checkout <release-tag>
sudo cp -r jwt-cpp/include/jwt-cpp /usr/local/include/
rm -rf jwt-cpp
```

- 安装后以 `#include <jwt-cpp/jwt.h>` 使用；HS256 依赖 OpenSSL（已由 `libssl-dev` 提供）。

### 3.7 运维辅助

```bash
sudo apt install -y cron curl
sudo systemctl enable --now cron
```

### 3.8 判题 tmpfs 运行目录（一次性挂载）

```bash
sudo mkdir -p /opt/oj-tmpfs
sudo mount -t tmpfs -o size=2G,mode=1777 tmpfs /opt/oj-tmpfs
```

- 说明：
  - **不能加 `noexec`**：判题需在此目录执行编译产物。
  - `size=2G` 为容量上限，可按机器内存调整；`mode=1777` 允许判题子进程写入。
  - 该挂载点在 `/opt` 下、仓库目录之外，重启后需重新挂载。

- 开机自动挂载：在 `/etc/fstab` 追加一行

  ```
  tmpfs /opt/oj-tmpfs tmpfs defaults,size=2G,mode=1777 0 0
  ```

---

## 4. 安装后验证

```bash
g++ --version && gcc --version
cmake --version
sqlite3 --version
dpkg -l libseccomp-dev libargon2-dev libssl-dev nlohmann-json3-dev libcpp-httplib-dev | tail -n +6
test -f /usr/local/include/jwt-cpp/jwt.h && echo "jwt-cpp OK"
mount | grep oj-tmpfs
```

---

## 5. 不需要安装的部分

- 前端：原生 HTML/CSS/JS + CodeMirror（CDN 引入，UI-04），无构建流程、无需安装。
- JWT 逻辑、argon 哈希调用、判题器、Rejudge、日志等均为本项目代码实现。
- tmpfs 挂载与 cron 定时任务属于运行时配置，非软件安装。

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

数据库（`libsqlite3-dev`）与密码哈希（`libargon2-dev`）在 3.2 / 3.4 节已列出，
安装命令见对应小节；无额外新增系统包。
