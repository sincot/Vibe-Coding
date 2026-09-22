// 顶部导航：品牌、题目入口、当前用户展示与退出登录。
// 退出登录只清理前端凭证与用户状态，并在提示中说明不代表已撤销服务端 JWT。

import { clearAuth, getUser, isAdmin, isLoggedIn } from "./auth.js";
import { resetUnauthorizedGuard } from "./api.js";
import { currentPath, navigate } from "./router.js";
import { h, showToast } from "./util.js";

function navLink(label, path) {
  const active = currentPath().startsWith(path);
  return h("a", {
    class: "nav-link" + (active ? " active" : ""),
    text: label,
    attrs: { href: "#" + path },
  });
}

export function renderNav() {
  const nav = document.getElementById("site-nav");
  if (!nav) return;
  nav.replaceChildren();

  nav.appendChild(
    h("a", { class: "brand", text: "OJ 在线判题", attrs: { href: "#/problems" } })
  );

  const links = h("div", { class: "nav-links" }, [navLink("题目列表", "/problems")]);
  // 只有满足「已登录 + 已完成首次改密 + admin 角色」的账号显示管理入口。
  if (isAdmin()) {
    links.appendChild(navLink("管理后台", "/admin"));
  }
  nav.appendChild(links);
  nav.appendChild(h("span", { class: "nav-spacer" }));

  if (isLoggedIn()) {
    const user = getUser();
    const nickname = user && user.nickname ? user.nickname : "已登录用户";
    const parts = [
      h("span", { class: "nav-link", text: nickname }),
    ];
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
    logoutButton.addEventListener("click", () => {
      clearAuth();
      resetUnauthorizedGuard();
      renderNav();
      navigate("/login");
      showToast("已退出登录（仅清理本机凭证，服务端已签发的 JWT 仍有效至过期）");
    });
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

  // 页脚管理员入口（admin 专属），SPEC 2.6.1。
  const footerAdmin = document.getElementById("footer-admin");
  if (footerAdmin) {
    footerAdmin.replaceChildren();
    if (isAdmin()) {
      footerAdmin.appendChild(
        h("a", { class: "nav-link", text: "管理员入口", attrs: { href: "#/admin" } })
      );
    }
  }
}