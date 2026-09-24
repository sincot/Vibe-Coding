// 后台题目管理：列表（分页、可见性筛选）、创建/编辑表单、公开/隐藏与删除。
// 复用现有 API（GET /api/problems、GET /api/problems/{id}、POST/PUT/DELETE
// /api/admin/problems[/{id}]）。前端只做基本校验，最终以服务端校验为准。

import { api } from "../api.js";
import { ensureLifecycle } from "../lifecycle.js";
import { navigate } from "../router.js";
import {
  confirmDialog,
  difficultyClass,
  difficultyText,
  h,
  pagination,
  setBusy,
  setMessage,
  showToast,
  tagList,
} from "../util.js";
import {
  adminShell,
  emptyBlock,
  handleRevoked,
  loadingBlock,
  renderRequestError,
} from "./admin-common.js";

const VISIBILITY_OPTIONS = [
  { value: "all", label: "全部（含隐藏）" },
  { value: "1", label: "仅公开" },
  { value: "0", label: "仅隐藏" },
];

const DIFFICULTY_OPTIONS = [
  { value: "", label: "全部难度" },
  { value: "easy", label: "易" },
  { value: "medium", label: "中" },
  { value: "hard", label: "难" },
];

export function renderAdminHome(container) {
  document.title = "后台管理 · OJ";
  container.replaceChildren();
  container.appendChild(h("h1", { class: "page-title", text: "后台管理" }));
  container.appendChild(
    h("p", {
      class: "page-subtitle",
      text: "管理题目、测试用例与用户。所有写入操作最终由后端校验并落库。",
    })
  );
  container.appendChild(
    h("div", { class: "admin-home" }, [
      adminCard("题目管理", "创建、编辑、公开/隐藏与删除题目，维护公开样例。", "/admin/problems"),
      adminCard("测试用例管理", "为指定题目录入、编辑、排序与删除隐藏测试用例。", "/admin/problems"),
      adminCard("用户管理", "查看用户列表，重置密码与修改角色。", "/admin/users"),
      adminCard("重判", "按提交 ID 使用当前题目配置重新判题，更新结果与状态统计。", "/admin/rejudge"),
    ])
  );
}

function adminCard(title, desc, path) {
  return h("a", { class: "card admin-card", attrs: { href: "#" + path } }, [
    h("h3", { text: title }),
    h("p", { class: "muted", text: desc }),
  ]);
}

export function renderAdminProblems(container, context = {}) {
  const area = adminShell(container, {
    title: "题目管理",
    subtitle: "可查看公开与隐藏题目，并按可见性筛选。",
    active: "problems",
  });

  const query = context.query || new URLSearchParams();
  const state = {
    page: parsePage(query.get("page")),
    visible: normalizeVisibility(query.get("visible")),
    q: query.get("q") || "",
    difficulty: query.get("difficulty") || "",
  };

  const createBtn = h("a", {
    class: "btn btn-primary",
    text: "新建题目",
    attrs: { href: "#/admin/problems/new" },
  });

  const toolbar = h("div", { class: "admin-toolbar" }, [
    createBtn,
    h("span", { class: "nav-spacer" }),
  ]);
  const listArea = h("div");
  area.appendChild(toolbar);
  area.appendChild(listArea);

  const lifecycle = ensureLifecycle(context.lifecycle);

  function updateQuery() {
    const params = new URLSearchParams();
    if (state.page > 1) params.set("page", String(state.page));
    if (state.visible !== "all") params.set("visible", state.visible);
    if (state.q) params.set("q", state.q);
    if (state.difficulty) params.set("difficulty", state.difficulty);
    const qs = params.toString();
    navigate("/admin/problems" + (qs ? "?" + qs : ""));
  }

  function buildFilters() {
    const search = h("input", {
      attrs: { type: "text", value: state.q, placeholder: "按标题搜索…" },
    });
    const difficulty = selectInput(
      DIFFICULTY_OPTIONS.map((o) => [o.value, o.label]),
      state.difficulty
    );
    const visible = selectInput(
      VISIBILITY_OPTIONS.map((o) => [o.value, o.label]),
      state.visible
    );
    const apply = h("button", {
      class: "btn btn-secondary",
      text: "筛选",
      attrs: { type: "button" },
    });
    apply.addEventListener("click", () => {
      state.q = search.value.trim();
      state.difficulty = difficulty.value;
      state.visible = visible.value;
      state.page = 1;
      updateQuery();
    });
    search.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        apply.click();
      }
    });
    return h("div", { class: "admin-filters" }, [
      h("div", { class: "filter-field grow" }, [
        h("label", { text: "搜索标题" }),
        search,
      ]),
      h("div", { class: "filter-field" }, [
        h("label", { text: "难度" }),
        difficulty,
      ]),
      h("div", { class: "filter-field" }, [
        h("label", { text: "可见性" }),
        visible,
      ]),
      h("div", { class: "filter-actions" }, [apply]),
    ]);
  }

  async function load() {
    const token = lifecycle.next();
    listArea.replaceChildren(buildFilters());
    listArea.appendChild(loadingBlock("题目加载中…"));
    const params = new URLSearchParams();
    params.set("page", String(state.page));
    params.set("visible", state.visible);
    if (state.q) params.set("q", state.q);
    if (state.difficulty) params.set("difficulty", state.difficulty);

    let data;
    try {
      data = await api.get("/api/problems?" + params.toString(), {
        signal: lifecycle.signal,
      });
    } catch (error) {
      if (error.aborted || !lifecycle.isCurrent(token)) return;
      renderRequestError(listArea, error, load);
      return;
    }
    if (!lifecycle.isCurrent(token)) return;

    const problems = data && Array.isArray(data.problems) ? data.problems : [];
    listArea.appendChild(h("div", { class: "muted list-count", text: `共 ${data.total ?? problems.length} 道题` }));
    if (problems.length === 0) {
      listArea.appendChild(emptyBlock("没有符合条件的题目。"));
      return;
    }
    listArea.appendChild(buildTable(problems, load));
    listArea.appendChild(
      pagination(state.page, data.total_pages || 1, (next) => {
        state.page = next;
        updateQuery();
      })
    );
  }

  load();
  return () => lifecycle.dispose();
}

