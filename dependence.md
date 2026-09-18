# 依赖清单与安装指南

> 适用环境：Ubuntu 22.04 空白系统
> 依据 `SPEC.md` 的实现方案制定（后端 C++ / cpp-httplib，判题 g++/gcc + ASan，存储 SQLite，沙箱 seccomp）

---

## 1. 依赖总览

| 软件/库 | 用途 | 来源 |
|---|---|---|
| `build-essential`（gcc/g++/make） | 编译服务端；判题语言 C++17/C11；ASan/UBSan 随 gcc 自带 | apt |
| `cmake` | 构建工程 | apt |
| `sqlite3` + `libsqlite3-dev` | SQLite 存储 + 备份 `.dump` | apt |
| `libseccomp-dev` | seccomp-bpf 沙箱（禁网/文件/读 proc） | apt |
| `libargon2-dev` | 密码哈希 argon2id（AUTH-03） | apt |
| `libssl-dev` | JWT HS256 签名所需 OpenSSL | apt |
| `nlohmann-json3-dev` | JSON 序列化/解析 | apt |
| `libcpp-httplib-dev` | 后端 HTTP 服务（静态托管 + JSON API） | apt |
| `jwt-cpp` | JWT 生成/校验（header-only） | GitHub 下载 |
| `cron` | cron 定期 `.dump` 备份（PERS-04） | apt |
| `curl` | 回归脚本 `scripts/regression.sh` 用 | apt |
| tmpfs 挂载点 | 判题一次性运行目录（JUDGE-04） | mount（运行时） |

> 说明：`libcpp-httplib-dev` 在 Ubuntu 22.04 中版本约 0.9.x，满足 cpp-httplib 使用需求；若需更新版本可从 GitHub 单独拉取。

---

## 2. 安装命令

### 2.1 系统更新与构建工具

```bash
sudo apt update
sudo apt install -y build-essential cmake
```

### 2.2 存储

```bash
sudo apt install -y sqlite3 libsqlite3-dev
```

### 2.3 判题沙箱

```bash
sudo apt install -y libseccomp-dev
```

### 2.4 认证与安全

```bash
sudo apt install -y libargon2-dev libssl-dev
```

### 2.5 JSON 与 HTTP 服务

```bash
sudo apt install -y nlohmann-json3-dev libcpp-httplib-dev
```

### 2.6 jwt-cpp（header-only，APT 无包，源码安装）

```bash
cd /tmp
git clone https://github.com/Thalhammer/jwt-cpp.git
sudo cp -r jwt-cpp/include/jwt-cpp /usr/local/include/
rm -rf jwt-cpp
```

### 2.7 运维辅助

```bash
sudo apt install -y cron curl
sudo systemctl enable --now cron
```

### 2.8 判题 tmpfs 运行目录（一次性挂载）

```bash
sudo mkdir -p /opt/oj-tmpfs
sudo mount -t tmpfs -o size=2G,mode=1777 tmpfs /opt/oj-tmpfs
```

- 说明：不能加 `noexec`，判题需在此目录执行编译产物；大小 2G 可依机器内存调整。
- 开机自动挂载：在 `/etc/fstab` 加入

  ```
  tmpfs /opt/oj-tmpfs tmpfs defaults,size=2G,mode=1777 0 0
  ```

---

## 3. 安装后验证

```bash
g++ --version && gcc --version && cmake --version && sqlite3 --version
```

---

## 4. 不需要安装的部分

- 前端：原生 HTML/CSS/JS + CodeMirror（CDN 引入，UI-04），无构建流程、无需安装
- JWT 逻辑、argon 哈希调用、Rejudge、日志等均为代码实现
- tmpfs 挂载与 cron 定时任务属运行时配置，非软件安装