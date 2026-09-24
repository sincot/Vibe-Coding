// M4.1 页面基础设施 DOM 级验证（jsdom 执行真实 web/js 模块，连接真实服务）。
//
// 覆盖：默认/未知/非法参数路由、游客访问后台重定向并携带目标、登录返回原目标、
// 越权目标回退、首次强制改密保留原目标、退出清理、导航按状态渲染、401 清理跳转、
// 快速切换页面丢弃过期响应、重复进入不重复请求。
//
// 仅用于开发验证，不属于项目运行依赖，不注册 CTest；运行方式见 run_m41.sh。
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
    throw new Error(`无法解析 jsdom。请先安装：npm install --prefix ${jsdomBase} jsdom`);
  }
}
const { JSDOM, VirtualConsole } = jsdomModule;

const consoleErrors = [];
const virtualConsole = new VirtualConsole();
virtualConsole.on("jsdomError", (e) => consoleErrors.push(String((e && (e.stack || e.message)) || e)));
virtualConsole.on("error", (...args) => consoleErrors.push("console.error: " + args.map(String).join(" ")));
process.on("unhandledRejection", (r) => consoleErrors.push("unhandledRejection: " + String((r && r.stack) || r)));

const BASE = process.env.BASE;
if (!BASE) throw new Error("BASE env required");
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const realFetch = globalThis.fetch;

const html = fs.readFileSync(path.join(ROOT, "web/index.html"), "utf8");
const dom = new JSDOM(html, { url: BASE + "/", runScripts: "outside-only", pretendToBeVisual: true, virtualConsole });
const { window } = dom;

globalThis.window = window;
globalThis.document = window.document;
globalThis.location = window.location;
globalThis.history = window.history;
globalThis.localStorage = window.localStorage;
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
const storageUrl = pathToFileURL(path.join(ROOT, "web/js/storage.js")).href;

await import(mainUrl);
const router = await import(routerUrl);
const auth = await import(authUrl);
const nav = await import(navUrl);
const storage = await import(storageUrl);
await new Promise((r) => setTimeout(r, 150));

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
      if (Date.now() - start > timeout) return reject(new Error("waitFor timeout: " + label));
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
async function loginViaUI(account, password) {
  await waitFor(() => el("login-account"), { label: "login form" });
  el("login-account").value = account;
  el("login-password").value = password;
  fireSubmit(q("#app form"));
}
async function changePasswordViaUI(oldPwd, newPwd) {
  await waitFor(() => el("pwd-old"), { label: "password form" });
  el("pwd-old").value = oldPwd;
  el("pwd-new").value = newPwd;
  el("pwd-confirm").value = newPwd;
  fireSubmit(q("#app form"));
}

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

// ===========================================================================
await scenario("M41-1 未知路由 → 页面不存在", async () => {
  clearDomAuth();
  await goto("/does-not-exist");
  await waitFor(() => /页面不存在/.test(bodyText()), { label: "not found" });
  check("未知路由显示页面不存在", /页面不存在/.test(bodyText()), bodyText().slice(0, 120));
  check("未触发脚本错误", consoleErrors.length === 0, consoleErrors.join(" || "));
});

// ===========================================================================
await scenario("M41-2 非法路由参数 → 地址参数无效", async () => {
  clearDomAuth();
  location.hash = "#/problems/%E0%A4%A";
  await waitFor(() => /地址参数无效/.test(bodyText()), { label: "invalid param" });
  check("非法编码显示地址参数无效", /地址参数无效/.test(bodyText()), bodyText().slice(0, 120));
});

// ===========================================================================
await scenario("M41-3 游客访问后台 → 登录页并携带目标", async () => {
  clearDomAuth();
  await goto("/problems");
  await goto("/admin");
  await waitFor(() => hash().startsWith("#/login"), { label: "redirect login" });
  check("游客被引导到登录页", hash().startsWith("#/login"), hash());
  check("重定向携带原目标", /redirect=%2Fadmin/.test(hash()), hash());
  check("不显示后台内容", !/后台管理/.test(bodyText()), bodyText().slice(0, 120));
  check("已保存待返回目标", storage.peekPendingTarget() === "/admin", storage.peekPendingTarget());
  storage.clearPendingTarget();
});

// ===========================================================================
await scenario("M41-4 普通用户越权目标 → 回退题目列表", async () => {
  clearDomAuth();
  const reg = await api("POST", "/api/register", { body: { nickname: "m41user" + Date.now(), password: "UserPass123" } });
  check("注册普通用户", reg.status === 201, JSON.stringify(reg.data));
  ctx.userAccount = reg.data.account;
  ctx.userPassword = "UserPass123";

  await goto("/problems");
  await goto("/admin");
  await waitFor(() => el("login-account"), { label: "login form" });
  await loginViaUI(ctx.userAccount, ctx.userPassword);
  await waitFor(() => hash() === "#/problems", { label: "fallback problems" });
  check("普通用户被回退到题目列表", hash() === "#/problems", hash());
  check("未进入后台", !/后台管理/.test(bodyText()), bodyText().slice(0, 150));
  check("待返回目标已清理", storage.peekPendingTarget() === "", storage.peekPendingTarget());
});