function buildTable(problems, reload) {
  const header = h("tr", {}, [
    h("th", { text: "#" }),
    h("th", { text: "标题" }),
    h("th", { text: "难度" }),
    h("th", { text: "标签" }),
    h("th", { text: "可见性" }),
    h("th", { text: "通过人数" }),
    h("th", { text: "操作" }),
  ]);
  const body = h("tbody");
  for (const problem of problems) {
    body.appendChild(buildRow(problem, reload));
  }
  return h("div", { class: "table-wrap" }, [
    h("table", { class: "data" }, [h("thead", {}, [header]), body]),
  ]);
}

function buildRow(problem, reload) {
  const tags = tagList(problem.tags);
  const visibilityBadge = h("span", {
    class: "badge " + (problem.visible ? "badge-public" : "badge-hidden"),
    text: problem.visible ? "公开" : "隐藏",
  });

  const editLink = h("a", {
    class: "btn btn-secondary btn-sm",
    text: "编辑",
    attrs: { href: "#/admin/problems/" + problem.id + "/edit" },
  });
  const casesLink = h("a", {
    class: "btn btn-secondary btn-sm",
    text: "用例",
    attrs: { href: "#/admin/problems/" + problem.id + "/testcases" },
  });

  const toggleBtn = h("button", {
    class: "btn btn-secondary btn-sm",
    text: problem.visible ? "设为隐藏" : "设为公开",
    attrs: { type: "button" },
  });
  toggleBtn.addEventListener("click", async () => {
    const target = !problem.visible;
    const ok = await confirmDialog({
      title: target ? "设为公开？" : "设为隐藏？",
      body: `题目 #${problem.id}「${problem.title || ""}」将${
        target ? "对普通用户可见" : "对普通用户不可见（管理员仍可见）"
      }。已有提交与做题状态不受影响。`,
      confirmText: target ? "设为公开" : "设为隐藏",
    });
    if (!ok) return;
    setBusy(toggleBtn, true, "处理中…");
    try {
      await api.put("/api/admin/problems/" + problem.id, { visible: target });
      showToast(target ? "已设为公开" : "已设为隐藏");
      reload();
    } catch (error) {
      setBusy(toggleBtn, false, null, target ? "设为公开" : "设为隐藏");
      showToast(adminErrorText(error));
      handleRevoked(error);
    }
  });

  const deleteBtn = h("button", {
    class: "btn btn-danger btn-sm",
    text: "删除",
    attrs: { type: "button" },
  });
  deleteBtn.addEventListener("click", async () => {
    const ok = await confirmDialog({
      title: "确认删除题目？",
      danger: true,
      confirmText: "删除",
      body: [
        `题目 #${problem.id}「${problem.title || ""}」将被删除。`,
        "删除策略：若该题已有任何提交记录或正在判题的任务，后端会拒绝删除并说明原因；",
        "两者都没有时，将同时删除该题的全部测试用例（公开样例与隐藏用例）、",
        "做题状态记录与题目本身。此操作不可撤销。",
      ].join("\n"),
    });
    if (!ok) return;
    setBusy(deleteBtn, true, "删除中…");
    try {
      await api.delete("/api/admin/problems/" + problem.id);
      showToast("题目已删除");
      reload();
    } catch (error) {
      setBusy(deleteBtn, false, null, "删除");
      if (error.status === 409) {
        await confirmDialog({
          title: "无法删除",
          body:
            error.message ||
            "该题目已有提交记录或正在判题的任务，按既定删除策略（保留学生提交历史）不能删除。",
          confirmText: "知道了",
          cancelText: "关闭",
        });
      } else {
        showToast(adminErrorText(error));
      }
      handleRevoked(error);
    }
  });

  return h("tr", {}, [
    h("td", { text: String(problem.id) }),
    h("td", {}, [
      h("a", {
        text: problem.title || "",
        attrs: { href: "#/admin/problems/" + problem.id + "/edit" },
      }),
    ]),
    h("td", {}, [
      h("span", {
        class: "badge " + difficultyClass(problem.difficulty),
        text: difficultyText(problem.difficulty),
      }),
    ]),
    h(
      "td",
      {},
      tags.length
        ? tags.map((tag) => h("span", { class: "tag", text: tag }))
        : [h("span", { class: "muted", text: "—" })]
    ),
    h("td", {}, [visibilityBadge]),
    h("td", {
      text: problem.pass_count === null || problem.pass_count === undefined
        ? "—"
        : String(problem.pass_count),
    }),
    h("td", {}, [
      h("div", { class: "row-actions" }, [editLink, casesLink, toggleBtn, deleteBtn]),
    ]),
  ]);
}

