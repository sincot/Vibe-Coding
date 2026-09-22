// 登录页：使用 account（10 位数字账号或 admin）与 password 登录。
// 成功后保存认证信息与用户状态；若后端标记需首次改密，则引导到改密页；
// 否则回到登录前的目标页（redirect 参数）。

import { api } from "../api.js";
import { setAuth } from "../auth.js";
import { renderNav } from "../nav.js";
import { navigate } from "../router.js";
import { field, h, setBusy, setMessage } from "../util.js";

function safeRedirect(value) {
  if (!value) return "/problems";
  const target = String(value);
  if (!target.startsWith("/") || target.startsWith("//")) return "/problems";
  return target;
}

export function renderLogin(container, context) {
  document.title = "登录 · OJ";
  const accountParam = context.query.get("account") || "";
  const redirect = context.query.get("redirect") || "";

  const account = h("input", {
    attrs: {
      type: "text",
      id: "login-account",
      autocomplete: "username",
      value: accountParam,
    },
  });
  const password = h("input", {
    attrs: {
      type: "password",
      id: "login-password",
      autocomplete: "current-password",
    },
  });
  const message = h("div");
  const submit = h("button", {
    class: "btn btn-primary btn-block",
    text: "登录",
    attrs: { type: "submit" },
  });

  const form = h("form", { class: "form" }, [
    field("账号", account, "普通用户为注册时分配的 10 位数字账号；管理员为 admin"),
    field("密码", password, ""),
    message,
    submit,
  ]);

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    setMessage(message, "info", "");
    if (!account.value || !password.value) {
      setMessage(message, "error", "请填写账号和密码");
      return;
    }

    setBusy(submit, true, "登录中…");
    try {
      const result = await api.post(
        "/api/login",
        { account: account.value, password: password.value },
        { auth: false }
      );
      setAuth(result.token, result.user);
      renderNav();
      if (result.user && result.user.reset_pwd_flag) {
        navigate("/password");
      } else {
        navigate(safeRedirect(redirect));
      }
    } catch (error) {
      setBusy(submit, false, null, "登录");
      if (error.status === 429) {
        setMessage(message, "warn", error.message || "登录尝试过于频繁，请稍后再试");
      } else if (error.status === 401) {
        setMessage(message, "error", "账号或密码错误");
      } else {
        setMessage(message, "error", error.message || "登录失败");
      }
    }
  });

  container.appendChild(h("h1", { class: "page-title", text: "登录" }));
  container.appendChild(
    h("p", { class: "page-subtitle", text: "使用系统分配的账号与密码登录。" })
  );
  container.appendChild(h("div", { class: "card" }, [form]));
  container.appendChild(
    h("p", { class: "page-subtitle" }, [
      h("span", { text: "还没有账号？" }),
      h("a", { text: " 去注册", attrs: { href: "#/register" } }),
    ])
  );
}
