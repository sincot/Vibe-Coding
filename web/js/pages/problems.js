// 题目列表页（SPEC 2.6.1 / M4.2）：展示题目 ID、本人做题状态、标题、难度、标签与
// 通过人数，支持关键词搜索、难度/标签组合筛选与分页；管理员额外提供可见性筛选并标识
// 隐藏题目。
//
// 设计要点：
//   - 搜索/筛选/页码全部由 hash 路由查询串承载（`#/problems?q=&difficulty=&tag=&page=`，
//     管理员另加 `visible=`），刷新、浏览器前进后退、从题目详情返回都可恢复；
//   - 条件变化统一回第 1 页；空值不发送，界面「全部」不是实际标签/难度；
//   - 标签选项来自 `GET /api/problem-tags`（按当前身份可见范围去重排序），不从前端
//     分页结果拼凑，翻页/重新请求失败不会使已选标签消失；
//   - 读取请求可被取消或按代次丢弃，只有最新结果更新列表、总数与分页；
//   - 身份/角色/首改状态变化后按最新身份重建，退出或降级即回到合法普通列表条件。

import { api } from "../api.js";
import { getUser, isAdmin, isLoggedIn, subscribeAuth } from "../auth.js";
import { ensureLifecycle } from "../lifecycle.js";
import { navigate } from "../router.js";
import { saveProblemsReturn } from "../storage.js";
import {
  compactPagination,
  difficultyClass,
  difficultyText,
  h,
  tagList,
} from "../util.js";

const DIFFICULTY_VALUES = ["easy", "medium", "hard"];
const VISIBLE_VALUES = ["all", "1", "0"];
const MAX_PAGE = 1000000;
const DEFAULT_PAGE_SIZE = 20;

// 模块级标签选项缓存：同一会话内翻页或条件变化时先复用，后台再刷新，
// 避免标签下拉框因一次请求失败或翻页而瞬间变空。
// 按可见范围分桶（管理员含隐藏、普通/游客仅公开），避免退出/降级后把管理员
// 缓存中仅隐藏题目使用的标签短暂展示给无权查看者。
const cachedTags = { admin: null, public: null };

function tagScope(admin) {
  return admin ? "admin" : "public";
}

