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
- [x] M1.4 最小题目数据与查询（`testcases.is_sample` 区分公开样例/隐藏用例 + 3 道幂等种子题 + `GET /api/problems` / `GET /api/problems/{id}` + 题目可见性）
- [x] M1.5 最小判题器（`IExecutor` 抽象 + `LocalExecutor` + `JudgeEngine`：C++17/C11 编译、逐点执行、基础超时、有界输出、归一化比对与 AC/WA/CE/TLE/RE/SYSERR 汇总；仅开发环境验证，完整沙箱见 M3）
- [x] M1.6 提交接口与持久化（`POST /api/problems/{id}/submit`：登录/首改/可见性校验 + 后端隐藏用例判题 + 单事务写入 `submissions` 与 `user_problem_status` + 同步返回逐点结果）
- [x] M1.7 最小前端（cpp-httplib 静态托管 `web/` + 原生 HTML/CSS/ES Module + hash 路由 + 注册/登录/改密/题目列表/题目页 `textarea` 提交 + 统一 API 封装；仅开发环境验证）
- [x] M2.1 管理员题目接口（`POST`/`PUT`/`DELETE /api/admin/problems`：字段校验与默认值 + 部分更新语义 + 可见性设置 + 删除关联数据规则 + 统一管理员权限检查）
- [x] M2.2 管理员测试用例接口（`GET`/`POST`/`PUT`/`DELETE /api/admin/problems/{id}/testcases[/{tid}]`：隐藏用例读/增/改/删 + 归属与权限校验 + `ord` 排序规则 + 公开样例隔离 + 判题快照与历史数据不受影响）

后续阶段（题目搜索筛选分页、用户管理、后台页面、Rejudge、完整沙箱与判题线程池、CodeMirror）尚未实现。

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
| `--web` | `web` | 前端静态资源目录；仅该目录对外只读托管（M1.7） |
| `--seed` | 关闭 | 仅导入内置种子题目后退出，不启动服务（幂等，详见「种子数据导入」） |
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

### 前端页面（M1.7，最小实现）

服务启动后，浏览器直接访问根路径即可打开前端（默认 <http://127.0.0.1:8080/>）：

```bash
OJ_JWT_SECRET="$(openssl rand -hex 32)" OJ_ADMIN_PASSWORD='请改为强密码' \
  ./build/oj_server --db data/oj.db
# 浏览器打开 http://127.0.0.1:8080/
```

- **静态托管**：cpp-httplib 仅把 `--web`（默认 `web/`）目录只读挂载到 URL 根路径 `/`，并处理目录下的 `index.html`。项目根目录、`data/oj.db`、`src/`、配置文件与判题临时目录都不在托管范围内；`..` 与 URL 编码的越界路径由路径校验拦截并返回 `404`（可自行验证：`curl -i --path-as-is http://127.0.0.1:8080/SPEC.md`、`/data/oj.db`、`/../SPEC.md`、`/%2e%2e/SPEC.md` 均为 `404`）。`/api/*` 路由与 `/api/health` 行为保持不变。
- **无构建流程**：纯原生 HTML/CSS/ES Module，无打包器、无 React/Vue 等框架。本阶段源码编辑器为 `textarea`，CodeMirror 属 M4.3。
- **hash 路由**：`#/problems`（列表）、`#/problems/{id}`（题目页）、`#/login`、`#/register`、`#/password`。游客可浏览公开题目；`#/password` 为受保护路由，未登录时重定向到登录页并携带 `redirect` 参数，登录成功后返回原目标页。
- **统一 API 封装**（`web/js/api.js`）：负责 JSON 序列化/解析、`Authorization: Bearer <token>`、HTTP 错误与网络异常归一化。token 保存在浏览器 `localStorage`，仅放入请求头，不进入 URL 或日志。
- **认证行为**：身份失效（`401`）会清理本地凭证并跳转登录页（内部去重，避免并发请求重复跳转）；登录失败只显示表单错误；改密接口的「旧密码错误」按表单错误处理，不会误退出登录；后端返回 `code:"PASSWORD_CHANGE_REQUIRED"` 时引导到改密页。
- **功能范围**：注册（成功显著展示系统分配的 10 位账号并引导用该账号登录，不依赖未实现的自动登录）、登录（保存 token 与用户状态、导航显示昵称、退出登录）、改密（沿用后端字段与密码规则，admin 首登强制引导）、题目列表（题目 ID/标题/难度/标签，含加载中、空列表、加载失败状态）、题目页（左侧题面+公开样例+难度标签+时空限制，右侧语言选择+`textarea`+提交+逐点结果）。退出登录只清理前端凭证，不声称已撤销后端 JWT。
- **结果展示**：展示提交 ID、总体状态、逐点结果、编译信息、诊断，以及 WA 失败点的输入/期望输出/实际输出；后端未采集的内存显示为「未采集」而非真实的 `0`；不为展示结果额外获取隐藏用例。
- **提交行为**：请求期间按钮禁用并显示「判题中」，避免重复提交；失败后保留编辑器源码、恢复可操作状态且不自动重试；网络中断时说明「结果无法确认」，不断言后端未保存提交。
- **输出安全**：题面、样例、昵称、编译信息与程序输出一律通过 `textContent`/`<pre>` 作为纯文本渲染，HTML 特殊字符不会被解释执行。
- **响应式**：题目页左右两栏，窗口宽度 ≤ 900px 时改为上下排列；长题面与长输出可滚动阅读。

