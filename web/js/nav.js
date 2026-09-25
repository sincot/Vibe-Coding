// 顶部导航：品牌、题目入口、当前用户展示与退出登录。
//
// 导航按认证状态渲染（SPEC M4.1）：
//   - "unknown"：身份核实中，只显示品牌与入口骨架，不显示登录/注册或后台入口，
//     避免短暂显示错误角色；
//   - "guest"：显示登录/注册入口；
//   - "authenticated"：显示昵称、修改密码与退出登录；管理员额外显示后台入口。
//
// 退出登录清理本地凭证、用户状态与未完成的返回目标，并跳转到公开页面；同时重置
// 会话核实与错误去重标记，避免退出前发起的旧请求在返回后恢复已退出的身份。
// 界面文案不声称前端退出已撤销服务端 JWT。

import {
  AUTH_UNKNOWN,
  clearAuth,
  getAuthStatus,
  getUser,
  isAdmin,
  isLoggedIn,
} from "./auth.js";
import { resetPasswordChangeGuard, resetUnauthorizedGuard } from "./api.js";
import { resetSessionVerification } from "./session.js";
import { clearPendingTarget } from "./storage.js";
import { currentPath, navigate } from "./router.js";
import { h, showToast } from "./util.js";

function isActive(current, path) {
  if (path === "/admin") {
    return current === "/admin" || current.startsWith("/admin/");
  }
  return current === path || current.startsWith(path + "/");
}

function navLink(label, path) {
  const active = isActive(currentPath(), path);
  return h("a", {
    class: "nav-link" + (active ? " active" : ""),
    text: label,
    attrs: { href: "#" + path, "aria-current": active ? "page" : null },
  });
}

function performLogout() {
  clearAuth();
  resetUnauthorizedGuard();
  resetPasswordChangeGuard();
  resetSessionVerification();
  clearPendingTarget();
  navigate("/login");
  showToast("已退出登录（仅清理本机凭证，服务端已签发的 JWT 仍有效至过期）");
}

export function renderNav() {
  const nav = document.getElementById("site-nav");
  if (!nav) return;
  nav.replaceChildren();

  nav.appendChild(
    h("a", { class: "brand", text: "OJ 在线判题", attrs: { href: "#/home" } })
  );

  const status = getAuthStatus();
  if (status === AUTH_UNKNOWN) {
    // 身份核实中：仅展示骨架，避免根据本地旧信息短暂显示需登录的入口。
    nav.appendChild(h("span", { class: "nav-spacer" }));
    nav.appendChild(h("span", { class: "nav-link muted", text: "身份确认中…" }));
    renderFooterAdmin();
    return;
  }

  const links = h("div", { class: "nav-links" }, [navLink("首页", "/home")]);
  // 题库、题目详情、排行榜与提交历史均需登录；游客不显示会跳转登录的入口。
  if (isLoggedIn()) {
    links.appendChild(navLink("题目列表", "/problems"));
    links.appendChild(navLink("排行榜", "/leaderboard"));
    links.appendChild(navLink("提交历史", "/submissions"));
  }
  // 只有满足「已登录 + 已完成首次改密 + admin 角色」的账号显示管理入口。
  if (isAdmin()) {
    links.appendChild(navLink("管理后台", "/admin"));
  }
  nav.appendChild(links);
  nav.appendChild(h("span", { class: "nav-spacer" }));

  if (isLoggedIn()) {
    const user = getUser();
    const nickname = user && user.nickname ? user.nickname : "已登录用户";
    const parts = [h("span", { class: "nav-link nick", text: nickname })];
    if (user && user.role === "admin") {
      parts.push(h("span", { class: "badge", text: "管理员" }));
    }
    parts.push(
      h("a", {
        class: "nav-link",
        text: "修改密码",
        attrs: { href: "#/password" },
      })
    );
    const logoutButton = h("button", {
      class: "btn btn-secondary",
      text: "退出登录",
      attrs: { type: "button" },
    });
    logoutButton.addEventListener("click", performLogout);
    parts.push(logoutButton);
    nav.appendChild(h("div", { class: "nav-user" }, parts));
  } else {
    nav.appendChild(
      h("div", { class: "nav-user" }, [
        h("a", { class: "nav-link", text: "登录", attrs: { href: "#/login" } }),
        h("a", { class: "nav-link", text: "注册", attrs: { href: "#/register" } }),
      ])
    );
  }

  renderFooterAdmin();
}

// 页脚管理员入口（admin 专属），SPEC 2.6.1。
function renderFooterAdmin() {
  const footerAdmin = document.getElementById("footer-admin");
  if (!footerAdmin) return;
  footerAdmin.replaceChildren();
  if (isAdmin()) {
    footerAdmin.appendChild(
      h("a", { class: "nav-link", text: "管理员入口", attrs: { href: "#/admin" } })
    );
  }
}