export function renderProblems(container, context = {}) {
  document.title = "题目列表 · OJ";
  const lifecycle = ensureLifecycle(context.lifecycle);
  container.replaceChildren();

  const query = context.query || new URLSearchParams();
  const admin = isAdmin();
  const loggedIn = isLoggedIn();

  // 从路由查询串解析条件；非法难度/可见性归一为「不限」，非法页码归一为 1，
  // 避免把非法值发给后端导致反复 400 或页面一直加载。
  const state = {
    q: (query.get("q") || "").trim(),
    difficulty: normalizeDifficulty(query.get("difficulty")),
    tag: (query.get("tag") || "").trim(),
    page: parsePage(query.get("page")),
    visible: admin ? normalizeVisible(query.get("visible")) : "all",
  };

  const listArea = h("div");
  container.appendChild(h("h1", { class: "page-title", text: "题目列表" }));
  container.appendChild(
    h("p", {
      class: "page-subtitle",
      text: "点击题目进入详情；游客可浏览公开题目，提交前需先登录。",
    })
  );

  const tagSelect = h("select", {
    attrs: { id: "problem-tag-filter" },
  });
  const scope = tagScope(admin);
  fillTagOptions(tagSelect, state.tag, cachedTags[scope]);

  const filters = buildFilters({ state, admin, tagSelect, apply, loggedIn });
  container.appendChild(filters);
  container.appendChild(listArea);

  let inFlight = null;

  function apply(patch) {
    const next = { ...state, ...patch };
    navigate(buildListHash(next, admin));
  }

  function resetFilters() {
    apply({ q: "", difficulty: "", tag: "", visible: "all", page: 1 });
  }

  async function load() {
    const token = lifecycle.next();
    if (inFlight) {
      try {
        inFlight.abort();
      } catch (error) {
        /* 忽略重复取消 */
      }
    }
    const controller =
      typeof AbortController !== "undefined" ? new AbortController() : null;
    inFlight = controller;

    renderLoading(listArea);

    const params = buildParams(state, admin);
    const queryString = params.toString();
    const url = "/api/problems" + (queryString ? "?" + queryString : "");
    let data;
    try {
      data = await api.get(url, {
        signal: controller ? controller.signal : lifecycle.signal,
      });
    } catch (error) {
      if (error.aborted || !lifecycle.isCurrent(token)) return;
      renderError(listArea, error, load);
      return;
    }
    if (!lifecycle.isCurrent(token)) return;

    const problems = data && Array.isArray(data.problems) ? data.problems : [];
    const total = Number.isFinite(data && data.total) ? data.total : problems.length;
    const totalPages =
      Number.isFinite(data && data.total_pages) && data.total_pages >= 0
        ? data.total_pages
        : 0;
    const pageSize =
      Number.isFinite(data && data.page_size) && data.page_size > 0
        ? data.page_size
        : DEFAULT_PAGE_SIZE;

    // 超出末页（含筛选后为空但页码 > 1）：修正到合法页码并重取一次，避免空白或循环。
    const targetPage = totalPages >= 1 ? Math.min(state.page, totalPages) : 1;
    if (targetPage !== state.page) {
      navigate(buildListHash({ ...state, page: targetPage }, admin), {
        replace: true,
      });
      return;
    }

    if (total === 0) {
      renderEmpty(listArea, hasActiveFilters(state, admin), resetFilters);
      return;
    }
    renderResult(listArea, {
      state,
      admin,
      loggedIn,
      problems,
      total,
      totalPages,
      pageSize,
      onPage: (page) => apply({ page }),
    });
  }

  async function loadTags() {
    try {
      const data = await api.get("/api/problem-tags", {
        signal: lifecycle.signal,
      });
      if (lifecycle.disposed) return;
      const tags =
        data && Array.isArray(data.tags)
          ? data.tags.filter((tag) => typeof tag === "string" && tag !== "")
          : [];
      cachedTags[scope] = tags;
      fillTagOptions(tagSelect, state.tag, cachedTags[scope]);
    } catch (error) {
      // 标签选项是非关键路径：失败时保留已有缓存与已选标签，不打断列表展示。
      // 401 等身份失效仍由 api.js 统一处理，这里不额外跳转。
    }
  }

  // 身份/角色/首改状态变化：按最新身份重建页面，普通用户清除管理员可见性条件。
  let authSignature = currentAuthSignature();
  const unsubscribe = subscribeAuth(() => {
    if (lifecycle.disposed) return;
    const next = currentAuthSignature();
    if (next === authSignature) return;
    authSignature = next;
    const cleaned = isAdmin() ? state : { ...state, visible: "all" };
    navigate(buildListHash(cleaned, isAdmin()), { replace: true });
  });

  load();
  loadTags();

  return () => {
    unsubscribe();
    if (inFlight) {
      try {
        inFlight.abort();
      } catch (error) {
        /* 忽略 */
      }
    }
    lifecycle.dispose();
  };
}

// ---------------------------------------------------------------------------
// 筛选栏
// ---------------------------------------------------------------------------

function buildFilters({ state, admin, tagSelect, apply, loggedIn }) {
  const searchInput = h("input", {
    attrs: {
      type: "text",
      id: "problem-search",
      placeholder: "输入关键字…",
      autocomplete: "off",
    },
  });
  searchInput.value = state.q;

  const searchButton = h("button", {
    class: "btn btn-primary",
    text: "搜索",
    attrs: { type: "button" },
  });
  const doSearch = () => apply({ q: searchInput.value.trim(), page: 1 });
  searchButton.addEventListener("click", doSearch);
  searchInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      doSearch();
    }
  });

  const clearButton = h("button", {
    class: "btn btn-secondary",
    text: "清空条件",
    attrs: { type: "button" },
  });
  clearButton.addEventListener("click", () =>
    apply({ q: "", difficulty: "", tag: "", visible: "all", page: 1 })
  );

  const difficultySelect = selectInput(
    [
      ["", "全部难度"],
      ["easy", "易"],
      ["medium", "中"],
      ["hard", "难"],
    ],
    state.difficulty
  );
  difficultySelect.id = "problem-difficulty-filter";
  difficultySelect.addEventListener("change", () =>
    apply({ q: searchInput.value.trim(), difficulty: difficultySelect.value, page: 1 })
  );

  tagSelect.addEventListener("change", () =>
    apply({ q: searchInput.value.trim(), tag: tagSelect.value, page: 1 })
  );

  const fields = [
    h("div", { class: "filter-field grow" }, [
      h("label", { text: "搜索", attrs: { for: "problem-search" } }),
      searchInput,
    ]),
    h("div", { class: "filter-field" }, [
      h("label", { text: "难度", attrs: { for: "problem-difficulty-filter" } }),
      difficultySelect,
    ]),
    h("div", { class: "filter-field" }, [
      h("label", { text: "标签", attrs: { for: "problem-tag-filter" } }),
      tagSelect,
    ]),
  ];

  if (admin) {
    const visibleSelect = selectInput(
      [
        ["all", "全部"],
        ["1", "公开"],
        ["0", "隐藏"],
      ],
      state.visible
    );
    visibleSelect.id = "problem-visible-filter";
    visibleSelect.addEventListener("change", () =>
      apply({ q: searchInput.value.trim(), visible: visibleSelect.value, page: 1 })
    );
    fields.push(
      h("div", { class: "filter-field" }, [
        h("label", { text: "可见性", attrs: { for: "problem-visible-filter" } }),
        visibleSelect,
      ])
    );
  }

  fields.push(h("div", { class: "filter-actions" }, [searchButton, clearButton]));

  const wrap = h("div", { class: "problem-filters" }, fields);
  if (!loggedIn) {
    wrap.appendChild(
      h("p", {
        class: "filter-hint muted",
        text: "登录后可查看本人 AC 状态并按账号记录做题进度。",
      })
    );
  }
  return wrap;
}

