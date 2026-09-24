# M4.1 真实浏览器验证（playwright-cli）

本目录存放 M4.1「页面基础设施」的真实浏览器验证，**不注册 CTest、不属于项目运行依赖**。
使用 `@playwright/cli`（命令 `playwright-cli`）驱动真实 Chromium，覆盖路由、导航、登录/
退出、认证失效、权限不足、首次改密与返回原目标，以及桌面与窄视口布局。

## 文件

- `m41_browser_scenarios.js`：`playwright-cli run-code` 脚本（函数 `async (page) => …`）。
  运行前替换占位符 `__BASE__`（后端根 URL）与 `__OUT__`（截图目录）。
- `run_m41_browser.sh`：Linux 端一键运行（自起隔离服务 + 真实浏览器 + 截图）。
- `run_m41_browser.ps1`：Windows 端一键运行（连接已启动/已转发的服务，可用 `-Headed`）。

## 运行方式

### A. Windows（有头，本机浏览器）

1. 云服务器启动隔离服务（示例，监听 127.0.0.1:8080）：

   ```bash
   # 在云服务器仓库根目录
   OJ_JWT_SECRET="$(openssl rand -hex 32)" OJ_ADMIN_PASSWORD='AdminPass123' \
   OJ_JUDGE_WORKSPACE=/dev/shm ./build/oj_server --db /tmp/oj-browser.db --seed
   OJ_JWT_SECRET="$(openssl rand -hex 32)" OJ_ADMIN_PASSWORD='AdminPass123' \
   OJ_JUDGE_WORKSPACE=/dev/shm ./build/oj_server --db /tmp/oj-browser.db \
     --host 127.0.0.1 --port 8080 --web ./web
   ```

2. 通过 VS Code/SSH 把远端 `127.0.0.1:8080` 转发到本机 `localhost:8080`。
3. 把本目录的 `m41_browser_scenarios.js` 复制到 Windows 本地（或直接在可访问的路径），
   在本机执行：

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\run_m41_browser.ps1 -Base http://127.0.0.1:8080 -Headed
   ```

   或手动：

   ```powershell
   playwright-cli open about:blank --headed
   playwright-cli run-code --filename="C:\path\to\m41_run.js"   # 已替换 __BASE__/__OUT__
   playwright-cli close
   ```

### B. Linux（CI/服务器本地）

```bash
PWCLI=/path/to/node_modules/.bin/playwright-cli \
PWCLI_BROWSER_LIBS=/path/to/browser-libs \
bash tests/frontend/browser/run_m41_browser.sh
```

- `PWCLI_HEADED=1`：有头模式（`playwright-cli open --headed`）；需要可用的显示环境。
- `BROWSER_ARTIFACTS_DIR=<dir>`：把截图另存到该目录（默认随临时目录清理）。
- 无桌面的服务器可用 Xvfb 提供虚拟显示后设有头运行；例如：

  ```bash
  PWCLI_HEADED=1 BROWSER_ARTIFACTS_DIR=/tmp/m41-artifacts \
  PWCLI=/path/to/playwright-cli bash tests/frontend/browser/run_m41_browser.sh
  ```

  （在已有 `:99` 显示的容器/服务器上，直接 `export DISPLAY=:99` 即可。若 Xvfb 缺少
  `/usr/bin/xkbcomp`，可在用户命名空间内用 overlay 补齐，本仓库不内置该环境适配。）

`PWCLI_BROWSER_LIBS` 仅在浏览器依赖库不在系统默认路径时使用（如缺少
`libasound.so.2`/`libgbm.so.1`/`libwayland-server.so.0`，可用 `apt-get download` +
`dpkg-deb -x` 解出后指向其 `usr/lib/x86_64-linux-gnu`）。

## 场景

`B-01` 桌面题目列表；`B-05` 桌面题面两列；`B-窄视口 360×640 / 390×844 / 768×1024`
（列表与题面无横向溢出、题面单列）；`B-11` 未知路由/非法参数；`B-06` 游客访问后台重定向；
`B-07` 首次改密保留原目标并返回；`B-12` 登录后导航；`B-09` 退出清理；`B-10` 认证失效；
`B-08` 普通用户越权回退。脚本还断言无未捕获脚本错误（预期的 401/403/404 资源错误除外）。

> 真实图形浏览器渲染与窄视口布局以本目录脚本为准；`tests/frontend/m41_logic_test.mjs`、
> `m41_infrastructure_dom.mjs`（jsdom）负责纯逻辑、时序竞态与无浏览器环境的回归。

## 故障排查：转发后页面空白/一直加载

1. 在云服务器确认服务在监听且健康：
   `ss -ltnp | grep <端口>`、`curl -sS --max-time 5 http://127.0.0.1:<端口>/api/health`
   应返回 `{"status":"ok"}`。
2. 服务未启动或启动失败最常见于：判题工作目录非 tmpfs、首次初始化未设
   `OJ_ADMIN_PASSWORD`（二者都会让 `oj_server` 以非零码退出）。用
   `bash scripts/run_dev_server.sh` 启动会自动处理并显式报错。
3. 从 Windows 访问转发地址下的 `/api/health`，应同样返回 `{"status":"ok"}`；否则是转发
   目标/本地端口问题，而不是前端。
4. 健康但页面仍空白：打开 DevTools 的 Network/Console，确认是否有 pending/失败的
   JS/CSS/API 请求及其状态与 MIME（本目录脚本的 `-Base` 可直接复用该地址）。
   M4.1 的 `api.js` 对请求设 15s 超时，后端无响应时会显示失败而非永久加载。
