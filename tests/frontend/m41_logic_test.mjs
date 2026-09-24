// M4.1 前端基础设施逻辑单元验证（Node 直接执行真实 web/js 模块）。
//
// 不依赖 jsdom / 浏览器：为 localStorage、sessionStorage、location、history、
// window、fetch 提供最小 fake，验证路由目标校验、认证状态机、API 错误分类、
// 401/改密去重、生命周期与待返回目标存储等纯逻辑行为。
//
// 仅用于开发验证，不属于项目运行依赖，不注册 CTest。
// 用法：node tests/frontend/m41_logic_test.mjs

import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const mod = (rel) => pathToFileURL(path.join(ROOT, "web/js", rel)).href;

// ------------------------- fake browser globals ---------------------------
function makeStorage() {
  const map = new Map();
  return {
    getItem: (k) => (map.has(k) ? map.get(k) : null),
    setItem: (k, v) => map.set(k, String(v)),
    removeItem: (k) => map.delete(k),
    clear: () => map.clear(),
    key: (i) => [...map.keys()][i] ?? null,
    get length() {
      return map.size;
    },
  };
}
globalThis.localStorage = makeStorage();
globalThis.sessionStorage = makeStorage();
globalThis.location = { hash: "" };
globalThis.history = { replaceState() {} };
globalThis.window = { addEventListener() {}, removeEventListener() {} };

// ------------------------- result collection ------------------------------
const results = [];
function check(name, cond, detail = "") {
  results.push({ name, ok: !!cond, detail });
  if (!cond) console.error("  FAIL:", name, detail ? "| " + detail : "");
  else console.log("  ok  :", name);
}

// ------------------------- imports (after globals) ------------------------
const auth = await import(mod("auth.js"));
const storage = await import(mod("storage.js"));
const lifecycleMod = await import(mod("lifecycle.js"));
const api = await import(mod("api.js"));
const router = await import(mod("router.js"));

// ==========================================================================
// T-01 路径规范化
// ==========================================================================
console.log("\n== T-01 normalizePath ==");
check("去重前导斜杠", router.normalizePath("//a") === "/a", router.normalizePath("//a"));
check("补前导斜杠", router.normalizePath("problems") === "/problems");
check("去尾部斜杠", router.normalizePath("/problems/") === "/problems");
check("空值归一为根", router.normalizePath("") === "/");

// ==========================================================================
// T-02 默认 / 未知 / 非法参数路由
// ==========================================================================
console.log("\n== T-02 parseHash / matchRoute ==");
globalThis.location.hash = "";
check("空 hash 默认 /problems", router.parseHash().path === "/problems", router.parseHash().path);
globalThis.location.hash = "#";
check("仅 # 默认 /problems", router.parseHash().path === "/problems", router.parseHash().path);
globalThis.location.hash = "#?q=1";
check("空路径带查询默认 /problems", router.parseHash().path === "/problems", router.parseHash().path);
// 注册与 main.js 同构的最小路由表
router.addRoute("/problems", () => {}, { access: "public" });
router.addRoute("/problems/:id", () => {}, { access: "public" });
router.addRoute("/login", () => {}, { access: "public" });
router.addRoute("/password", () => {}, { access: "auth" });
router.addRoute("/admin", () => {}, { access: "admin" });
globalThis.location.hash = "#/problems/3?x=1";
const ph = router.parseHash();
check("解析路径与查询", ph.path === "/problems/3" && ph.query.get("x") === "1");
check("未知路由目标被拒", router.sanitizeTarget("/nope") === "", router.sanitizeTarget("/nope"));
check("非法编码目标被拒", router.sanitizeTarget("/problems/%E0%A4%A") === "");
check("路径穿越目标被拒", router.sanitizeTarget("/problems/%2e%2e") === "");
check("解码出斜杠目标被拒", router.sanitizeTarget("/problems/a%2Fb") === "");

// ==========================================================================
// T-03 sanitizeTarget 开放重定向防护
// ==========================================================================
console.log("\n== T-03 sanitizeTarget ==");
check("合法站内路径保留", router.sanitizeTarget("/problems/3?p=2") === "/problems/3?p=2");
check("去掉 hash 前缀", router.sanitizeTarget("#/problems") === "/problems");
check("拒绝外部 http URL", router.sanitizeTarget("https://evil.example/x") === "");
check("拒绝协议相对 //", router.sanitizeTarget("//evil.example") === "");
check("拒绝反斜杠", router.sanitizeTarget("/\\evil.example") === "");
check("拒绝控制字符", router.sanitizeTarget("/problems\u0000") === "");
check("拒绝流程页 /login", router.sanitizeTarget("/login") === "");
check("拒绝流程页 /register", router.sanitizeTarget("/register") === "");
check("拒绝流程页 /password", router.sanitizeTarget("/password") === "");
check("查询串含 # 被拒", router.sanitizeTarget("/problems?a=1#x") === "");

