// 题目页（SPEC UI-02 / UI-03 / M4.3）：左侧题面、样例、限制与本人做题状态，
// 右侧 CodeMirror 编辑器、语言选择、提交按钮与逐测试点结果。窄窗口下改为上下排列。
//
// 设计要点：
//   - 题面与公开样例只来自 `GET /api/problems/{id}`（不请求管理员用例接口、
//     不预加载隐藏用例）；本人 AC 状态取自已验证身份下的同接口 `solved` 字段，
//     游客不显示，绝不通过「列表第一页是否包含该题」推断。
//   - 编辑器由 CodeMirror（CDN，固定 5.65.21）增强既有 textarea；CDN 失败或初始化
//     异常时保留 textarea 作为可编辑降级入口，页面不空白、源码不丢失。
//   - 提交按钮与 Ctrl+Enter 共用同一入口；同一时刻只有一个事件来源，且以
//     `submitting` 去重，绝不会重复提交。
//   - 提交瞬间保存本题 ID、语言与源码快照；请求期间继续编辑不会改变已发出的提交，
//     也不会把结果描述成新编辑内容的结果。
//   - 提交是写请求，不随页面切换取消（前端停止等待不代表后端取消）；页面切换后旧响应
//     不写入新页面。网络失败说明结果无法确认、不自动重试。

import { api } from "../api.js";
import { isLoggedIn, requiresPasswordChange } from "../auth.js";
import { createSourceEditor } from "../editor.js";
import { renderJudgeResult } from "../judge.js";
import { ensureLifecycle } from "../lifecycle.js";
import { navigate, sanitizeTarget } from "../router.js";
import { peekProblemsReturn } from "../storage.js";
import {
  difficultyClass,
  difficultyText,
  h,
  limitText,
  memoryText,
  preBlock,
  setMessage,
  tagList,
} from "../util.js";

export async function renderProblem(container, context = {}) {
  const id = context.params ? context.params.id : "";
  const lifecycle = ensureLifecycle(context.lifecycle);
  document.title = `题目 #${id} · OJ`;
  const returnTarget = listReturnTarget();
  container.appendChild(
    h("div", { class: "state" }, [
      h("span", { class: "spinner" }),
      h("span", { text: "题目加载中…" }),
    ])
  );

  let problem;
  try {
    problem = await api.get("/api/problems/" + encodeURIComponent(id), {
      signal: lifecycle.signal,
    });
  } catch (error) {
    if (error.aborted || lifecycle.disposed) return;
    container.replaceChildren();
    renderLoadError(container, error, returnTarget);
    return;
  }

  if (lifecycle.disposed) return;
  document.title = `${problem.title || "题目"} · OJ`;
  container.replaceChildren();

  const left = buildLeftPane(problem, returnTarget);
  const right = buildRightPane(problem, lifecycle, {
    onResult: () => refreshSolvedStatus(problem.id, left.statusEl, lifecycle),
  });
  const layout = h("div", { class: "problem-layout" }, [left.node, right.node]);
  container.appendChild(layout);
  // 布局完成后再刷新一次编辑器尺寸，避免右侧内容不可操作。
  if (typeof requestAnimationFrame === "function") {
    requestAnimationFrame(() => right.refresh());
  }
  // 页面销毁时释放编辑器实例、快捷键与监听（路由统一调用 lifecycle.dispose）。
  lifecycle.onDispose(() => right.dispose());

  return () => {
    right.dispose();
    lifecycle.dispose();
  };
}

// ---------------------------------------------------------------------------
// 加载失败 / 不存在 / 无权限状态
// ---------------------------------------------------------------------------

function renderLoadError(container, error, returnTarget) {
  let text;
  if (error.status === 404) text = "题目不存在或你没有权限查看。";
  else if (error.status === 400) text = "题目 ID 无效，请从题目列表进入。";
  else if (error.status === 401) text = "登录状态已失效，请重新登录后查看。";
  else if (error.network) text = "网络连接失败，请稍后重试。";
  else text = "题目加载失败：" + (error.message || "未知错误");
  const back = h("button", {
    class: "btn btn-secondary",
    text: "返回题目列表",
    attrs: { type: "button" },
  });
  back.addEventListener("click", () => navigate(returnTarget));
  container.appendChild(
    h("div", { class: "card state" }, [
      h("div", { class: "alert alert-error", text }),
      h("div", { attrs: { style: "margin-top:12px" } }, [back]),
    ])
  );
}

