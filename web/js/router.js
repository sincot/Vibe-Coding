// hash 路由：集中维护路由匹配、参数解析、访问条件、重定向与页面生命周期。
//
// 采用原生 hash 路由，无构建流程（SPEC M4.1 / UI-04）。
//   - 路由以 pattern + handler + access（public/auth/admin）注册；
//   - 访问条件由本模块统一判定：未登录引导登录并保留原目标、需首次改密引导改密、
//     非管理员显示无权访问；前端检查仅用于页面体验，真正权限仍由后端接口执行；
//   - 直接打开带 hash 的地址、刷新、浏览器前进后退都走同一套规则；
//   - 每次渲染创建生命周期，切换页面时统一清理并中止未完成的读取请求；
//   - 登录后返回原目标前重新校验目标合法性与当前用户权限，避免开放重定向与越权。

import { isAdmin, isLoggedIn, requiresPasswordChange } from "./auth.js";
import { ensureAuth } from "./session.js";
import {
  consumePendingTarget,
  savePendingTarget,
} from "./storage.js";
import { createLifecycle } from "./lifecycle.js";
import { h, showToast } from "./util.js";

const routes = [];
let currentCleanup = null;
let started = false;
// 渲染代次：快速切换/异步核实期间，旧渲染不得覆盖新页面。
let renderGeneration = 0;

// 流程页（登录/注册/改密）：不把返回目标指向自身，避免流程互相循环。
const FLOW_PATHS = new Set(["/login", "/register", "/password"]);

function compile(pattern) {
  const keys = [];
  const source = pattern
    .split("/")
    .map((segment) => {
      if (segment.startsWith(":")) {
        keys.push(segment.slice(1));
        return "([^/]+)";
      }
      return segment.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    })
    .join("/");
  return { regex: new RegExp("^" + source + "$"), keys };
}

// 统一路径规范化：补前导斜杠、去重复前导斜杠与末尾斜杠。
export function normalizePath(value) {
  let path = String(value === undefined || value === null ? "" : value);
  if (!path.startsWith("/")) path = "/" + path;
  path = path.replace(/^\/+/, "/");
  if (path.length > 1) path = path.replace(/\/+$/, "");
  return path || "/";
}

export function addRoute(pattern, handler, options = {}) {
  const {
    access = "public",
    // 兼容早期 {protected, adminOnly} 调用方式。
    protected: isProtected = false,
    adminOnly = false,
    allowDuringPasswordChange = false,
  } = options;
  let level = access;
  if (adminOnly) level = "admin";
  else if (isProtected && level === "public") level = "auth";
  routes.push({
    pattern,
    handler,
    access: level,
    allowDuringPasswordChange,
    ...compile(pattern),
  });
}

export function setNotFound(handler) {
  routes.push({ pattern: "*", handler, access: "public", regex: null, keys: [] });
}

export function parseHash() {
  let raw = location.hash.startsWith("#") ? location.hash.slice(1) : location.hash;
  if (!raw || raw === "/") raw = "/home";
  const questionIndex = raw.indexOf("?");
  const rawPath = questionIndex >= 0 ? raw.slice(0, questionIndex) : raw;
  const search = questionIndex >= 0 ? raw.slice(questionIndex + 1) : "";
  const normalized = normalizePath(rawPath);
  return {
    // 根路径与空路径都进入首页。
    path: normalized === "/" ? "/home" : normalized,
    raw,
    query: new URLSearchParams(search),
  };
}

export function currentPath() {
  return parseHash().path;
}

function matchRoute(path) {
  for (const route of routes) {
    if (route.pattern === "*") continue;
    const match = route.regex.exec(path);
    if (!match) continue;
    const params = {};
    let invalid = false;
    route.keys.forEach((key, index) => {
      const value = match[index + 1];
      try {
        params[key] = decodeURIComponent(value);
      } catch (error) {
        // 非法百分号编码：保留原值并标记参数异常，由路由统一处理。
        params[key] = value;
        invalid = true;
      }
      // 控制字符、解码后含斜杠或路径穿越片段：视为非法参数。
      if (/[\u0000-\u001f\u007f]/.test(params[key])) invalid = true;
      if (params[key].includes("/")) invalid = true;
      if (params[key] === "." || params[key] === "..") invalid = true;
    });
    return { route, params, invalid };
  }
  return null;
}

