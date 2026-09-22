// 后台用户管理：用户列表（分页）、重置密码、修改角色。
// 密码重置使用密码输入控件，不显示明文、不写日志、不做浏览器持久化，成功后清空。
// 角色修改前展示目标用户、原角色与新角色并确认；自我降级成功后立即更新本地状态并
// 退出后台。真正权限与「最后一名管理员」保护由后端接口执行。

import { api } from "../api.js";
import { getUser } from "../auth.js";
import { navigate } from "../router.js";
import {
  confirmDialog,
  h,
  openModal,
  pagination,
  setBusy,
  setMessage,
  showToast,
} from "../util.js";
import {
  adminShell,
  emptyBlock,
  handleRevoked,
  loadingBlock,
  refreshCurrentUser,
  renderRequestError,
} from "./admin-common.js";

function roleText(role) {
  if (role === "admin") return "管理员";
  if (role === "user") return "普通用户";
  return role || "未知";
}

export function renderAdminUsers(container, context) {
  const area = adminShell(container, {
    title: "用户管理",
    subtitle: "可重置密码与修改角色；不提供删除用户功能。",
    active: "users",
  });

  const state = { page: parsePage(context.query.get("page")) };

  const listArea = h("div");
  area.appendChild(listArea);

  let disposed = false;
  let token = 0;

  function updateQuery() {
    navigate("/admin/users" + (state.page > 1 ? "?page=" + state.page : ""));
  }

  async function load() {
    const current = ++token;
    listArea.replaceChildren(loadingBlock("用户加载中…"));
    let data;
    try {
      data = await api.get("/api/admin/users?page=" + state.page);
    } catch (error) {
      if (disposed || current !== token) return;
      renderRequestError(listArea, error, load);
      return;
    }
    if (disposed || current !== token) return;

    const users = data && Array.isArray(data.users) ? data.users : [];
    listArea.appendChild(
      h("div", { class: "muted list-count", text: `共 ${data.total ?? users.length} 名用户` })
    );
    if (users.length === 0) {
      listArea.appendChild(emptyBlock("暂无用户。"));
      return;
    }
    listArea.appendChild(buildTable(users, manageReset, manageRole, () => load()));
    listArea.appendChild(
      pagination(state.page, data.total_pages || 1, (next) => {
        state.page = next;
        updateQuery();
      })
    );
  }

  // 自我相关变更后同步本地用户状态，必要时退出后台。
  async function afterMutation(targetUserId) {
    const me = getUser();
    if (!me || me.id !== targetUserId) {
      load();
      return;
    }
    const updated = await refreshCurrentUser();
    if (!updated || updated.reset_pwd_flag) {
      showToast("请先完成密码修改");
      navigate("/password", { replace: true });
      return;
    }
    if (updated.role !== "admin") {
      showToast("已更新你的角色，现已退出后台管理");
      navigate("/problems", { replace: true });
      return;
    }
    load();
  }

  async function manageReset(user) {
    await openResetPasswordDialog(user, afterMutation);
  }

  async function manageRole(user) {
    const newRole = user.role === "admin" ? "user" : "admin";
    const me = getUser();
    const isSelf = me && me.id === user.id;
    const body = [
      `目标用户：#${user.id}　账号：${user.account}　昵称：${user.nickname}`,
      `原角色：${roleText(user.role)}　→　新角色：${roleText(newRole)}`,
    ];
    if (isSelf) {
      body.push("注意：这是你本人账号的自我降级，确认后将立即失去后台管理权限。");
    }
    const ok = await confirmDialog({
      title: "确认修改角色？",
      confirmText: "确认修改",
      body: body.join("\n"),
    });
    if (!ok) return;
    try {
      await api.put("/api/admin/users", {
        action: "change_role",
        user_id: user.id,
        role: newRole,
      });
      showToast(`用户 #${user.id} 的角色已改为「${roleText(newRole)}」`);
      await afterMutation(user.id);
    } catch (error) {
      if (error.status === 409) {
        await confirmDialog({
          title: "无法修改角色",
          body: error.message || "不能取消最后一个管理员的权限。",
          confirmText: "知道了",
          cancelText: "关闭",
        });
      } else {
        showToast(errorMessage(error));
      }
      handleRevoked(error);
    }
  }

  load();
  return () => {
    disposed = true;
    token++;
  };
}