// 从列表进入详情时保存的列表地址（含搜索/筛选/页码），经路由校验后用于返回；
// 无有效保存值时回退默认题目列表。刷新后由 sessionStorage 恢复。
function listReturnTarget() {
  const saved = sanitizeTarget(peekProblemsReturn());
  return saved || "/problems";
}

// ---------------------------------------------------------------------------
// 左侧题面
// ---------------------------------------------------------------------------

function buildLeftPane(problem, returnTarget) {
  const tags = tagList(problem.tags);
  const meta = h("div", { class: "problem-meta" }, [
    h("span", {
      class: "badge " + difficultyClass(problem.difficulty),
      text: difficultyText(problem.difficulty),
    }),
    ...tags.map((tag) => h("span", { class: "tag", text: tag })),
  ]);

  const back = h("a", {
    class: "back-link",
    text: "← 返回题目列表",
    attrs: { href: "#" + returnTarget },
  });

  const children = [
    back,
    h("h1", { class: "page-title", text: problem.title || "" }),
    meta,
    h("div", {
      class: "limits",
      text: `时间限制：${limitText(problem.time_limit_ms)}　·　内存限制：${memoryText(
        problem.memory_limit_kb
      )}`,
    }),
  ];

  // 本人状态：仅登录用户显示；游客不显示。数据来自详情接口的 `solved`。
  const statusEl = isLoggedIn() ? h("div", { class: "problem-status" }) : null;
  if (statusEl) {
    updateSolvedStatus(statusEl, problem.solved);
    children.push(statusEl);
    // 本题提交入口（M4.4）：链接到提交历史并按 problem_id 由后端筛选，而不是
    // 仅筛选当前页数据；题目本身仍由题目接口执行可见性检查。
    children.push(
      h("a", {
        class: "back-link",
        text: "查看本题提交记录 →",
        attrs: {
          href:
            "#/submissions?problem_id=" +
            encodeURIComponent(String(problem.id)),
        },
      })
    );
  }

  children.push(preBlock(problem.description || "", "problem-description"));

  const samples = Array.isArray(problem.samples) ? problem.samples : [];
  if (samples.length > 0) {
    children.push(h("h3", { text: "公开样例" }));
    samples.forEach((sample, index) => {
      children.push(
        h("div", { class: "sample" }, [
          h("div", { class: "sample-title", text: `样例 ${index + 1}` }),
          h("div", { class: "sample-body" }, [
            h("div", { class: "sample-col" }, [
              h("h4", { text: "输入" }),
              preBlock(sample.input === undefined ? "" : sample.input),
            ]),
            h("div", { class: "sample-col" }, [
              h("h4", { text: "输出" }),
              preBlock(sample.output === undefined ? "" : sample.output),
            ]),
          ]),
        ])
      );
    });
  }

  return { node: h("section", { class: "pane" }, children), statusEl };
}

function updateSolvedStatus(statusEl, solved) {
  if (!statusEl) return;
  statusEl.replaceChildren();
  const known = solved === true || solved === false;
  const isSolved = solved === true;
  statusEl.appendChild(h("span", { class: "muted", text: "本人状态：" }));
  statusEl.appendChild(
    h("span", {
      class: "badge " + (isSolved ? "status-AC" : ""),
      text: !known ? "未知" : isSolved ? "已 AC" : "未 AC",
      attrs: {
        title: !known
          ? "暂无本人状态数据"
          : isSolved
          ? "本人已通过该题"
          : "本人尚未通过该题",
      },
    })
  );
}

