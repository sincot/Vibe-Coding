// 提交详情页（SPEC UI-01 / M4.4）：展示数据库保存的完整源码与判题结果。
//
// 设计要点：
//   - 普通用户只能查看本人记录，管理员可查看他人记录；越权/不存在统一由后端
//     按既有约定处理（404），前端只做展示，不据客户端角色放行；
//   - 只读取 `GET /api/submissions/{id}` 返回的持久化结果，不重新判题、不按当前
//     用例重拼历史 WA；损坏的逐点结果以 per_case_parse_error 明确标记；
//   - 源码用只读文本区域展示（安全纯文本），本页不提供提交入口，浏览历史不会误提交；
//   - 未持久化的指标（如编译耗时）显示为「未采集」，不将 null 显示成真实的 0；
//   - 本人该题当前状态来自 `GET /api/status?problem_id=`，与详情页合并展示，避免
//     在已有页面重复发起状态查询；
//   - 管理员可就地重判：成功后重新读取详情与状态，失败时保留原结果展示。

import { api } from "../api.js";
import { isAdmin, isLoggedIn, subscribeAuth } from "../auth.js";
import { renderJudgeResult } from "../judge.js";
import { ensureLifecycle } from "../lifecycle.js";
import { navigate, sanitizeTarget } from "../router.js";
import { peekSubmissionsReturn } from "../storage.js";
import {
  confirmDialog,
  h,
  languageText,
  setBusy,
  setMessage,
  showToast,
  statusBadge,
} from "../util.js";
import { adminErrorMessage, handleRevoked } from "./admin-common.js";

export function renderSubmissionDetail(container, context = {}) {
  const id = context.params ? context.params.id : "";
  const lifecycle = ensureLifecycle(context.lifecycle);
  const returnTarget = historyReturnTarget();
  document.title = `提交 #${id} · OJ`;

  let currentRequest = null;

  async function load() {
    const token = lifecycle.next();
    if (currentRequest) {
      try {
        currentRequest.abort();
      } catch (error) {
        /* 忽略重复取消 */
      }
    }
    const controller =
      typeof AbortController !== "undefined" ? new AbortController() : null;
    currentRequest = controller;

    renderLoading(container);
    let detail;
    try {
      detail = await api.get("/api/submissions/" + encodeURIComponent(id), {
        signal: controller ? controller.signal : lifecycle.signal,
      });
    } catch (error) {
      if (error.aborted || !lifecycle.isCurrent(token)) return;
      renderLoadError(container, error, returnTarget);
      return;
    }
    if (!lifecycle.isCurrent(token)) return;
    renderDetail(container, detail, { lifecycle, returnTarget, reload: load });
  }

  function cleanup() {
    if (currentRequest) {
      try {
        currentRequest.abort();
      } catch (error) {
        /* 忽略 */
      }
    }
    lifecycle.dispose();
  }

  // 路由对异步页面可能不会调用返回的清理函数，注册到生命周期兜底，保证取消读取。
  lifecycle.onDispose(() => {
    if (currentRequest) {
      try {
        currentRequest.abort();
      } catch (error) {
        /* 忽略 */
      }
    }
  });

  // 身份变化（退出/切换）时立即清除受保护内容，避免残留他人数据。
  const unsubscribe = subscribeAuth(() => {
    if (lifecycle.disposed) return;
    if (!isLoggedIn()) container.replaceChildren();
  });

  load();
  return () => {
    unsubscribe();
    cleanup();
  };
}

// ---------------------------------------------------------------------------
// 加载 / 错误状态
// ---------------------------------------------------------------------------

function renderLoading(container) {
  container.replaceChildren(
    h("div", { class: "state" }, [
      h("span", { class: "spinner" }),
      h("span", { text: "提交详情加载中…" }),
    ])
  );
}

