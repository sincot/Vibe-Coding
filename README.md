# 仿 LeetCode 在线判题系统（OJ）

> 后端：C++（cpp-httplib） · 前端：原生 HTML/CSS/JS + CodeMirror · 存储：SQLite
> 需求与架构的唯一依据见 [`SPEC.md`](SPEC.md)，依赖安装见 [`dependence.md`](dependence.md)。

## 当前进度

- [x] M0.1 项目结构
- [x] M0.2 构建与 HTTP 服务（CMake 接入 cpp-httplib，`GET /api/health` 健康检查，优雅停止）
- [x] M0.3 数据库基础（SQLite 接入、五张业务表建表 + 约束/索引、WAL 与外键、首次启动自动初始化、argon2id 预置 admin、生命周期接入）

后续阶段（认证、题目、判题、前端）尚未实现。

## 环境要求

Ubuntu 22.04 LTS，安装依赖：

```bash
sudo apt update
sudo apt install -y build-essential cmake libcpp-httplib-dev nlohmann-json3-dev \
  libsqlite3-dev libargon2-dev
```

> 完整依赖清单（seccomp、jwt-cpp、tmpfs 挂载等后续阶段使用）见 `dependence.md`。

## 构建

```bash
cmake -S . -B build
cmake --build build -j
```

生成可执行程序 `build/oj_server`。

## 测试

数据库集成测试（使用 `/tmp` 下的隔离临时库，不触碰 `data/oj.db`）：

```bash
ctest --test-dir build -R db_integration --output-on-failure
```

覆盖建表与预置 admin、WAL / 外键、唯一性约束、非法外键、密码哈希、重复初始化幂等、
持久化及失败路径。

## 运行

```bash
./build/oj_server                     # 默认监听 0.0.0.0:8080
./build/oj_server --host 127.0.0.1 --port 9000
./build/oj_server --help
```

### 配置方式

命令行参数与环境变量，均可省略：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--host` | `0.0.0.0` | 监听地址 |
| `--port` | `8080` | 监听端口（1-65535 的整数） |
| `--db` | `data/oj.db` | SQLite 数据库路径 |
| `OJ_ADMIN_PASSWORD` | （无） | 首次初始化（尚无 admin）时预置的管理员初始密码 |

非法输入（如 `--port abc`、`--port 0`、未知参数）会打印错误信息并以非零返回码退出；
端口被占用或地址不可用时同样报错并以非零返回码退出。

### 数据库与初始管理员

- 首次启动自动创建数据目录、`oj.db` 及五张业务表，并预置管理员 `admin`（角色 `admin`，`reset_pwd_flag=1`，首次登录强制改密）。
- 初始管理员密码通过环境变量 `OJ_ADMIN_PASSWORD` 提供，仅以 argon2id 哈希落库，不写入源码、版本控制或日志。示例：

  ```bash
  OJ_ADMIN_PASSWORD='请改为强密码' ./build/oj_server
  ```

- 数据库中已有 `admin` 时无需再设置该变量；重复启动不会重复创建 admin，也不会覆盖密码或重置首次改密标记。
- 数据库未就绪（缺少初始密码、路径不可写等）时，服务在开始监听前报错并以非零返回码退出。

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

### 停止服务

前台运行时按 `Ctrl+C`（或发送 `SIGTERM`）即可优雅停止：服务会停止监听、
回收线程后退出。验证方式：

```bash
# 前台启动后按 Ctrl+C，随后确认无残留进程
pgrep -a oj_server
```

停止后可在同一端口重新启动。
