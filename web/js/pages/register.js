// 注册页：提交 nickname 与 password。成功后显著展示系统分配的 10 位账号，
// 并引导用户使用该账号登录（本阶段注册接口不返回 token，不做自动登录）。

import { api } from "../api.js";
import { navigate } from "../router.js";
import { field, h, setBusy, setMessage } from "../util.js";

export function renderRegister(container) {
  document.title = "注册 · OJ";

  const nickname = h("input", {
    attrs: { type: "text", id: "reg-nickname", maxlength: "30", autocomplete: "username" },
  });
  const password = h("input", {
    attrs: {
      type: "password",
      id: "reg-password",
      maxlength: "128",
      autocomplete: "new-password",
    },
  });
  const confirm = h("input", {
    attrs: {
      type: "password",
      id: "reg-confirm",
      maxlength: "128",
      autocomplete: "new-password",
    },
  });

  const message = h("div");
  const submit = h("button", {
    class: "btn btn-primary btn-block",
    text: "注册",
    attrs: { type: "submit" },
  });

  const form = h(
    "form",
    { class: "form" },
    [
      field("昵称", nickname, "去除首尾空白后非空，长度不超过 30，全局唯一"),
      field("密码", password, "非空，长度不超过 128；空白视为有效内容"),
      field("确认密码", confirm, ""),
      message,
      submit,
    ]
  );

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    setMessage(message, "info", "");
    if (!nickname.value.trim()) {
      setMessage(message, "error", "请填写昵称");
      return;
    }
    if (!password.value) {
      setMessage(message, "error", "请填写密码");
      return;
    }
    if (password.value !== confirm.value) {
      setMessage(message, "error", "两次输入的密码不一致");
      return;
    }

    setBusy(submit, true, "注册中…");
    try {
      const result = await api.post(
        "/api/register",
        { nickname: nickname.value, password: password.value },
        { auth: false }
      );
      showSuccess(container, result);
    } catch (error) {
      setBusy(submit, false, null, "注册");
      setMessage(message, "error", error.message || "注册失败");
    }
  });

  container.appendChild(h("h1", { class: "page-title", text: "注册账号" }));
  container.appendChild(
    h("p", {
      class: "page-subtitle",
      text: "系统将随机分配一个 10 位数字账号，请牢记并在登录时使用。",
    })
  );
  container.appendChild(h("div", { class: "card" }, [form]));
}

// 注册成功视图：突出显示账号，并提供跳转登录（预填账号）。
function showSuccess(container, result) {
  const account = result && result.account ? String(result.account) : "";
  container.replaceChildren();
  document.title = "注册成功 · OJ";

  const accountBox = h("div", { class: "account-box" }, [
    h("div", { class: "muted", text: "你的登录账号（10 位数字）" }),
    h("span", { class: "account-number", text: account }),
    h("div", {
      class: "muted",
      text: "请妥善保存该账号，登录时使用账号而非昵称。",
    }),
  ]);

  const loginButton = h("button", {
    class: "btn btn-primary",
    text: "去登录",
    attrs: { type: "button" },
  });
  loginButton.addEventListener("click", () => {
    navigate("/login?account=" + encodeURIComponent(account));
  });

  container.appendChild(
    h("div", { class: "alert alert-success", text: "注册成功！" })
  );
  container.appendChild(
    h("h1", { class: "page-title", text: "注册成功" })
  );
  container.appendChild(
    h("p", {
      class: "page-subtitle",
      text: "请使用下面分配给你的账号登录（昵称仅用于展示，不能用于登录）。",
    })
  );
  const card = h("div", { class: "card" }, [
    h("div", { class: "field" }, [
      h("label", { text: "昵称" }),
      h("div", { text: result && result.nickname ? result.nickname : "" }),
    ]),
    accountBox,
    loginButton,
  ]);
  container.appendChild(card);
}
