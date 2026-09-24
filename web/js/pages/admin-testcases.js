// 后台测试用例管理：读取管理员用例接口，按 ord 展示完整用例列表。
// 公开样例（is_sample=1）只读展示，由题目表单维护；隐藏用例（is_sample=0）
// 支持新增、编辑、删除与排序。内容原样保存，不做 trim 或换行归一化。
//
// 归属：所有写请求都同时携带 URL 中的题目 ID 与对应的用例 ID，避免把某题的
// 用例修改到另一题。切换题目或快速导航时通过生命周期（占位代次 + AbortSignal）
// 丢弃过期响应。

import { api } from "../api.js";
import { ensureLifecycle } from "../lifecycle.js";
import {
  confirmDialog,
  h,
  preBlock,
  setBusy,
  setMessage,
  showToast,
} from "../util.js";
import {
  adminShell,
  emptyBlock,
  handleRevoked,
  loadingBlock,
  renderRequestError,
} from "./admin-common.js";

const MAX_TEXT_BYTES = 64 * 1024;

export function renderAdminTestcases(container, context = {}) {
  const problemId = (context.params || {}).id;
  const area = adminShell(container, {
    title: `测试用例 · 题目 #${problemId}`,
    subtitle:
      "公开样例随题面展示（只读，由题目表单维护）；隐藏用例仅管理员可见，不会出现在公开响应中。",
    active: "problems",
  });

  const lifecycle = ensureLifecycle(context.lifecycle);

  async function load() {
    const current = lifecycle.next();
    area.replaceChildren(loadingBlock("用例加载中…"));
    let problem;
    let data;
    try {
      problem = await api.get("/api/problems/" + encodeURIComponent(problemId), {
        signal: lifecycle.signal,
      });
      data = await api.get(
        "/api/admin/problems/" + encodeURIComponent(problemId) + "/testcases",
        { signal: lifecycle.signal }
      );
    } catch (error) {
      if (error.aborted || !lifecycle.isCurrent(current)) return;
      renderRequestError(area, error, load);
      return;
    }
    if (!lifecycle.isCurrent(current)) return;
    area.replaceChildren(buildContent(problemId, problem, data, load));
  }

  load();
  return () => lifecycle.dispose();
}

function buildContent(problemId, problem, data, reload) {
  const cases = data && Array.isArray(data.testcases) ? data.testcases : [];
  const samples = cases.filter((c) => c.is_sample);
  const hidden = cases.filter((c) => !c.is_sample);

  const backLink = h("a", {
    class: "btn btn-secondary btn-sm",
    text: "返回题目管理",
    attrs: { href: "#/admin/problems" },
  });
  const editLink = h("a", {
    class: "btn btn-secondary btn-sm",
    text: "编辑题目 / 公开样例",
    attrs: { href: "#/admin/problems/" + problemId + "/edit" },
  });
  const refresh = h("button", {
    class: "btn btn-secondary btn-sm",
    text: "刷新",
    attrs: { type: "button" },
  });
  refresh.addEventListener("click", reload);

  const header = h("div", { class: "admin-toolbar" }, [
    h("div", {}, [
      h("span", { class: "muted", text: "题目：" }),
      h("strong", { text: problem && problem.title ? problem.title : `#${problemId}` }),
      h("span", {
        class: "badge " + (problem && problem.visible ? "badge-public" : "badge-hidden"),
        text: problem && problem.visible ? "公开" : "隐藏",
      }),
    ]),
    h("span", { class: "nav-spacer" }),
    editLink,
    backLink,
    refresh,
  ]);

  const sampleSection = h("section", { class: "admin-section" }, [
    h("h3", { text: `公开样例（${samples.length}）` }),
    h("p", {
      class: "muted",
      text: "公开样例随题面下发给所有访问者，请通过「编辑题目 / 公开样例」维护，避免两套逻辑互相覆盖。",
    }),
  ]);
  if (samples.length === 0) {
    sampleSection.appendChild(emptyBlock("暂无公开样例。"));
  } else {
    samples.forEach((c, index) => {
      sampleSection.appendChild(
        h("div", { class: "case-card readonly" }, [
          h("header", {}, [
            h("span", { class: "badge badge-public", text: "公开样例" }),
            h("span", { class: "muted", text: `#${c.id} · ord=${c.ord}` }),
            h("span", { class: "muted", text: `样例 ${index + 1}` }),
          ]),
          h("div", { class: "sample-body" }, [
            h("div", { class: "sample-col" }, [h("h4", { text: "输入" }), preBlock(c.input)]),
            h("div", { class: "sample-col" }, [h("h4", { text: "输出" }), preBlock(c.output)]),
          ]),
        ])
      );
    });
  }

  const hiddenSection = h("section", { class: "admin-section" }, [
    h("h3", { text: `隐藏用例（${hidden.length}）` }),
    h("p", {
      class: "muted",
      text: "按 (ord 升序, id 升序) 展示与判题；空串、空格与换行会原样保存，不做裁剪。",
    }),
  ]);
  if (hidden.length === 0) {
    hiddenSection.appendChild(emptyBlock("暂无隐藏用例。"));
  }
  hidden.forEach((c) => {
    hiddenSection.appendChild(buildCaseEditor(problemId, c, reload));
  });

  hiddenSection.appendChild(buildCreateForm(problemId, reload));

  return h("div", {}, [header, sampleSection, hiddenSection]);
}

