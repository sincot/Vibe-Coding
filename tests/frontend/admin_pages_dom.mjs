// M2.5 后台管理页面 DOM 级验证（jsdom 执行真实 web/js 模块，连接真实服务）。
// 仅用于开发验证，不属于项目运行依赖，不注册到 CTest；运行方式见 run.sh。
// jsdom 通过 JSDOM_DIR 指向的目录解析（默认 /tmp/opencode/domtest），保持前端无构建、
// 无测试框架。
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL, fileURLToPath } from "node:url";
import { createRequire } from "node:module";

const jsdomBase = process.env.JSDOM_DIR || "/tmp/opencode/domtest";
let jsdomModule;
try {
  jsdomModule = createRequire(import.meta.url)("jsdom");
} catch (e1) {
  try {
    jsdomModule = createRequire(path.join(jsdomBase, "package.json"))("jsdom");
  } catch (e2) {
    throw new Error(
      `无法解析 jsdom。请先安装：npm install --prefix ${jsdomBase} jsdom，` +
        `或用 JSDOM_DIR 指向已安装 jsdom 的目录。`
    );
  }
}
const { JSDOM, VirtualConsole } = jsdomModule;

const consoleErrors = [];
const virtualConsole = new VirtualConsole();
virtualConsole.on("jsdomError", (e) => {
  consoleErrors.push(String((e && (e.stack || e.message)) || e));
});
virtualConsole.on("error", (...args) => {
  consoleErrors.push("console.error: " + args.map(String).join(" "));
});
process.on("unhandledRejection", (r) => {
  consoleErrors.push("unhandledRejection: " + String((r && r.stack) || r));
});

const BASE = process.env.BASE;
if (!BASE) throw new Error("BASE env required");
// 仓库根目录：本文件位于 <root>/tests/frontend/。
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const realFetch = globalThis.fetch;

const html = fs.readFileSync(path.join(ROOT, "web/index.html"), "utf8");
const dom = new JSDOM(html, {
  url: BASE + "/",
  runScripts: "outside-only",
  pretendToBeVisual: true,
  virtualConsole,
});
const { window } = dom;

globalThis.window = window;
globalThis.document = window.document;
globalThis.location = window.location;
globalThis.history = window.history;
globalThis.localStorage = window.localStorage;
// M4.1 起「登录后返回目标」使用 sessionStorage；jsdom 在 Node 下不会自动暴露，需显式绑定。
globalThis.sessionStorage = window.sessionStorage;
globalThis.fetch = (input, init) => {
  const url = typeof input === "string" ? new URL(input, BASE).toString() : input;
  return realFetch(url, init);
};
window.fetch = globalThis.fetch;

const mainUrl = pathToFileURL(path.join(ROOT, "web/js/main.js")).href;
const routerUrl = pathToFileURL(path.join(ROOT, "web/js/router.js")).href;
const authUrl = pathToFileURL(path.join(ROOT, "web/js/auth.js")).href;
const navUrl = pathToFileURL(path.join(ROOT, "web/js/nav.js")).href;

await import(mainUrl);
const router = await import(routerUrl);
const auth = await import(authUrl);
const nav = await import(navUrl);
await new Promise((r) => setTimeout(r, 120));

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function waitFor(fn, { timeout = 8000, interval = 25, label = "condition" } = {}) {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    (function tick() {
      let value;
      try {
        value = fn();
      } catch (e) {
        value = null;
      }
      if (value) return resolve(value);
      if (Date.now() - start > timeout) {
        return reject(new Error("waitFor timeout: " + label));
      }
      setTimeout(tick, interval);
    })();
  });
}
const el = (id) => document.getElementById(id);
const q = (sel) => document.querySelector(sel);
const bodyText = () => document.body.textContent || "";
const hash = () => location.hash;

