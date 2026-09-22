// 最小 hash 路由：注册路由、分发渲染、受保护路由重定向。
// 页面模块导出 (container, context) => cleanup? 的渲染函数。
// 受保护路由在未登录时重定向到登录页，并携带 redirect 参数以便登录后返回原目标页。

import { isAdmin, isLoggedIn, requiresPasswordChange } from "./auth.js";
import { h, showToast } from "./util.js";

const routes = [];
let currentCleanup = null;
let started = false;

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

export function addRoute(pattern, handler, options = {}) {
  const { protected: isProtected = false, adminOnly = false } = options;
  routes.push({
    pattern,
    handler,
    isProtected,
    adminOnly,
    ...compile(pattern),
  });
}

export function setNotFound(handler) {
  routes.push({ pattern: "*", handler, isProtected: false, regex: null, keys: [] });
}

export function parseHash() {
  let raw = location.hash.startsWith("#") ? location.hash.slice(1) : location.hash;
  if (!raw || raw === "/") raw = "/problems";
  const questionIndex = raw.indexOf("?");
  const path = questionIndex >= 0 ? raw.slice(0, questionIndex) : raw;
  const search = questionIndex >= 0 ? raw.slice(questionIndex + 1) : "";
  return {
    path,
    raw,
    query: new URLSearchParams(search),
  };
}

export function currentPath() {
  return parseHash().path;
}

export function navigate(path, options = {}) {
  const target = path.startsWith("#") ? path : "#" + path;
  if (location.hash === target) {
    render();
    return;
  }
  if (options.replace) {
    // 用 replaceState 替换当前历史记录项并立即重渲染（不会触发 hashchange）。
    history.replaceState(null, "", target);
    render();
  } else {
    location.hash = target;
  }
}

function matchRoute(path) {
  for (const route of routes) {
    if (route.pattern === "*") continue;
    const match = route.regex.exec(path);
    if (match) {
      const params = {};
      route.keys.forEach((key, index) => {
        params[key] = decodeURIComponent(match[index + 1]);
      });
      return { route, params };
    }
  }
  return null;
}

function notFoundRoute() {
  return routes.find((route) => route.pattern === "*") || null;
}

// 已登录但不具备后台权限时的页面（前端体验层；后端接口仍会独立鉴权）。
function renderAdminForbidden(container) {
  const back = h("a", {
    class: "btn btn-secondary",
    text: "返回题目列表",
    attrs: { href: "#/problems" },
  });
  container.appendChild(
    h("div", { class: "card state" }, [
      h("h1", { class: "page-title", text: "无权访问后台" }),
      h("div", {
        class: "alert alert-error",
        text: "当前账号没有后台管理权限；若权限刚被调整，请刷新页面以获取最新状态。",
      }),
      h("div", { attrs: { style: "margin-top:12px;text-align:center" } }, [back]),
    ])
  );
}

export function render() {
  const container = document.getElementById("app");
  if (!container) return;
  const { path, raw, query } = parseHash();

  const matched = matchRoute(path);
  if (!matched) {
    const fallback = notFoundRoute();
    if (currentCleanup) {
      try {
        currentCleanup();
      } catch (error) {
        /* 清理失败不应阻断渲染 */
      }
      currentCleanup = null;
    }
    container.replaceChildren();
    document.title = "页面不存在 · OJ";
    if (fallback) fallback.handler(container, { params: {}, query });
    return;
  }

  const { route, params } = matched;
  if (route.isProtected && !isLoggedIn()) {
    navigate("/login?redirect=" + encodeURIComponent(raw), { replace: true });
    return;
  }

  // 后台路由：前端检查身份、角色与首次改密状态，作为页面体验层。
  // 真正权限仍由后端接口按数据库最新状态执行。
  if (route.adminOnly) {
    if (requiresPasswordChange()) {
      if (currentPath() !== "/password") {
        showToast("请先完成首次改密后再进入后台");
        navigate("/password", { replace: true });
      }
      return;
    }
    if (!isAdmin()) {
      if (currentCleanup) {
        try {
          currentCleanup();
        } catch (error) {
          /* 忽略 */
        }
        currentCleanup = null;
      }
      container.replaceChildren();
      document.title = "无权访问 · OJ";
      renderAdminForbidden(container);
      return;
    }
  }

  if (currentCleanup) {
    try {
      currentCleanup();
    } catch (error) {
      /* 忽略 */
    }
    currentCleanup = null;
  }

  container.replaceChildren();
  if (typeof route.handler === "function") {
    const cleanup = route.handler(container, { params, query });
    if (typeof cleanup === "function") currentCleanup = cleanup;
  }
}

export function startRouter() {
  if (started) return;
  started = true;
  window.addEventListener("hashchange", render);
  render();
}
