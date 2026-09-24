# 前端页面级验证（M2.5 / M4.1 / M4.3）

本目录存放可重复运行的前端页面级验证，**不注册到 CTest、不属于项目运行依赖**，保持前端
“无构建、无测试框架”。在真实启动的后端上，用 [jsdom](https://github.com/jsdom/jsdom)
执行仓库中真实的 `web/js` 模块（hash 路由、页面视图、API 封装）并驱动 DOM 交互。

- `m41_logic_test.mjs`：M4.1 基础设施纯逻辑验证（Node 直接执行 `web/js`，stub
  `fetch`/`localStorage`/`sessionStorage`/`location`，无需 jsdom 与服务）。运行：
  `node tests/frontend/m41_logic_test.mjs`。
- `m41_infrastructure_dom.mjs` + `run_m41.sh`：M4.1 路由/访问条件/返回目标/首次改密/
  退出/401/生命周期页面级验证。运行：`bash tests/frontend/run_m41.sh`。
- `admin_pages_dom.mjs` + `run.sh`：M2.5 后台管理页面 DOM 级验证（M4.1 回归复用）。
- `m43_problem_page_dom.mjs` + `run_m43.sh`：M4.3 题目与做题页面 DOM 级验证（题面/样例/
  限制/本人状态、降级 textarea、语言切换、`Ctrl+Enter` 去重、结果渲染规则、编辑器适配
  与释放、网络失败保留源码）。运行：`bash tests/frontend/run_m43.sh`。
- `m44_history_dom.mjs` + `run_m44.sh`：M4.4 提交历史与详情页面 DOM 级验证。运行：
  `bash tests/frontend/run_m44.sh`。
- `m45_leaderboard_dom.mjs` + `run_m45.sh`：M4.5 排行榜页面 DOM 级验证（游客可访问、
  渲染列/分页/无 AC「—」、登录当前用户高亮、退出移除高亮、刷新、无脚本错误）。运行：
  `bash tests/frontend/run_m45.sh`。
- `run_m43_coverage.sh`：在隔离服务上执行同一 M4.3 页面级测试，并用
  [c8](https://github.com/bcoe/c8) 采集被执行的 `web/js` 模块覆盖率。运行：
  `npm install --prefix /tmp/opencode/covtool c8 && bash tests/frontend/run_m43_coverage.sh`；
  可用 `C8` 指定其它路径、`FRONTEND_COVERAGE_REPORT` 另存报告。
- `browser/m43_browser_scenarios.js` + `browser/run_m43_browser.sh`：M4.3 真实 Chromium
  验证（真实 CDN CodeMirror 加载与 C/C++ 高亮、语言切换、ResizeObserver/refresh、
  窄视口布局、编辑器释放）。运行：`bash tests/frontend/browser/run_m43_browser.sh`；
  说明见 `browser/README.md`。
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
bash tests/frontend/run.sh              # M2.5 后台管理页面
bash tests/frontend/run_m41.sh          # M4.1 页面基础设施
bash tests/frontend/run_m43.sh          # M4.3 题目与做题页面
bash tests/frontend/run_m43_coverage.sh # M4.3 前端覆盖率（需 c8）
node tests/frontend/m41_logic_test.mjs  # M4.1 纯逻辑（无需服务）
```

`run.sh` / `run_m41.sh` / `run_m43.sh` 会：

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
- M4.3：题面/样例/限制/本人状态、降级 textarea、语言切换不清空、`Ctrl+Enter` 与按钮
  去重、结果字段渲染规则、编辑器适配与释放、网络失败保留源码；覆盖率见
  `run_m43_coverage.sh`；真实渲染/窄视口/真实 CodeMirror 由 `browser/run_m43_browser.sh` 覆盖。

## 覆盖率

M4.3 起可用 `run_m43_coverage.sh` 采集前端覆盖率（c8 + Node 内置 V8 coverage），仅为
本次测试触达的 `web/js` 代码，不代表全前端或全分支覆盖。

## 边界

- jsdom 验证不等同于真实图形浏览器渲染；真实渲染/窄视口/真实 CDN CodeMirror 由
  `browser/run_m43_browser.sh`（真实 Chromium）覆盖，但未做真机与多浏览器内核验证。
- 覆盖率仅来自对应页面级测试，未采集全前端覆盖率。