function fillTagOptions(select, selected, tags) {
  select.replaceChildren();
  select.appendChild(h("option", { text: "全部标签", attrs: { value: "" } }));
  const values = Array.isArray(tags)
    ? tags.filter((tag) => typeof tag === "string" && tag !== "")
    : [];
  // 已选标签即使在当前选项中缺失（缓存未加载/已被删除）也保留，条件仍可理解。
  if (selected && !values.includes(selected)) values.push(selected);
  for (const tag of values) {
    select.appendChild(h("option", { text: tag, attrs: { value: tag } }));
  }
  select.value = selected || "";
}

// ---------------------------------------------------------------------------
// 列表状态
// ---------------------------------------------------------------------------

function renderLoading(area) {
  area.replaceChildren(
    h("div", { class: "state" }, [
      h("span", { class: "spinner" }),
      h("span", { text: "题目加载中…" }),
    ])
  );
}

function renderEmpty(area, hasFilters, onReset) {
  if (!hasFilters) {
    area.replaceChildren(
      h("div", { class: "card state", text: "题库暂时没有题目，请等待管理员发布。" })
    );
    return;
  }
  const reset = h("button", {
    class: "btn btn-secondary",
    text: "清空筛选条件",
    attrs: { type: "button" },
  });
  reset.addEventListener("click", onReset);
  area.replaceChildren(
    h("div", { class: "card state" }, [
      h("div", { text: "没有符合当前条件的题目，请调整关键词或筛选条件。" }),
      h("div", { attrs: { style: "margin-top:12px" } }, [reset]),
    ])
  );
}

function renderError(area, error, retry) {
  const message = error.network
    ? "网络连接失败，无法加载题目列表。"
    : error.message || "加载失败";
  const retryButton = h("button", {
    class: "btn btn-secondary",
    text: "重试",
    attrs: { type: "button" },
  });
  retryButton.addEventListener("click", retry);
  area.replaceChildren(
    h("div", { class: "card state" }, [
      h("div", { class: "alert alert-error", text: "题目加载失败：" + message }),
      h("div", { attrs: { style: "margin-top:12px" } }, [retryButton]),
    ])
  );
}

function renderResult(area, options) {
  const {
    state,
    admin,
    loggedIn,
    problems,
    total,
    totalPages,
    pageSize,
    onPage,
  } = options;

  const currentPage = state.page;
  const countText =
    totalPages > 1
      ? `共 ${total} 道题　·　第 ${currentPage} / ${totalPages} 页（每页 ${pageSize} 条）`
      : `共 ${total} 道题`;

  const nodes = [h("div", { class: "muted list-count", text: countText })];
  nodes.push(buildTable(problems, admin, loggedIn));

  if (totalPages > 1) {
    nodes.push(compactPagination(currentPage, totalPages, onPage));
  }

  area.replaceChildren(...nodes);
}

function buildTable(problems, admin, loggedIn) {
  const headCells = [h("th", { text: "#" })];
  if (loggedIn) headCells.push(h("th", { text: "状态" }));
  headCells.push(h("th", { text: "标题" }));
  headCells.push(h("th", { text: "难度" }));
  headCells.push(h("th", { text: "标签" }));
  headCells.push(h("th", { text: "通过人数" }));
  if (admin) headCells.push(h("th", { text: "可见性" }));

  const header = h("tr", {}, headCells);
  const body = h("tbody");

  for (const problem of problems) {
    body.appendChild(buildRow(problem, admin, loggedIn));
  }

  const table = h("table", { class: "data problem-table" }, [
    h("thead", {}, [header]),
    body,
  ]);
  return h("div", { class: "table-wrap" }, [table]);
}