function adminErrorText(error) {
  if (error.network) return "网络失败：无法确认后端是否已执行，请勿重复操作。";
  if (error.status === 404) return "记录不存在或已被删除。";
  return error.message || "操作失败";
}

function parsePage(value) {
  const n = parseInt(value, 10);
  if (!Number.isInteger(n) || n < 1) return 1;
  return n;
}

function normalizeVisibility(value) {
  if (value === "1" || value === "true") return "1";
  if (value === "0" || value === "false") return "0";
  return "all";
}

// ---------------------------------------------------------------------------
// 创建 / 编辑表单
// ---------------------------------------------------------------------------

export function renderAdminProblemForm(container, context = {}) {
  const id = (context.params || {}).id;
  const isEdit = id !== undefined && id !== null && id !== "";
  const area = adminShell(container, {
    title: isEdit ? `编辑题目 #${id}` : "新建题目",
    subtitle: "字段沿用后端约定：时限单位为毫秒（ms），内存上限单位为千字节（KB）。",
    active: "problems",
  });

  const lifecycle = ensureLifecycle(context.lifecycle);
  (async () => {
    area.replaceChildren(loadingBlock("加载题目中…"));
    let problem = null;
    if (isEdit) {
      try {
        problem = await api.get("/api/problems/" + encodeURIComponent(id), {
          signal: lifecycle.signal,
        });
      } catch (error) {
        if (error.aborted || lifecycle.disposed) return;
        renderRequestError(area, error, () => renderAdminProblemForm(container, context));
        return;
      }
      if (lifecycle.disposed) return;
    }
    area.replaceChildren(buildForm(problem, isEdit, id, () => lifecycle.disposed));
  })();

  return () => lifecycle.dispose();
}

