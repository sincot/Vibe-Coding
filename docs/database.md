# 数据库初始化与配置

> 对应 SPEC M0「SQLite 建表脚本 + WAL 开启 + 数据库初始化（含预置 admin 账号）」。

## 1. 配置项

| 配置 | 默认值 | 说明 |
|---|---|---|
| `--db PATH` | `data/oj.db` | SQLite 数据库路径；父目录与文件不存在时自动创建 |
| `OJ_ADMIN_PASSWORD`（环境变量） | 无 | **推荐**：首次初始化时预置 admin 账号的初始密码 |
| `--admin-password PWD` | 无 | 同上，命令行传入；注意会出现在 `ps` 进程列表，优先用环境变量 |

- 初始密码只在 **admin 不存在** 的首次初始化时使用；admin 已存在时该值被忽略（不会重置密码）。
- 初始密码不会写入源码、数据库或日志，仅用于在初始化时生成 argon2id 哈希。

## 2. 建表与约束

启动时自动执行（放入单个事务，失败整体回滚，可重复执行、不重复建记录）：

- `users`：`account`、`nickname` 全局唯一；`role∈('admin','user')`；普通用户账号 10 位数字由注册 API 保证（M1），`admin` 为库层不设格式约束的例外。
- `problems`：时限/内存默认 2s / 64MB（PRB-02）；难度 `easy|medium|hard`；`visible∈(0,1)`。
- `testcases`：外键 problem_id，`UNIQUE(problem_id, ord)` 防同题重复序号。
- `submissions`：完整提交记录全部落库（PERS-01）；状态 `AC|WA|CE|TLE|RE|MLE|SYSERR`；语言 `cpp17|c11`。
- `user_problem_status`：`UNIQUE(user_id, problem_id)`；状态 `accepted|none`。

**删除策略**：所有外键均为默认 `NO ACTION`，**不加级联删除**——删题、删用户不会连带删除提交历史等持久化记录。

每次启动做只读结构校验：已存在的表若缺必要列，判定为旧库不兼容，**明确报错并拒绝启动**（不自动迁移、不删除重建），以便人工迁移保护已有数据。

## 3. 数据库连接设置

每个连接在 `Open` 时设置：

- `PRAGMA journal_mode=WAL;` —— 校验返回值必须为 `wal`
- `PRAGMA foreign_keys=ON;` —— 每个连接都启用外键检查
- `sqlite3_busy_timeout(5000)` —— 锁等待 5s
- `PRAGMA synchronous=NORMAL;` —— WAL 推荐级别

注意：`foreign_keys` 是连接级设置，新增数据库连接时必须重新执行，不能依赖全局。

## 4. 预置 admin

首次初始化且 `account='admin'` 不存在时创建：

- `nickname='admin'`（与 account 一致），`role='admin'`，`reset_pwd_flag=1`（首次登录强制改密，改密流程在 M1/M5 落地）
- 密码以 argon2id 存储（t=2, m=64MiB, p=1，随机盐）
- 若昵称/账号被其他用户占用，明确报错并停止启动，**不覆盖已有数据**

## 5. 运行方式

```bash
# 首次初始化（提供初始 admin 密码）
OJ_ADMIN_PASSWORD='<your-strong-password>' ./build/oj-server --port 8080

# 之后正常启动（admin 已存在，无需再给密码）
./build/oj-server --port 8080
```

- 数据库不可用 / WAL 未生效 / 结构不兼容 / 首次缺初始密码 → 打印 `[oj] fatal: ...` 并退出，**不会启动 HTTP 服务**。
- `Ctrl+C` / SIGTERM 优雅停止，正常释放数据库资源。

## 6. 备份

数据库文件为 `data/oj.db`（WAL 模式，运行期会生成 `-wal`/`-shm` 伴生文件）。
备份用 `scripts/backup.sh` 定期 `sqlite3 .dump`（见 SPEC PERS-04）。上述运行期文件均已加入 `.gitignore`，不入版本库。