function buildRow(problem, admin, loggedIn) {
  const tags = tagList(problem.tags);
  const cells = [
    h("td", { text: String(problem.id) }),
  ];

  if (loggedIn) {
    const solved = !!problem.solved;
    cells.push(
      h("td", {}, [
        h("span", {
          class: "badge " + (solved ? "status-AC" : ""),
          text: solved ? "AC" : "未AC",
          attrs: { title: solved ? "本人已通过" : "本人尚未通过" },
        }),
      ])
    );
  }

  const title = problem.title || "";
  const link = h("a", {
    text: title,
    attrs: { href: "#/problems/" + problem.id },
  });
  link.addEventListener("click", (event) => {
    if (
      event.button !== 0 ||
      event.metaKey ||
      event.ctrlKey ||
      event.shiftKey ||
      event.altKey
    ) {
      return; // 交给浏览器：新标签/新窗口打开
    }
    event.preventDefault();
    goToProblem(problem.id);
  });
  cells.push(h("td", { class: "col-title" }, [link]));

  cells.push(
    h("td", {}, [
      h("span", {
        class: "badge " + difficultyClass(problem.difficulty),
        text: difficultyText(problem.difficulty),
      }),
    ])
  );

  cells.push(
    h(
      "td",
      { class: "col-tags" },
      tags.length
        ? tags.map((tag) => h("span", { class: "tag", text: tag }))
        : [h("span", { class: "muted", text: "—" })]
    )
  );

  cells.push(
    h("td", {
      text:
        problem.pass_count === null || problem.pass_count === undefined
          ? "—"
          : String(problem.pass_count),
    })
  );

  if (admin) {
    const visible = !!problem.visible;
    cells.push(
      h("td", {}, [
        h("span", {
          class: "badge " + (visible ? "badge-public" : "badge-hidden"),
          text: visible ? "公开" : "隐藏",
        }),
      ])
    );
  }

  const row = h("tr", { class: "clickable" }, cells);
  row.addEventListener("click", (event) => {
    if (event.defaultPrevented) return;
    const target = event.target;
    // 行内可交互元素（链接/按钮等）自行处理，不误触发行跳转。
    if (
      target &&
      typeof target.closest === "function" &&
      target.closest("a, button, input, select, textarea, label")
    ) {
      return;
    }
    goToProblem(problem.id);
  });
  return row;
}

// 进入题目详情：先记录当前列表地址，供详情页「返回题目列表」恢复查询条件与页码。
function goToProblem(id) {
  const current =
    typeof location !== "undefined" && location.hash ? location.hash : "#/problems";
  saveProblemsReturn(current);
  navigate("/problems/" + id);
}

// ---------------------------------------------------------------------------
// 路由 / 参数工具
// ---------------------------------------------------------------------------

// 由当前条件构造列表 hash 地址；空值与默认值不写入查询串。
function buildListHash(state, admin) {
  const params = buildParams(state, admin);
  const qs = params.toString();
  return "/problems" + (qs ? "?" + qs : "");
}

function buildParams(state, admin) {
  const params = new URLSearchParams();
  if (state.q) params.set("q", state.q);
  if (state.difficulty) params.set("difficulty", state.difficulty);
  if (state.tag) params.set("tag", state.tag);
  if (state.page > 1) params.set("page", String(state.page));
  if (admin && state.visible && state.visible !== "all") {
    params.set("visible", state.visible);
  }
  return params;
}

function hasActiveFilters(state, admin) {
  return !!(
    state.q ||
    state.difficulty ||
    state.tag ||
    (admin && state.visible !== "all")
  );
}

function normalizeDifficulty(value) {
  const text = String(value || "").trim();
  return DIFFICULTY_VALUES.includes(text) ? text : "";
}

function normalizeVisible(value) {
  const text = String(value || "").trim();
  return VISIBLE_VALUES.includes(text) ? text : "all";
}

function parsePage(value) {
  const n = parseInt(value, 10);
  if (!Number.isInteger(n) || n < 1) return 1;
  return Math.min(n, MAX_PAGE);
}

function currentAuthSignature() {
  const user = getUser();
  return [
    isLoggedIn() ? "1" : "0",
    user && user.id !== undefined ? user.id : "",
    user && user.role ? user.role : "",
    user && user.reset_pwd_flag ? "1" : "0",
  ].join("|");
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
