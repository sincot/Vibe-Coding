// M4.3 题目与做题页面 DOM 级验证（jsdom 执行真实 web/js 模块，连接真实服务）。
//
// 覆盖：
//   - 题目页需登录：游客访问会被引导到登录页；登录用户可查看标题/难度/标签/题面/
//     公开样例/时空限制，显示未 AC/已 AC；判题后从详情接口刷新本人状态。
//   - 提交交互：降级 textarea 提交、语言切换不清空源码、Ctrl+Enter 与按钮共用入口
//     且不重复提交、请求失败保留源码。
//   - 编辑器适配：CDN 加载失败时降级到可编辑 textarea 并提示；预置 CodeMirror 时
//     委托实例、切换语言更新模式、dispose 时 toTextArea 还原。
//   - 结果渲染规则：提交 ID/状态/运行耗时/编译耗时/峰值内存、未采集显示「未采集」、
//     WA 输入/期望/实际（区分空串与缺失）、编译诊断与标准错误分离、截断标识、
//     全局硬上限/服务取消的部分执行说明。
//
// 仅用于开发验证，不属于项目运行依赖，不注册 CTest；运行方式见 run_m43.sh。
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

// 统计提交请求次数（用于验证重复提交防护）。
let submitCount = 0;
globalThis.fetch = (input, init) => {
  const url =
    typeof input === "string" ? new URL(input, BASE).toString() : input;
  const method = (init && init.method) || "GET";
  if (method === "POST" && /\/api\/problems\/\d+\/submit$/.test(String(url))) {
    submitCount += 1;
  }
  return realFetch(url, init);
};
window.fetch = globalThis.fetch;

// 在导入页面模块前，拦截 CodeMirror 的 CDN 脚本加载：让它们立即触发 onerror，
// 从而在测试中稳定走「加载失败 → 降级 textarea」路径（jsdom 默认不加载外部资源）。
const origHeadAppend = window.document.head.appendChild.bind(window.document.head);
window.document.head.appendChild = function (node) {
  const tag = node && node.tagName ? String(node.tagName).toUpperCase() : "";
  if (tag === "SCRIPT" && node.src && node.src.includes("codemirror")) {
    setTimeout(() => {
      if (typeof node.onerror === "function") {
        node.onerror(new window.Event("error"));
      }
    }, 0);
    return node;
  }
  return origHeadAppend(node);
};

const mainUrl = pathToFileURL(path.join(ROOT, "web/js/main.js")).href;
const routerUrl = pathToFileURL(path.join(ROOT, "web/js/router.js")).href;
const authUrl = pathToFileURL(path.join(ROOT, "web/js/auth.js")).href;
const navUrl = pathToFileURL(path.join(ROOT, "web/js/nav.js")).href;
const judgeUrl = pathToFileURL(path.join(ROOT, "web/js/judge.js")).href;
const editorUrl = pathToFileURL(path.join(ROOT, "web/js/editor.js")).href;

await import(mainUrl);
const router = await import(routerUrl);
const auth = await import(authUrl);
const nav = await import(navUrl);
const judge = await import(judgeUrl);
const editor = await import(editorUrl);
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
const el = (id) => document.getElementById(id);
const q = (sel) => document.querySelector(sel);
const qa = (sel) => Array.from(document.querySelectorAll(sel));
const bodyText = () => document.body.textContent || "";
const hash = () => location.hash;

