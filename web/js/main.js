// 前端入口：注册全部路由、绑定全局认证处理、核实已有 token、启动路由。
// 采用原生 ES Module，无构建流程，由后端静态托管（SPEC M4.1 / UI-04）。

import {
  setPasswordRequiredHandler,
  setUnauthorizedHandler,
} from "./api.js";
import { subscribeAuth } from "./auth.js";
import { ensureAuth, resetSessionVerification } from "./session.js";
import { renderNav } from "./nav.js";
import { renderProblem } from "./pages/problem.js";
import { renderProblems } from "./pages/problems.js";
import { renderSubmissionDetail } from "./pages/submission-detail.js";
import { renderSubmissions } from "./pages/submissions.js";
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
  navigate,
  parseHash,
  sanitizeTarget,
  savePendingTarget,
  setNotFound,
  startRouter,
} from "./router.js";
import { h, showToast } from "./util.js";

// 身份失效：api.js 已清理本地失效凭证；这里负责提示并引导登录，携带当前路径
// 以便登录后返回原目标。api.js 内部已做去重，避免多个并发 401 触发重复跳转。
setUnauthorizedHandler(() => {
  resetSessionVerification();
  const parsed = parseHash();
  if (parsed.path === "/login" || parsed.path === "/register") return;
  savePendingTarget(sanitizeTarget(parsed.raw));
  showToast("登录状态已失效，请重新登录");
  navigate("/login?redirect=" + encodeURIComponent(parsed.raw), { replace: true });
});

// 后端要求先改密：保留当前目标并引导到改密页（若已在改密页则不重复跳转）。
setPasswordRequiredHandler(() => {
  const parsed = parseHash();
  if (parsed.path === "/password") return;
  savePendingTarget(sanitizeTarget(parsed.raw));
  navigate("/password", { replace: true });
});

addRoute("/register", renderRegister, { access: "public" });
addRoute("/login", renderLogin, { access: "public" });
addRoute("/password", renderPassword, { access: "auth", allowDuringPasswordChange: true });
addRoute("/problems", renderProblems, { access: "public" });
addRoute("/problems/:id", renderProblem, { access: "public" });

// 本人提交历史与提交详情（M4.4）：需登录；详情接口在后台再按本人/管理员权限
// 二次校验，前端检查只用于页面体验。
addRoute("/submissions", renderSubmissions, { access: "auth" });
addRoute("/submissions/:id", renderSubmissionDetail, { access: "auth" });

// 后台管理路由（仅管理员；前端检查身份/角色/首改状态，后端接口仍独立鉴权）。
addRoute("/admin", renderAdminHome, { access: "admin" });
addRoute("/admin/problems", renderAdminProblems, { access: "admin" });
addRoute("/admin/problems/new", renderAdminProblemForm, { access: "admin" });
addRoute("/admin/problems/:id/edit", renderAdminProblemForm, { access: "admin" });
addRoute("/admin/problems/:id/testcases", renderAdminTestcases, { access: "admin" });
addRoute("/admin/users", renderAdminUsers, { access: "admin" });
addRoute("/admin/rejudge", renderAdminRejudge, { access: "admin" });

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

// 导航订阅认证状态变化：登录、退出、改密后自动刷新昵称与入口。
subscribeAuth(() => renderNav());

async function bootstrap() {
  renderNav();
  // 先核实身份再渲染首个路由，避免初始化阶段错误跳转或短暂显示无权内容。
  await ensureAuth();
  startRouter();
}

bootstrap();
