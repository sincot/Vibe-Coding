// 前端入口：注册路由、绑定全局认证处理、校验已有 token、启动路由。
// 采用原生 ES Module，无构建流程，由后端静态托管。

import { api, setPasswordRequiredHandler, setUnauthorizedHandler } from "./api.js";
import { clearAuth, getToken, setUser } from "./auth.js";
import { renderNav } from "./nav.js";
import { renderProblem } from "./pages/problem.js";
import { renderProblems } from "./pages/problems.js";
import { renderLogin } from "./pages/login.js";
import { renderPassword } from "./pages/password.js";
import { renderRegister } from "./pages/register.js";
import {
  renderAdminHome,
  renderAdminProblemForm,
  renderAdminProblems,
} from "./pages/admin-problems.js";
import { renderAdminRejudge } from "./pages/admin-rejudge.js";
import { renderAdminTestcases } from "./pages/admin-testcases.js";
import { renderAdminUsers } from "./pages/admin-users.js";
import {
  addRoute,
  currentPath,
  navigate,
  setNotFound,
  startRouter,
} from "./router.js";
import { h, showToast } from "./util.js";

// 身份失效：清理本地凭证并跳转登录，携带当前路径以便登录后返回。
// api.js 内部已做去重，避免多个并发 401 触发重复跳转。
setUnauthorizedHandler(() => {
  clearAuth();
  renderNav();
  const current = currentPath();
  if (current === "/login" || current === "/register") return;
  showToast("登录状态已失效，请重新登录");
  navigate("/login?redirect=" + encodeURIComponent(current), { replace: true });
});

// 后端要求先改密：引导到改密页（若已在改密页则不重复跳转）。
setPasswordRequiredHandler(() => {
  if (currentPath() !== "/password") navigate("/password");
});

addRoute("/register", renderRegister);
addRoute("/login", renderLogin);
addRoute("/password", renderPassword, { protected: true });
addRoute("/problems", renderProblems);
addRoute("/problems/:id", renderProblem);

// 后台管理路由（仅管理员；前端检查身份/角色/首改状态，后端接口仍独立鉴权）。
addRoute("/admin", renderAdminHome, { protected: true, adminOnly: true });
addRoute("/admin/problems", renderAdminProblems, { protected: true, adminOnly: true });
addRoute("/admin/problems/new", renderAdminProblemForm, { protected: true, adminOnly: true });
addRoute("/admin/problems/:id/edit", renderAdminProblemForm, {
  protected: true,
  adminOnly: true,
});
addRoute("/admin/problems/:id/testcases", renderAdminTestcases, {
  protected: true,
  adminOnly: true,
});
addRoute("/admin/users", renderAdminUsers, { protected: true, adminOnly: true });
addRoute("/admin/rejudge", renderAdminRejudge, { protected: true, adminOnly: true });

setNotFound((container) => {
  document.title = "页面不存在 · OJ";
  container.appendChild(
    h("div", { class: "card state" }, [
      h("h1", { class: "page-title", text: "页面不存在" }),
      h("p", { class: "muted", text: "请检查地址，或返回题目列表。" }),
      h("a", { class: "btn btn-secondary", text: "返回题目列表", attrs: { href: "#/problems" } }),
    ])
  );
});

async function bootstrap() {
  renderNav();
  if (getToken()) {
    try {
      const me = await api.get("/api/me");
      setUser(me);
    } catch (error) {
      // 401 已由全局处理器清理凭证并跳转登录；其它错误保留当前状态。
    }
    renderNav();
  }
  startRouter();
}

bootstrap();