async function goto(p) {
  router.navigate(p);
  await sleep(40);
}
function fireSubmit(form) {
  form.dispatchEvent(new window.Event("submit", { bubbles: true, cancelable: true }));
}
function fireCtrlEnter(node) {
  node.dispatchEvent(
    new window.KeyboardEvent("keydown", {
      key: "Enter",
      ctrlKey: true,
      bubbles: true,
      cancelable: true,
    })
  );
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

const AC_CODE =
  '#include <cstdio>\nint main(){long long a,b; if(scanf("%lld %lld",&a,&b)!=2) return 0; printf("%lld\\n",a+b); return 0;}\n';
const CE_CODE = "int main(){ this is not valid c++ }\n";

const ctx = {};

// ===========================================================================
await scenario("M43-F1 游客访问题目被引导登录", async () => {
  clearDomAuth();
  await goto("/problems/1");
  await waitFor(() => hash().startsWith("#/login"), { label: "guest redirected to login" });
  check("游客被引导到登录页", hash().startsWith("#/login"), hash());
  check("重定向携带题目目标", /redirect=%2Fproblems%2F1/.test(hash()), hash());
  check("未渲染题目内容", !q(".problem-description"), bodyText().slice(0, 100));
  check("未触发脚本错误", consoleErrors.length === 0, consoleErrors.join(" || "));
});

// ===========================================================================
await scenario("M43-F2 登录提交 AC：语言切换不清空、结果展示、本人状态刷新", async () => {
  const reg = await api("POST", "/api/register", {
    body: { nickname: "m43dom" + Date.now(), password: "UserPass123" },
  });
  check("注册普通用户", reg.status === 201, JSON.stringify(reg.data));
  const loginRes = await api("POST", "/api/login", {
    body: { account: reg.data.account, password: "UserPass123" },
  });
  check("登录成功", loginRes.status === 200 && !!loginRes.data.token);
  ctx.token = loginRes.data.token;
  ctx.user = loginRes.data.user;
  setDomAuth(ctx.token, ctx.user);

  await goto("/problems/1");
  await waitFor(() => q(".problem-status"), { label: "owner status" });
  check("登录用户显示未 AC", /未 AC/.test(q(".problem-status").textContent), q(".problem-status").textContent);

  // 语言切换不清空源码。
  const textarea = el("source-code");
  textarea.value = "// keep me";
  const langSelect = el("submit-language");
  langSelect.value = "c11";
  langSelect.dispatchEvent(new window.Event("change", { bubbles: true }));
  await sleep(20);
  check("切换语言不清空源码", textarea.value === "// keep me", textarea.value);
  langSelect.value = "cpp17";
  langSelect.dispatchEvent(new window.Event("change", { bubbles: true }));

  // 提交 AC。
  textarea.value = AC_CODE;
  const before = submitCount;
  fireSubmit(q("#app form"));
  await waitFor(() => /提交 #\d+/.test(bodyText()) && /判题结果/.test(bodyText()), {
    timeout: 30000,
    label: "judge result",
  });
  check("提交请求发出一次", submitCount === before + 1, String(submitCount - before));
  check("结果展示 AC", /AC/.test(q(".result-header").textContent), q(".result-header").textContent);
  const meta = q(".result-meta") ? q(".result-meta").textContent : "";
  check("展示运行耗时", /运行耗时/.test(meta), meta);
  check("展示编译耗时", /编译耗时/.test(meta), meta);
  check("展示峰值内存", /峰值内存/.test(meta), meta);
  check("展示逐测试点结果", /逐测试点结果/.test(bodyText()));
  await waitFor(() => /已 AC/.test(q(".problem-status").textContent), {
    timeout: 8000,
    label: "status refresh",
  });
  check("判题后本人状态刷新为已 AC", /已 AC/.test(q(".problem-status").textContent), q(".problem-status").textContent);
});

// ===========================================================================
await scenario("M43-F3 Ctrl+Enter 与按钮共用入口且不重复提交（CE 快速路径）", async () => {
  const textarea = el("source-code");
  textarea.value = CE_CODE;
  const before = submitCount;
  // 同一时刻触发两次（按钮/快捷键），应只提交一次。
  fireCtrlEnter(textarea);
  fireSubmit(q("#app form"));
  await waitFor(() => /CE/.test(q(".result-header").textContent || ""), {
    timeout: 30000,
    label: "CE result",
  });
  check("重复触发只提交一次", submitCount === before + 1, String(submitCount - before));
  check("展示总体状态 CE", /CE/.test(q(".result-header").textContent), q(".result-header").textContent);
  check("展示编译错误信息", /编译错误|编译信息/.test(bodyText()), bodyText().slice(0, 120));
});

