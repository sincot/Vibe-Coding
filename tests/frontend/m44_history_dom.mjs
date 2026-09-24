// M4.4 提交历史与详情页面 DOM 级验证（jsdom 执行真实 web/js 模块，连接真实服务）。
//
// 覆盖：
//   - 导航：登录后显示「提交历史」，游客不显示。
//   - 历史页：列表渲染（提交 ID/题目/语言/状态/耗时/内存/时间）、分页、题目筛选、
//     空结果提示。
//   - 详情页：只读源码（无提交入口）、复用判题结果组件、本人该题状态、返回链接。
//   - 错误与权限：不存在的提交、他人提交（404）、游客访问重定向登录。
//   - 退出登录清除受保护内容。
//
// 不注册 CTest；运行方式见 run_m44.sh。

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
const ACCOUNT = process.env.M44_ACCOUNT;
const PASSWORD = process.env.M44_PASSWORD;
if (!ACCOUNT || !PASSWORD) throw new Error("M44_ACCOUNT/M44_PASSWORD required");

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

const ctx = {};

async function main() {
  // 登录种子用户。
  const loginRes = await api("POST", "/api/login", {
    body: { account: ACCOUNT, password: PASSWORD },
  });
  check("种子用户登录成功", loginRes.status === 200 && !!loginRes.data);
  if (loginRes.status !== 200) return;
  ctx.token = loginRes.data.token;
  ctx.user = loginRes.data.user;
  setDomAuth(ctx.token, ctx.user);
  await sleep(30);

  // 读取一条提交与两个题目的 ID，供导航断言。
  const hist = await api("GET", "/api/submissions?mine", { token: ctx.token });
  ctx.history = hist.data;
  if (
    !hist.data ||
    !Array.isArray(hist.data.submissions) ||
    hist.data.submissions.length === 0
  ) {
    check("历史接口返回预置数据", false, "submissions 为空");
    return;
  }
  ctx.submissionId = hist.data.submissions[0].id;
  ctx.problemId = hist.data.submissions[0].problem_id;
  const problems = await api("GET", "/api/problems");
  const other = problems.data.problems.find((p) => p.id !== ctx.problemId);
  ctx.otherProblemId = other ? other.id : ctx.problemId + 999;

  await scenario("F1 导航（登录显示提交历史）", async () => {
    const navText = (document.getElementById("site-nav") || {}).textContent || "";
    check("导航含「提交历史」链接", navText.includes("提交历史"));
  });

  await scenario("F2 历史列表渲染与分页", async () => {
    await goto("/submissions");
    await waitFor(
      () => qa("#app table.submissions-table tbody tr").length > 0,
      { label: "history rows" }
    );
    const header = qa("#app table.submissions-table thead th").map((th) =>
      th.textContent.trim()
    );
    for (const col of ["提交 ID", "题目", "语言", "状态", "耗时", "内存", "提交时间"]) {
      check("表头包含「" + col + "」", header.includes(col));
    }
    check(
      "第 1 页渲染 20 行",
      qa("#app table.submissions-table tbody tr").length === 20
    );
    const total = ctx.history.total;
    check(
      "展示总数「共 " + total + " 条提交」",
      bodyText().includes("共 " + total + " 条提交")
    );
    check("存在分页控件", !!q(".pagination"));
    check(
      "提交 ID 链接指向详情",
      !!q('a[href^="#/submissions/"]')
    );

    // 翻到第 2 页。
    const page2 = qa(".pagination-page").find(
      (b) => b.textContent.trim() === "2"
    );
    check("存在第 2 页按钮", !!page2);
    if (page2) {
      const expected = total - 20;
      page2.click();
      await waitFor(() => hash().includes("page=2"), { label: "page=2 hash" });
      // 等待重新渲染完成：行数变为第 2 页应有数量（避免误读上一页残留 DOM）。
      await waitFor(
        () => qa("#app table.submissions-table tbody tr").length === expected,
        { label: "page2 rows=" + expected }
      );
      check(
        "第 2 页渲染 " + expected + " 行",
        qa("#app table.submissions-table tbody tr").length === expected
      );
    }
  });

  await scenario("F3 题目筛选与空结果", async () => {
    await goto("/submissions?problem_id=" + ctx.otherProblemId);
    await waitFor(
      () => bodyText().includes("暂无本人提交记录") || bodyText().includes("仅显示题目"),
      { label: "filtered state" }
    );
    check(
      "筛选无结果时提示「暂无本人提交记录」",
      bodyText().includes("暂无本人提交记录")
    );
  });

  await scenario("F4 详情渲染（只读源码、无提交入口、复用结果组件）", async () => {
    await goto("/submissions/" + ctx.submissionId);
    await waitFor(() => !!q("textarea.source-readonly"), { label: "source view" });
    const src = q("textarea.source-readonly");
    check("存在只读源码文本区域", !!src && src.readOnly === true);
    check(
      "源码内容非空且为纯文本",
      src && src.value.length > 0 && src.value.includes("main")
    );
    check(
      "详情页不含提交按钮（浏览历史不会误提交）",
      !qa("button").some((b) => b.textContent.includes("提交判题"))
    );
    check("复用判题结果组件（判题结果标题）", bodyText().includes("判题结果"));
    check(
      "结果头部含提交编号",
      !!q(".result-header") && q(".result-header").textContent.includes("#" + ctx.submissionId)
    );
    check("显示本人该题状态", bodyText().includes("本人该题状态"));
    check("存在返回提交历史链接", bodyText().includes("返回提交历史"));
  });

  await scenario("F5 不存在的提交", async () => {
    await goto("/submissions/999999");
    await waitFor(
      () => bodyText().includes("不存在") || bodyText().includes("没有权限"),
      { label: "not found" }
    );
    check(
      "提示不存在或无权访问",
      bodyText().includes("不存在") || bodyText().includes("没有权限")
    );
  });

  await scenario("F6 他人提交不可见（404）", async () => {
    const nick = "m44dom_other_" + Math.floor(Math.random() * 100000);
    const reg = await api("POST", "/api/register", {
      body: { nickname: nick, password: "OtherPass123" },
    });
    check("注册他人账号", reg.status === 201);
    if (reg.status === 201) {
      const otherLogin = await api("POST", "/api/login", {
        body: { account: reg.data.account, password: "OtherPass123" },
      });
      setDomAuth(otherLogin.data.token, otherLogin.data.user);
      await goto("/submissions/" + ctx.submissionId);
      await waitFor(
        () => bodyText().includes("不存在") || bodyText().includes("没有权限"),
        { label: "forbidden detail" }
      );
      check(
        "他人查看提交详情显示不存在/无权限",
        bodyText().includes("不存在") || bodyText().includes("没有权限")
      );
    }
  });

  await scenario("F7 游客访问重定向登录", async () => {
    clearDomAuth();
    nav.renderNav();
    const navText = (document.getElementById("site-nav") || {}).textContent || "";
    check("游客导航不显示「提交历史」", !navText.includes("提交历史"));
    await goto("/submissions");
    await waitFor(() => hash().includes("/login"), { label: "redirect login" });
    check("游客访问历史被重定向到登录", hash().includes("/login"));
    check("登录重定向携带 redirect", hash().includes("redirect"));
  });

  await scenario("F8 退出登录清除受保护内容", async () => {
    setDomAuth(ctx.token, ctx.user);
    await goto("/submissions");
    await waitFor(
      () => qa("#app table.submissions-table tbody tr").length > 0,
      { label: "history before logout" }
    );
    clearDomAuth();
    await sleep(50);
    check(
      "退出后历史列表内容被清除",
      qa("#app table.submissions-table tbody tr").length === 0
    );
  });

  await scenario("F9 无脚本错误", async () => {
    check("无未处理的页面脚本错误", consoleErrors.length === 0, consoleErrors.join(" | "));
  });
}

await main();

const failed = results.filter((r) => !r.ok).length;
console.log(
  "\n结果：" + (results.length - failed) + "/" + results.length + " 通过，失败 " + failed
);
process.exit(failed === 0 ? 0 : 1);