> 安全边界：本阶段仍使用 M1.5 的开发环境判题器，前端可提交不等于已具备公开运行不可信代码的能力；完整沙箱见 M3。

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

内置 3 道简单种子题（A+B Problem、整数求和、求最大值），均为标准 ACM 输入输出模式，
包含标题、纯文本题面、输入输出说明、公开样例、隐藏测试用例、难度、标签、时限与内存限制。

导入方式（`--seed`，显式执行一次，**不在服务启动时自动运行**）：

```bash
# 先导入种子题目（只创建/迁移表结构，不涉及 admin，也不需要 JWT 密钥）
./build/oj_server --db data/oj.db --seed
```

输出示例：

```
种子数据导入完成：新建题目 3 道（已存在的题目已跳过）
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
复用已有身份验证。按 `id` 升序稳定排序，为后续搜索/筛选/分页保留扩展空间。

```bash
curl -i http://127.0.0.1:8080/api/problems
```

成功响应（`200`）：

```
{"problems":[
  {"id":1,"title":"A+B Problem","difficulty":"easy","tags":["入门","数学"],"visible":true},
  {"id":2,"title":"整数求和","difficulty":"easy","tags":["数组","入门"],"visible":true}
 ],"total":2}
```

### 题目详情接口

`GET /api/problems/{id}`（公开，无需登录），只返回元数据与**公开样例**，
绝不包含隐藏用例：

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

### 题目可见性

| 身份 | 列表可见范围 | 详情可见范围 |
|---|---|---|
| 游客（无 token） | `visible=1` 的题目 | `visible=1` 的题目 |
| 普通用户 | `visible=1` 的题目 | `visible=1` 的题目 |
| 管理员（已登录 + 已完成首次改密） | 全部题目（含隐藏） | 全部题目（含隐藏） |
| 管理员（`reset_pwd_flag=1`，未完成首次改密） | 同普通用户 | 同普通用户 |

- 列表与详情使用**一致的可见性规则**，无法通过直接请求隐藏题 ID 绕过。
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

- **题目已有提交记录时拒绝删除**，返回 `409`，保留学生提交历史与做题状态；
- 无提交时在同一事务内删除该题的全部测试用例（公开样例与隐藏用例）、
  `user_problem_status` 记录与题目本身，不产生孤立记录；
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
| `409` | 删除冲突：题目已有提交记录 |
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
 "runtime_ms":18,"memory_kb":null,"compile_ok":true,"compile_output":"",
 "message":"全部测试点通过","created_at":"2026-09-21 12:00:00",
 "results":[{"index":0,"status":"AC","time_ms":3,"memory_kb":null},
            {"index":1,"status":"AC","time_ms":4,"memory_kb":null}]}
```

- `language`：仅接受 `cpp17`（C++17）与 `c11`（C11），大小写不敏感；其它取值返回
  `400`。入库保存规范小写值。
- `code`：完整用户源码，原样送入编译与入库，不做裁剪或修改。
- `status`：`AC/WA/CE/TLE/RE/MLE/SYSERR`。`runtime_ms` 为各测试点执行耗时之和。
- `memory_kb`：**固定为 `null`**——M1.6 判题器尚未采集内存，明确表示未采集，不伪造
  测量结果（真实内存采集在 M3.4）。
- `results`：逐测试点结果。通过（AC）测试点只含 `index/status/time_ms/memory_kb`，
  **绝不附带隐藏测试输入或标准答案**；`WA` 点按 SPEC PRB-05/JUDGE-07 附上该失败点的
  `input`/`expected_output`/`actual_output`。仅本次提交者可见。
- 编译失败返回 `status:"CE"` 并在 `compile_output` 给出编译器诊断，`results` 为空。

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
- 判题阶段（编译 + 逐点运行）在进程内**串行**执行：M1.6 的执行器不提供并发保证，
  故对判题加简单可靠的互斥保护，避免 HTTP 并发下临时目录/执行状态混用；M3 将以
  worker 线程池替代，本阶段不引入。

