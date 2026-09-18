# 仿 LeetCode 在线判题系统（OJ）

> 后端：C++（cpp-httplib） · 前端：原生 HTML/CSS/JS + CodeMirror · 存储：SQLite
> 需求与架构的唯一依据见 [`SPEC.md`](SPEC.md)，依赖安装见 [`dependence.md`](dependence.md)。

## 当前进度

- [x] M0.1 项目结构
- [x] M0.2 构建与 HTTP 服务（CMake 接入 cpp-httplib，`GET /api/health` 健康检查，优雅停止）

后续阶段（数据库、认证、题目、判题、前端）尚未实现。

## 环境要求

Ubuntu 22.04 LTS，安装依赖：

```bash
sudo apt update
sudo apt install -y build-essential cmake libcpp-httplib-dev nlohmann-json3-dev
```

> 完整依赖清单（SQLite、argon2、seccomp 等后续阶段使用）见 `dependence.md`。

## 构建

```bash
cmake -S . -B build
cmake --build build -j
```

生成可执行程序 `build/oj_server`。

## 运行

```bash
./build/oj_server                     # 默认监听 0.0.0.0:8080
./build/oj_server --host 127.0.0.1 --port 9000
./build/oj_server --help
```

### 配置方式

仅命令行参数，均可省略：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--host` | `0.0.0.0` | 监听地址 |
| `--port` | `8080` | 监听端口（1-65535 的整数） |

非法输入（如 `--port abc`、`--port 0`、未知参数）会打印错误信息并以非零返回码退出；
端口被占用或地址不可用时同样报错并以非零返回码退出。

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
