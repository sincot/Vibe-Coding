# 仿 LeetCode 在线判题系统（OJ）

> 后端：C++（cpp-httplib） · 前端：原生 HTML/CSS/JS + CodeMirror · 存储：SQLite
> 需求与架构的唯一依据见 [`SPEC.md`](SPEC.md)，依赖安装见 [`dependence.md`](dependence.md)。

## 当前进度

- [x] M0.1 项目结构
- [x] M0.2 构建与 HTTP 服务（CMake 接入 cpp-httplib，`GET /api/health` 健康检查，优雅停止）
- [x] M0.3 数据库基础（SQLite 接入、五张业务表建表 + 约束/索引、WAL 与外键、首次启动自动初始化、argon2id 预置 admin、生命周期接入）
- [x] M1.1 注册（10 位随机账号分配 + 昵称唯一性 + argon2id 密码哈希 + `POST /api/register`）
- [x] M1.2 登录与身份验证（JWT 签发/校验 + `POST /api/login` + 登录限速 + Bearer 鉴权 + `GET /api/me`）
- [x] M1.3 改密与权限检查（`POST /api/me/password` + admin 首登强制改密 + 可复用管理员权限检查）

后续阶段（题目、判题、前端）尚未实现。

## 环境要求

Ubuntu 22.04 LTS，安装依赖：

```bash
sudo apt update
sudo apt install -y build-essential cmake libcpp-httplib-dev nlohmann-json3-dev \
  libsqlite3-dev libargon2-dev libssl-dev libgtest-dev
```

> 完整依赖清单（seccomp、jwt-cpp、tmpfs 挂载等后续阶段使用）见 `dependence.md`。
> M1.2 起需要 jwt-cpp（header-only）与 libssl（JWT HS256 所用 libcrypto），
> jwt-cpp 安装方式见 `dependence.md` 3.6 节。

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

注册单元测试与接口集成测试（同样使用隔离临时库与随机端口，不触碰正式数据库）：

```bash
ctest --test-dir build -R register_unit --output-on-failure
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

配置管理单元测试（基于 gtest，无外部依赖，不触碰数据库与网络）：

```bash
ctest --test-dir build -R config_unit --output-on-failure
```

覆盖命令行参数解析（`--host`/`--port`/`--db`/`--help`）、端口校验、默认值与组合参数、
非法/未知参数、初始管理员密码环境变量读取，以及 JWT 配置（`OJ_JWT_SECRET` /
`OJ_JWT_EXPIRES_SECONDS`）的读取与边界校验。

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
| `OJ_JWT_SECRET` | （无，必需） | JWT HS256 签名密钥，长度不少于 16 字节，无默认值 |
| `OJ_JWT_EXPIRES_SECONDS` | `3600` | JWT 有效期（秒），须为 1..31536000 的整数 |

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

### 登录限速

- 维度：来源 IP（`remote_addr`，不信任 `X-Forwarded-For` 等客户端提供的转发头）。
- 阈值：同一 IP 在 15 分钟窗口内连续 **5 次** 登录失败后，第 6 次起返回 `429`。
- 解除：窗口自最早失败时刻起 15 分钟后自动解除；登录成功会清空该 IP 的失败计数。
- 触发限速时响应含 `Retry-After`（秒）头。
- 实现为进程内状态（线程安全），不跨重启持久化；重启后计数清零。

### 错误约定

错误响应体统一为 `{"error":"..."}`，内部故障返回通用文案，不泄露数据库/哈希/密钥/token 细节：

| 状态码 | 含义 |
|---|---|
| `200` | 登录成功 / 获取用户信息成功 / 修改密码成功 |
| `201` | 注册成功 |
| `400` | 非法输入：JSON 解析失败、字段缺失/类型错误、非法昵称/密码/账号/密码为空、非法新密码、新密码与旧密码相同 |
| `401` | 登录失败（账号或密码错误）/ 认证无效（缺失、损坏、伪造、篡改、过期、无签名、算法不匹配或引用不存在用户等 token）/ 旧密码错误 |
| `403` | 权限不足（非管理员访问管理员功能）/ 必须先改密（含 `code:"PASSWORD_CHANGE_REQUIRED"`） |
| `409` | 昵称已被使用（含并发冲突） |
| `429` | 登录尝试过于频繁（触发限速） |
| `500` | 内部故障 |

### 停止服务

前台运行时按 `Ctrl+C`（或发送 `SIGTERM`）即可优雅停止：服务会停止监听、
回收线程后退出。验证方式：

```bash
# 前台启动后按 Ctrl+C，随后确认无残留进程
pgrep -a oj_server
```

停止后可在同一端口重新启动。