// ===========================================================================
await scenario("M41-5 首次强制改密保留原目标并返回后台", async () => {
  clearDomAuth();
  // 预置 admin 首次登录需强制改密；从其原目标 /admin 进入。
  await goto("/problems");
  await goto("/admin");
  await waitFor(() => el("login-account"), { label: "login form" });
  check("目标保留为 /admin", storage.peekPendingTarget() === "/admin", storage.peekPendingTarget());
  await loginViaUI("admin", "AdminPass123");
  await waitFor(() => hash() === "#/password", { label: "forced password" });
  check("未改密管理员被引导到改密页", hash() === "#/password", hash());
  check("改密期间仍保留原目标", storage.peekPendingTarget() === "/admin", storage.peekPendingTarget());

  await changePasswordViaUI("AdminPass123", "AdminNewPass456");
  await waitFor(() => hash() === "#/admin", { label: "back to admin" });
  check("改密成功后返回原目标 /admin", hash() === "#/admin", hash());
  check("改密后进入后台", /后台管理/.test(bodyText()), bodyText().slice(0, 120));
  ctx.adminPassword = "AdminNewPass456";
  const tg = await api("POST", "/api/login", { body: { account: "admin", password: ctx.adminPassword } });
  ctx.adminToken = tg.data.token;
  ctx.adminUser = tg.data.user;
  check("改密后 reset_pwd_flag 清除", ctx.adminUser.reset_pwd_flag === 0, JSON.stringify(ctx.adminUser));
});

// ===========================================================================
await scenario("M41-6 非改密管理员登录返回含参数目标", async () => {
  clearDomAuth();
  await goto("/problems");
  await goto("/admin/problems/new");
  await waitFor(() => el("login-account"), { label: "login form" });
  await loginViaUI("admin", ctx.adminPassword);
  await waitFor(() => hash() === "#/admin/problems/new", { label: "return target" });
  check("登录后回到含参数原目标", hash() === "#/admin/problems/new", hash());
  await waitFor(() => el("admin-problem-title"), { label: "new problem form" });
  check("目标页面正确渲染", !!el("admin-problem-title"));
});

// ===========================================================================
await scenario("M41-7 导航按认证状态渲染且不提供未实现入口", async () => {
  clearDomAuth();
  nav.renderNav();
  check("游客显示登录入口", /登录/.test(q("#site-nav").textContent));
  check("游客显示注册入口", /注册/.test(q("#site-nav").textContent));
  check("游客无后台入口", !/管理后台/.test(q("#site-nav").textContent));

  setDomAuth(ctx.adminToken, ctx.adminUser);
  const navText = q("#site-nav").textContent;
  check("登录后显示昵称", navText.includes(ctx.adminUser.nickname), navText);
  check("管理员显示后台入口", /管理后台/.test(navText), navText);
  check("不提供排行榜空白入口", !/排行榜/.test(navText), navText);
  check("不提供提交历史空白入口", !/提交历史/.test(navText), navText);
  clearDomAuth();
});

// ===========================================================================
await scenario("M41-8 退出登录清理凭证与待返回目标", async () => {
  setDomAuth(ctx.adminToken, ctx.adminUser);
  storage.savePendingTarget("/admin");
  await goto("/problems");
  const logoutBtn = [...document.querySelectorAll("#site-nav button")].find((b) => /退出登录/.test(b.textContent));
  check("存在退出按钮", !!logoutBtn);
  logoutBtn.click();
  await sleep(50);
  check("退出后跳转登录页", hash().startsWith("#/login"), hash());
  check("退出后 token 被清理", auth.getToken() === "", auth.getToken());
  check("退出后待返回目标被清理", storage.peekPendingTarget() === "", storage.peekPendingTarget());
  check("退出后无后台入口", !/管理后台/.test(q("#site-nav").textContent));
});

// ===========================================================================
await scenario("M41-9 受保护请求 401 → 清理并跳登录", async () => {
  setDomAuth("bogus.invalid.token", { id: 1, role: "admin", reset_pwd_flag: 0, nickname: "x" });
  await goto("/admin/users");
  await waitFor(() => hash().startsWith("#/login"), { label: "401 login" });
  check("无效 token 触发重新登录", hash().startsWith("#/login"), hash());
  check("失效凭证被清理", auth.getToken() === "", auth.getToken());
});