function notFoundRoute() {
  return routes.find((route) => route.pattern === "*") || null;
}

function accessOfRoute(route) {
  return (route && route.access) || "public";
}

// 校验并规范化「登录后返回」的站内目标。
// 只接受：以 "/" 开头、非 "//"（协议相对）、不含反斜杠与控制字符、
// 能匹配已注册路由、且不是登录/注册/改密等流程页的地址。
// 外部 URL、协议地址、恶意编码一概返回空串（不使用该目标）。
export function sanitizeTarget(raw) {
  if (raw === undefined || raw === null) return "";
  let value = String(raw).trim();
  if (value.startsWith("#")) value = value.slice(1);
  if (!value) return "";
  if (!value.startsWith("/")) return "";
  if (value.startsWith("//")) return "";
  if (value.includes("\\")) return "";
  if (/[\u0000-\u001f\u007f]/.test(value)) return "";

  const questionIndex = value.indexOf("?");
  const path = normalizePath(questionIndex >= 0 ? value.slice(0, questionIndex) : value);
  const search = questionIndex >= 0 ? value.slice(questionIndex + 1) : "";
  if (FLOW_PATHS.has(path)) return "";

  const matched = matchRoute(path);
  if (!matched || matched.invalid) return "";
  // 查询串可能承载筛选/分页参数；拒绝可能引入新片段或控制字符的内容。
  if (search && /[\u0000-\u001f\u007f#]/.test(search)) return "";
  return search ? path + "?" + search : path;
}

// 登录/改密成功后，按当前用户权限决定最终落点。
// 目标无效或无权访问时给公开页面作为退路，并说明原因。
export function resolvePostAuthTarget(raw) {
  const target = sanitizeTarget(raw);
  if (!target) {
    return { path: "/problems", reason: raw ? "invalid" : "" };
  }
  const questionIndex = target.indexOf("?");
  const path = questionIndex >= 0 ? target.slice(0, questionIndex) : target;
  const matched = matchRoute(path);
  const access = matched ? accessOfRoute(matched.route) : "public";
  if (access === "admin" && !isAdmin()) {
    return { path: "/problems", reason: "forbidden" };
  }
  if (access !== "public" && !isLoggedIn()) {
    return { path: "/login", reason: "unauthenticated", target };
  }
  return { path: target, reason: "" };
}

// 登录/改密成功后统一落点：解析返回目标、清理待返回项、必要时给出说明。
export function completePostAuthRedirect(raw) {
  const { path, reason } = resolvePostAuthTarget(raw);
  consumePendingTarget();
  if (reason === "forbidden") {
    showToast("你没有访问该页面的权限，已返回题目列表");
  } else if (reason === "invalid" && raw) {
    showToast("登录目标无效，已返回题目列表");
  }
  navigate(path, { replace: true });
}

function safeRender() {
  render().catch(() => {});
}

export function navigate(path, options = {}) {
  const target = path.startsWith("#") ? path : "#" + path;
  if (location.hash === target) {
    safeRender();
    return;
  }
  if (options.replace) {
    // 用 replaceState 替换当前历史记录项并立即重渲染（不会触发 hashchange）。
    history.replaceState(null, "", target);
    safeRender();
  } else {
    location.hash = target;
  }
}

function teardown() {
  const cleanup = currentCleanup;
  currentCleanup = null;
  if (cleanup) {
    try {
      cleanup();
    } catch (error) {
      /* 清理失败不应阻断渲染 */
    }
  }
}

function resetView(container) {
  teardown();
  if (container) container.replaceChildren();
}

function renderAdminForbidden(container) {
  container.appendChild(
    h("div", { class: "card state" }, [
      h("h1", { class: "page-title", text: "无权访问后台" }),
      h("div", {
        class: "alert alert-error",
        text: "当前账号没有后台管理权限（需已登录、已完成首次改密且角色为管理员）。若权限刚被调整，请刷新页面获取最新状态。",
      }),
      h("div", { attrs: { style: "margin-top:12px;text-align:center" } }, [
        h("a", {
          class: "btn btn-secondary",
          text: "返回题目列表",
          attrs: { href: "#/problems" },
        }),
      ]),
    ])
  );
}

function renderBadAddress(container, { invalidParams = false } = {}) {
  // 非法参数/编码异常与「路由不存在」语义不同，分别提示，不用 notFound 兜底覆盖。
  if (!invalidParams) {
    const fallback = notFoundRoute();
    if (fallback && typeof fallback.handler === "function") {
      try {
        fallback.handler(container, { params: {}, query: new URLSearchParams() });
        return;
      } catch (error) {
        /* 兜底失败则展示内置提示 */
      }
    }
  }
  document.title = (invalidParams ? "地址参数无效" : "页面不存在") + " · OJ";
  container.appendChild(
    h("div", { class: "card state" }, [
      h("h1", { class: "page-title", text: invalidParams ? "地址参数无效" : "页面不存在" }),
      h("p", {
        class: "muted",
        text: invalidParams ? "地址中包含无法解析的参数，请检查后重试。" : "请检查地址，或返回题目列表。",
      }),
      h("a", {
        class: "btn btn-secondary",
        text: "返回题目列表",
        attrs: { href: "#/problems" },
      }),
    ])
  );
}

export async function render() {
  const container = document.getElementById("app");
  if (!container) return;
  const generation = ++renderGeneration;
  const parsed = parseHash();

  // 统一核实身份：区分「身份仍在确认」「游客」「已登录」，避免初始化误跳转。
  await ensureAuth();
  if (generation !== renderGeneration) return;

  const matched = matchRoute(parsed.path);
  if (!matched || matched.invalid) {
    resetView(container);
    document.title = "页面不存在 · OJ";
    renderBadAddress(container, { invalidParams: !!(matched && matched.invalid) });
    return;
  }

  const { route, params } = matched;
  const access = accessOfRoute(route);

  // 受保护页面：未登录时保留原目标并引导登录。
  if (access !== "public" && !isLoggedIn()) {
    savePendingTarget(sanitizeTarget(parsed.raw));
    navigate("/login?redirect=" + encodeURIComponent(parsed.raw), { replace: true });
    return;
  }

  if (access === "admin") {
    // 需首次改密：保留原目标，先完成改密。改密页本身允许访问，不被反复拦截。
    if (requiresPasswordChange() && !route.allowDuringPasswordChange) {
      savePendingTarget(sanitizeTarget(parsed.raw));
      if (parsed.path !== "/password") {
        showToast("请先完成首次改密后再进入后台");
        navigate("/password", { replace: true });
      }
      return;
    }
    if (!isAdmin()) {
      resetView(container);
      document.title = "无权访问 · OJ";
      renderAdminForbidden(container);
      return;
    }
  }

  resetView(container);
  if (typeof route.handler !== "function") return;

  const lifecycle = createLifecycle();
  let cleanup = null;
  try {
    cleanup = route.handler(container, {
      params,
      query: parsed.query,
      path: parsed.path,
      raw: parsed.raw,
      lifecycle,
      signal: lifecycle.signal,
    });
  } catch (error) {
    renderHandlerError(container);
    return;
  }

  // 异步页面（如题目页）返回 Promise：吞掉未处理的异常，避免打断后续渲染。
  if (cleanup && typeof cleanup.then === "function") {
    cleanup.catch(() => {
      if (!lifecycle.disposed) renderHandlerError(container);
    });
  }

  currentCleanup = () => {
    if (typeof cleanup === "function") cleanup();
    lifecycle.dispose();
  };
}

function renderHandlerError(container) {
  resetView(container);
  document.title = "加载失败 · OJ";
  container.appendChild(
    h("div", { class: "card state" }, [
      h("h1", { class: "page-title", text: "页面加载失败" }),
      h("div", { class: "alert alert-error", text: "页面初始化出现异常，请返回题目列表重试。" }),
      h("a", {
        class: "btn btn-secondary",
        text: "返回题目列表",
        attrs: { href: "#/problems" },
      }),
    ])
  );
}

export function startRouter() {
  if (started) return;
  started = true;
  window.addEventListener("hashchange", () => {
    // render 为异步；捕获异常避免产生未处理的 Promise 拒绝。
    render().catch(() => {});
  });
  render().catch(() => {});
}

export { consumePendingTarget, savePendingTarget };