function renderLoadError(container, error, returnTarget) {
  let text;
  if (error.status === 404) text = "提交记录不存在或你没有权限查看。";
  else if (error.status === 400) text = "提交 ID 无效，请从提交历史进入。";
  else if (error.status === 401) text = "登录状态已失效，请重新登录后查看。";
  else if (error.network) text = "网络连接失败，请稍后重试。";
  else text = "提交详情加载失败：" + (error.message || "未知错误");

  const back = h("button", {
    class: "btn btn-secondary",
    text: "返回提交历史",
    attrs: { type: "button" },
  });
  back.addEventListener("click", () => navigate(returnTarget));
  container.replaceChildren(
    h("div", { class: "card state" }, [
      h("div", { class: "alert alert-error", text }),
      h("div", { attrs: { style: "margin-top:12px" } }, [back]),
    ])
  );
}

// ---------------------------------------------------------------------------
// 详情主体
// ---------------------------------------------------------------------------

function renderDetail(container, detail, options) {
  const { lifecycle, returnTarget, reload } = options;
  container.replaceChildren();

  const problemId = detail.problem_id;
  const title = cleanText(detail.problem_title);
  const problemLink = h("a", {
    text: title || "#" + problemId,
    attrs: { href: "#/problems/" + encodeURIComponent(String(problemId)) },
  });

  const back = h("a", {
    class: "back-link",
    text: "← 返回提交历史",
    attrs: { href: "#" + returnTarget },
  });

  container.appendChild(back);
  container.appendChild(
    h("h1", { class: "page-title" }, [
      h("span", { text: "提交 " }),
      h("span", { text: "#" + detail.id }),
    ])
  );

  container.appendChild(
    h("div", { class: "detail-meta" }, [
      statusBadge(detail.status),
      h("span", { class: "muted", text: "题目：" }),
      problemLink,
      h("span", { class: "muted", text: "语言：" + languageText(detail.language) }),
      h("span", { class: "muted", text: "提交时间：" + (detail.created_at || "未提供") }),
    ])
  );

  // 本人该题当前状态：无记录表示未提交/未 AC，不伪造状态行。
  if (isLoggedIn()) {
    const statusEl = h("div", { class: "problem-status" }, [
      h("span", { class: "muted", text: "本人该题状态：加载中…" }),
    ]);
    container.appendChild(statusEl);
    loadProblemStatus(problemId, statusEl, lifecycle);
  }

  // 源码：只读文本区域，纯文本展示，保留原始换行；本页不提供提交入口。
  container.appendChild(h("h3", { text: "源码" }));
  container.appendChild(
    h("div", { class: "muted", text: "以下为提交时保存的源码（只读，本页不提供提交入口）。" })
  );
  container.appendChild(buildSourceView(detail.source_code));

  const resultArea = h("div", { class: "judge-result" });

  if (detail.per_case_parse_error) {
    container.appendChild(
      h("div", {
        class: "alert alert-warn",
        text: "该提交保存的逐点结果无法解析（数据可能损坏），仅能展示总体状态与编译信息，逐点详情不可用。",
      })
    );
    // 即使逐点不可解析，仍展示总体状态、编译信息与指标（renderJudgeResult 仅会
    // 因 results 为空而不显示逐点表格，不会把异常记录显示为 AC 或正常空结果）。
  }

  container.appendChild(resultArea);
  renderJudgeResult(resultArea, detail);

  if (isAdmin()) {
    container.appendChild(buildRejudgeSection(detail, reload));
  }
}

function buildSourceView(sourceCode) {
  const text =
    sourceCode === undefined || sourceCode === null ? "" : String(sourceCode);
  const textarea = h("textarea", {
    class: "source-readonly",
    attrs: {
      readonly: "readonly",
      spellcheck: "false",
      autocomplete: "off",
      autocapitalize: "off",
      "aria-label": "提交源码（只读）",
      wrap: "off",
    },
  });
  textarea.value = text;
  return h("div", { class: "source-readonly-wrap" }, [textarea]);
}