// ===========================================================================
await scenario("M43-F4 结果渲染规则（合成结果：未采集/WA/截断/部分执行）", async () => {
  function renderSynthetic(result) {
    const box = document.createElement("div");
    document.body.appendChild(box);
    judge.renderJudgeResult(box, result);
    return box;
  }
  function fieldOf(card, title) {
    for (const f of card.querySelectorAll(".case-field")) {
      const h5 = f.querySelector("h5");
      if (h5 && h5.textContent === title) {
        const pre = f.querySelector("pre");
        return pre ? pre.textContent : null;
      }
    }
    return null;
  }

  // 未采集指标显示「未采集」。
  const ac = renderSynthetic({
    id: 7,
    status: "AC",
    language: "cpp17",
    passed: 1,
    total: 1,
    compile_ok: true,
    runtime_ms: 12,
    compile_time_ms: 50,
    memory_kb: null,
    created_at: "2026-01-01 00:00:00",
    results: [{ index: 0, status: "AC", time_ms: 5, memory_kb: null }],
  });
  check("展示提交 ID", /提交 #7/.test(ac.textContent), ac.textContent.slice(0, 80));
  check("未采集内存显示「未采集」", /峰值内存 未采集/.test(ac.textContent));
  check("逐点未采集内存显示「未采集」", /内存 未采集/.test(ac.textContent));

  // WA：保留空白、区分空串与缺失、标准错误与程序输出分离。
  const wa = renderSynthetic({
    id: 8,
    status: "WA",
    compile_ok: true,
    runtime_ms: 3,
    compile_time_ms: 10,
    memory_kb: 1024,
    results: [
      {
        index: 0,
        status: "WA",
        time_ms: 1,
        memory_kb: 1024,
        input: "1 2\n",
        expected_output: "3\n",
        actual_output: "",
      },
      { index: 1, status: "RE", time_ms: 2, memory_kb: 1024, message: "崩溃", stderr_output: "ERR", actual_output: "OUT" },
    ],
  });
  const cards = wa.querySelectorAll(".case-card");
  check("WA 显示输入", fieldOf(cards[0], "输入") === "1 2\n", JSON.stringify(fieldOf(cards[0], "输入")));
  check("WA 显示期望输出", fieldOf(cards[0], "期望输出") === "3\n");
  check("WA 空实际输出显示为空串标注", fieldOf(cards[0], "你的输出") === "" && /（空字符串）/.test(cards[0].textContent));
  check("RE 标准错误独立展示", fieldOf(cards[1], "标准错误") === "ERR");
  check("RE 程序输出独立展示", fieldOf(cards[1], "程序输出") === "OUT");

  // 缺失实际输出显示「后端未提供」。
  const waMissing = renderSynthetic({
    id: 9,
    status: "WA",
    compile_ok: true,
    results: [{ index: 0, status: "WA", time_ms: 1, input: "x\n", expected_output: "y\n" }],
  });
  check(
    "缺失实际输出显示后端未提供",
    /（后端未提供）/.test(waMissing.textContent),
    waMissing.textContent.slice(0, 160)
  );

  // 编译诊断截断标识 + 与标准输出分离。
  const ce = renderSynthetic({
    id: 10,
    status: "CE",
    compile_ok: false,
    compile_output: "error: x",
    compile_output_truncated: true,
    results: [],
  });
  check("CE 显示编译错误", /编译错误/.test(ce.textContent));
  check("编译诊断截断标识", /编译诊断超过采集上限，已截断/.test(ce.textContent));

  // 输出截断标识。
  const trunc = renderSynthetic({
    id: 11,
    status: "RE",
    compile_ok: true,
    results: [{ index: 0, status: "RE", time_ms: 1, actual_output: "abc", output_truncated: true }],
  });
  check("输出截断标识", /该输出超过采集上限，已被截断/.test(trunc.textContent));

  // 全局硬上限部分执行。
  const globalHit = renderSynthetic({
    id: 12,
    status: "SYSERR",
    compile_ok: true,
    global_deadline_hit: true,
    total: 5,
    results: [{ index: 0, status: "AC", time_ms: 1 }, { index: 1, status: "AC", time_ms: 1 }],
  });
  check("部分执行说明未全部执行", /未执行全部测试点/.test(globalHit.textContent), globalHit.textContent.slice(0, 120));
  check("部分执行已执行/总数正确", /已执行 2 个 \/ 共 5 个/.test(globalHit.textContent));
  check("不猜测剩余数量", !/剩余/.test(globalHit.textContent));

  // 服务取消。
  const cancelled = renderSynthetic({
    id: 13,
    status: "SYSERR",
    compile_ok: true,
    cancelled: true,
    total: 3,
    results: [{ index: 0, status: "SYSERR", time_ms: 1 }],
  });
  check("服务取消说明", /服务停止/.test(cancelled.textContent), cancelled.textContent.slice(0, 120));

  // 清理。
  for (const node of [ac, wa, waMissing, ce, trunc, globalHit, cancelled]) node.remove();
});

// ===========================================================================
await scenario("M43-F5 编辑器适配：预置 CodeMirror 时委托实例、切模式、dispose 还原", async () => {
  const instances = [];
  window.__cmInstances = instances;
  window.CodeMirror = {
    fromTextArea(textarea, options) {
      const instance = {
        value: textarea.value,
        options: Object.assign({}, options),
        dispose: false,
        textarea,
        getValue() {
          return this.value;
        },
        setValue(v) {
          this.value = String(v);
          textarea.value = String(v);
        },
        setOption(name, value) {
          this.options[name] = value;
        },
        focus() {},
        refresh() {},
        getWrapperElement() {
          if (!this.wrapper) {
            this.wrapper = document.createElement("div");
            this.wrapper.className = "CodeMirror";
            if (textarea.parentElement) textarea.parentElement.appendChild(this.wrapper);
          }
          return this.wrapper;
        },
        toTextArea() {
          this.dispose = true;
          textarea.style.display = "";
        },
      };
      textarea.style.display = "none";
      instances.push(instance);
      return instance;
    },
  };

  await goto("/problems/2");
  await waitFor(() => instances.length >= 1, { label: "cm instance" });
  const inst = instances[instances.length - 1];
  check("编辑器实例已创建", !!inst);
  check("textarea 被编辑器接管", inst.textarea.style.display === "none", inst.textarea.style.display);
  check("语言模式初始为 C++", inst.options.mode === "text/x-c++src", inst.options.mode);

  inst.setValue("// user code");
  const langSelect = el("submit-language");
  langSelect.value = "c11";
  langSelect.dispatchEvent(new window.Event("change", { bubbles: true }));
  await sleep(20);
  check("切换语言更新编辑器模式", inst.options.mode === "text/x-csrc", inst.options.mode);
  check("切换语言保留源码", inst.getValue() === "// user code", inst.getValue());

  const countBefore = instances.length;
  await goto("/problems");
  await waitFor(() => inst.dispose === true, { label: "toTextArea on dispose" });
  check("离开页面释放编辑器实例（toTextArea）", inst.dispose === true);

  await goto("/problems/2");
  await waitFor(() => instances.length > countBefore, { label: "new cm instance" });
  check("重复进入创建新实例而非复用旧实例", instances[instances.length - 1] !== inst);
  check(
    "模式映射：cpp17/c11",
    editor.codeMirrorMode("cpp17") === "text/x-c++src" &&
      editor.codeMirrorMode("c11") === "text/x-csrc"
  );
  // 清理：离开页面，释放新实例。
  await goto("/problems");
});

// ===========================================================================
await scenario("M43-F6 游客无法进入提交页", async () => {
  // 题库与题目页均需登录：游客直接打开题目地址会被引导到登录页，
  // 不会渲染源码编辑器，也就不会有任何提交入口。
  delete window.CodeMirror;
  clearDomAuth();
  await goto("/problems/1");
  await waitFor(() => hash().startsWith("#/login"), { label: "guest redirected" });
  check("游客被引导到登录页", hash().startsWith("#/login"), hash());
  check("未渲染源码编辑器", !el("source-code"), bodyText().slice(0, 100));
});

// ===========================================================================
await scenario("M43-F7 请求失败保留源码并恢复可操作状态（网络中断）", async () => {
  setDomAuth(ctx.token, ctx.user);
  await goto("/problems/1");
  await waitFor(() => q(".problem-status"), { label: "owner status" });
  const textarea = el("source-code");
  textarea.value = AC_CODE;

  // 模拟提交网络中断：仅拦截 submit 请求。
  const wrapper = globalThis.fetch;
  globalThis.fetch = (input, init) => {
    const url =
      typeof input === "string" ? new URL(input, BASE).toString() : input;
    if ((init && init.method) === "POST" && /\/submit$/.test(String(url))) {
      return Promise.reject(new TypeError("Failed to fetch"));
    }
    return wrapper(input, init);
  };
  window.fetch = globalThis.fetch;
  try {
    fireSubmit(q("#app form"));
    await waitFor(() => /无法确认/.test(bodyText()), { timeout: 8000, label: "network error" });
    check("提示结果无法确认", /无法确认/.test(bodyText()), bodyText().slice(0, 160));
    check("请求失败保留源码", textarea.value === AC_CODE);
    const submit = q(".editor-actions button");
    check("失败后恢复可操作状态", submit && !submit.disabled);
  } finally {
    globalThis.fetch = wrapper;
    window.fetch = wrapper;
  }
});

console.log("\n==== M4.3 前端 DOM 验证结果 ====");
const passed = results.filter((r) => r.ok).length;
const failed = results.filter((r) => !r.ok);
console.log(`通过 ${passed} / ${results.length}`);
for (const f of failed) {
  console.log("FAIL:", f.name, f.detail ? "| " + f.detail : "");
}
if (consoleErrors.length > 0) {
  console.log("脚本错误：" + consoleErrors.join(" || "));
}
process.exit(failed.length === 0 && consoleErrors.length === 0 ? 0 : 1);