// ==========================================================================
// T-04 resolvePostAuthTarget 权限判定
// ==========================================================================
console.log("\n== T-04 resolvePostAuthTarget ==");
auth.clearAuth();
auth.setAuth("tok-user", { id: 2, role: "user", reset_pwd_flag: 0, nickname: "u" });
let r = router.resolvePostAuthTarget("/admin");
check("普通用户请求后台→回退 /problems", r.path === "/problems", JSON.stringify(r));
check("普通用户越权原因为 forbidden", r.reason === "forbidden", r.reason);
auth.setAuth("tok-admin", { id: 1, role: "admin", reset_pwd_flag: 0, nickname: "a" });
r = router.resolvePostAuthTarget("/admin?x=1");
check("管理员后台目标保留", r.path === "/admin?x=1", JSON.stringify(r));
r = router.resolvePostAuthTarget("https://evil.example");
check("无效目标回退且标注 invalid", r.path === "/problems" && r.reason === "invalid", JSON.stringify(r));
r = router.resolvePostAuthTarget("");
check("空目标静默回退", r.path === "/problems" && r.reason === "", JSON.stringify(r));

// ==========================================================================
// T-05 认证状态机
// ==========================================================================
console.log("\n== T-05 auth 状态机 ==");
auth.clearAuth();
check("clearAuth 后为 guest", auth.getAuthStatus() === "guest" && !auth.isLoggedIn());
const e0 = auth.getAuthEpoch();
auth.setAuth("tok-a", { id: 1, role: "admin", reset_pwd_flag: 0, nickname: "a" });
check("setAuth 后 authenticated", auth.getAuthStatus() === "authenticated" && auth.isLoggedIn());
check("管理员 isAdmin 为真", auth.isAdmin());
check("setAuth 递增 epoch", auth.getAuthEpoch() === e0 + 1, String(auth.getAuthEpoch() - e0));
auth.setUser({ id: 1, role: "admin", reset_pwd_flag: 1, nickname: "a" });
check("reset_pwd_flag=1 时 requiresPasswordChange", auth.requiresPasswordChange());
check("reset_pwd_flag=1 时 isAdmin 为假", !auth.isAdmin());
const e1 = auth.getAuthEpoch();
auth.clearAuth();
check("clearAuth 回到 guest 且递增 epoch", auth.getAuthStatus() === "guest" && auth.getAuthEpoch() === e1 + 1);
check("clearAuth 清 token", auth.getToken() === "");

// ==========================================================================
// T-12 待返回目标存储
// ==========================================================================
console.log("\n== T-12 storage ==");
storage.clearPendingTarget();
check("初始为空", storage.peekPendingTarget() === "");
storage.savePendingTarget("/admin/problems");
check("保存后可读", storage.peekPendingTarget() === "/admin/problems");
check("consume 返回并清理", storage.consumePendingTarget() === "/admin/problems" && storage.peekPendingTarget() === "");

// ==========================================================================
// T-11 生命周期
// ==========================================================================
console.log("\n== T-11 lifecycle ==");
const lc = lifecycleMod.createLifecycle();
const t1 = lc.next();
check("next 后代次有效", lc.isCurrent(t1));
const t2 = lc.next();
check("旧代次失效", !lc.isCurrent(t1) && lc.isCurrent(t2));
check("未 dispose 时 signal 未中止", lc.signal && !lc.signal.aborted);
lc.dispose();
check("dispose 标记 disposed", lc.disposed);
check("dispose 中止 signal", lc.signal.aborted);
check("dispose 后 isCurrent 为假", !lc.isCurrent(t2));
lc.dispose();
check("重复 dispose 幂等", lc.disposed && lc.signal.aborted);
const lc2 = lifecycleMod.createLifecycle();
check("ensureLifecycle 复用实例", lifecycleMod.ensureLifecycle(lc2) === lc2);

// ==========================================================================
// T-07 API 错误分类
// ==========================================================================
console.log("\n== T-07 ApiError 分类 ==");
const mk = (status, opts = {}) => new api.ApiError("m", { status, ...opts });
check("401 auth invalid", mk(401).isAuthInvalid());
check("403+code 改密要求", mk(403, { code: "PASSWORD_CHANGE_REQUIRED" }).isPasswordChangeRequired());
check("普通 403 权限不足", mk(403).isForbidden() && !mk(403).isPasswordChangeRequired());
check("404 not found", mk(404).isNotFound());
check("409 conflict", mk(409).isConflict());
check("429 rate limited", mk(429).isRateLimited());
check("503 unavailable", mk(503).isUnavailable());
check("500 server error", mk(500).isServerError());
check("网络错误标志", new api.ApiError("n", { network: true }).network);
check("取消标志", new api.ApiError("a", { aborted: true }).aborted);
check("retryable 透传", new api.ApiError("q", { status: 503, retryable: true }).retryable);

