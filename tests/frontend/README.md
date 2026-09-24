# 前端页面级验证（M2.5 / M4.1）

本目录存放可重复运行的前端页面级验证，**不注册到 CTest、不属于项目运行依赖**，保持前端
“无构建、无测试框架”。在真实启动的后端上，用 [jsdom](https://github.com/jsdom/jsdom)
执行仓库中真实的 `web/js` 模块（hash 路由、页面视图、API 封装）并驱动 DOM 交互。

- `m41_logic_test.mjs`：M4.1 基础设施纯逻辑验证（Node 直接执行 `web/js`，stub
  `fetch`/`localStorage`/`sessionStorage`/`location`，无需 jsdom 与服务）。运行：
  `node tests/frontend/m41_logic_test.mjs`。
- `m41_infrastructure_dom.mjs` + `run_m41.sh`：M4.1 路由/访问条件/返回目标/首次改密/
  退出/401/生命周期页面级验证。运行：`bash tests/frontend/run_m41.sh`。
- `admin_pages_dom.mjs` + `run.sh`：M2.5 后台管理页面 DOM 级验证（M4.1 回归复用）。
- `browser/`：M4.1 真实浏览器验证（`playwright-cli` + 真实 Chromium，桌面与窄视口，
  路由/登录/退出/首改/返回原目标）。Linux 用 `browser/run_m41_browser.sh`，Windows 用
  `browser/run_m41_browser.ps1`；说明见 `browser/README.md`。

## 前置条件

- 后端已构建：`cmake --build build --parallel 1`（需要 `build/oj_server`）。
- Node.js 与 jsdom：

  ```bash
  npm install --prefix /tmp/opencode/domtest jsdom
  ```

  可用 `JSDOM_DIR` 指向其它已安装 jsdom 的目录。

## 运行

```bash
bash tests/frontend/run.sh        # M2.5 后台管理页面
bash tests/frontend/run_m41.sh    # M4.1 页面基础设施
node tests/frontend/m41_logic_test.mjs   # M4.1 纯逻辑（无需服务）
```

`run.sh` / `run_m41.sh` 会：

1. 在 `mktemp` 临时目录创建隔离 SQLite 库并导入种子题；
2. 以随机端口启动 `oj_server`（测试密钥/管理员密码，仅内存环境变量，不落盘）；
3. 用 jsdom 执行对应 `.mjs`，连接该服务完成验证；
4. 结束时停止服务并清理临时目录，不影响正式 `data/oj.db`。

可选环境变量：`JSDOM_DIR`、`OJ_FRONTEND_PORT`、`OJ_JUDGE_WORKSPACE`（默认 `/dev/shm`，
因本机 `/opt/oj-tmpfs` 未挂载；也可用 `OJ_JUDGE_ALLOW_NON_TMPFS=1`）。

## 覆盖范围

- 后台入口与访问检查（游客/普通用户/未改密管理员/无效 token/权限被撤销）；
- 题目管理：列表分页与筛选、创建/编辑、公开隐藏、删除与冲突、表单校验负路径、公开样例维护；
- 测试用例管理：增/改/删、原样文本、`ord` 规则（自动追加/显式/越界）、公开样例只读、跨题不串用；
- 用户管理：列表、重置密码（新旧登录与首改标记）、角色提升/降级、最后管理员保护、自我降级；
- 状态与安全：加载/空/保存中/失败、重复提交、网络失败不假成功、XSS 文本渲染、敏感数据不入 localStorage。
- M4.1：默认/未知/非法参数路由、游客重定向携带目标、登录返回原目标与越权回退、首次强制
  改密保留原目标、退出清理、导航按状态渲染、401 清理、快速切换丢弃过期响应、重复进入
  不重复请求；逻辑层另覆盖返回目标防开放重定向、认证状态机、API 错误分类与去重。

## 边界

- 该验证在 jsdom 中执行，**不等同于真实图形浏览器渲染**，也未覆盖真机窄屏布局。
- 未采集前端覆盖率。