function buildTable(users, onReset, onRole) {
  const header = h("tr", {}, [
    h("th", { text: "#" }),
    h("th", { text: "账号" }),
    h("th", { text: "昵称" }),
    h("th", { text: "角色" }),
    h("th", { text: "需改密" }),
    h("th", { text: "注册时间" }),
    h("th", { text: "操作" }),
  ]);
  const body = h("tbody");
  for (const user of users) {
    const resetBtn = h("button", {
      class: "btn btn-secondary btn-sm",
      text: "重置密码",
      attrs: { type: "button" },
    });
    resetBtn.addEventListener("click", () => onReset(user));

    const roleBtn = h("button", {
      class: "btn btn-secondary btn-sm",
      text: user.role === "admin" ? "降为普通用户" : "设为管理员",
      attrs: { type: "button" },
    });
    roleBtn.addEventListener("click", () => onRole(user));

    body.appendChild(
      h("tr", {}, [
        h("td", { text: String(user.id) }),
        h("td", { text: user.account || "" }),
        h("td", { text: user.nickname || "" }),
        h("td", {}, [
          h("span", {
            class: "badge " + (user.role === "admin" ? "badge-role-admin" : "badge-role-user"),
            text: roleText(user.role),
          }),
        ]),
        h("td", { text: user.reset_pwd_flag ? "是" : "否" }),
        h("td", { text: user.created_at || "—" }),
        h("td", {}, [h("div", { class: "row-actions" }, [resetBtn, roleBtn])]),
      ])
    );
  }
  return h("div", { class: "table-wrap" }, [
    h("table", { class: "data" }, [h("thead", {}, [header]), body]),
  ]);
}

// 重置密码对话框：使用密码输入控件，成功后清空并关闭。
function openResetPasswordDialog(user, afterMutation) {
  return new Promise((resolve) => {
    const password = h("input", {
      attrs: {
        type: "password",
        id: "admin-reset-password",
        autocomplete: "new-password",
        maxlength: "128",
      },
    });
    const confirm = h("input", {
      attrs: {
        type: "password",
        id: "admin-reset-confirm",
        autocomplete: "new-password",
        maxlength: "128",
      },
    });
    const message = h("div");
    const submit = h("button", {
      class: "btn btn-primary",
      text: "重置密码",
      attrs: { type: "submit" },
    });
    const cancel = h("button", {
      class: "btn btn-secondary",
      text: "取消",
      attrs: { type: "button" },
    });

    const form = h("form", { class: "form" }, [
      h("p", { class: "muted", text: `目标用户：#${user.id}　账号：${user.account}　昵称：${user.nickname}` }),
      h("div", {
        class: "alert alert-warn",
        text: "重置后该用户需在下次登录后先修改密码（沿用既有强制改密策略）。无需旧密码。",
      }),
      h("div", { class: "field" }, [
        h("label", { text: "新密码", attrs: { for: "admin-reset-password" } }),
        password,
        h("span", { class: "hint", text: "非空、≤128，不裁剪不截断；不会记录到日志或本地存储" }),
      ]),
      h("div", { class: "field" }, [
        h("label", { text: "确认新密码", attrs: { for: "admin-reset-confirm" } }),
        confirm,
      ]),
      message,
      h("div", { class: "modal-actions" }, [cancel, submit]),
    ]);

    const { close } = openModal(form, { onClose: () => resolve() });
    cancel.addEventListener("click", () => close());
    setTimeout(() => password.focus(), 0);

    let saving = false;
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      if (saving) return;
      setMessage(message, "info", "");
      if (!password.value) {
        setMessage(message, "error", "新密码不能为空");
        return;
      }
      if (password.value !== confirm.value) {
        setMessage(message, "error", "两次输入的新密码不一致");
        return;
      }
      saving = true;
      setBusy(submit, true, "重置中…");
      try {
        await api.put("/api/admin/users", {
          action: "reset_password",
          user_id: user.id,
          new_password: password.value,
        });
        // 成功后立即清空输入，避免密码残留在 DOM 中。
        password.value = "";
        confirm.value = "";
        close();
        showToast(`用户 #${user.id} 的密码已重置，需下次登录后修改密码`);
        await afterMutation(user.id);
        resolve();
      } catch (error) {
        saving = false;
        setBusy(submit, false, null, "重置密码");
        setMessage(message, "error", errorMessage(error));
        handleRevoked(error);
      }
    });
  });
}

function errorMessage(error) {
  if (error.network) {
    return "网络失败：无法确认后端是否已执行，请勿重复提交，系统不会自动重试。";
  }
  if (error.status === 404) return "目标用户不存在或已被删除。";
  return error.message || "操作失败";
}

function parsePage(value) {
  const n = parseInt(value, 10);
  if (!Number.isInteger(n) || n < 1) return 1;
  return n;
}
