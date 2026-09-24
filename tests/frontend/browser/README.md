# M4.1 / M4.3 真实浏览器验证（playwright-cli）

本目录存放真实浏览器验证，**不注册 CTest、不属于项目运行依赖**。
使用 `@playwright/cli`（命令 `playwright-cli`）驱动真实 Chromium：M4.1 覆盖路由、导航、
登录/退出、认证失效、权限不足、首次改密与返回原目标；M4.3 覆盖真实 CDN CodeMirror
加载与 C/C++ 高亮、语言切换、ResizeObserver/refresh、窄视口布局与编辑器释放。

## 文件

- `m41_browser_scenarios.js`：M4.1 `playwright-cli run-code` 脚本（函数 `async (page) => …`）。
  运行前替换占位符 `__BASE__`（后端根 URL）与 `__OUT__`（截图目录）。
- `run_m41_browser.sh`：M4.1 Linux 端一键运行（自起隔离服务 + 真实浏览器 + 截图）。
- `run_m41_browser.ps1`：M4.1 Windows 端一键运行（连接已启动/已转发的服务，可用 `-Headed`）。
- `m43_browser_scenarios.js` + `run_m43_browser.sh`：M4.3 真实浏览器验证。与 M4.1 脚本
  相同占位符；runner 会解析场景返回的 JSON 统计通过/失败并以非零码退出。运行：

  ```bash
  # PWCLI 默认探测 /tmp/opencode/pwcli；也支持全局 playwright-cli
  PWCLI=/path/to/playwright-cli PWCLI_BROWSER_LIBS=/path/to/libs \
  BROWSER_ARTIFACTS_DIR=/tmp/m43-artifacts bash tests/frontend/browser/run_m43_browser.sh
  ```
- `m64_browser_scenarios.js` + `run_m64_browser.ps1` / `run_m64_browser.sh`：M6.4 最终验收 A
  全流程真实浏览器场景（注册/重复昵称/登录/admin 首改/UI 建题与隐藏用例/列表搜索·难度·
  标签·可见性筛选/提交 AC/列表与排行榜/越权与伪造 token/窄视口）。除 `__BASE__`/`__OUT__`
  外还有 `__ADMIN_PW__`（首次管理员密码）。`run_m64_browser.sh` 优先用 `playwright-cli`，
  否则回退到 `playwright-core`（`PW_CORE_DIR`）；Windows 端见下方「M6.4 有头验收」。

### 环境准备（Linux，无 root）

- 浏览器依赖库缺失时，可用 `apt-get download` + `dpkg-deb -x` 解包后经
  `PWCLI_BROWSER_LIBS`（追加 `LD_LIBRARY_PATH`）提供，例如 `libasound2`、`libgbm1`、
  `libwayland-server0`。本机已有可用解包目录 `/tmp/opencode/browserlibs`。
- **中文渲染**：headless Chromium 若缺少 CJK 字体，中文会显示为方框。用户级安装（无需
  root）示例：

  ```bash
  cd /tmp && apt-get download fonts-wqy-zenhei
  dpkg-deb -x fonts-wqy-zenhei_*.deb /tmp/cjk && \
    mkdir -p ~/.local/share/fonts && \
    cp /tmp/cjk/usr/share/fonts/truetype/wqy/*.ttc ~/.local/share/fonts/ && \
    fc-cache -f ~/.local/share/fonts
  ```

- 真实 CodeMirror 从 cdnjs 加载，需浏览器可访问外网；若离线，页面会按设计降级到
  textarea（M4.3 场景中的高亮断言将不适用）。

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

M4.3（`m43_browser_scenarios.js`）：`B43-01` 真实 CodeMirror 加载与 C/C++ 高亮；
`B43-02` 语言切换更新模式且保留源码；`B43-03` ResizeObserver 触发 refresh 的实际布局
调整；`B43-窄视口 360×640 / 390×844` 单列与可操作性；`B43-05` 离开页面释放编辑器。

M4.1（`m41_browser_scenarios.js`）：`B-01` 桌面题目列表；`B-05` 桌面题面两列；`B-窄视口 360×640 / 390×844 / 768×1024`
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

## M6.4 有头验收（服务器端，虚拟显示）

服务器无物理显示时，可用私有虚拟 X 显示跑**窗口化** Chromium（无 root、不改系统）：

```bash
# 先按脚本头部说明解包 Xvfb 到 /tmp/opencode/xvfb/root
bash tests/frontend/browser/run_m64_browser_headed_xvfb.sh
```

脚本在 `unshare -rm` 私有用户+挂载命名空间内启动 Xvfb（叠加 `/usr/bin` 仅提供
`xkbcomp` 与 `sh`），再以 `PWCLI_HEADED=1` 运行 `run_m64_browser.sh`。当前实测同一场景
有头 **24/24** 通过，截图见 `build/acceptance-logs/m64/headed/`。这不是 Windows 通道，
但确为真实有头 Chromium（窗口在虚拟显示上）。

## M6.4 有头验收（Windows，可选跨平台补充）

M6.4 最终验收 A 的完整业务流在 Windows 有头浏览器上只需三步：

1. 云服务器启动长期隔离服务（独立临时库/密钥/判题目录，不影响正式 `data/oj.db`）：

   ```bash
   bash build/acceptance-logs/m64/start_windows_server.sh          # 监听 127.0.0.1:18080
   # 干净复跑（管理员首改后需复位）：--reset
   ```

2. 通过 VS Code/SSH 把远端 `127.0.0.1:18080` 转发到本机。
3. 在 Windows 仓库 `tests\frontend\browser\` 目录（含 `m64_browser_scenarios.js`）：

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\run_m64_browser.ps1 `
     -Base http://localhost:18080 -AdminPassword 'M64WinPw123!' -Headed
   ```

   脚本以真实有头 Chromium 执行场景，解析并打印逐项通过/失败与截图目录，全部通过退出码 0。
   服务器本机已用同一场景文件在真实 Chromium（无头）验证 24/24。