// 判题结果更新后，从可靠数据来源（题目详情接口的 `solved`）刷新本人状态。
// 当前提交 WA 不代表历史 AC 失效——状态始终以后端 user_problem_status 为准，
// 不把本次总体结果直接映射为永久状态。刷新失败不覆盖已展示内容。
async function refreshSolvedStatus(problemId, statusEl, lifecycle) {
  if (!statusEl || !isLoggedIn() || lifecycle.disposed) return;
  try {
    const fresh = await api.get("/api/problems/" + encodeURIComponent(problemId), {
      signal: lifecycle.signal,
    });
    if (lifecycle.disposed) return;
    // 仅在拿到明确的布尔状态时更新，避免身份变化等异常响应把状态显示为未知。
    if (fresh && (fresh.solved === true || fresh.solved === false)) {
      updateSolvedStatus(statusEl, fresh.solved);
    }
  } catch (error) {
    /* 状态刷新非关键路径：失败时保留原展示，不误导为未 AC。 */
  }
}

// ---------------------------------------------------------------------------
// 右侧做题区
// ---------------------------------------------------------------------------

function buildRightPane(problem, lifecycle, hooks = {}) {
  const loggedIn = isLoggedIn();
  const mustChangePassword = requiresPasswordChange();

  const language = h(
    "select",
    { attrs: { id: "submit-language" } },
    [
      h("option", { text: "C++17", attrs: { value: "cpp17" } }),
      h("option", { text: "C11", attrs: { value: "c11" } }),
    ]
  );

  const editorHost = h("div", { class: "code-editor-host" });
  const textarea = h("textarea", {
    class: "source-editor",
    text: "",
    attrs: {
      id: "source-code",
      spellcheck: "false",
      autocomplete: "off",
      autocapitalize: "off",
      placeholder: "在此输入源代码…（Ctrl + Enter 提交）",
    },
  });
  editorHost.appendChild(textarea);

  const editorStatus = h("div", { class: "editor-status" });
  editorStatus.appendChild(
    h("span", { class: "muted", text: "正在初始化代码编辑器…" })
  );

  const message = h("div");
  const submit = h("button", {
    class: "btn btn-primary",
    text: "提交判题",
    attrs: { type: "submit" },
  });

  const resultArea = h("div", { class: "judge-result" });

  // 编辑器：在 textarea 基础上增强；失败时 textarea 保持可编辑。
  const editor = createSourceEditor({
    textarea,
    initialLanguage: language.value,
    onShortcut: () => submitCode(),
  });
  editor.ready.then((outcome) => {
    if (lifecycle.disposed) return;
    if (outcome && outcome.ok) {
      setMessage(editorStatus, "info", "");
      return;
    }
    // CDN 加载失败 / 初始化异常：明确提示并保留可编辑的 textarea。
    editorStatus.replaceChildren(
      h("div", {
        class: "alert alert-warn",
        text: "代码编辑器（CodeMirror）加载失败，已切换到基础文本编辑框，源码仍可正常提交。",
      })
    );
    if (outcome && outcome.error && outcome.error.message) {
      editorStatus.appendChild(
        h("div", { class: "muted editor-status-detail", text: outcome.error.message })
      );
    }
  });

  const children = [h("h3", { text: "提交代码" })];

  if (!loggedIn) {
    submit.disabled = true;
    const loginLink = h("a", { text: "去登录", attrs: { href: loginHref(problem.id) } });
    children.push(
      h("div", { class: "alert alert-info" }, [
        h("span", { text: "提交前请先登录。游客可以浏览公开题目。" }),
        loginLink,
      ])
    );
  } else if (mustChangePassword) {
    submit.disabled = true;
    children.push(
      h("div", { class: "alert alert-warn" }, [
        h("span", { text: "首次登录需先修改密码后才能提交。" }),
        h("a", { text: "去修改密码", attrs: { href: "#/password" } }),
      ])
    );
  }

  children.push(
    h("div", { class: "editor-toolbar" }, [
      h("label", { text: "语言", attrs: { for: "submit-language" } }),
      language,
    ]),
    editorHost,
    editorStatus,
    h("div", { class: "editor-actions" }, [
      submit,
      h("span", { class: "muted", text: "Ctrl + Enter 快速提交" }),
    ]),
    message,
    resultArea
  );

  const form = h("form", {}, children);
  let submitting = false;

  function canSubmit() {
    return loggedIn && !mustChangePassword;
  }

  async function submitCode() {
    if (submitting) return;
    if (!loggedIn) {
      setMessage(message, "info", "请先登录后再提交。");
      return;
    }
    if (mustChangePassword) {
      setMessage(message, "warn", "请先修改密码后再提交。");
      return;
    }

    // 提交瞬间快照：题目 ID、语言与源码。请求期间继续编辑不会改变已发出的提交，
    // 结果也只描述本次快照，而不是之后的新编辑内容。
    const snapshot = {
      problemId: problem.id,
      language: language.value,
      code: editor.getValue(),
    };
    const code = snapshot.code === null || snapshot.code === undefined
      ? ""
      : String(snapshot.code);
    if (!code.trim()) {
      setMessage(message, "error", "源码不能为空");
      return;
    }
    snapshot.code = code;

    submitting = true;
    submit.disabled = true;
    const idleLabel = submit.textContent;
    submit.textContent = "判题中…";
    setMessage(message, "info", "");
    resultArea.replaceChildren(
      h("div", { class: "state" }, [
        h("span", { class: "spinner" }),
        h("span", { text: "正在等待判题结果，请稍候…" }),
      ])
    );

    try {
      const result = await api.post(
        `/api/problems/${encodeURIComponent(snapshot.problemId)}/submit`,
        { language: snapshot.language, code: snapshot.code }
      );
      // 页面已切换：结果已由后端保存，不再写入已分离的 DOM。
      if (lifecycle.disposed) return;
      renderJudgeResult(resultArea, result);
      if (typeof hooks.onResult === "function") hooks.onResult();
    } catch (error) {
      if (lifecycle.disposed) return;
      resultArea.replaceChildren();
      handleSubmitError(message, error);
    } finally {
      submitting = false;
      if (!lifecycle.disposed) {
        submit.textContent = idleLabel;
        submit.disabled = !canSubmit();
      }
    }
  }

  form.addEventListener("submit", (event) => {
    event.preventDefault();
    if (canSubmit()) submitCode();
  });

  // 降级文本框的快捷键（编辑器就绪后文本域被隐藏，不会与 CodeMirror 快捷键
  // 同时触发；即便触发，`submitting` 也会阻止重复提交）。
  textarea.addEventListener("keydown", (event) => {
    if ((event.ctrlKey || event.metaKey) && event.key === "Enter") {
      event.preventDefault();
      if (canSubmit()) submitCode();
    }
  });

  // 切换语言只更新编辑器模式，绝不清空用户源码。
  language.addEventListener("change", () => {
    editor.setLanguage(language.value);
  });

  return {
    node: h("section", { class: "pane" }, [form]),
    refresh: () => editor.refresh(),
    dispose: () => editor.dispose(),
  };
}

