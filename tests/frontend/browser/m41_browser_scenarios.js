// M4.1 真实浏览器场景（playwright-cli run-code 脚本）。
//
// 用法（在已 `playwright-cli open` 的会话中）：
//   playwright-cli run-code --filename=m41_browser_scenarios.js
// 运行前把 __BASE__（后端根 URL）与 __OUT__（截图输出目录）替换为实际值。
//
// 返回 JSON：{ results:[{name,ok,detail}], errors:[...] }。
// 真实 Chromium 验证桌面 1280×800 与窄视口 360×640 / 390×844 / 768×1024；
// 覆盖 M4.1 路由、导航、登录/退出、认证失效、权限不足、首次改密、返回原目标。
async (page) => {
  const BASE = "__BASE__";
  const OUT = "__OUT__";
  const results = [];
  const errors = [];
  const ck = (name, cond, detail) =>
    results.push({ name, ok: !!cond, detail: detail === undefined ? "" : String(detail).slice(0, 240) });

  page.on("pageerror", (e) => errors.push("pageerror: " + (e && e.message)));
  page.on("console", (m) => {
    if (m.type() !== "error") return;
    const t = m.text();
    // 资源类错误（如预期的 401/403/404）不算脚本错误。
    if (/Failed to load resource|server responded with a status|net::ERR/.test(t)) return;
    errors.push("console: " + t);
  });

  const waitHash = (substr, timeout = 8000) =>
    page.waitForFunction((s) => location.hash.includes(s), substr, { timeout });
  const waitExactHash = (h, timeout = 8000) =>
    page.waitForFunction((s) => location.hash === s, h, { timeout });
  const waitText = (substr, timeout = 8000) =>
    page.waitForFunction(
      (s) => {
        const app = document.getElementById("app");
        return !!app && (app.textContent || "").includes(s);
      },
      substr,
      { timeout }
    );
  const noOverflow = () =>
    page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1);
  const layoutCols = () =>
    page.evaluate(() => {
      const el = document.querySelector(".problem-layout");
      if (!el) return null;
      return getComputedStyle(el).gridTemplateColumns.trim().split(/\s+/).filter(Boolean).length;
    });
  const api = (path, opts) =>
    page.evaluate(
      async ([p, o]) => {
        const r = await fetch(p, o || {});
        const t = await r.text();
        let d = null;
        try {
          d = JSON.parse(t);
        } catch (e) {
          /* ignore */
        }
        return { status: r.status, data: d };
      },
      [BASE + path, opts]
    );
  const clearAuth = () =>
    page.evaluate(() => {
      localStorage.removeItem("oj.token");
      localStorage.removeItem("oj.user");
      sessionStorage.removeItem("oj.pendingTarget");
    });

  async function scenario(name, fn) {
    try {
      await fn();
    } catch (e) {
      ck(name + " (no exception)", false, (e && e.message) || String(e));
    }
  }

  // 首批种子题：取一个 id 供题目页使用。
  await page.goto(BASE + "/?bust=" + Date.now() + "#/problems", { waitUntil: "domcontentloaded" });
  await page.waitForSelector("#app table.data tbody tr", { timeout: 10000 });
  const listResp = await api("/api/problems");
  const seedId = listResp.data && listResp.data.problems && listResp.data.problems[0] && listResp.data.problems[0].id;

  // B-01 桌面题目列表
  await scenario("B-01 桌面题目列表", async () => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.goto(BASE + "/#/problems", { waitUntil: "domcontentloaded" });
    await page.waitForSelector("#app table.data tbody tr", { timeout: 8000 });
    const rows = await page.$$eval("#app table.data tbody tr", (r) => r.length);
    ck("题目列表渲染行", rows >= 3, "rows=" + rows);
    ck("桌面无横向溢出", await noOverflow());
    const nav = await page.textContent("#site-nav");
    ck("游客导航含登录/注册", /登录/.test(nav) && /注册/.test(nav), nav);
    ck("无排行榜空白入口", !/排行榜/.test(nav), nav);
    ck("无提交历史空白入口", !/提交历史/.test(nav), nav);
    await page.screenshot({ path: OUT + "/desktop-1280x800-problems.png", fullPage: true });
  });

  // B-05 桌面题目页两列
  await scenario("B-05 桌面题目页两列", async () => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.goto(BASE + "/#/problems/" + seedId, { waitUntil: "domcontentloaded" });
    await page.waitForSelector(".problem-layout", { timeout: 8000 });
    const cols = await layoutCols();
    ck("桌面题面为两列", cols === 2, "cols=" + cols);
    ck("桌面题面无横向溢出", await noOverflow());
    await page.screenshot({ path: OUT + "/desktop-1280x800-problem.png", fullPage: true });
  });

  // B-02/03/04 窄视口
  for (const [vw, vh] of [[360, 640], [390, 844], [768, 1024]]) {
    await scenario(`B-窄视口 ${vw}x${vh}`, async () => {
      await page.setViewportSize({ width: vw, height: vh });
      await page.goto(BASE + "/#/problems", { waitUntil: "domcontentloaded" });
      await page.waitForSelector("#app table.data tbody tr", { timeout: 8000 });
      ck(`${vw}x${vh} 列表无横向溢出`, await noOverflow());
      await page.goto(BASE + "/#/problems/" + seedId, { waitUntil: "domcontentloaded" });
      await page.waitForSelector(".problem-layout", { timeout: 8000 });
      const cols = await layoutCols();
      ck(`${vw}x${vh} 题面为单列`, cols === 1, "cols=" + cols);
      ck(`${vw}x${vh} 题面无横向溢出`, await noOverflow());
      await page.screenshot({ path: `${OUT}/narrow-${vw}x${vh}-problem.png`, fullPage: true });
    });
  }

  // B-11 未知路由 / 非法参数
  await scenario("B-11 未知路由与非法参数", async () => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.goto(BASE + "/#/definitely-nope", { waitUntil: "domcontentloaded" });
    await waitText("页面不存在");
    ck("未知路由显示页面不存在", /页面不存在/.test(await page.textContent("#app")));
    await page.evaluate(() => {
      location.hash = "#/problems/%E0%A4%A";
    });
    await waitText("地址参数无效");
    ck("非法编码显示地址参数无效", /地址参数无效/.test(await page.textContent("#app")));
  });

  // B-06 游客访问后台 → 登录页并携带目标
  await scenario("B-06 游客访问后台重定向", async () => {
    await clearAuth();
    await page.goto(BASE + "/#/problems", { waitUntil: "domcontentloaded" });
    await page.goto(BASE + "/#/admin", { waitUntil: "domcontentloaded" });
    await waitHash("/login");
    const h = await page.evaluate(() => location.hash);
    ck("游客被引导到登录页", h.startsWith("#/login"), h);
    ck("重定向携带原目标", /redirect=%2Fadmin/.test(h), h);
    ck("未显示后台内容", !/后台管理/.test(await page.textContent("#app")));
  });

  // B-07 首次强制改密：保留原目标并返回
  await scenario("B-07 首次改密保留原目标", async () => {
    // 当前处于登录页（由 B-06 重定向而来）
    await page.waitForSelector("#login-account", { timeout: 8000 });
    await page.fill("#login-account", "admin");
    await page.fill("#login-password", "AdminPass123");
    await page.click("#app form button[type=submit]");
    await waitExactHash("#/password");
    ck("未改密管理员被引导到改密页", (await page.evaluate(() => location.hash)) === "#/password");
    const pending = await page.evaluate(() => sessionStorage.getItem("oj.pendingTarget"));
    ck("改密期间保留原目标", pending === "/admin", "pending=" + pending);
    await page.fill("#pwd-old", "AdminPass123");
    await page.fill("#pwd-new", "AdminNewPass456");
    await page.fill("#pwd-confirm", "AdminNewPass456");
    await page.click("#app form button[type=submit]");
    await waitExactHash("#/admin");
    ck("改密成功后返回原目标 /admin", (await page.evaluate(() => location.hash)) === "#/admin");
    await waitText("后台管理");
    ck("改密后进入后台", /后台管理/.test(await page.textContent("#app")));
    await page.screenshot({ path: OUT + "/desktop-admin-home.png", fullPage: true });
  });

  // B-12 登录后导航
  await scenario("B-12 登录后导航", async () => {
    const nav = await page.textContent("#site-nav");
    ck("导航显示昵称", nav.includes("admin"), nav);
    ck("管理员显示后台入口", /管理后台/.test(nav), nav);
    ck("仍无排行榜入口", !/排行榜/.test(nav), nav);
  });

  // B-09 退出登录清理
  await scenario("B-09 退出登录清理", async () => {
    await page.evaluate(() => {
      sessionStorage.setItem("oj.pendingTarget", "/admin");
    });
    const btn = page.locator("#site-nav button", { hasText: "退出登录" });
    await btn.click();
    await waitHash("/login");
    ck("退出后跳转登录页", (await page.evaluate(() => location.hash)).startsWith("#/login"));
    const token = await page.evaluate(() => localStorage.getItem("oj.token"));
    const pending = await page.evaluate(() => sessionStorage.getItem("oj.pendingTarget"));
    ck("退出后 token 被清理", token === null, "token=" + token);
    ck("退出后待返回目标被清理", pending === null, "pending=" + pending);
    ck("退出后无后台入口", !/管理后台/.test(await page.textContent("#site-nav")));
  });

  // B-10 认证失效：伪造 token 访问后台
  await scenario("B-10 认证失效跳登录", async () => {
    await page.goto(BASE + "/#/problems", { waitUntil: "domcontentloaded" });
    await page.evaluate(() => {
      localStorage.setItem("oj.token", "bogus.invalid.token");
      localStorage.setItem(
        "oj.user",
        JSON.stringify({ id: 1, role: "admin", reset_pwd_flag: 0, nickname: "x" })
      );
    });
    // 强制整页重载，使 bootstrap 用伪造 token 核实 /api/me
    await page.goto(BASE + "/?bust=" + Date.now() + "#/admin/users", { waitUntil: "domcontentloaded" });
    await waitHash("/login");
    ck("无效 token 触发重新登录", (await page.evaluate(() => location.hash)).startsWith("#/login"));
    const token = await page.evaluate(() => localStorage.getItem("oj.token"));
    ck("失效凭证被清理", token === null, "token=" + token);
  });

  // B-08 普通用户越权目标回退
  await scenario("B-08 普通用户越权回退", async () => {
    await clearAuth();
    const reg = await api("/api/register", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ nickname: "m41br" + Date.now(), password: "UserPass123" }),
    });
    ck("注册普通用户", reg.status === 201, "status=" + reg.status);
    const account = reg.data && reg.data.account;
    await page.goto(BASE + "/#/problems", { waitUntil: "domcontentloaded" });
    await page.goto(BASE + "/#/admin", { waitUntil: "domcontentloaded" });
    await waitHash("/login");
    await page.waitForSelector("#login-account", { timeout: 8000 });
    await page.fill("#login-account", account);
    await page.fill("#login-password", "UserPass123");
    await page.click("#app form button[type=submit]");
    await waitExactHash("#/problems");
    ck("普通用户回退题目列表", (await page.evaluate(() => location.hash)) === "#/problems");
    ck("未进入后台", !/后台管理/.test(await page.textContent("#app")));
  });

  return JSON.stringify({ results, errors });
}