// ===========================================================================
await scenario("M41-10 快速切换丢弃过期读取响应", async () => {
  clearDomAuth();
  const orig = globalThis.fetch;
  globalThis.fetch = (input, init) => {
    const url = typeof input === "string" ? input : input.url;
    if (url.includes("/api/problems") && (!init || !init.method || init.method === "GET")) {
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => resolve(orig(input, init)), 250);
        if (init && init.signal) {
          init.signal.addEventListener("abort", () => {
            clearTimeout(timer);
            const err = new Error("aborted");
            err.name = "AbortError";
            reject(err);
          });
        }
      });
    }
    return orig(input, init);
  };
  window.fetch = globalThis.fetch;

  await goto("/problems"); // 开始加载但被延迟
  await goto("/login"); // 立即切走
  await waitFor(() => el("login-account"), { label: "login form after switch" });
  await sleep(400); // 等旧响应本应到达
  check("切走后仍停留在登录页", hash().startsWith("#/login"), hash());
  check("旧响应未覆盖当前页面", !!el("login-account") && !q("#app table.data"), "stale overwrite");

  globalThis.fetch = orig;
  window.fetch = orig;
});

// ===========================================================================
await scenario("M41-11 反复进入不重复触发请求", async () => {
  clearDomAuth();
  const orig = globalThis.fetch;
  let count = 0;
  globalThis.fetch = (input, init) => {
    const url = typeof input === "string" ? input : input.url;
    const method = (init && init.method) || "GET";
    if (method === "GET" && /\/api\/problems$/.test(url)) count += 1;
    return orig(input, init);
  };
  window.fetch = globalThis.fetch;

  await goto("/login");
  await waitFor(() => el("login-account"), { label: "login" });
  await goto("/problems");
  await waitFor(() => q("#app table.data"), { label: "problems 1" });
  await goto("/login");
  await waitFor(() => el("login-account"), { label: "login 2" });
  await goto("/problems");
  await waitFor(() => q("#app table.data"), { label: "problems 2" });
  check("每次进入列表仅一次请求", count === 2, "count=" + count);

  globalThis.fetch = orig;
  window.fetch = orig;
});

// ===========================================================================
await scenario("M41-12 快速连续切页只渲染最终页", async () => {
  clearDomAuth();
  // 不等待中间渲染，连续切换多个路由，最后停在题目列表。
  router.navigate("/login");
  router.navigate("/register");
  router.navigate("/problems/999999");
  router.navigate("/problems");
  await waitFor(() => q("#app table.data"), { label: "final problems table" });
  await sleep(80);
  check("最终呈现题目列表", !!q("#app table.data"), "no table");
  check("未残留登录表单", !el("login-account"));
  check("未残留注册表单", !el("reg-nickname"));
  check("哈希为题目列表", hash() === "#/problems", hash());
});

// ===========================================================================
await scenario("M41-13 退出后晚返回的请求不恢复页面", async () => {
  setDomAuth(ctx.adminToken, ctx.adminUser);
  const orig = globalThis.fetch;
  let releaseUsers = null;
  globalThis.fetch = (input, init) => {
    const url = typeof input === "string" ? input : input.url;
    if (url.includes("/api/admin/users")) {
      return new Promise((resolve, reject) => {
        // 用 then 的两种回调接管底层请求，避免请求被 abort 后又 resolve 其
        // 已拒绝的 Promise 而产生未处理拒绝。
        releaseUsers = () => {
          orig(input, init).then(
            (res) => resolve(res),
            (err) => reject(err)
          );
        };
        if (init && init.signal) {
          init.signal.addEventListener("abort", () => {
            const err = new Error("aborted");
            err.name = "AbortError";
            reject(err);
          });
        }
      });
    }
    return orig(input, init);
  };
  window.fetch = globalThis.fetch;

  await goto("/admin/users");
  await sleep(60); // 让请求进入等待
  const logoutBtn = [...document.querySelectorAll("#site-nav button")].find((b) => /退出登录/.test(b.textContent));
  check("存在退出按钮", !!logoutBtn);
  logoutBtn.click();
  await sleep(50);
  if (releaseUsers) releaseUsers(); // 退出后放行晚到响应
  await sleep(250);
  check("退出后仍停留登录页", hash().startsWith("#/login"), hash());
  check("晚返回的用户列表未渲染", !q("#app table.data"), "stale users rendered");
  check("退出后未恢复身份", auth.getToken() === "" && !auth.isLoggedIn(), auth.getToken());

  globalThis.fetch = orig;
  window.fetch = orig;
});

// ---------------------------------------------------------------------------
check("无浏览器脚本错误 / 未处理拒绝", consoleErrors.length === 0, consoleErrors.join(" || "));
const passed = results.filter((r) => r.ok).length;
const failed = results.filter((r) => !r.ok);
console.log("\n========================================");
console.log(`M4.1 DOM 验证：${passed}/${results.length} 项通过，${failed.length} 项失败`);
if (failed.length) {
  console.log("失败项：");
  for (const f of failed) console.log(" - " + f.name + (f.detail ? " | " + f.detail : ""));
}
console.log("========================================");
process.exit(failed.length ? 1 : 0);
