// 题目页：左侧题面 + 公开样例 + 难度/标签/时空限制，右侧语言选择、textarea
// 源码输入、提交按钮与判题结果。窄窗口下自动改为上下排列。
//
// 提交同步等待判题结果：请求期间禁用按钮并显示「判题中」，避免重复提交；
// 请求失败时保留编辑器中的源码、恢复可操作状态且不自动重试。
// 页面切换会取消未完成的题目读取；但提交是写请求，不随页面切换取消——
// 前端停止等待不代表后端已取消，网络失败时结果无法确认、不自动重试。

import { api } from "../api.js";
import { isLoggedIn, requiresPasswordChange } from "../auth.js";
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
    const back = h("button", {
      class: "btn btn-secondary",
      text: "返回题目列表",
      attrs: { type: "button" },
    });
    back.addEventListener("click", () => navigate(returnTarget));
    let text;
    if (error.status === 404) text = "题目不存在或你没有权限查看。";
    else if (error.status === 400) text = "题目 ID 无效，请从题目列表进入。";
    else if (error.network) text = "网络连接失败，请稍后重试。";
    else text = "题目加载失败：" + (error.message || "未知错误");
    container.appendChild(
      h("div", { class: "card state" }, [
        h("div", { class: "alert alert-error", text }),
        h("div", { attrs: { style: "margin-top:12px" } }, [back]),
      ])
    );
    return;
  }

  if (lifecycle.disposed) return;
  document.title = `${problem.title || "题目"} · OJ`;
  container.replaceChildren();
  const layout = h("div", { class: "problem-layout" }, [
    buildLeftPane(problem, returnTarget),
    buildRightPane(problem, lifecycle),
  ]);
  container.appendChild(layout);
  return () => lifecycle.dispose();
}

// 从列表进入详情时保存的列表地址（含搜索/筛选/页码），经路由校验后用于返回；
// 无有效保存值时回退默认题目列表。刷新后由 sessionStorage 恢复。
function listReturnTarget() {
  const saved = sanitizeTarget(peekProblemsReturn());
  return saved || "/problems";
}

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
    preBlock(problem.description || "", "problem-description"),
  ];

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

  return h("section", { class: "pane" }, children);
}

function buildRightPane(problem, lifecycle) {
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

  const editor = h("textarea", {
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

  const message = h("div");
  const submit = h("button", {
    class: "btn btn-primary",
    text: "提交判题",
    attrs: { type: "submit" },
  });

  const resultArea = h("div", { class: "judge-result" });

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
    editor,
    h("div", { class: "editor-actions" }, [submit, h("span", { class: "muted", text: "Ctrl + Enter 快速提交" })]),
    message,
    resultArea
  );

  const form = h("form", {}, children);
  let submitting = false;

  async function submitCode() {
    if (submitting) return;
    setMessage(message, "info", "");
    const code = editor.value;
    if (!code || !code.trim()) {
      setMessage(message, "error", "源码不能为空");
      return;
    }

    submitting = true;
    submit.disabled = true;
    const idleLabel = submit.textContent;
    submit.textContent = "判题中…";
    resultArea.replaceChildren(
      h("div", { class: "state" }, [
        h("span", { class: "spinner" }),
        h("span", { text: "正在判题，请稍候…" }),
      ])
    );

    try {
      const result = await api.post(
        `/api/problems/${encodeURIComponent(problem.id)}/submit`,
        { language: language.value, code }
      );
      // 页面已切换：结果已由后端保存，不再写入已分离的 DOM。
      if (lifecycle.disposed) return;
      renderJudgeResult(resultArea, result);
      setMessage(message, "info", "");
    } catch (error) {
      if (lifecycle.disposed) return;
      resultArea.replaceChildren();
      if (error.aborted) {
        /* 页面切换导致的取消，忽略 */
      } else if (error.network) {
        setMessage(
          message,
          "warn",
          "网络中断，无法确认本次提交结果（后端可能已保存该提交）。源码已保留，请勿重复点击，稍后可自行确认；系统不会自动重试。"
        );
      } else if (error.isPasswordChangeRequired && error.isPasswordChangeRequired()) {
        setMessage(message, "warn", "请先修改密码后再提交。");
      } else if (error.isUnavailable && error.isUnavailable()) {
        setMessage(message, "warn", error.message || "判题服务暂时不可用，请稍后重试。");
      } else {
        setMessage(message, "error", error.message || "提交失败（源码已保留）");
      }
    } finally {
      submitting = false;
      if (!lifecycle.disposed) {
        if (!(!loggedIn || mustChangePassword)) {
          submit.disabled = false;
        }
        submit.textContent = idleLabel;
      }
    }
  }

  form.addEventListener("submit", (event) => {
    event.preventDefault();
    submitCode();
  });

  editor.addEventListener("keydown", (event) => {
    if ((event.ctrlKey || event.metaKey) && event.key === "Enter") {
      event.preventDefault();
      if (loggedIn && !mustChangePassword) submitCode();
    }
  });

  return h("section", { class: "pane" }, [form]);
}

function loginHref(problemId) {
  const target = "/problems/" + problemId;
  return "#/login?redirect=" + encodeURIComponent(target);
}