function handleSubmitError(message, error) {
  if (error && error.aborted) return;
  if (error && typeof error.isPasswordChangeRequired === "function" && error.isPasswordChangeRequired()) {
    setMessage(message, "warn", "请先修改密码后再提交。");
  } else if (error && error.network) {
    setMessage(
      message,
      "warn",
      "网络中断，无法确认本次提交结果（后端可能已接收并保存该提交）。源码与语言选择已保留，系统不会自动重试，请稍后自行确认。"
    );
  } else if (error && typeof error.isUnavailable === "function" && error.isUnavailable()) {
    setMessage(message, "warn", error.message || "判题服务暂时不可用，请稍后重试。");
  } else if (error && error.status === 401) {
    setMessage(message, "warn", "登录状态已失效，请重新登录后再提交。");
  } else if (error && error.status === 429) {
    setMessage(message, "warn", error.message || "请求过于频繁，请稍后重试。");
  } else if (error && error.status === 404) {
    setMessage(message, "error", "题目不存在或已被删除，本次提交未被受理。");
  } else if (error && typeof error.isForbidden === "function" && error.isForbidden()) {
    setMessage(message, "error", error.message || "没有权限向该题目提交。");
  } else {
    setMessage(
      message,
      "error",
      (error && error.message) || "提交失败，源码已保留，请稍后重试。"
    );
  }
}

function loginHref(problemId) {
  const target = "/problems/" + problemId;
  return "#/login?redirect=" + encodeURIComponent(target);
}
