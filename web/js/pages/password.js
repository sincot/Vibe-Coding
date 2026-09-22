// 改密页：沿用后端字段 old_password / new_password 与密码规则。
// 管理员首次登录（reset_pwd_flag=1）需在此完成改密后才能进行其他受限操作。
// 改密不撤销后端已签发的 JWT；本页在成功后回查 /api/me 刷新本地用户状态。

import { api } from "../api.js";
import { getUser, setUser } from "../auth.js";
import { renderNav } from "../nav.js";
import { navigate } from "../router.js";
import { field, h, setBusy, setMessage, showToast } from "../util.js";

export function renderPassword(container) {
  document.title = "修改密码 · OJ";
  const user = getUser();
  const mustChange = !!(user && user.reset_pwd_flag);

  const oldPassword = h("input", {
    attrs: {
      type: "password",
      id: "pwd-old",
      autocomplete: "current-password",
    },
  });
  const newPassword = h("input", {
    attrs: {
      type: "password",
      id: "pwd-new",
      autocomplete: "new-password",
      maxlength: "128",
    },
  });
  const confirm = h("input", {
    attrs: {
      type: "password",
      id: "pwd-confirm",
      autocomplete: "new-password",
      maxlength: "128",
    },
  });
  const message = h("div");
  const submit = h("button", {
    class: "btn btn-primary btn-block",
    text: "修改密码",
    attrs: { type: "submit" },
  });

  const form = h("form", { class: "form" }, [
    field("旧密码", oldPassword, ""),
    field("新密码", newPassword, "非空、长度不超过 128，且不能与旧密码相同"),
    field("确认新密码", confirm, ""),
    message,
    submit,
  ]);

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    setMessage(message, "info", "");
    if (!oldPassword.value || !newPassword.value) {
      setMessage(message, "error", "请填写旧密码与新密码");
      return;
    }
    if (newPassword.value !== confirm.value) {
      setMessage(message, "error", "两次输入的新密码不一致");
      return;
    }
    if (newPassword.value === oldPassword.value) {
      setMessage(message, "error", "新密码不能与旧密码相同");
      return;
    }

    setBusy(submit, true, "提交中…");
    try {
      await api.post(
        "/api/me/password",
        {
          old_password: oldPassword.value,
          new_password: newPassword.value,
        },
        // 旧密码错误的 401 属于表单错误，不应触发退出登录。
        { skipAuthRedirect: true }
      );
      // 回查当前用户，刷新 reset_pwd_flag，使后续权限判断使用最新状态。
      try {
        const me = await api.get("/api/me");
        setUser(me);
      } catch (error) {
        /* 回查失败不阻断：改密本身已成功 */
      }
      renderNav();
      showToast("密码修改成功，可继续操作（后端 JWT 未撤销，仍按原策略有效）");
      navigate("/problems");
    } catch (error) {
      setBusy(submit, false, null, "修改密码");
      if (error.status === 401) {
        setMessage(message, "error", "旧密码错误");
      } else {
        setMessage(message, "error", error.message || "修改密码失败");
      }
    }
  });

  if (mustChange) {
    container.appendChild(
      h("div", {
        class: "alert alert-warn",
        text: "首次登录需要先修改密码，完成后才能使用其余功能。",
      })
    );
  }
  container.appendChild(h("h1", { class: "page-title", text: "修改密码" }));
  container.appendChild(
    h("p", {
      class: "page-subtitle",
      text: "修改成功后，后端已签发的旧 token 在过期前仍然有效（当前无会话撤销机制）。",
    })
  );
  container.appendChild(h("div", { class: "card" }, [form]));
}