function buildForm(problem, isEdit, id, isDisposed) {
  const p = problem || {};
  const title = inputText(p.title || "");
  title.id = "admin-problem-title";
  const difficulty = selectInput(
    [
      ["easy", "易"],
      ["medium", "中"],
      ["hard", "难"],
    ],
    p.difficulty || "easy"
  );
  difficulty.id = "admin-problem-difficulty";
  const description = textArea(p.description || "", 10);
  description.id = "admin-problem-description";
  const tags = inputText(Array.isArray(p.tags) ? p.tags.join(",") : p.tags || "");
  tags.id = "admin-problem-tags";
  const timeLimit = inputNumber(
    p.time_limit_ms === undefined ? 2000 : p.time_limit_ms,
    1,
    60000
  );
  timeLimit.id = "admin-problem-time";
  const memoryLimit = inputNumber(
    p.memory_limit_kb === undefined ? 65536 : p.memory_limit_kb,
    1,
    1048576
  );
  memoryLimit.id = "admin-problem-memory";
  const visible = h("input", {
    attrs: { type: "checkbox", id: "admin-problem-visible" },
  });
  visible.checked = p.visible === undefined ? true : !!p.visible;

  const samplesWrap = h("div", { class: "samples-editor" });
  const sampleRows = [];
  function addSampleRow(initial) {
    const init = initial || { input: "", output: "" };
    const inputEl = textArea(init.input === undefined ? "" : String(init.input), 3);
    const outputEl = textArea(init.output === undefined ? "" : String(init.output), 3);
    const removeBtn = h("button", {
      class: "btn btn-danger btn-sm",
      text: "移除该样例",
      attrs: { type: "button" },
    });
    const row = { inputEl, outputEl, node: null };
    const node = h("div", { class: "sample-edit" }, [
      h("div", { class: "sample-edit-head" }, [
        h("span", { class: "muted", text: "公开样例（随题面展示）" }),
        removeBtn,
      ]),
      h("div", { class: "sample-edit-body" }, [
        h("div", { class: "sample-col" }, [h("h4", { text: "输入" }), inputEl]),
        h("div", { class: "sample-col" }, [h("h4", { text: "输出" }), outputEl]),
      ]),
    ]);
    row.node = node;
    removeBtn.addEventListener("click", () => {
      const index = sampleRows.indexOf(row);
      if (index >= 0) sampleRows.splice(index, 1);
      node.remove();
    });
    sampleRows.push(row);
    samplesWrap.appendChild(node);
  }
  const initialSamples = Array.isArray(p.samples) ? p.samples : [];
  initialSamples.forEach((sample) => addSampleRow(sample));

  const addSampleBtn = h("button", {
    class: "btn btn-secondary",
    text: "添加公开样例",
    attrs: { type: "button" },
  });
  addSampleBtn.addEventListener("click", () => addSampleRow());

  const message = h("div");
  const submit = h("button", {
    class: "btn btn-primary",
    text: isEdit ? "保存修改" : "创建题目",
    attrs: { type: "submit" },
  });
  const cancel = h("a", {
    class: "btn btn-secondary",
    text: "取消",
    attrs: { href: "#/admin/problems" },
  });

  const form = h("form", { class: "form admin-form" }, [
    h("div", { class: "admin-form-grid" }, [
      h("div", { class: "field" }, [
        h("label", { text: "标题", attrs: { for: "admin-problem-title" } }),
        title,
        h("span", { class: "hint", text: "必填；去首尾空白后非空，≤ 200 字节" }),
      ]),
      h("div", { class: "field" }, [
        h("label", { text: "难度", attrs: { for: "admin-problem-difficulty" } }),
        difficulty,
      ]),
    ]),
    h("div", { class: "field" }, [
      h("label", { text: "题面描述（纯文本）", attrs: { for: "admin-problem-description" } }),
      description,
      h("span", { class: "hint", text: "纯文本，不渲染 Markdown；≤ 64 KiB" }),
    ]),
    h("div", { class: "field" }, [
      h("label", { text: "标签", attrs: { for: "admin-problem-tags" } }),
      tags,
      h("span", {
        class: "hint",
        text: "多个标签用英文逗号分隔；单个标签不含逗号、≤ 30 字节，最多 20 个",
      }),
    ]),
    h("div", { class: "admin-form-grid" }, [
      h("div", { class: "field" }, [
        h("label", { text: "时间限制（毫秒 ms）", attrs: { for: "admin-problem-time" } }),
        timeLimit,
        h("span", { class: "hint", text: "范围 1..60000 ms；默认 2000 ms" }),
      ]),
      h("div", { class: "field" }, [
        h("label", { text: "内存上限（千字节 KB）", attrs: { for: "admin-problem-memory" } }),
        memoryLimit,
        h("span", { class: "hint", text: "范围 1..1048576 KB；默认 65536 KB" }),
      ]),
    ]),
    h("div", { class: "field checkbox-field" }, [
      h("label", {}, [visible, h("span", { text: "公开（普通用户可见；取消勾选则仅管理员可见）" })]),
    ]),
    h("div", { class: "field" }, [
      h("label", { text: "公开样例" }),
      samplesWrap,
      h("div", {}, [addSampleBtn]),
      h("span", {
        class: "hint",
        text: "公开样例随题面下发；隐藏测试用例请到「用例」页面维护。保存时整体替换公开样例。",
      }),
    ]),
    message,
    h("div", { class: "editor-actions" }, [submit, cancel]),
  ]);

  let saving = false;

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (saving) return;
    setMessage(message, "info", "");

    const validation = validateProblemForm({
      title,
      difficulty,
      description,
      tags,
      timeLimit,
      memoryLimit,
      sampleRows,
      visible,
    });
    if (!validation.ok) {
      setMessage(message, "error", validation.error);
      return;
    }

    saving = true;
    setBusy(submit, true, "保存中…");
    try {
      if (isEdit) {
        await api.put("/api/admin/problems/" + encodeURIComponent(id), validation.payload);
        showToast("题目已保存");
      } else {
        const created = await api.post("/api/admin/problems", validation.payload);
        showToast("题目已创建" + (created && created.id ? `（#${created.id}）` : ""));
      }
      if (!isDisposed()) navigate("/admin/problems");
    } catch (error) {
      setBusy(submit, false, null, isEdit ? "保存修改" : "创建题目");
      saving = false;
      setMessage(message, "error", adminErrorText(error) + "（表单内容已保留）");
      handleRevoked(error);
    }
  });

  return form;
}

