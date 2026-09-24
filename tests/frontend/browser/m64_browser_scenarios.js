// M6.4 最终验收 A 真实浏览器场景（playwright-cli run-code 脚本，Windows/Linux 通用）。
//
// 用法（在已 `playwright-cli open` 的会话中）：
//   playwright-cli run-code --filename=m64_browser_scenarios.js
// 运行前把 __BASE__（后端根 URL）、__OUT__（截图输出目录）、__ADMIN_PW__（首次管理员
// 密码）替换为实际值；由 run_m64_browser.ps1 / run_m64_browser.sh 自动完成。
//
// 覆盖 SPEC §5 A：注册取 10 位账号、重复昵称拒绝、账号登录、admin 首登强制改密、
// 管理员 UI 建题与隐藏用例、学生列表搜索/难度/标签/可见性筛选、提交 AC、列表 AC
// 标记与通过人数、排行榜、越权/伪造 token 拒绝、桌面与窄视口布局。真实 Chromium，
// 非 jsdom。返回 JSON：{ results:[{name,ok,detail}], errors:[...] }。
async (page) => {
  const BASE = "__BASE__";
  const OUT = "__OUT__";
  const ADMIN_PW = "__ADMIN_PW__";
  const results = [];
  const errors = [];
  const ck = (name, cond, detail) =>
    results.push({ name, ok: !!cond, detail: detail === undefined ? "" : String(detail).slice(0, 260) });

  page.on("pageerror", (e) => errors.push("pageerror: " + (e && e.message)));
  page.on("console", (m) => {
    if (m.type() !== "error") return;
    const t = m.text();
    if (/Failed to load resource|server responded with a status|net::ERR/.test(t)) return;
    errors.push("console: " + t);
  });

  const G = (h) => page.goto(BASE + "/" + h, { waitUntil: "domcontentloaded" });
  const waitHash = (s, t = 10000) =>
    page.waitForFunction((x) => location.hash.includes(x), s, { timeout: t });
  const goOn = (p, h) => p.goto(BASE + "/" + h, { waitUntil: "domcontentloaded" });
  const waitHashOn = (p, s, t = 10000) =>
    p.waitForFunction((x) => location.hash.includes(x), s, { timeout: t });
  const sleep = (ms) => page.waitForTimeout(ms);
  const apiFrom = (p, path, opts) =>
    p.evaluate(
      async ([pp, o]) => {
        const r = await fetch(pp, o || {});
        const text = await r.text();
        let data = null;
        try { data = JSON.parse(text); } catch (e) {}
        return { status: r.status, data };
      },
      [path, opts]
    );
  const api = (path, opts) => apiFrom(page, path, opts);
  const reg = async (nick, pw) =>
    api("/api/register", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ nickname: nick, password: pw }),
    });
  const loginApi = async (account, pw) =>
    api("/api/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ account, password: pw }),
    });
  const loginUi = async (p, account, pw) => {
    await p.goto(BASE + "/#/login", { waitUntil: "domcontentloaded" });
    await p.waitForSelector("#login-account", { timeout: 10000 });
    await p.fill("#login-account", account);
    await p.fill("#login-password", pw);
    await p.click("#app form button[type=submit]");
  };
  const token = () => page.evaluate(() => localStorage.getItem("oj.token"));
  const setSource = async (code) => {
    await page.waitForFunction(
      () => document.querySelector(".CodeMirror") || document.querySelector("#source-code"),
      null,
      { timeout: 20000 }
    );
    await sleep(500);
    const hasCM = await page.evaluate(() => {
      const el = document.querySelector(".CodeMirror");
      return !!(el && el.CodeMirror);
    });
    if (hasCM) {
      await page.evaluate((c) => document.querySelector(".CodeMirror").CodeMirror.setValue(c), code);
    } else {
      await page.fill("#source-code", code);
    }
  };

  const A = async (name, fn) => {
    try { await fn(); } catch (e) { ck(name + "（异常）", false, e && e.message); }
  };

  const suffix = Date.now().toString().slice(-6);
  const nick = "m64win_" + suffix;
  const pw = "M64StuPw123";
  const newAdminPw = "M64WinAdminNew123!";
  const title = "M64 Windows A+B " + suffix;
  const hiddenTitle = "M64 Windows 隐藏题 " + suffix;
  let account = "";
  let pid = null;

  // ---------- A1 注册 + 重复昵称 ----------
  await A("A1 注册与重复昵称", async () => {
    await G("#/register");
    await page.waitForSelector("#reg-nickname", { timeout: 10000 });
    await page.fill("#reg-nickname", nick);
    await page.fill("#reg-password", pw);
    await page.fill("#reg-confirm", pw);
    await page.click("#app form button[type=submit]");
    await page.waitForSelector(".account-number", { timeout: 15000 });
    account = (await page.textContent(".account-number")).trim();
    ck("A1 注册返回 10 位数字账号", /^\d{10}$/.test(account), "account=" + account);
    await page.screenshot({ path: OUT + "/a1_register.png" });

    await G("#/login");
    await sleep(300);
    await G("#/register");
    await page.waitForSelector("#reg-nickname", { timeout: 10000 });
    await page.fill("#reg-nickname", nick);
    await page.fill("#reg-password", pw);
    await page.fill("#reg-confirm", pw);
    await page.click("#app form button[type=submit]");
    await page.waitForSelector(".alert-error", { timeout: 15000 });
    const txt = (await page.textContent(".alert-error")) || "";
    ck("A1 重复昵称注册被拒", /昵称|存在|占用|冲突/.test(txt), txt.trim());

    await loginUi(page, account, pw);
    await waitHash("#/problems");
    ck("A1 学生登录成功", /m64win_/.test((await page.textContent("nav")) || ""));
  });

  // ---------- A2 admin 首登强制改密 ----------
  // 用独立 context，避免与学生会话共享 localStorage。
  const adminCtx = await page.context().browser().newContext({ viewport: { width: 1280, height: 800 } });
  const adminPage = await adminCtx.newPage();
  await A("A2 admin 首改", async () => {
    await loginUi(adminPage, "admin", ADMIN_PW);
    await waitHashOn(adminPage, "#/password");
    await adminPage.waitForSelector("#pwd-old", { timeout: 10000 });
    const navBefore = (await adminPage.textContent("nav")) || "";
    ck("A2 首登落在改密页且无管理入口", !navBefore.includes("管理后台"), "hash=" + (await adminPage.evaluate(() => location.hash)));
    const tok = await adminPage.evaluate(() => localStorage.getItem("oj.token"));
    const pre = await apiFrom(adminPage, "/api/admin/problems", {
      method: "POST",
      headers: { "Content-Type": "application/json", Authorization: "Bearer " + tok },
      body: JSON.stringify({ title: "x", difficulty: "easy" }),
    });
    ck("A2 未改密管理员接口 403", pre.status === 403 && pre.data && pre.data.code === "PASSWORD_CHANGE_REQUIRED", "status=" + pre.status);
    await adminPage.fill("#pwd-old", ADMIN_PW);
    await adminPage.fill("#pwd-new", newAdminPw);
    await adminPage.fill("#pwd-confirm", newAdminPw);
    await adminPage.click("#app form button[type=submit]");
    await adminPage.waitForFunction(() => (document.querySelector("nav") || {}).innerText && document.querySelector("nav").innerText.includes("管理后台"), null, { timeout: 15000 });
    ck("A2 admin 改密后出现管理入口", true);
  });

  // ---------- A3 admin UI 建题 + 隐藏用例 ----------
  await A("A3 建题与隐藏用例", async () => {
    await goOn(adminPage, "#/admin/problems/new");
    await adminPage.waitForSelector("#admin-problem-title", { timeout: 15000 });
    await adminPage.fill("#admin-problem-title", title);
    await adminPage.selectOption("#admin-problem-difficulty", "medium");
    await adminPage.fill("#admin-problem-description", "M6.4 Windows 浏览器验收");
    await adminPage.fill("#admin-problem-tags", "入门,win验收");
    await adminPage.fill("#admin-problem-time", "5000");
    await adminPage.fill("#admin-problem-memory", "131072");
    await adminPage.click("#app button:has-text('添加公开样例')");
    const row = adminPage.locator(".sample-edit").first();
    await row.locator("textarea").nth(0).fill("1 2");
    await row.locator("textarea").nth(1).fill("3");
    await adminPage.click("#app form button[type=submit]:has-text('创建题目')");
    await adminPage.waitForFunction((t) => (document.body.innerText || "").includes(t), title, { timeout: 20000 });
    const tok = await adminPage.evaluate(() => localStorage.getItem("oj.token"));
    const list = await apiFrom(adminPage, "/api/problems?q=" + encodeURIComponent(title), { headers: { Authorization: "Bearer " + tok } });
    const prob = list.data && list.data.problems.find((p) => p.title === title);
    pid = prob && prob.id;
    ck("A3 经 UI 创建题目", !!pid, "id=" + pid);
    const addHidden = async (input, output) => {
      await goOn(adminPage, "#/admin/problems/" + pid + "/testcases");
      await adminPage.waitForSelector(".case-card.create", { timeout: 15000 });
      const create = adminPage.locator(".case-card.create");
      await create.locator("textarea.case-input").nth(0).fill(input);
      await create.locator("textarea.case-input").nth(1).fill(output);
      const before = await adminPage.locator(".case-card").count();
      await create.locator("button:has-text('新增隐藏用例')").click();
      await adminPage.waitForFunction((n) => document.querySelectorAll(".case-card").length > n, before, { timeout: 15000 });
    };
    await addHidden("2 3", "5");
    await addHidden("100 200", "300");
    const tcs = await apiFrom(adminPage, "/api/admin/problems/" + pid + "/testcases", { headers: { Authorization: "Bearer " + tok } });
    const hidden = (tcs.data && tcs.data.testcases ? tcs.data.testcases : []).filter((c) => !c.is_sample).length;
    ck("A3 录入多组隐藏用例", hidden >= 2, "hidden=" + hidden);
    const det = await apiFrom(adminPage, "/api/problems/" + pid, { headers: { Authorization: "Bearer " + tok } });
    ck("A3 详情仅下发样例、不含隐藏用例", det.status === 200 && JSON.stringify(det.data).indexOf("100 200") === -1, "samples=" + ((det.data && det.data.samples) || []).length);
    const mk = await apiFrom(adminPage, "/api/admin/problems", {
      method: "POST",
      headers: { "Content-Type": "application/json", Authorization: "Bearer " + tok },
      body: JSON.stringify({ title: hiddenTitle, difficulty: "easy", visible: false }),
    });
    ck("A3 创建隐藏题", mk.status === 201, "status=" + mk.status);
  });

  // ---------- A3b 学生列表搜索/筛选 + M4.2 ----------
  await A("M4.2 列表与筛选", async () => {
    await G("#/problems?m64=" + Date.now());
    await page.waitForSelector(".problem-table", { timeout: 15000 });
    await sleep(600);
    const all = (await page.textContent(".problem-table")) || "";
    ck("M4.2 学生列表可见公开新题", all.includes(title));
    ck("M4.2 学生列表不含隐藏题", !all.includes(hiddenTitle));
    await page.fill("#problem-search", title);
    await page.click("#app button:has-text('搜索')");
    await page.waitForFunction((t) => (document.querySelector(".problem-table") || {}).innerText && document.querySelector(".problem-table").innerText.includes(t), title, { timeout: 15000 });
    ck("M4.2 搜索生效", (await page.locator(".problem-table tbody tr").count()) >= 1);
    await page.click("#app button:has-text('清空条件')");
    await sleep(800);
    await page.selectOption("#problem-tag-filter", "win验收");
    await sleep(1000);
    ck("M4.2 标签筛选生效", (await page.locator(".problem-table tbody tr").count()) >= 1);
    await page.click("#app button:has-text('清空条件')");
    await sleep(800);
    await page.selectOption("#problem-difficulty-filter", "medium");
    await sleep(1000);
    ck("M4.2 难度筛选生效", (await page.locator(".problem-table tbody tr").count()) >= 1);
    await page.click("#app button:has-text('清空条件')");
    await sleep(1000);
    const before = await page.evaluate((t) => {
      for (const tr of document.querySelectorAll(".problem-table tbody tr")) {
        if (tr.innerText.includes(t)) {
          const tds = tr.querySelectorAll("td");
          return { status: tds[1] ? tds[1].innerText.trim() : "", pass: tds[tds.length - 1].innerText.trim() };
        }
      }
      return null;
    }, title);
    ck("M4.2 未 AC 状态显示", before && before.status.includes("未"), JSON.stringify(before));
  });

  // ---------- A4 提交 AC + 列表 + 排行榜 ----------
  await A("A4 提交与排行", async () => {
    await page.locator('a[href="#/problems/' + pid + '"]').first().click();
    await waitHash("#/problems/" + pid);
    await setSource('#include <iostream>\nint main(){long long a,b;std::cin>>a>>b;std::cout<<(a+b)<<"\\n";}\n');
    await page.click("#app button:has-text('提交判题')");
    await page.waitForFunction(
      () => {
        const el = document.querySelector(".judge-result");
        return el && /(^|\s)AC(\s|$)/.test(el.innerText) && el.innerText.includes("测试点");
      },
      null,
      { timeout: 90000 }
    );
    ck("A4 提交 AC 并显示逐点结果", true);
    await page.screenshot({ path: OUT + "/a4_student_ac.png" });
    await G("#/problems");
    await page.waitForSelector(".problem-table", { timeout: 15000 });
    await sleep(800);
    const after = await page.evaluate((t) => {
      for (const tr of document.querySelectorAll(".problem-table tbody tr")) {
        if (tr.innerText.includes(t)) {
          const tds = tr.querySelectorAll("td");
          return { status: tds[1] ? tds[1].innerText.trim() : "", pass: tds[tds.length - 1].innerText.trim() };
        }
      }
      return null;
    }, title);
    ck("A4 列表标记已 AC", after && after.status.includes("AC"), JSON.stringify(after));
    ck("A4 通过人数 +1", after && Number(after.pass) >= 1, "pass=" + (after && after.pass));
    await G("#/leaderboard");
    await page.waitForSelector(".data, .state", { timeout: 15000 });
    await sleep(500);
    ck("A4 排行榜显示学生", ((await page.textContent("body")) || "").includes(nick));
    await page.screenshot({ path: OUT + "/a4_leaderboard.png" });
  });

  // ---------- 权限 ----------
  await A("权限检查", async () => {
    const tok = await token();
    const forbid = await api("/api/admin/problems", {
      method: "POST",
      headers: { "Content-Type": "application/json", Authorization: "Bearer " + tok },
      body: JSON.stringify({ title: "x", difficulty: "easy" }),
    });
    ck("普通用户调管理员接口 403", forbid.status === 403, "status=" + forbid.status);
    const forged = await api("/api/me", { headers: { Authorization: "Bearer forged.token.value" } });
    ck("伪造 token 401", forged.status === 401, "status=" + forged.status);
    const nick2 = "m64winother_" + suffix;
    const r2 = await reg(nick2, "OtherPw12345");
    const l2 = await loginApi(r2.data.account, "OtherPw12345");
    const s2 = await api("/api/problems/" + pid + "/submit", {
      method: "POST",
      headers: { "Content-Type": "application/json", Authorization: "Bearer " + l2.data.token },
      body: JSON.stringify({ language: "cpp17", code: '#include <iostream>\nint main(){long long a,b;std::cin>>a>>b;std::cout<<(a+b)<<"\\n";}' }),
    });
    const cross = await api("/api/submissions/" + s2.data.id, { headers: { Authorization: "Bearer " + tok } });
    ck("学生无法读取他人提交 404", cross.status === 404, "status=" + cross.status);
  });

  // ---------- 窄视口 ----------
  await A("窄视口", async () => {
    await page.setViewportSize({ width: 360, height: 640 });
    await G("#/problems/" + pid);
    await page.waitForSelector(".problem-layout", { timeout: 15000 });
    const layout = await page.evaluate(() => {
      const el = document.querySelector(".problem-layout");
      const cs = getComputedStyle(el);
      const cols = cs.gridTemplateColumns.split(" ").filter((x) => x.trim()).length;
      return { display: cs.display, cols, scrollW: document.documentElement.scrollWidth, innerW: window.innerWidth };
    });
    ck("窄视口 360 单列且无横向溢出", layout.display === "grid" && layout.cols === 1 && layout.scrollW <= layout.innerW + 4, JSON.stringify(layout));
    await page.screenshot({ path: OUT + "/narrow_360.png", fullPage: true });
  });

  await adminCtx.close();
  return JSON.stringify({ results, errors });
}
