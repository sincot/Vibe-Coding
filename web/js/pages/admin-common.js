// 后台管理页面共享工具：后台子导航、错误处理、状态区块。
// 前端检查只用于页面体验；真正的权限仍由后端接口按数据库最新状态执行。

import { api } from "../api.js";
import { setUser } from "../auth.js";
import { renderNav } from "../nav.js";
import { currentPath, navigate } from "../router.js";
import { h, showToast } from "../util.js";

// 后台子导航（在后台页面顶部展示）。
export function adminSubNav(active) {
  function link(label, path, key) {
    return h("a", {
      class: "admin-tab" + (active === key ? " active" : ""),
      text: label,
      attrs: { href: "#" + path },
    });
  }
  return h("div", { class: "admin-tabs" }, [
    link("题目管理", "/admin/problems", "problems"),
    link("用户管理", "/admin/users", "users"),
    link("重判", "/admin/rejudge", "rejudge"),
    h("span", { class: "nav-spacer" }),
    h("a", { class: "admin-tab", text: "返回前台", attrs: { href: "#/problems" } }),
  ]);
}

// 统一的可读错误文案，不把所有 403 都解释为 token 失效。
export function adminErrorMessage(error) {
  if (!error) return "请求失败。";
  if (error.network) {
    return "网络连接失败，无法确认后端是否已执行；请检查网络后自行确认，系统不会自动重试。";
  }
  switch (error.status) {
    case 401:
      return "登录状态已失效，请重新登录。";
    case 403:
      return error.code === "PASSWORD_CHANGE_REQUIRED"
        ? "请先完成首次改密后再使用后台功能。"
        : "权限不足：当前账号已不具备后台管理权限。";
    case 404:
      return "记录不存在，或已被其他操作删除。";
    case 409:
      return error.message || "操作存在冲突，无法完成。";
    case 413:
      return "提交内容过大，请精简后重试。";
    default:
      return error.message || `请求失败（HTTP ${error.status || 0}）。`;
  }
}

// 刷新当前用户状态（角色/首改标记可能已被其它管理员修改）。
export async function refreshCurrentUser() {
  try {
    const me = await api.get("/api/me");
    setUser(me);
    return me;
  } catch (error) {
    return null;
  } finally {
    renderNav();
  }
}

// 识别「管理员权限被撤销」：403 且不是必须先改密。
export function isPermissionRevoked(error) {
  return !!error && error.status === 403 && error.code !== "PASSWORD_CHANGE_REQUIRED";
}

// 权限被撤销时刷新用户状态并退出后台，返回 true 表示已处理。
export async function handleRevoked(error) {
  if (!isPermissionRevoked(error)) return false;
  const me = await refreshCurrentUser();
  if (!me || me.role !== "admin" || me.reset_pwd_flag) {
    showToast("后台管理权限已失效，已退出管理页面");
    if (currentPath().startsWith("/admin")) {
      navigate("/problems", { replace: true });
    }
  }
  return true;
}

export function loadingBlock(text = "加载中…") {
  return h("div", { class: "state" }, [
    h("span", { class: "spinner" }),
    h("span", { text }),
  ]);
}

export function emptyBlock(text) {
  return h("div", { class: "card state", text });
}

export function errorBlock(message, onRetry) {
  const children = [h("div", { class: "alert alert-error", text: message })];
  if (typeof onRetry === "function") {
    const retry = h("button", {
      class: "btn btn-secondary",
      text: "重试",
      attrs: { type: "button" },
    });
    retry.addEventListener("click", onRetry);
    children.push(h("div", { attrs: { style: "margin-top:12px" } }, [retry]));
  }
  return h("div", { class: "card state" }, children);
}

// 后台页面外壳：标题 + 副标题 + 子导航 + 内容区。
export function adminShell(container, { title, subtitle, active }) {
  document.title = title + " · 后台 · OJ";
  container.replaceChildren();
  container.appendChild(h("h1", { class: "page-title", text: title }));
  if (subtitle) {
    container.appendChild(h("p", { class: "page-subtitle", text: subtitle }));
  }
  container.appendChild(adminSubNav(active));
  const area = h("div", { class: "admin-area" });
  container.appendChild(area);
  return area;
}

// 请求失败时统一渲染错误，并在权限被撤销时退出后台。
export function renderRequestError(area, error, onRetry) {
  const message = adminErrorMessage(error);
  area.replaceChildren(errorBlock(message, onRetry));
  handleRevoked(error);
}