function validateProblemForm({
  title,
  difficulty,
  description,
  tags,
  timeLimit,
  memoryLimit,
  sampleRows,
  visible,
}) {
  const titleValue = title.value.trim();
  if (!titleValue) return { ok: false, error: "标题不能为空" };
  if (byteLength(titleValue) > 200) return { ok: false, error: "标题超过 200 字节" };
  if (!["easy", "medium", "hard"].includes(difficulty.value)) {
    return { ok: false, error: "请选择难度" };
  }

  const tagParts = tags.value
    .split(",")
    .map((t) => t.trim())
    .filter(Boolean);
  if (tagParts.length > 20) return { ok: false, error: "标签最多 20 个" };
  const seen = new Set();
  for (const tag of tagParts) {
    if (byteLength(tag) > 30) return { ok: false, error: `标签「${tag}」超过 30 字节` };
    if (seen.has(tag)) return { ok: false, error: `标签「${tag}」重复` };
    seen.add(tag);
  }

  const time = parseIntField(timeLimit.value);
  if (!Number.isInteger(time) || time < 1 || time > 60000) {
    return { ok: false, error: "时间限制需为 1..60000 之间的整数（毫秒）" };
  }
  const memory = parseIntField(memoryLimit.value);
  if (!Number.isInteger(memory) || memory < 1 || memory > 1048576) {
    return { ok: false, error: "内存上限需为 1..1048576 之间的整数（KB）" };
  }

  if (sampleRows.length > 50) return { ok: false, error: "公开样例最多 50 组" };
  const samples = sampleRows.map((row) => ({
    input: row.inputEl.value,
    output: row.outputEl.value,
  }));

  return {
    ok: true,
    payload: {
      title: titleValue,
      difficulty: difficulty.value,
      description: description ? description.value : "",
      tags: tagParts,
      time_limit_ms: time,
      memory_limit_kb: memory,
      visible: visible ? visible.checked : true,
      samples,
    },
  };
}

function byteLength(text) {
  const value = String(text);
  if (typeof TextEncoder !== "undefined") {
    return new TextEncoder().encode(value).length;
  }
  return unescape(encodeURIComponent(value)).length;
}

function parseIntField(value) {
  const text = String(value).trim();
  if (!/^-?\d+$/.test(text)) return NaN;
  return parseInt(text, 10);
}

// ---------------------------------------------------------------------------
// 表单控件工厂
// ---------------------------------------------------------------------------

function inputText(value) {
  const el = h("input", { attrs: { type: "text" } });
  el.value = value === null || value === undefined ? "" : String(value);
  return el;
}

function inputNumber(value, min, max) {
  const el = h("input", {
    attrs: { type: "number", min: String(min), max: String(max), step: "1" },
  });
  el.value = value === null || value === undefined ? "" : String(value);
  return el;
}

function textArea(value, rows) {
  const el = h("textarea", { attrs: { rows: String(rows || 3) } });
  el.value = value === null || value === undefined ? "" : String(value);
  return el;
}

function selectInput(options, selected) {
  const el = h(
    "select",
    {},
    options.map(([value, label]) =>
      h("option", { text: label, attrs: { value } })
    )
  );
  el.value = selected;
  return el;
}