async function goto(p) {
  router.navigate(p);
  await sleep(30);
}
function fireSubmit(form) {
  form.dispatchEvent(new window.Event("submit", { bubbles: true, cancelable: true }));
}
async function api(method, p, { token, body } = {}) {
  const headers = {};
  if (token) headers.Authorization = "Bearer " + token;
  if (body !== undefined) headers["Content-Type"] = "application/json";
  const res = await realFetch(BASE + p, {
    method,
    headers,
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  const text = await res.text();
  let data = null;
  if (text) {
    try {
      data = JSON.parse(text);
    } catch {
      data = null;
    }
  }
  return { status: res.status, data, text };
}
function setDomAuth(token, user) {
  auth.setAuth(token, user);
  nav.renderNav();
}
function clearDomAuth() {
  auth.clearAuth();
  nav.renderNav();
}
async function clickModalConfirm() {
  const overlay = await waitFor(() => q(".modal-overlay"), { label: "modal" });
  const btns = overlay.querySelectorAll(".modal-actions button");
  btns[btns.length - 1].click();
  await sleep(40);
}
async function clickModalCancel() {
  const overlay = await waitFor(() => q(".modal-overlay"), { label: "modal" });
  overlay.querySelector(".modal-actions button").click();
  await sleep(40);
}
async function waitModalGone() {
  await waitFor(() => !q(".modal-overlay"), { label: "modal gone" });
}
function modalTitle() {
  const t = q(".modal .modal-title");
  return t ? t.textContent.trim() : "";
}
function rowByHref(substr) {
  return [...document.querySelectorAll("#app table.data tbody tr")].find((tr) =>
    tr.querySelector(`a[href*="${substr}"]`)
  );
}
function buttonByText(root, text) {
  return [...root.querySelectorAll("button")].find((b) => b.textContent.trim() === text);
}

// ---------------------------------------------------------------------------
// result collection
// ---------------------------------------------------------------------------
const results = [];
function check(name, cond, detail = "") {
  results.push({ name, ok: !!cond, detail });
  if (!cond) console.error("  FAIL:", name, detail ? "| " + detail : "");
  else console.log("  ok  :", name);
}
async function scenario(name, fn) {
  console.log("\n== " + name + " ==");
  try {
    await fn();
  } catch (e) {
    check(name + " (no exception)", false, e && (e.stack || e.message));
  }
}

const ctx = {};

// ---------------------------------------------------------------------------
// scenarios
// ---------------------------------------------------------------------------

await scenario("S1 游客访问后台路由", async () => {
  clearDomAuth();
  await goto("/admin");
  await sleep(30);
  check("游客 /admin 重定向到登录", hash().startsWith("#/login"), hash());
  check("重定向携带目标页", /redirect=/.test(hash()), hash());
});

await scenario("S2 游客访问题库被引导登录", async () => {
  clearDomAuth();
  await goto("/problems");
  await waitFor(() => hash().startsWith("#/login"), { label: "guest redirected to login" });
  check("游客访问题库被引导到登录页", hash().startsWith("#/login"), hash());
  check("未渲染题目表格", !q("#app table.data"));
  check("重定向携带原目标", /redirect=%2Fproblems/.test(hash()), hash());
});

await scenario("S3 普通用户直访后台", async () => {
  const reg = await api("POST", "/api/register", {
    body: { nickname: "m25user", password: "UserPass123" },
  });
  check("注册普通用户成功", reg.status === 201, JSON.stringify(reg.data));
  ctx.userAccount = reg.data.account;
  ctx.userPassword = "UserPass123";
  const lg = await api("POST", "/api/login", {
    body: { account: ctx.userAccount, password: ctx.userPassword },
  });
  check("普通用户登录成功", lg.status === 200, JSON.stringify(lg.data));
  ctx.userToken = lg.data.token;
  ctx.userId = lg.data.user.id;
  setDomAuth(lg.data.token, lg.data.user);
  await goto("/admin");
  await sleep(40);
  check("普通用户被拒（无 token 失效误判）", bodyText().includes("无权访问后台"), hash());
  check("普通用户未跳转登录", !hash().startsWith("#/login"), hash());
});

await scenario("S4 未改密管理员直访后台并完成首次改密", async () => {
  const al = await api("POST", "/api/login", {
    body: { account: "admin", password: "AdminPass123" },
  });
  check("admin 登录成功", al.status === 200, JSON.stringify(al.data));
  check("admin 首次改密标记为 1", al.data.user.reset_pwd_flag === 1);
  ctx.adminToken = al.data.token;
  setDomAuth(al.data.token, al.data.user);
  await goto("/admin");
  await sleep(40);
  check("未改密管理员被引导到改密页", hash() === "#/password", hash());

  el("pwd-old").value = "AdminPass123";
  el("pwd-new").value = "AdminNewPass456";
  el("pwd-confirm").value = "AdminNewPass456";
  fireSubmit(q("#app form"));
  // M4.1：登录后若需强制改密，保留原目标，改密成功后返回原目标（此处为 /admin）。
  await waitFor(() => hash() === "#/admin", { label: "password done (return to target)" });
  check("首次改密后返回原目标 /admin", hash() === "#/admin", hash());
  const me = await api("GET", "/api/me", { token: ctx.adminToken });
  check("改密后 reset_pwd_flag 清除", me.data && me.data.reset_pwd_flag === 0, JSON.stringify(me.data));
  ctx.adminUser = me.data;
  setDomAuth(ctx.adminToken, me.data);
  nav.renderNav();
  check("导航显示管理后台入口", q("#site-nav").textContent.includes("管理后台"));
  check("页脚显示管理员入口", !!q("#footer-admin a"));
});

await scenario("S5 管理员进入后台", async () => {
  await goto("/admin");
  await waitFor(() => bodyText().includes("后台管理"), { label: "admin home" });
  check("后台首页可访问", bodyText().includes("题目管理"));
  await goto("/admin/problems");
  await waitFor(() => q("#app table.data") || bodyText().includes("没有符合条件的题目"), {
    label: "admin problems",
  });
  check("管理员题目列表可访问", !!q("#app table.data") || bodyText().includes("共"));
});

await scenario("S6 创建题目（含 HTML 特殊字符）", async () => {
  await goto("/admin/problems/new");
  await waitFor(() => el("admin-problem-title"), { label: "new form" });
  const xssTitle = 'M25 测试题 <img src=x onerror="window.__xss=1">';
  ctx.xssTitle = xssTitle;
  el("admin-problem-title").value = xssTitle;
  el("admin-problem-difficulty").value = "hard";
  el("admin-problem-description").value = "描述 <b>加粗</b>\n第二行";
  el("admin-problem-tags").value = "m25,标签";
  el("admin-problem-time").value = "1500";
  el("admin-problem-memory").value = "32768";
  buttonByText(q("#app"), "添加公开样例").click();
  const sample = [...document.querySelectorAll(".samples-editor .sample-edit")].pop();
  const [si, so] = sample.querySelectorAll("textarea");
  si.value = "1  2\n";
  so.value = "3\n";
  fireSubmit(q("form.admin-form"));
  await waitFor(() => hash() === "#/admin/problems", { label: "back to list" });
  await waitFor(() => bodyText().includes("M25 测试题"), { label: "created title in list" });
  check(
    "XSS 标题按文本渲染未被执行为 HTML",
    !q('#app img[src="x"]') && bodyText().includes("<img src=x"),
    bodyText().slice(0, 300)
  );

  const listed = await api("GET", "/api/problems?q=M25&visible=all", { token: ctx.adminToken });
  const found = (listed.data.problems || []).find((p) => p.title.startsWith("M25 测试题"));
  check("创建后列表可查到", !!found, JSON.stringify(listed.data && listed.data.problems));
  ctx.problemId = found && found.id;
  const detail = await api("GET", "/api/problems/" + ctx.problemId, { token: ctx.adminToken });
  check("字段已保存（难度 hard）", detail.data.difficulty === "hard", JSON.stringify(detail.data));
  check("标签已保存", JSON.stringify(detail.data.tags) === JSON.stringify(["m25", "标签"]), JSON.stringify(detail.data.tags));
  check("时限按 ms 保存", detail.data.time_limit_ms === 1500, String(detail.data.time_limit_ms));
  check("内存按 KB 保存", detail.data.memory_limit_kb === 32768, String(detail.data.memory_limit_kb));
  check("公开样例已保存", detail.data.samples.length === 1 && detail.data.samples[0].output === "3\n", JSON.stringify(detail.data.samples));
});

await scenario("S7 编辑题目并刷新后校验", async () => {
  await goto(`/admin/problems/${ctx.problemId}/edit`);
  await waitFor(() => el("admin-problem-title"), { label: "edit form" });
  check("编辑表单回填标题", el("admin-problem-title").value === ctx.xssTitle, el("admin-problem-title").value);
  el("admin-problem-title").value = "M25 已编辑题目";
  el("admin-problem-difficulty").value = "medium";
  el("admin-problem-visible").checked = false;
  fireSubmit(q("form.admin-form"));
  await sleep(1000);
  if (hash() !== "#/admin/problems") {
    const alert = q("#app .alert");
    console.log("  S7 DEBUG alert:", alert ? alert.textContent : "(none)");
  }
  await waitFor(() => hash() === "#/admin/problems", { label: "back after edit" });
  await waitFor(() => bodyText().includes("M25 已编辑题目"), { label: "edited title" });
  const detail = await api("GET", "/api/problems/" + ctx.problemId, { token: ctx.adminToken });
  check("标题已更新", detail.data.title === "M25 已编辑题目", JSON.stringify(detail.data.title));
  check("难度已更新", detail.data.difficulty === "medium", String(detail.data.difficulty));
  check("可见性已更新为隐藏", detail.data.visible === false, String(detail.data.visible));
});

await scenario("S8 公开 / 隐藏操作影响可见范围", async () => {
  const visBadge = () => {
    const r = rowByHref(`/admin/problems/${ctx.problemId}/edit`);
    if (!r) return "";
    const b = r.querySelector(".badge.badge-hidden, .badge.badge-public");
    return b ? b.textContent.trim() : "";
  };
  await goto("/admin/problems");
  await waitFor(() => rowByHref(`/admin/problems/${ctx.problemId}/edit`), { label: "row" });
  let row = rowByHref(`/admin/problems/${ctx.problemId}/edit`);
  check("列表显示隐藏状态", visBadge() === "隐藏", visBadge());
  buttonByText(row, "设为公开").click();
  await clickModalConfirm();
  await waitFor(() => visBadge() === "公开", { label: "now public" });

  const guestList = await api("GET", "/api/problems?q=M25%20已编辑");
  check("公开后游客列表可见", (guestList.data.problems || []).some((p) => p.id === ctx.problemId), JSON.stringify(guestList.data.problems));

  row = rowByHref(`/admin/problems/${ctx.problemId}/edit`);
  buttonByText(row, "设为隐藏").click();
  await clickModalConfirm();
  await waitFor(() => visBadge() === "隐藏", { label: "now hidden" });
  const guestList2 = await api("GET", "/api/problems?q=M25%20已编辑");
  check("隐藏后游客列表不可见", !(guestList2.data.problems || []).some((p) => p.id === ctx.problemId), JSON.stringify(guestList2.data.problems));
  const adminList = await api("GET", "/api/problems?q=M25%20已编辑&visible=all", { token: ctx.adminToken });
  check("管理员仍可见隐藏题", (adminList.data.problems || []).some((p) => p.id === ctx.problemId), JSON.stringify(adminList.data.problems));
});

await scenario("S9 测试用例新增/编辑/删除与隐藏隔离", async () => {
  await goto(`/admin/problems/${ctx.problemId}/testcases`);
  await waitFor(() => q(".case-card.create"), { label: "testcases page" });

  const create = q(".case-card.create");
  const [ci, co] = create.querySelectorAll("textarea");
  ci.value = "  a  b\n\n";
  co.value = "";
  buttonByText(create, "新增隐藏用例").click();
  await waitFor(
    () =>
      [...document.querySelectorAll(".case-card:not(.create):not(.readonly) textarea")].some(
        (t) => t.value.includes("a  b")
      ),
    { label: "case added" }
  );

  let adminCases = await api("GET", `/api/admin/problems/${ctx.problemId}/testcases`, { token: ctx.adminToken });
  let added = adminCases.data.testcases.find((c) => c.input === "  a  b\n\n");
  check("空输出与空白/换行原样保存", !!added && added.output === "", JSON.stringify(added));
  ctx.caseId = added && added.id;

  const publicDetail = await api("GET", "/api/problems/" + ctx.problemId);
  check(
    "隐藏用例不出现在公开详情",
    !JSON.stringify(publicDetail.data).includes("a  b"),
    JSON.stringify(publicDetail.data)
  );

  // 编辑
  await goto(`/admin/problems/${ctx.problemId}/testcases`);
  await waitFor(() => q(".case-card.create"), { label: "reload cases" });
  const editCards = [...document.querySelectorAll(".case-card")].filter(
    (c) => !c.classList.contains("create") && !c.classList.contains("readonly")
  );
  const target = editCards[editCards.length - 1];
  const [ei, eo] = target.querySelectorAll("textarea");
  ei.value = "x\n\n  y ";
  eo.value = "out ";
  target.querySelector('input[type="number"]').value = "7";
  buttonByText(target, "保存").click();
  await waitFor(async () => true, { timeout: 50 });
  await sleep(200);
  adminCases = await api("GET", `/api/admin/problems/${ctx.problemId}/testcases`, { token: ctx.adminToken });
  added = adminCases.data.testcases.find((c) => c.id === ctx.caseId);
  check("编辑内容原样保存", !!added && added.input === "x\n\n  y " && added.output === "out ", JSON.stringify(added));
  check("ord 已更新", !!added && added.ord === 7, JSON.stringify(added));

  // 删除
  await goto(`/admin/problems/${ctx.problemId}/testcases`);
  await waitFor(() => q(".case-card.create"), { label: "reload cases2" });
  const cards = [...document.querySelectorAll(".case-card")].filter(
    (c) => !c.classList.contains("create") && !c.classList.contains("readonly")
  );
  const t2 = cards[cards.length - 1];
  buttonByText(t2, "删除").click();
  await clickModalConfirm();
  await sleep(300);
  adminCases = await api("GET", `/api/admin/problems/${ctx.problemId}/testcases`, { token: ctx.adminToken });
  check("用例已删除", !adminCases.data.testcases.some((c) => c.id === ctx.caseId), JSON.stringify(adminCases.data.testcases));
});

await scenario("S10 不同题目用例不串用 + 快速切换", async () => {
  const createA = await api("POST", "/api/admin/problems", {
    token: ctx.adminToken,
    body: { title: "M25 用例隔离 A", difficulty: "easy", tags: ["m25"] },
  });
  const createB = await api("POST", "/api/admin/problems", {
    token: ctx.adminToken,
    body: { title: "M25 用例隔离 B", difficulty: "easy", tags: ["m25"] },
  });
  const pidA = createA.data.id;
  const pidB = createB.data.id;
  await api("POST", `/api/admin/problems/${pidA}/testcases`, {
    token: ctx.adminToken,
    body: { input: "AAA-ONLY\n", output: "1\n" },
  });
  await api("POST", `/api/admin/problems/${pidB}/testcases`, {
    token: ctx.adminToken,
    body: { input: "BBB-ONLY\n", output: "2\n" },
  });

  // 快速连续导航
  router.navigate(`/admin/problems/${pidA}/testcases`);
  router.navigate(`/admin/problems/${pidB}/testcases`);
  const hiddenInputs = () =>
    [...document.querySelectorAll(".case-card textarea")].map((t) => t.value);
  await waitFor(() => hiddenInputs().some((v) => v.includes("BBB-ONLY")), {
    label: "B cases shown",
  });
  await sleep(150);
  check(
    "切换到 B 只显示 B 的用例",
    hiddenInputs().some((v) => v.includes("BBB-ONLY")) &&
      !hiddenInputs().some((v) => v.includes("AAA-ONLY")),
    hiddenInputs().join("|")
  );
  const bTitle = bodyText().includes("用例隔离 B");
  check("页面标题对应题目 B", bTitle, bodyText().slice(0, 200));
});

await scenario("S11 用户列表与密码重置", async () => {
  await goto("/admin/users");
  await waitFor(() => q("#app table.data"), { label: "users table" });
  const row = [...document.querySelectorAll("#app table.data tbody tr")].find((tr) =>
    tr.textContent.includes(ctx.userAccount)
  );
  check("用户列表含目标账号", !!row, bodyText().slice(0, 300));
  buttonByText(row, "重置密码").click();
  await waitFor(() => el("admin-reset-password"), { label: "reset modal" });
  el("admin-reset-password").value = "ResetPass789";
  el("admin-reset-confirm").value = "ResetPass789";
  fireSubmit(q(".modal form"));
  await waitModalGone();
  await sleep(200);

  const oldLogin = await api("POST", "/api/login", {
    body: { account: ctx.userAccount, password: ctx.userPassword },
  });
  check("旧密码无法登录", oldLogin.status === 401, JSON.stringify(oldLogin.data));
  const newLogin = await api("POST", "/api/login", {
    body: { account: ctx.userAccount, password: "ResetPass789" },
  });
  check("新密码可登录", newLogin.status === 200, JSON.stringify(newLogin.data));
  check("重置后强制改密标记生效", newLogin.data.user.reset_pwd_flag === 1, JSON.stringify(newLogin.data.user));
  ctx.userPassword = "ResetPass789";
});

await scenario("S12 角色修改（提升 / 降级）", async () => {
  await goto("/admin/users");
  await waitFor(() => q("#app table.data"), { label: "users table" });
  let row = [...document.querySelectorAll("#app table.data tbody tr")].find((tr) =>
    tr.textContent.includes(ctx.userAccount)
  );
  buttonByText(row, "设为管理员").click();
  await clickModalConfirm();
  await sleep(300);
  let users = await api("GET", "/api/admin/users?page=1", { token: ctx.adminToken });
  let target = users.data.users.find((u) => u.id === ctx.userId);
  check("提升为管理员生效", target && target.role === "admin", JSON.stringify(target));

  await goto("/admin/users");
  await waitFor(() => q("#app table.data"), { label: "users table" });
  row = [...document.querySelectorAll("#app table.data tbody tr")].find((tr) =>
    tr.textContent.includes(ctx.userAccount)
  );
  buttonByText(row, "降为普通用户").click();
  await clickModalConfirm();
  await sleep(300);
  users = await api("GET", "/api/admin/users?page=1", { token: ctx.adminToken });
  target = users.data.users.find((u) => u.id === ctx.userId);
  check("降级为普通用户生效", target && target.role === "user", JSON.stringify(target));
});

await scenario("S13 删除取消 / 确认", async () => {
  const tmp = await api("POST", "/api/admin/problems", {
    token: ctx.adminToken,
    body: { title: "M25 待删除题目", difficulty: "easy", tags: ["m25"] },
  });
  const tmpId = tmp.data.id;
  await goto("/admin/problems");
  await waitFor(() => rowByHref(`/admin/problems/${tmpId}/edit`), { label: "tmp row" });
  let row = rowByHref(`/admin/problems/${tmpId}/edit`);
  buttonByText(row, "删除").click();
  await clickModalCancel();
  await sleep(150);
  let still = await api("GET", "/api/problems/" + tmpId, { token: ctx.adminToken });
  check("取消删除不发起删除", still.status === 200, "status=" + still.status);

  row = rowByHref(`/admin/problems/${tmpId}/edit`);
  buttonByText(row, "删除").click();
  await clickModalConfirm();
  await sleep(300);
  still = await api("GET", "/api/problems/" + tmpId, { token: ctx.adminToken });
  check("确认删除后记录不存在", still.status === 404, "status=" + still.status);
});

await scenario("S14 删除冲突（已有提交）", async () => {
  // 选择一道种子题并产生一次提交（AC），再尝试删除。
  const list = await api("GET", "/api/problems?q=A%2BB&visible=all", { token: ctx.adminToken });
  const seed = (list.data.problems || [])[0];
  check("找到种子题", !!seed, JSON.stringify(list.data.problems));
  ctx.seedId = seed && seed.id;
  const code = "#include <iostream>\nint main(){long long a,b;if(std::cin>>a>>b)std::cout<<(a+b)<<std::endl;return 0;}";
  const sub = await api("POST", `/api/problems/${ctx.seedId}/submit`, {
    token: ctx.adminToken,
    body: { language: "cpp17", code },
  });
  check("产生一次提交", sub.status === 200, JSON.stringify(sub.data && sub.data.status));

  await goto("/admin/problems");
  await waitFor(() => rowByHref(`/admin/problems/${ctx.seedId}/edit`), { label: "seed row" });
  const row = rowByHref(`/admin/problems/${ctx.seedId}/edit`);
  buttonByText(row, "删除").click();
  await clickModalConfirm();
  await waitFor(() => modalTitle().includes("无法删除"), { label: "conflict modal" });
  check("删除冲突有清晰提示", modalTitle().includes("无法删除"), modalTitle());
  await clickModalConfirm();
  await waitModalGone();
  const alive = await api("GET", "/api/problems/" + ctx.seedId, { token: ctx.adminToken });
  check("冲突时题目保留", alive.status === 200, "status=" + alive.status);
});

await scenario("S15 文本安全（昵称不被执行）", async () => {
  const reg = await api("POST", "/api/register", {
    body: { nickname: "<b>bad</b>", password: "XssPass123" },
  });
  check("含特殊字符昵称注册成功", reg.status === 201, JSON.stringify(reg.data));
  setDomAuth(ctx.adminToken, ctx.adminUser);
  await goto("/admin/users");
  await waitFor(() => q("#app table.data"), { label: "users table" });
  check("用户列表未注入 b 元素", !q("#app table.data b"), "b found");
  check("昵称按文本显示", bodyText().includes("<b>bad</b>"), bodyText().slice(0, 300));
});

await scenario("S16 无效 token 访问后台", async () => {
  setDomAuth("bogus.invalid.token", { id: 1, role: "admin", reset_pwd_flag: 0, nickname: "x" });
  await goto("/admin/users");
  await waitFor(() => hash().startsWith("#/login"), { label: "redirect to login" });
  check("无效 token 触发重新登录", hash().startsWith("#/login"), hash());
  check("本地凭证已清理", auth.getToken() === "", auth.getToken());
});

await scenario("S17 网络失败不假成功、不卡加载", async () => {
  setDomAuth(ctx.adminToken, ctx.adminUser);
  const orig = globalThis.fetch;
  globalThis.fetch = () => Promise.reject(new TypeError("network down"));
  window.fetch = globalThis.fetch;
  await goto("/admin/users");
  await waitFor(() => q("#app .alert-error"), { label: "network error block" });
  const txt = bodyText();
  check("网络失败显示错误而非成功", /网络/.test(txt), txt.slice(0, 200));
  check("未停留在加载中", !txt.includes("用户加载中"), txt.slice(0, 200));
  globalThis.fetch = orig;
  window.fetch = orig;
  await goto("/admin/users");
  await waitFor(() => q("#app table.data"), { label: "recovered" });
  check("恢复后可继续操作", !!q("#app table.data"));
});

// ===========================================================================
// 测试审查补充（R 系列）：实现轮 DOM 验证未覆盖的校验、筛选、ord、只读、
// 失败保真、保存中状态、敏感数据与分页边界。
// ===========================================================================
async function pollUntil(fn, { timeout = 6000, interval = 60, label = "poll" } = {}) {
  const start = Date.now();
  for (;;) {
    const value = await fn();
    if (value) return value;
    if (Date.now() - start > timeout) throw new Error("poll timeout: " + label);
    await sleep(interval);
  }
}

await scenario("R1 题目表单前端校验（负路径）", async () => {
  await goto("/admin/problems/new");
  await waitFor(() => el("admin-problem-title"), { label: "form" });
  const form = () => q("form.admin-form");
  const err = () => {
    const a = q("#app .alert-error");
    return a ? a.textContent : "";
  };
  const stayed = () => hash() === "#/admin/problems/new";

  fireSubmit(form());
  await sleep(60);
  check("空标题被拒且停留表单页", /标题不能为空/.test(err()) && stayed(), err());

  el("admin-problem-title").value = "R1 校验题";
  el("admin-problem-time").value = "0";
  fireSubmit(form());
  await sleep(60);
  check("时限 0（越界）被拒", /时间限制/.test(err()) && stayed(), err());

  el("admin-problem-time").value = "2000";
  el("admin-problem-memory").value = "0";
  fireSubmit(form());
  await sleep(60);
  check("内存 0（越界）被拒", /内存上限/.test(err()) && stayed(), err());

  el("admin-problem-memory").value = "65536";
  el("admin-problem-tags").value = "dup,dup";
  fireSubmit(form());
  await sleep(60);
  check("重复标签被拒", /重复/.test(err()) && stayed(), err());

  el("admin-problem-tags").value = Array.from({ length: 21 }, (_, i) => "t" + i).join(",");
  fireSubmit(form());
  await sleep(60);
  check("标签超过 20 个被拒", /最多 20/.test(err()) && stayed(), err());

  el("admin-problem-tags").value = "x".repeat(31);
  fireSubmit(form());
  await sleep(60);
  check("单个标签超过 30 字节被拒", /30 字节/.test(err()) && stayed(), err());

  el("admin-problem-tags").value = "r1";
  fireSubmit(form());
  await waitFor(() => hash() === "#/admin/problems", { label: "created" });
  const list = await api("GET", "/api/problems?q=" + encodeURIComponent("R1 校验题") + "&visible=all", {
    token: ctx.adminToken,
  });
  const found = (list.data.problems || []).find((p) => p.title === "R1 校验题");
  check("修正后创建成功", !!found, JSON.stringify(list.data.problems));
  if (found) await api("DELETE", "/api/admin/problems/" + found.id, { token: ctx.adminToken });
});

await scenario("R2 公开样例整体替换且隐藏用例不受影响", async () => {
  const created = await api("POST", "/api/admin/problems", {
    token: ctx.adminToken,
    body: {
      title: "R2 样例维护",
      difficulty: "easy",
      tags: ["r2"],
      samples: [
        { input: "OLD1\n", output: "1\n" },
        { input: "OLD2\n", output: "2\n" },
      ],
    },
  });
  const pid = created.data.id;
  const tc = await api("POST", `/api/admin/problems/${pid}/testcases`, {
    token: ctx.adminToken,
    body: { input: "HIDDEN-KEEP\n", output: "keep\n" },
  });
  const hid = tc.data.id;

  await goto(`/admin/problems/${pid}/edit`);
  await waitFor(() => el("admin-problem-title"), { label: "edit" });
  let rows = [...document.querySelectorAll(".samples-editor .sample-edit")];
  check("编辑表单回填两组公开样例", rows.length === 2, String(rows.length));

  buttonByText(rows[0], "移除该样例").click();
  buttonByText(q("#app"), "添加公开样例").click();
  rows = [...document.querySelectorAll(".samples-editor .sample-edit")];
  const [ni, no] = rows[rows.length - 1].querySelectorAll("textarea");
  ni.value = "NEWSAMPLE\n";
  no.value = "new\n";
  fireSubmit(q("form.admin-form"));
  await waitFor(() => hash() === "#/admin/problems", { label: "saved" });

  const detail = await api("GET", "/api/problems/" + pid, { token: ctx.adminToken });
  const inputs = detail.data.samples.map((s) => s.input);
  check(
    "公开样例被整体替换",
    inputs.length === 2 &&
      inputs.includes("OLD2\n") &&
      inputs.includes("NEWSAMPLE\n") &&
      !inputs.includes("OLD1\n"),
    JSON.stringify(inputs)
  );
  const cases = await api("GET", `/api/admin/problems/${pid}/testcases`, {
    token: ctx.adminToken,
  });
  check(
    "隐藏用例不受公开样例替换影响",
    cases.data.testcases.some((c) => c.id === hid && c.input === "HIDDEN-KEEP\n"),
    JSON.stringify(cases.data.testcases)
  );
});

await scenario("R3 用例 ord 规则（自动追加 / 显式 / 越界）", async () => {
  const created = await api("POST", "/api/admin/problems", {
    token: ctx.adminToken,
    body: { title: "R3 ord", difficulty: "easy", tags: ["r3"] },
  });
  const pid = created.data.id;
  await goto(`/admin/problems/${pid}/testcases`);
  await waitFor(() => q(".case-card.create"), { label: "testcases" });

  const casesNow = async () =>
    (await api("GET", `/api/admin/problems/${pid}/testcases`, { token: ctx.adminToken })).data
      .testcases;
  const addCase = async (input, ord) => {
    await waitFor(() => q(".case-card.create"), { label: "create form" });
    const card = q(".case-card.create");
    const [i, o] = card.querySelectorAll("textarea");
    i.value = input;
    o.value = "out\n";
    card.querySelector('input[type="number"]').value = ord === undefined ? "" : String(ord);
    buttonByText(card, "新增隐藏用例").click();
    await pollUntil(async () => (await casesNow()).some((c) => c.input === input), {
      label: "add " + input,
    });
  };

  await addCase("AUTO1\n");
  let cs = await casesNow();
  check("首个缺省 ord=0", cs.length === 1 && cs[0].ord === 0, JSON.stringify(cs));

  await addCase("AUTO2\n");
  cs = await casesNow();
  const auto2 = cs.find((c) => c.input === "AUTO2\n");
  check("第二个缺省 ord=最大+1", auto2 && auto2.ord === 1, JSON.stringify(cs));

  await addCase("EXPLICIT\n", 5);
  cs = await casesNow();
  const exp = cs.find((c) => c.input === "EXPLICIT\n");
  check("显式 ord=5 生效", exp && exp.ord === 5, JSON.stringify(cs));

  // 编辑器 ord 越界：前端拒绝、不发请求、服务端不变
  const editableCards = () =>
    [...document.querySelectorAll(".case-card")].filter(
      (c) => !c.classList.contains("create") && !c.classList.contains("readonly")
    );
  await waitFor(() => editableCards().length >= 3, { label: "editable cards" });
  const editCards = editableCards();
  const target = editCards[editCards.length - 1];
  target.querySelector('input[type="number"]').value = "1000001";
  buttonByText(target, "保存").click();
  await sleep(200);
  const msg = [...target.querySelectorAll(".alert-error")].map((n) => n.textContent).join("");
  check("编辑 ord 越界被拒", /0\.\.1000000/.test(msg), msg);
  cs = await casesNow();
  check("越界后服务端 ord 不变", cs.find((c) => c.id === exp.id).ord === 5, JSON.stringify(cs));

  // 新增表单 ord 越界
  const card = q(".case-card.create");
  const [i, o] = card.querySelectorAll("textarea");
  i.value = "BADORD\n";
  o.value = "x\n";
  card.querySelector('input[type="number"]').value = "1000001";
  buttonByText(card, "新增隐藏用例").click();
  await sleep(200);
  const creMsg = [...card.querySelectorAll(".alert-error")].map((n) => n.textContent).join("");
  check("新增 ord 越界被拒", /0\.\.1000000/.test(creMsg), creMsg);
  cs = await casesNow();
  check("越界未新增记录", !cs.some((c) => c.input === "BADORD\n"), JSON.stringify(cs));
});

await scenario("R4 题目列表筛选与空状态", async () => {
  await api("POST", "/api/admin/problems", {
    token: ctx.adminToken,
    body: { title: "R4 难度hard", difficulty: "hard", tags: ["r4"] },
  });

  await goto("/admin/problems");
  await waitFor(() => q(".admin-filters"), { label: "filters" });
  const searchInput = q(".admin-filters input");
  searchInput.value = "不存在的题目关键字ZZZ";
  buttonByText(q(".admin-filters"), "筛选").click();
  await waitFor(() => bodyText().includes("没有符合条件的题目"), { label: "empty" });
  check("搜索无结果显示空状态", bodyText().includes("没有符合条件的题目"), bodyText().slice(0, 200));

  await goto("/admin/problems");
  await waitFor(() => q(".admin-filters"), { label: "filters2" });
  [...document.querySelectorAll(".admin-filters select")][1].value = "0";
  buttonByText(q(".admin-filters"), "筛选").click();
  const vBadges = await pollUntil(
    () => {
      if (!hash().includes("visible=0")) return false;
      const rows = [...document.querySelectorAll("#app table.data tbody tr")];
      if (rows.length === 0) return false;
      const badges = rows.map((r) => {
        const b = r.querySelector(".badge.badge-hidden, .badge.badge-public");
        return b ? b.textContent.trim() : "";
      });
      return badges.length > 0 && badges.every((b) => b === "隐藏") ? badges : false;
    },
    { label: "hidden-only list" }
  );
  check("仅隐藏筛选只显示隐藏题", vBadges.length > 0 && vBadges.every((b) => b === "隐藏"), vBadges.join(","));

  await goto("/admin/problems");
  await waitFor(() => q(".admin-filters"), { label: "filters3" });
  [...document.querySelectorAll(".admin-filters select")][0].value = "hard";
  buttonByText(q(".admin-filters"), "筛选").click();
  const dBadges = await pollUntil(
    () => {
      if (!hash().includes("difficulty=hard")) return false;
      const rows = [...document.querySelectorAll("#app table.data tbody tr")];
      if (rows.length === 0) return false;
      const badges = rows.map((r) => {
        const hit = [...r.querySelectorAll(".badge")]
          .map((b) => b.textContent.trim())
          .find((t) => ["难", "中", "易"].includes(t));
        return hit || "";
      });
      return badges.length > 0 && badges.every((d) => d === "难") ? badges : false;
    },
    { label: "hard-only list" }
  );
  check("难度筛选只显示 hard", dBadges.length > 0 && dBadges.every((d) => d === "难"), dBadges.join(","));
});

await scenario("R5 可见性切换失败保留真实状态", async () => {
  await goto("/admin/problems?visible=all");
  await waitFor(() => rowByHref(`/admin/problems/${ctx.problemId}/edit`), { label: "row" });
  const visBadge = () => {
    const r = rowByHref(`/admin/problems/${ctx.problemId}/edit`);
    const b = r && r.querySelector(".badge.badge-hidden, .badge.badge-public");
    return b ? b.textContent.trim() : "";
  };
  if (visBadge() === "公开") {
    buttonByText(rowByHref(`/admin/problems/${ctx.problemId}/edit`), "设为隐藏").click();
    await clickModalConfirm();
    await waitFor(() => visBadge() === "隐藏", { label: "hidden" });
  }
  const before = visBadge();
  const orig = globalThis.fetch;
  globalThis.fetch = () => Promise.reject(new TypeError("network down"));
  window.fetch = globalThis.fetch;
  buttonByText(rowByHref(`/admin/problems/${ctx.problemId}/edit`), "设为公开").click();
  await clickModalConfirm();
  await sleep(200);
  check("切换失败后状态未变（不假成功）", visBadge() === before, "before=" + before + " after=" + visBadge());
  globalThis.fetch = orig;
  window.fetch = orig;
});

await scenario("R6 保存中禁用重复提交并保留输入", async () => {
  await goto("/admin/problems/new");
  await waitFor(() => el("admin-problem-title"), { label: "form" });
  el("admin-problem-title").value = "R6 保存中禁用";
  el("admin-problem-difficulty").value = "easy";
  const submit = buttonByText(q("#app"), "创建题目");
  const real = globalThis.fetch;
  let release = null;
  globalThis.fetch = (input, init) => {
    const u = new URL(input, BASE).toString();
    if (u.endsWith("/api/admin/problems") && init && init.method === "POST") {
      return new Promise((resolve, reject) => {
        release = () => real(u, init).then(resolve, reject);
      });
    }
    return real(u, init);
  };
  window.fetch = globalThis.fetch;

  fireSubmit(q("form.admin-form"));
  await sleep(50);
  check("保存中按钮禁用", submit.disabled === true, "disabled=" + submit.disabled);
  check("保存中显示进行中文案", /保存中/.test(submit.textContent), submit.textContent);
  fireSubmit(q("form.admin-form")); // 保存中的再次提交应被忽略
  release();
  await waitFor(() => hash() === "#/admin/problems", { label: "saved" });
  globalThis.fetch = real;
  window.fetch = real;
  await sleep(200);
  const list = await api("GET", "/api/problems?q=" + encodeURIComponent("R6 保存中禁用") + "&visible=all", {
    token: ctx.adminToken,
  });
  const matches = (list.data.problems || []).filter((p) => p.title === "R6 保存中禁用");
  check("保存中重复提交只创建一条", matches.length === 1, JSON.stringify(matches.map((p) => p.id)));
  if (matches[0]) await api("DELETE", "/api/admin/problems/" + matches[0].id, { token: ctx.adminToken });
});

await scenario("R7 隐藏用例与密码不进入本地存储", async () => {
  const created = await api("POST", "/api/admin/problems", {
    token: ctx.adminToken,
    body: { title: "R7 本地存储", difficulty: "easy", tags: ["r7"] },
  });
  const pid = created.data.id;
  const marker = "LOCALSTORAGE-SECRET-" + Date.now();
  await api("POST", `/api/admin/problems/${pid}/testcases`, {
    token: ctx.adminToken,
    body: { input: marker + "\n", output: "x\n" },
  });
  await goto(`/admin/problems/${pid}/testcases`);
  await waitFor(
    () => [...document.querySelectorAll(".case-card textarea")].some((t) => t.value.includes(marker)),
    { label: "hidden case rendered" }
  );

  // 通过 DOM 重置一个专用用户密码（走真实弹窗路径）
  const reg = await api("POST", "/api/register", {
    body: { nickname: "r7user" + Date.now(), password: "R7PassOld123" },
  });
  const account = reg.data.account;
  let targetId = null;
  for (let page = 1; page <= 5 && targetId === null; page++) {
    const users = await api("GET", `/api/admin/users?page=${page}`, { token: ctx.adminToken });
    const u = (users.data.users || []).find((x) => x.account === account);
    if (u) targetId = u.id;
    if (users.data.total_pages && page >= users.data.total_pages) break;
  }
  const secretPwd = "R7SecretPwd456";
  check("找到专用用户", targetId !== null, "account=" + account);
  if (targetId !== null) {
    await goto("/admin/users");
    await waitFor(() => q("#app table.data"), { label: "users table" });
    const row = [...document.querySelectorAll("#app table.data tbody tr")].find((tr) =>
      tr.textContent.includes(account)
    );
    buttonByText(row, "重置密码").click();
    await waitFor(() => el("admin-reset-password"), { label: "reset modal" });
    el("admin-reset-password").value = secretPwd;
    el("admin-reset-confirm").value = secretPwd;
    fireSubmit(q(".modal form"));
    await waitModalGone();
    await sleep(200);
  }

  const store = {};
  for (let i = 0; i < localStorage.length; i++) {
    const k = localStorage.key(i);
    store[k] = localStorage.getItem(k);
  }
  const dump = JSON.stringify(store);
  check("隐藏用例内容不在本地存储", !dump.includes(marker), dump.slice(0, 200));
  check("重置密码不在本地存储", !dump.includes(secretPwd), dump.slice(0, 200));
});

await scenario("R8 分页控件", async () => {
  let after = await api("GET", "/api/problems?visible=all&page=1", { token: ctx.adminToken });
  let guard = 0;
  while (after.data.total <= 20 && guard < 30) {
    await api("POST", "/api/admin/problems", {
      token: ctx.adminToken,
      body: { title: `R8 分页题 ${Date.now()}-${guard}`, difficulty: "easy", tags: ["r8"] },
    });
    guard++;
    after = await api("GET", "/api/problems?visible=all&page=1", { token: ctx.adminToken });
  }
  const pages = after.data.total_pages;
  check("题目分页存在多页", pages >= 2, "total=" + after.data.total + " pages=" + pages);

  await goto("/admin/problems?visible=all");
  await waitFor(() => q(".pagination"), { label: "pagination" });
  check("分页信息与服务端一致", q(".pagination-info").textContent.includes(`/ ${pages} 页`), q(".pagination-info").textContent);
  const rowEditHrefs = () =>
    [...document.querySelectorAll("#app table.data tbody tr")].map((tr) => {
      const a = tr.querySelector("a[href*='/edit']");
      return a ? a.getAttribute("href") : "";
    });
  const firstIds = rowEditHrefs();
  const next = buttonByText(q(".pagination"), "下一页");
  check("第一页下一页可用", next && !next.disabled);
  next.click();
  await waitFor(() => /page=2/.test(hash()), { label: "page2" });
  await waitFor(() => q(".pagination-info").textContent.includes("第 2 /"), { label: "page2 info" });
  await waitFor(() => q("#app table.data tbody tr"), { label: "page2 rows" });
  const secondIds = rowEditHrefs();
  check(
    "第二页内容与第一页不同",
    firstIds.length > 0 && JSON.stringify(firstIds) !== JSON.stringify(secondIds),
    "p1=" + firstIds.length + " p2=" + secondIds.length
  );
  check("第二页上一页可用", !buttonByText(q(".pagination"), "上一页").disabled);

  await goto("/admin/users");
  await waitFor(() => q(".pagination"), { label: "users pagination" });
  const uPages = /\/ (\d+) 页/.exec(q(".pagination-info").textContent);
  if (uPages && Number(uPages[1]) === 1) {
    check("用户单页时下一页禁用", buttonByText(q(".pagination"), "下一页").disabled === true, q(".pagination-info").textContent);
  } else {
    check("用户多页时下一页可用", !buttonByText(q(".pagination"), "下一页").disabled, q(".pagination-info").textContent);
  }
});

await scenario("R9 用例页公开样例只读", async () => {
  const created = await api("POST", "/api/admin/problems", {
    token: ctx.adminToken,
    body: {
      title: "R9 样例只读",
      difficulty: "easy",
      tags: ["r9"],
      samples: [{ input: "S-IN\n", output: "S-OUT\n" }],
    },
  });
  const pid = created.data.id;
  await api("POST", `/api/admin/problems/${pid}/testcases`, {
    token: ctx.adminToken,
    body: { input: "H-IN\n", output: "H-OUT\n" },
  });
  await goto(`/admin/problems/${pid}/testcases`);
  await waitFor(() => q(".case-card.readonly"), { label: "readonly sample" });
  const sampleCard = q(".case-card.readonly");
  check("公开样例有只读标识", sampleCard.textContent.includes("公开样例"), sampleCard.textContent.slice(0, 120));
  check(
    "公开样例无保存/删除按钮",
    !buttonByText(sampleCard, "保存") && !buttonByText(sampleCard, "删除"),
    [...sampleCard.querySelectorAll("button")].map((b) => b.textContent).join("|")
  );
  const hiddenCard = [...document.querySelectorAll(".case-card")].find(
    (c) => !c.classList.contains("readonly") && !c.classList.contains("create")
  );
  check(
    "隐藏用例可编辑（有保存与删除）",
    !!buttonByText(hiddenCard, "保存") && !!buttonByText(hiddenCard, "删除")
  );
});

await scenario("R11 记录不存在时的处理", async () => {
  const created = await api("POST", "/api/admin/problems", {
    token: ctx.adminToken,
    body: { title: "R11 已删除", difficulty: "easy", tags: ["r11"] },
  });
  const pid = created.data.id;
  await api("DELETE", "/api/admin/problems/" + pid, { token: ctx.adminToken });
  await goto(`/admin/problems/${pid}/edit`);
  await waitFor(() => q("#app .alert-error"), { label: "edit 404" });
  check("编辑已删除题目提示不存在", /不存在|已被删除/.test(bodyText()), bodyText().slice(0, 200));
  await goto(`/admin/problems/${pid}/testcases`);
  await waitFor(() => q("#app .alert-error"), { label: "testcase 404" });
  check("用例页对已删除题目提示不存在", /不存在|已被删除/.test(bodyText()), bodyText().slice(0, 200));
});

await scenario("S18 权限在使用中被撤销", async () => {
  // 新建一个无强制改密标记的用户并提升为管理员，作为第二个管理员。
  const reg2 = await api("POST", "/api/register", {
    body: { nickname: "m25admin2", password: "SecondAdmin123" },
  });
  check("注册第二名管理员候选", reg2.status === 201, JSON.stringify(reg2.data));
  const account2 = reg2.data.account;
  const lg2 = await api("POST", "/api/login", {
    body: { account: account2, password: "SecondAdmin123" },
  });
  check("候选账号登录成功", lg2.status === 200, JSON.stringify(lg2.data));
  ctx.admin2Token = lg2.data.token;
  ctx.admin2Id = lg2.data.user.id;
  const promote = await api("PUT", "/api/admin/users", {
    token: ctx.adminToken,
    body: { action: "change_role", user_id: ctx.admin2Id, role: "admin" },
  });
  check("提升为第二名管理员", promote.status === 200, JSON.stringify(promote.data));

  const demote = await api("PUT", "/api/admin/users", {
    token: ctx.admin2Token,
    body: { action: "change_role", user_id: ctx.adminUser.id, role: "user" },
  });
  check("原管理员被其他管理员降级", demote.status === 200, JSON.stringify(demote.data));

  // DOM 缓存的仍是旧 admin 身份，进入受限页面时应收到 403 并退出后台。
  setDomAuth(ctx.adminToken, ctx.adminUser);
  await goto("/admin/users");
  await waitFor(() => hash() === "#/problems", { label: "revoked redirect" });
  check("权限被撤销后退出后台", hash() === "#/problems", hash());
  check("权限被撤销后无后台入口", !q("#site-nav").textContent.includes("管理后台"));

  // 恢复：用 admin2 重新提升原管理员，并把临时管理员降回普通用户。
  const restore = await api("PUT", "/api/admin/users", {
    token: ctx.admin2Token,
    body: { action: "change_role", user_id: ctx.adminUser.id, role: "admin" },
  });
  check("恢复原管理员角色", restore.status === 200, JSON.stringify(restore.data));
  const back = await api("PUT", "/api/admin/users", {
    token: ctx.adminToken,
    body: { action: "change_role", user_id: ctx.admin2Id, role: "user" },
  });
  check("恢复单人管理员状态", back.status === 200, JSON.stringify(back.data));
});

await scenario("S19 最后一个管理员保护", async () => {
  // 此时 admin 是唯一管理员，自我降级应被后端拒绝。
  const me = await api("GET", "/api/me", { token: ctx.adminToken });
  check("当前仅一名管理员", me.data.role === "admin", JSON.stringify(me.data));
  setDomAuth(ctx.adminToken, me.data);
  await goto("/admin/users");
  await waitFor(() => q("#app table.data"), { label: "users table" });
  const row = [...document.querySelectorAll("#app table.data tbody tr")].find((tr) =>
    tr.textContent.includes("admin") && tr.textContent.includes("管理员")
  );
  buttonByText(row, "降为普通用户").click();
  await clickModalConfirm();
  await waitFor(() => modalTitle().includes("无法修改角色"), { label: "last admin modal" });
  check("最后一个管理员降级被拒并提示", modalTitle().includes("无法修改角色"), modalTitle());
  await clickModalConfirm();
  await waitModalGone();
  const still = await api("GET", "/api/me", { token: ctx.adminToken });
  check("降级失败后角色不变", still.data.role === "admin", JSON.stringify(still.data));
});

await scenario("S20 重复提交与写入失败", async () => {
  // 连续提交只应创建一条记录
  await goto("/admin/problems/new");
  await waitFor(() => el("admin-problem-title"), { label: "form" });
  const uniq = "M25 重复提交 " + Date.now();
  el("admin-problem-title").value = uniq;
  el("admin-problem-difficulty").value = "easy";
  const form = q("form.admin-form");
  fireSubmit(form);
  fireSubmit(form);
  await waitFor(() => hash() === "#/admin/problems", { label: "created" });
  await sleep(300);
  const listed = await api("GET", "/api/problems?q=M25%20%E9%87%8D%E5%A4%8D%E6%8F%90%E4%BA%A4&visible=all", {
    token: ctx.adminToken,
  });
  const matches = (listed.data.problems || []).filter((p) => p.title.startsWith("M25 重复提交"));
  check("连续点击只创建一条记录", matches.length === 1, JSON.stringify(matches.map((p) => p.id)));

  // 写入网络失败：不假成功、不跳转、保留输入
  await goto("/admin/problems/new");
  await waitFor(() => el("admin-problem-title"), { label: "form2" });
  el("admin-problem-title").value = "M25 网络失败题目";
  el("admin-problem-difficulty").value = "easy";
  const orig = globalThis.fetch;
  globalThis.fetch = () => Promise.reject(new TypeError("network down"));
  window.fetch = globalThis.fetch;
  fireSubmit(q("form.admin-form"));
  await waitFor(() => q("#app .alert-error"), { label: "write error" });
  const txt = bodyText();
  check("写入失败不假成功", /网络|无法确认/.test(txt), txt.slice(0, 200));
  check("写入失败停留在表单页", hash() === "#/admin/problems/new", hash());
  check(
    "写入失败保留输入",
    el("admin-problem-title").value === "M25 网络失败题目",
    el("admin-problem-title").value
  );
  globalThis.fetch = orig;
  window.fetch = orig;
});

await scenario("S22 重判入口与结果反馈", async () => {
  setDomAuth(ctx.adminToken, ctx.adminUser);
  // 准备一次 AC 提交（沿用种子题 A+B）。
  const list = await api("GET", "/api/problems?q=A%2BB&visible=all", { token: ctx.adminToken });
  const seed = (list.data.problems || [])[0];
  const code = "#include <iostream>\nint main(){long long a,b;if(std::cin>>a>>b)std::cout<<(a+b)<<std::endl;return 0;}";
  const sub = await api("POST", `/api/problems/${seed.id}/submit`, {
    token: ctx.adminToken,
    body: { language: "cpp17", code },
  });
  check("重判页准备提交成功", sub.status === 200, JSON.stringify(sub.data && sub.data.status));
  const sid = sub.data && sub.data.id;

  // 路由与入口可访问。
  await goto("/admin/rejudge");
  await waitFor(() => el("rejudge-id"), { label: "rejudge form" });
  check("重判页表单可渲染", !!el("rejudge-id"));
  check("子导航含重判入口", bodyText().includes("重判"), bodyText().slice(0, 200));

  // 非法 ID 前端校验。
  el("rejudge-id").value = "abc";
  fireSubmit(q("form.admin-form"));
  await waitFor(() => q("#app .alert-error"), { label: "invalid id" });
  check("非法 ID 前端提示", bodyText().includes("正整数"), bodyText().slice(0, 200));

  // 合法 ID：确认框说明后发起，完成后展示结果。
  el("rejudge-id").value = String(sid);
  fireSubmit(q("form.admin-form"));
  await waitFor(() => modalTitle().includes("确认重判"), { label: "confirm modal" });
  check("重判前有确认与说明", bodyText().includes("不增加提交次数") || modalTitle().includes("确认重判"),
        modalTitle());
  await clickModalConfirm();
  await waitFor(() => q("#app .alert-success"), { label: "rejudge done", timeout: 30000 });
  check("重判完成提示", bodyText().includes("重判完成"), bodyText().slice(0, 200));
  check("结果区域展示判题结果", bodyText().includes("判题结果"), bodyText().slice(0, 300));
  check("结果状态为 AC", bodyText().includes("AC"), bodyText().slice(0, 300));
  const btn = buttonByText(document, "确认重判");
  check("完成后按钮恢复", btn && !btn.disabled, btn ? String(btn.disabled) : "no button");

  // 不存在的提交：显示记录不存在。
  el("rejudge-id").value = "999999999";
  fireSubmit(q("form.admin-form"));
  await clickModalConfirm();
  await waitFor(() => q("#app .alert-error"), { label: "not found" });
  check("不存在提交显示错误", /不存在|404/.test(bodyText()), bodyText().slice(0, 200));

  // 网络失败：显示无法确认，不假成功。
  const orig = globalThis.fetch;
  globalThis.fetch = () => Promise.reject(new TypeError("network down"));
  window.fetch = globalThis.fetch;
  el("rejudge-id").value = String(sid);
  fireSubmit(q("form.admin-form"));
  await clickModalConfirm();
  await waitFor(() => q("#app .alert-error"), { label: "network error" });
  check("网络失败提示无法确认", /网络|无法确认/.test(bodyText()), bodyText().slice(0, 200));
  check("网络失败未显示成功", !q("#app .alert-success"), "unexpected success");
  globalThis.fetch = orig;
  window.fetch = orig;
});

await scenario("S21 自我降级成功后退出后台", async () => {
  // 先把普通用户提升为管理员，使自我降级可行。
  await api("PUT", "/api/admin/users", {
    token: ctx.adminToken,
    body: { action: "change_role", user_id: ctx.userId, role: "admin" },
  });
  await goto("/admin/users");
  await waitFor(() => q("#app table.data"), { label: "users table" });
  const row = [...document.querySelectorAll("#app table.data tbody tr")].find((tr) =>
    tr.textContent.includes("admin") && tr.textContent.includes("管理员")
  );
  buttonByText(row, "降为普通用户").click();
  await clickModalConfirm();
  await waitFor(() => hash() === "#/problems", { label: "left admin" });
  check("自我降级后退出后台", hash() === "#/problems", hash());
  check("后台入口消失", !q("#site-nav").textContent.includes("管理后台"));
  check("页脚入口消失", !q("#footer-admin a"));
});

// ---------------------------------------------------------------------------
// summary
// ---------------------------------------------------------------------------
check("无浏览器脚本错误 / 未处理拒绝", consoleErrors.length === 0, consoleErrors.join(" || "));
const passed = results.filter((r) => r.ok).length;
const failed = results.filter((r) => !r.ok);
console.log("\n========================================");
console.log(`DOM 验证：${passed}/${results.length} 项通过，${failed.length} 项失败`);
if (failed.length) {
  console.log("失败项：");
  for (const f of failed) console.log(" - " + f.name + (f.detail ? " | " + f.detail : ""));
}
console.log("========================================");
process.exit(failed.length ? 1 : 0);
