// M4.5 排行榜页面 DOM 级验证（jsdom 执行真实 web/js 模块，连接真实服务）。
//
// 覆盖：
//   - 导航：仅登录用户显示「排行榜」；游客不可见。
//   - 排行榜页：需登录；渲染名次/昵称/AC 数/提交次数/首次 AC 时间；无 AC 显示「—」；
//     分页到第 2 页；「刷新」按钮重新获取。
//   - 当前用户高亮：登录后匹配 user_id 的行有 current-user；退出后移除。
//   - 游客访问排行榜被重定向到登录；无脚本错误。
//
// 不注册 CTest；运行方式见 run_m45.sh。

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL, fileURLToPath } from "node:url";
import { createRequire } from "node:module";

const jsdomBase = process.env.JSDOM_DIR || "/tmp/opencode/domtest";
let jsdomModule;
try {
  jsdomModule = createRequire(import.meta.url)("jsdom");
} catch (e1) {
  jsdomModule = createRequire(path.join(jsdomBase, "package.json"))("jsdom");
}
const { JSDOM, VirtualConsole } = jsdomModule;

const consoleErrors = [];
const virtualConsole = new VirtualConsole();
virtualConsole.on("jsdomError", (e) =>
  consoleErrors.push(String((e && (e.stack || e.message)) || e))
);
virtualConsole.on("error", (...args) =>
  consoleErrors.push("console.error: " + args.map(String).join(" "))
);
process.on("unhandledRejection", (r) =>
  consoleErrors.push("unhandledRejection: " + String((r && r.stack) || r))
);

const BASE = process.env.BASE;
if (!BASE) throw new Error("BASE env required");
const ACCOUNT = process.env.M45_ACCOUNT;
const PASSWORD = process.env.M45_PASSWORD;
if (!ACCOUNT || !PASSWORD) throw new Error("M45_ACCOUNT/M45_PASSWORD required");

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
await new Promise((r) => setTimeout(r, 150));

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function waitFor(fn, { timeout = 15000, interval = 25, label = "condition" } = {}) {
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
      if (Date.now() - start > timeout)
        return reject(new Error("waitFor timeout: " + label));
      setTimeout(tick, interval);
    })();
  });
}
const q = (sel) => document.querySelector(sel);
const qa = (sel) => Array.from(document.querySelectorAll(sel));
const bodyText = () => document.body.textContent || "";
const hash = () => location.hash;

async function goto(p) {
  router.navigate(p);
  await sleep(50);
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

const rows = () => qa("#app table.leaderboard-table tbody tr");

async function main() {
  // 排行榜需登录：先验证游客被引导登录，再以登录身份浏览。
  await scenario("H1 导航（游客不显示排行榜）", async () => {
    const navText = (document.getElementById("site-nav") || {}).textContent || "";
    check("游客导航不含「排行榜」", !navText.includes("排行榜"));
  });

  let board = null;
  await scenario("H2 游客被引导登录，登录后可访问排行榜", async () => {
    await goto("/leaderboard");
    await waitFor(() => hash().startsWith("#/login"), { label: "guest redirected" });
    check("游客访问排行榜被引导到登录页", hash().startsWith("#/login"), hash());

    const loginRes = await api("POST", "/api/login", {
      body: { account: ACCOUNT, password: PASSWORD },
    });
    check("种子用户登录成功", loginRes.status === 200 && !!loginRes.data);
    if (loginRes.status !== 200) return;
    setDomAuth(loginRes.data.token, loginRes.data.user);

    await goto("/leaderboard");
    await waitFor(() => rows().length > 0, { label: "leaderboard rows" });
    check("登录后可访问排行榜（未跳登录）", !hash().includes("/login"));
    const header = qa("#app table.leaderboard-table thead th").map((th) =>
      th.textContent.trim()
    );
    for (const col of ["名次", "昵称", "AC 题目数", "总提交次数", "首次 AC 时间"]) {
      check("表头包含「" + col + "」", header.includes(col));
    }
    check("第 1 页渲染 20 行", rows().length === 20);
    const apiRes = await api("GET", "/api/leaderboard");
    board = apiRes.data;
    check("展示总数「共 " + board.total + " 名用户」",
      bodyText().includes("共 " + board.total + " 名用户"));
    check("存在分页控件", !!q(".pagination"));
    check("登录用户有当前用户高亮", qa("tr.current-user").length === 1);
    check("无 AC 用户显示「—」", rows().some((tr) => tr.textContent.includes("—")));
  });

  await scenario("H3 分页到第 2 页", async () => {
    const page2 = qa(".pagination-page").find(
      (b) => b.textContent.trim() === "2"
    );
    check("存在第 2 页按钮", !!page2);
    if (page2) {
      const expected = board.total - 20;
      page2.click();
      await waitFor(() => hash().includes("page=2"), { label: "page=2 hash" });
      await waitFor(() => rows().length === expected, {
        label: "page2 rows=" + expected,
      });
      check("第 2 页渲染 " + expected + " 行", rows().length === expected);
      const apiRes = await api("GET", "/api/leaderboard?page=2");
      const firstRank = apiRes.data.leaderboard[0].rank;
      check("第 2 页名次全局连续（首行 rank=" + firstRank + "）",
        firstRank === 21 && rows()[0].textContent.includes("21"));
    }
  });

  await scenario("H4 登录后当前用户高亮", async () => {
    const loginRes = await api("POST", "/api/login", {
      body: { account: ACCOUNT, password: PASSWORD },
    });
    check("种子用户登录成功", loginRes.status === 200 && !!loginRes.data);
    if (loginRes.status !== 200) return;
    setDomAuth(loginRes.data.token, loginRes.data.user);
    await goto("/leaderboard");
    await waitFor(() => qa("tr.current-user").length === 1, {
      label: "current user highlight",
    });
    const highlighted = q("tr.current-user");
    check("登录后恰有一行高亮", qa("tr.current-user").length === 1);
    check("高亮行昵称为当前用户",
      highlighted && highlighted.textContent.includes("m45dom"));
  });

  await scenario("H5 刷新按钮重新获取", async () => {
    const refresh = qa("button").find((b) => b.textContent.trim() === "刷新");
    check("存在「刷新」按钮", !!refresh);
    if (refresh) {
      refresh.click();
      await waitFor(() => rows().length > 0, { label: "refresh rows" });
      check("刷新后仍有排行榜数据", rows().length > 0);
    }
  });

  await scenario("H6 退出登录移除高亮", async () => {
    clearDomAuth();
    await sleep(50);
    check("退出后无当前用户高亮", qa("tr.current-user").length === 0);
    const navText = (document.getElementById("site-nav") || {}).textContent || "";
    check("退出后导航不再显示「排行榜」", !navText.includes("排行榜"));
  });

  await scenario("H7 无脚本错误", async () => {
    check("无未处理的页面脚本错误", consoleErrors.length === 0,
      consoleErrors.join(" | "));
  });
}

await main();

const failed = results.filter((r) => !r.ok).length;
console.log(
  "\n结果：" + (results.length - failed) + "/" + results.length + " 通过，失败 " + failed
);
process.exit(failed === 0 ? 0 : 1);