> **安全边界（重要）**：M1.6 调用的仍是 M1.5 的开发环境判题器，**没有**完整沙箱
> （无 setrlimit/seccomp/tmpfs），完成提交接口不代表已完成安全隔离，**不得公开接收
> 不可信代码**。完整沙箱、线程池与 Rejudge 见 M3。

### 判题器（M1.5，仅开发环境验证）

判题核心位于 `src/judge/`，不依赖 HTTP 与数据库，可独立调用和测试：

- `IExecutor`（`src/judge/executor.h`）：进程执行抽象，把「如何编译/运行子进程」与
  「如何比对、汇总」解耦；`LocalExecutor`（`src/judge/local_executor.h`）为当前本机
  实现，M3 将以 seccomp/setrlimit/tmpfs 沙箱实现替换。
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
  `gcc -O2 -std=c11 <src> -o program -lm`；均通过参数数组直接 `execvp` 启动，不拼接
  shell 命令。**尚未接入 ASan/UBSan**，属 M3.4。
- **工作目录**：每次判题经 `mkdtemp` 创建唯一目录（默认系统临时目录，可配置），保存
  源码与编译产物，结束后只删除本次目录。M3 将改为 tmpfs 下的随机目录。
- **编译结果**：编译进程正常且退出码为 0 视为成功；非零退出码返回 `CE`，并附有长度
  上限的编译诊断；编译器不存在、无法创建目录等环境故障返回 `SYSERR`，不伪装为 `CE`；
  编译过程有保护超时。
- **逐点运行**：每个测试点启动**新的**进程，输入经标准输入传入，标准输出与标准错误
  分别采集；输出有界（标准输出 64KB、标准错误 16KB，超限只标记截断并继续排空管道），
  标准输出超限不会被当作正常输出判为 `AC`。
- **超时**：单测试点超时（默认上限 60s，SPEC JUDGE-10）后 `SIGKILL` 强杀并通过
  `waitpid` 回收；超时计为 `TLE`。
- **比对**：去除每行行尾空白与文末空行后逐字符比较；保留行首空白、行内空白与中间
  空行的意义，不做分词比较。程序非正常退出（信号或非零退出码）计为 `RE`。
- **汇总**：按顺序执行全部测试点，普通 `WA` 不阻止后续测试点；全部 `AC` 才返回 `AC`，
  否则按 `SYSERR > TLE > MLE > RE > WA > AC` 取最严重状态。空测试集、非法语言、无效
  时间配置均返回明确的 `SYSERR`，绝不因未执行任何测试点而返回 `AC`。
- **清理**：成功、`CE`、`RE`、`TLE` 等所有路径都会关闭文件描述符、回收子进程并删除
  本次临时目录。

> **安全边界**：本阶段只有基础超时与进程清理，**没有** setrlimit、seccomp、tmpfs 与
> 内存限制，也**不是完整沙箱**，仅用于开发环境验证；不得用于公开接收不可信代码。
> 完整 MLE 判定、Sanitizer 诊断与异常分类在 M3 完成。

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
| `200` | 登录成功 / 获取用户信息成功 / 修改密码成功 / 提交并返回判题结果（含 CE/WA/TLE/RE/SYSERR） |
| `201` | 注册成功 |
| `400` | 非法输入：JSON 解析失败、字段缺失/类型错误、非法昵称/密码/账号/密码为空、非法新密码、新密码与旧密码相同、非法题目 ID、不支持的语言、空/纯空白/超长源码 |
| `401` | 登录失败（账号或密码错误）/ 认证无效（缺失、损坏、伪造、篡改、过期、无签名、算法不匹配或引用不存在用户等 token）/ 旧密码错误 |
| `403` | 权限不足（非管理员访问管理员功能）/ 必须先改密（含 `code:"PASSWORD_CHANGE_REQUIRED"`） |
| `404` | 题目不存在，或当前用户无权查看/提交的隐藏题目（统一返回，不区分） |
| `409` | 昵称已被使用（含并发冲突） |
| `413` | 请求体超过服务器上限（1 MiB，在解析前拒绝） |
| `429` | 登录尝试过于频繁（触发限速） |
| `500` | 内部故障（含提交持久化失败，不声称已保存，不泄露 SQL/路径） |

### 停止服务

前台运行时按 `Ctrl+C`（或发送 `SIGTERM`）即可优雅停止：服务会停止监听、
回收线程后退出。验证方式：

```bash
# 前台启动后按 Ctrl+C，随后确认无残留进程
pgrep -a oj_server
```

停止后可在同一端口重新启动。