function renderProblemStatus(statusEl, record) {
  statusEl.replaceChildren();
  statusEl.appendChild(h("span", { class: "muted", text: "本人该题状态：" }));
  if (!record) {
    // 无状态记录 = 从未提交，按未 AC 展示（不是接口失败）。
    statusEl.appendChild(
      h("span", {
        class: "badge",
        text: "未 AC（无提交记录）",
        attrs: { title: "暂无该题的做题状态记录" },
      })
    );
    return;
  }
  const accepted = record.status === "accepted";
  statusEl.appendChild(
    h("span", {
      class: "badge " + (accepted ? "status-AC" : ""),
      text: accepted ? "已 AC" : "未 AC",
      attrs: { title: accepted ? "本人已通过该题" : "本人尚未通过该题" },
    })
  );
  const count =
    Number.isFinite(record.submit_count) && record.submit_count >= 0
      ? record.submit_count
      : null;
  if (count !== null) {
    statusEl.appendChild(
      h("span", { class: "muted", text: `提交 ${count} 次` })
    );
  }
  if (accepted && record.first_ac_at) {
    statusEl.appendChild(
      h("span", { class: "muted", text: `首次 AC：${record.first_ac_at}` })
    );
  }
}

async function loadProblemStatus(problemId, statusEl, lifecycle) {
  if (!problemId) {
    renderProblemStatus(statusEl, null);
    return;
  }
  try {
    const data = await api.get(
      "/api/status?problem_id=" + encodeURIComponent(String(problemId)),
      { signal: lifecycle.signal }
    );
    if (lifecycle.disposed) return;
    const list = data && Array.isArray(data.statuses) ? data.statuses : [];
    const record = list.find(
      (item) => Number(item.problem_id) === Number(problemId)
    );
    renderProblemStatus(statusEl, record);
  } catch (error) {
    if (error.aborted || lifecycle.disposed) return;
    statusEl.replaceChildren(
      h("span", { class: "muted", text: "本人该题状态：暂时无法获取" })
    );
  }
}

// ---------------------------------------------------------------------------
// 管理员重判（复用既有入口；不新增全站提交管理平台）
// ---------------------------------------------------------------------------

function buildRejudgeSection(detail, reload) {
  const button = h("button", {
    class: "btn btn-secondary",
    text: "重判此提交",
    attrs: { type: "button" },
  });
  const message = h("div");

  button.addEventListener("click", async () => {
    const confirmed = await confirmDialog({
      title: "确认重判提交？",
      body: `即将对提交 #${detail.id} 发起重判。原提交结果将被覆盖为该次重判结果，该用户该题的做题状态将按最新结果重新计算，且不增加提交次数。`,
      confirmText: "开始重判",
    });
    if (!confirmed) return;

    setMessage(message, "info", "正在重判，请稍候…");
    setBusy(button, true, "重判中…");
    try {
      await api.post(
        "/api/admin/submissions/" + encodeURIComponent(String(detail.id)) + "/rejudge",
        {}
      );
      showToast("重判完成，正在刷新详情");
      // 重判成功后重新读取详情与状态；成功提示由 toast 承担，页面将整体重绘。
      if (typeof reload === "function") await reload();
    } catch (error) {
      if (error.network) {
        setMessage(
          message,
          "warn",
          "网络连接失败，无法确认重判结果，请检查网络后自行确认。系统不会自动重试。"
        );
      } else if (error.status === 409 && error.code === "REJUDGE_IN_PROGRESS") {
        setMessage(message, "warn", "该提交正在重判中，请等待完成后再试。");
      } else if (error.status === 503) {
        setMessage(message, "warn", error.message || "判题服务暂时不可用，请稍后重试。");
      } else if (error.status === 500) {
        setMessage(
          message,
          "warn",
          "本次重判未能完成（如判题环境故障），原结果与统计保持不变。"
        );
      } else {
        setMessage(message, "error", adminErrorMessage(error));
      }
      handleRevoked(error);
    } finally {
      setBusy(button, false, null, "重判此提交");
    }
  });

  return h("div", { class: "detail-admin" }, [
    h("h3", { text: "管理员操作" }),
    h("div", { class: "muted", text: "重判使用该提交保存的源码与当前题目配置重新判题，更新原记录且不增加提交次数。" }),
    h("div", { class: "state-actions" }, [button]),
    message,
  ]);
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

function cleanText(value) {
  return value === undefined || value === null ? "" : String(value);
}

function historyReturnTarget() {
  const saved = sanitizeTarget(peekSubmissionsReturn());
  return saved || "/submissions";
}