// ------------------------- fetch stub helpers -----------------------------
function jsonResponse(status, body) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json" },
  });
}
function installFetch(impl) {
  globalThis.fetch = impl;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ==========================================================================
// T-08 / T-09 401 全局处理去重与表单 401 不误清
// ==========================================================================
console.log("\n== T-08/T-09 401 处理 ==");
let unauthorized = 0;
api.setUnauthorizedHandler(() => {
  unauthorized += 1;
});
api.resetUnauthorizedGuard();
auth.setAuth("tok-401", { id: 1, role: "admin", reset_pwd_flag: 0 });
installFetch(async () => jsonResponse(401, { error: "无效" }));
const p1 = api.api.get("/api/me").catch(() => null);
const p2 = api.api.get("/api/me").catch(() => null);
await Promise.all([p1, p2]);
check("并发 401 只触发一次跳转", unauthorized === 1, "count=" + unauthorized);
check("401 清理本地凭证", auth.getToken() === "", auth.getToken());

unauthorized = 0;
api.resetUnauthorizedGuard();
installFetch(async () => jsonResponse(401, { error: "账号或密码错误" }));
await api.api.post("/api/login", { account: "x", password: "y" }, { auth: false }).catch(() => null);
check("登录 401 不触发全局跳转", unauthorized === 0, "count=" + unauthorized);

installFetch(async () => jsonResponse(401, { error: "旧密码错误" }));
auth.setAuth("tok-pwd", { id: 1, role: "admin", reset_pwd_flag: 0 });
await api.api
  .post("/api/me/password", { old_password: "a", new_password: "b" }, { skipAuthRedirect: true })
  .catch(() => null);
check("改密 401 不清理有效登录", auth.getToken() === "tok-pwd" && auth.isLoggedIn(), auth.getToken());
check("改密 401 不触发全局跳转", unauthorized === 0, "count=" + unauthorized);

// ==========================================================================
// T-10 改密要求去重
// ==========================================================================
console.log("\n== T-10 改密要求去重 ==");
let pwdReq = 0;
api.setPasswordRequiredHandler(() => {
  pwdReq += 1;
});
api.resetPasswordChangeGuard();
auth.setAuth("tok-pc", { id: 1, role: "admin", reset_pwd_flag: 1 });
installFetch(async () => jsonResponse(403, { error: "请先修改密码", code: "PASSWORD_CHANGE_REQUIRED" }));
await Promise.all([
  api.api.get("/api/admin/users").catch(() => null),
  api.api.get("/api/admin/users").catch(() => null),
]);
check("并发改密要求只触发一次", pwdReq === 1, "count=" + pwdReq);

// 成功请求应重置去重标记
api.resetPasswordChangeGuard();
pwdReq = 0;
installFetch(async () => jsonResponse(200, { ok: true }));
await api.api.get("/api/problems");
installFetch(async () => jsonResponse(403, { error: "请先修改密码", code: "PASSWORD_CHANGE_REQUIRED" }));
await api.api.get("/api/admin/users").catch(() => null);
check("成功后去重标记被重置", pwdReq === 1, "count=" + pwdReq);

// ==========================================================================
// T-06 会话核实 ensureAuth（单飞 / 成功 / 401 / 网络失败）
// ==========================================================================
console.log("\n== T-06 ensureAuth ==");
const session = await import(mod("session.js"));
auth.clearAuth();
globalThis.localStorage.setItem("oj.token", "tok-verify");
let calls = 0;
installFetch(async () => {
  calls += 1;
  await sleep(20);
  return jsonResponse(200, { id: 5, role: "user", reset_pwd_flag: 0, nickname: "u5" });
});
session.resetSessionVerification();
const [v1, v2] = await Promise.all([session.ensureAuth(), session.ensureAuth()]);
check("并发核实只请求一次", calls === 1, "calls=" + calls);
check("核实成功返回 true 并登录", v1 === true && v2 === true && auth.isLoggedIn());
check("核实后用户信息为服务端值", (auth.getUser() || {}).id === 5, JSON.stringify(auth.getUser()));

// 401 → 清理凭证
unauthorized = 0;
api.resetUnauthorizedGuard();
auth.clearAuth();
globalThis.localStorage.setItem("oj.token", "tok-expired");
installFetch(async () => jsonResponse(401, { error: "无效" }));
session.resetSessionVerification();
await session.ensureAuth();
check("核实 401 清理凭证", auth.getToken() === "" && !auth.isLoggedIn(), auth.getToken());
check("核实 401 触发一次跳转", unauthorized === 1, "count=" + unauthorized);

// 网络失败 → 未核实但不误清 token
auth.clearAuth();
globalThis.localStorage.setItem("oj.token", "tok-net");
installFetch(async () => {
  throw new TypeError("network down");
});
session.resetSessionVerification();
await session.ensureAuth();
check("网络失败标记未核实", !auth.isLoggedIn() && auth.getAuthStatus() === "guest");
check("网络失败保留 token（不误判凭证失效）", auth.getToken() === "tok-net", auth.getToken());

// 取消（AbortError）应归类为 aborted
api.resetUnauthorizedGuard();
auth.setAuth("tok-abort", { id: 1, role: "admin", reset_pwd_flag: 0 });
installFetch(async () => {
  const err = new Error("aborted");
  err.name = "AbortError";
  throw err;
});
let abortErr = null;
try {
  await api.api.get("/api/problems");
} catch (e) {
  abortErr = e;
}
check("AbortError 归类为 aborted", abortErr && abortErr.aborted === true && abortErr.network === false);

// ==========================================================================
// T-13 退出后旧核实响应返回不恢复身份（epoch 竞态）
// ==========================================================================
console.log("\n== T-13 退出后旧核实响应 ==");
api.resetUnauthorizedGuard();
auth.clearAuth();
globalThis.localStorage.setItem("oj.token", "tok-race");
let releaseVerify;
installFetch(
  () =>
    new Promise((resolve) => {
      releaseVerify = () =>
        resolve(jsonResponse(200, { id: 9, role: "admin", reset_pwd_flag: 0, nickname: "race" }));
    })
);
session.resetSessionVerification();
const pendingVerify = session.ensureAuth();
auth.clearAuth(); // 响应返回前退出登录
session.resetSessionVerification();
releaseVerify();
await pendingVerify;
check(
  "退出后旧核实响应不恢复身份",
  !auth.isLoggedIn() && auth.getToken() === "" && auth.getAuthStatus() === "guest",
  `logged=${auth.isLoggedIn()} token=${auth.getToken()} status=${auth.getAuthStatus()}`
);

// ==========================================================================
// T-14 请求超时：后端无响应时不再永久停留在加载中
// ==========================================================================
console.log("\n== T-14 请求超时 ==");
api.resetUnauthorizedGuard();
installFetch(
  (input, init) =>
    new Promise((resolve, reject) => {
      if (init && init.signal) {
        init.signal.addEventListener("abort", () => {
          const err = new Error("aborted");
          err.name = "AbortError";
          reject(err);
        });
      }
    })
);
const t0 = Date.now();
let timeoutErr = null;
try {
  await api.api.get("/api/problems", { timeoutMs: 60, auth: false });
} catch (e) {
  timeoutErr = e;
}
check(
  "无响应请求按超时错误返回",
  timeoutErr && timeoutErr.timeout === true && timeoutErr.network === true,
  timeoutErr ? `timeout=${timeoutErr.timeout} network=${timeoutErr.network}` : "no error"
);
check("超时后快速返回", Date.now() - t0 < 3000, `${Date.now() - t0}ms`);

// 调用方主动取消仍归类为 aborted（不误判为超时）
installFetch(
  (input, init) =>
    new Promise((resolve, reject) => {
      if (init && init.signal) {
        init.signal.addEventListener("abort", () => {
          const err = new Error("aborted");
          err.name = "AbortError";
          reject(err);
        });
      }
    })
);
const ctrl = new AbortController();
const cancelErrPromise = api.api
  .get("/api/problems", { signal: ctrl.signal, auth: false })
  .catch((e) => e);
ctrl.abort();
const cancelErr = await cancelErrPromise;
check("主动取消归类为 aborted", cancelErr && cancelErr.aborted === true, String(cancelErr && cancelErr.name));

// ------------------------- summary ----------------------------------------
const passed = results.filter((r) => r.ok).length;
const failed = results.filter((r) => !r.ok);
console.log("\n========================================");
console.log(`M4.1 逻辑验证：${passed}/${results.length} 项通过，${failed.length} 项失败`);
if (failed.length) {
  console.log("失败项：");
  for (const f of failed) console.log(" - " + f.name + (f.detail ? " | " + f.detail : ""));
}
console.log("========================================");
process.exit(failed.length ? 1 : 0);