function buildCaseEditor(problemId, testcase, reload) {
  const inputEl = h("textarea", { class: "case-input", attrs: { rows: "4" } });
  inputEl.value = testcase.input === undefined || testcase.input === null ? "" : testcase.input;
  const outputEl = h("textarea", { class: "case-input", attrs: { rows: "4" } });
  outputEl.value = testcase.output === undefined || testcase.output === null ? "" : testcase.output;
  const ordEl = h("input", {
    attrs: { type: "number", min: "0", max: "1000000", step: "1" },
  });
  ordEl.value = String(testcase.ord);

  const message = h("div");
  const save = h("button", {
    class: "btn btn-primary btn-sm",
    text: "保存",
    attrs: { type: "button" },
  });
  const remove = h("button", {
    class: "btn btn-danger btn-sm",
    text: "删除",
    attrs: { type: "button" },
  });

  let saving = false;
  save.addEventListener("click", async () => {
    if (saving) return;
    setMessage(message, "info", "");
    const ord = parseOrd(ordEl.value);
    if (ord === null) {
      setMessage(message, "error", "ord 需为 0..1000000 的整数");
      return;
    }
    if (byteLength(inputEl.value) > MAX_TEXT_BYTES || byteLength(outputEl.value) > MAX_TEXT_BYTES) {
      setMessage(message, "error", "输入/输出文本超过 64 KiB");
      return;
    }
    saving = true;
    setBusy(save, true, "保存中…");
    try {
      await api.put(
        `/api/admin/problems/${encodeURIComponent(problemId)}/testcases/${encodeURIComponent(
          testcase.id
        )}`,
        { input: inputEl.value, output: outputEl.value, ord }
      );
      showToast(`用例 #${testcase.id} 已保存`);
      reload();
    } catch (error) {
      saving = false;
      setBusy(save, false, null, "保存");
      setMessage(message, "error", caseErrorText(error));
      handleRevoked(error);
    }
  });

  remove.addEventListener("click", async () => {
    const ok = await confirmDialog({
      title: "确认删除该隐藏用例？",
      danger: true,
      confirmText: "删除",
      body: `将删除题目 #${problemId} 的用例 #${testcase.id}（ord=${testcase.ord}）。删除不重排、不回收空号，操作不可撤销。`,
    });
    if (!ok) return;
    setBusy(remove, true, "删除中…");
    try {
      await api.delete(
        `/api/admin/problems/${encodeURIComponent(problemId)}/testcases/${encodeURIComponent(
          testcase.id
        )}`
      );
      showToast(`用例 #${testcase.id} 已删除`);
      reload();
    } catch (error) {
      setBusy(remove, false, null, "删除");
      setMessage(message, "error", caseErrorText(error));
      handleRevoked(error);
    }
  });

  return h("div", { class: "case-card" }, [
    h("header", {}, [
      h("span", { class: "badge badge-hidden", text: "隐藏用例" }),
      h("span", { class: "muted", text: `#${testcase.id}` }),
      h("label", { class: "inline-field" }, [
        h("span", { text: "ord" }),
        ordEl,
      ]),
      h("span", { class: "nav-spacer" }),
      save,
      remove,
    ]),
    h("div", { class: "sample-body" }, [
      h("div", { class: "sample-col" }, [h("h4", { text: "输入（原样保存）" }), inputEl]),
      h("div", { class: "sample-col" }, [h("h4", { text: "输出（原样保存）" }), outputEl]),
    ]),
    message,
  ]);
}

function buildCreateForm(problemId, reload) {
  const inputEl = h("textarea", { class: "case-input", attrs: { rows: "4" } });
  const outputEl = h("textarea", { class: "case-input", attrs: { rows: "4" } });
  const ordEl = h("input", {
    attrs: { type: "number", min: "0", max: "1000000", step: "1", placeholder: "留空自动追加" },
  });
  const message = h("div");
  const add = h("button", {
    class: "btn btn-primary",
    text: "新增隐藏用例",
    attrs: { type: "button" },
  });

  let saving = false;
  add.addEventListener("click", async () => {
    if (saving) return;
    setMessage(message, "info", "");
    const payload = { input: inputEl.value, output: outputEl.value };
    if (ordEl.value.trim() !== "") {
      const ord = parseOrd(ordEl.value);
      if (ord === null) {
        setMessage(message, "error", "ord 需为 0..1000000 的整数，或留空自动追加");
        return;
      }
      payload.ord = ord;
    }
    if (
      byteLength(inputEl.value) > MAX_TEXT_BYTES ||
      byteLength(outputEl.value) > MAX_TEXT_BYTES
    ) {
      setMessage(message, "error", "输入/输出文本超过 64 KiB");
      return;
    }
    saving = true;
    setBusy(add, true, "新增中…");
    try {
      const result = await api.post(
        `/api/admin/problems/${encodeURIComponent(problemId)}/testcases`,
        payload
      );
      showToast(`已新增用例 #${result && result.id !== undefined ? result.id : ""}`);
      reload();
    } catch (error) {
      saving = false;
      setBusy(add, false, null, "新增隐藏用例");
      setMessage(message, "error", caseErrorText(error));
      handleRevoked(error);
    }
  });

  return h("div", { class: "case-card create" }, [
    h("header", {}, [h("h4", { text: "新增隐藏用例" })]),
    h("div", { class: "sample-body" }, [
      h("div", { class: "sample-col" }, [h("h4", { text: "输入" }), inputEl]),
      h("div", { class: "sample-col" }, [h("h4", { text: "输出" }), outputEl]),
    ]),
    h("div", { class: "editor-actions" }, [
      h("label", { class: "inline-field" }, [h("span", { text: "ord（可选）" }), ordEl]),
      add,
    ]),
    message,
  ]);
}

function caseErrorText(error) {
  if (error.network) {
    return "网络失败：无法确认后端是否已执行，请勿重复提交；系统不会自动重试。";
  }
  if (error.status === 404) return "题目或用例不存在 / 已被删除，或该记录属于公开样例。";
  if (error.status === 409) return error.message || "ord 分配冲突，请指定其它 ord。";
  return error.message || "操作失败";
}

function parseOrd(value) {
  const text = String(value).trim();
  if (!/^\d+$/.test(text)) return null;
  const n = parseInt(text, 10);
  if (!Number.isInteger(n) || n < 0 || n > 1000000) return null;
  return n;
}

function byteLength(text) {
  const value = String(text);
  if (typeof TextEncoder !== "undefined") {
    return new TextEncoder().encode(value).length;
  }
  return unescape(encodeURIComponent(value)).length;
}
