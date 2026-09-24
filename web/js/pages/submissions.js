// 本人提交历史页（SPEC UI-01 / M4.4）：分页展示本人提交记录的摘要，支持按题目筛选，
// 点击提交 ID 进入详情。
//
// 设计要点：
//   - 身份只来自后端验证后的当前用户：请求 `GET /api/submissions?mine`，不传
//     user_id，后端无论是否携带 mine 都只返回本人记录；
//   - 列表只展示摘要（提交 ID、题目、语言、状态、耗时、内存、时间），不含源码、
//     逐点结果或 WA 用例详情；
//   - 分页与题目筛选承载于 hash 路由查询串，刷新、前进后退可恢复；超出末页修正；
//   - 读取请求可取消或按代次丢弃，只有最新结果更新列表；退出登录不残留本人数据。

import { api } from "../api.js";
import { isLoggedIn, subscribeAuth } from "../auth.js";
import { ensureLifecycle } from "../lifecycle.js";
import { navigate } from "../router.js";
import { saveSubmissionsReturn } from "../storage.js";
import {
  compactPagination,
  h,
  languageText,
  memoryText,
  statusBadge,
  timeText,
} from "../util.js";

const MAX_PAGE = 1000000;
const DEFAULT_PAGE_SIZE = 20;

export function renderSubmissions(container, context = {}) {
  document.title = "提交历史 · OJ";
  const lifecycle = ensureLifecycle(context.lifecycle);
  container.replaceChildren();

  const query = context.query || new URLSearchParams();
  const state = {
    page: parsePage(query.get("page")),
    problemId: parseProblemId(query.get("problem_id")),
  };

  container.appendChild(h("h1", { class: "page-title", text: "提交历史" }));
  if (state.problemId) {
    container.appendChild(
      h("p", { class: "page-subtitle" }, [
        h("span", { text: `仅显示题目 #${state.problemId} 的提交记录。` }),
        h("a", { text: "查看全部提交", attrs: { href: "#/submissions" } }),
      ])
    );
  } else {
    container.appendChild(
      h("p", {
        class: "page-subtitle",
        text: "展示本人提交记录；点击提交 ID 查看源码与判题结果。",
      })
    );
  }

  const listArea = h("div");
  container.appendChild(listArea);

  let inFlight = null;

  function apply(patch) {
    navigate(buildHash({ ...state, ...patch }));
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

    const params = new URLSearchParams();
    // mine 参数始终携带（取值 1）；后端以参数出现与否解析，空值同样按本人历史处理。
    params.set("mine", "1");
    if (state.problemId) params.set("problem_id", String(state.problemId));
    if (state.page > 1) params.set("page", String(state.page));
    const url = "/api/submissions?" + params.toString();

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

    const items =
      data && Array.isArray(data.submissions) ? data.submissions : [];
    const total = Number.isFinite(data && data.total) ? data.total : items.length;
    const totalPages =
      Number.isFinite(data && data.total_pages) && data.total_pages >= 0
        ? data.total_pages
        : 0;
    const pageSize =
      Number.isFinite(data && data.page_size) && data.page_size > 0
        ? data.page_size
        : DEFAULT_PAGE_SIZE;

    // 超出末页（含筛选后为空但页码 > 1）：修正到合法页码并重取，避免空白或循环。
    const targetPage = totalPages >= 1 ? Math.min(state.page, totalPages) : 1;
    if (targetPage !== state.page) {
      navigate(buildHash({ ...state, page: targetPage }), { replace: true });
      return;
    }

    if (total === 0) {
      renderEmpty(listArea, !!state.problemId);
      return;
    }
    renderResult(listArea, {
      state,
      items,
      total,
      totalPages,
      pageSize,
      onPage: (page) => apply({ page }),
    });
  }

  // 身份变化（退出/切换）时立即清除受保护内容，避免残留他人数据；路由随后按
  // 最新身份重渲染或跳转登录。
  const unsubscribe = subscribeAuth(() => {
    if (lifecycle.disposed) return;
    if (!isLoggedIn()) listArea.replaceChildren();
  });

  load();

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
// 列表状态
// ---------------------------------------------------------------------------

function renderLoading(area) {
  area.replaceChildren(
    h("div", { class: "state" }, [
      h("span", { class: "spinner" }),
      h("span", { text: "提交记录加载中…" }),
    ])
  );
}

function renderEmpty(area, filtered) {
  const children = [
    h("div", {
      text: filtered
        ? `题目 #${filtered} 暂无本人提交记录。`
        : "你还没有提交记录，去题库选择一道题开始做题吧。",
    }),
  ];
  const actions = [];
  if (filtered) {
    actions.push(
      h("a", { class: "btn btn-secondary", text: "查看全部提交", attrs: { href: "#/submissions" } })
    );
  }
  actions.push(
    h("a", { class: "btn btn-secondary", text: "返回题目列表", attrs: { href: "#/problems" } })
  );
  children.push(h("div", { class: "state-actions" }, actions));
  area.replaceChildren(h("div", { class: "card state" }, children));
}

function renderError(area, error, retry) {
  const message = error.network
    ? "网络连接失败，无法加载提交历史。"
    : error.message || "加载失败";
  const retryButton = h("button", {
    class: "btn btn-secondary",
    text: "重试",
    attrs: { type: "button" },
  });
  retryButton.addEventListener("click", retry);
  area.replaceChildren(
    h("div", { class: "card state" }, [
      h("div", { class: "alert alert-error", text: "提交历史加载失败：" + message }),
      h("div", { attrs: { style: "margin-top:12px" } }, [retryButton]),
    ])
  );
}

function renderResult(area, options) {
  const { state, items, total, totalPages, pageSize, onPage } = options;
  const countText =
    totalPages > 1
      ? `共 ${total} 条提交　·　第 ${state.page} / ${totalPages} 页（每页 ${pageSize} 条）`
      : `共 ${total} 条提交`;

  const nodes = [h("div", { class: "muted list-count", text: countText })];
  nodes.push(buildTable(items));
  if (totalPages > 1) {
    nodes.push(compactPagination(state.page, totalPages, onPage));
  }
  area.replaceChildren(...nodes);
}

function buildTable(items) {
  const header = h("tr", {}, [
    h("th", { text: "提交 ID" }),
    h("th", { text: "题目" }),
    h("th", { text: "语言" }),
    h("th", { text: "状态" }),
    h("th", { text: "耗时" }),
    h("th", { text: "内存" }),
    h("th", { text: "提交时间" }),
  ]);

  const body = h("tbody");
  for (const item of items) {
    body.appendChild(buildRow(item));
  }

  const table = h("table", { class: "data submissions-table" }, [
    h("thead", {}, [header]),
    body,
  ]);
  return h("div", { class: "table-wrap" }, [table]);
}

function buildRow(item) {
  const id = item.id;
  const detailLink = h("a", {
    text: "#" + id,
    attrs: { href: "#/submissions/" + encodeURIComponent(String(id)) },
  });
  detailLink.addEventListener("click", (event) => {
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
    goToSubmissionDetail(id);
  });

  const problemId = item.problem_id;
  const title =
    item.problem_title === undefined || item.problem_title === null
      ? ""
      : String(item.problem_title);
  const problemLink = h("a", {
    text: title || "#" + problemId,
    attrs: { href: "#/problems/" + encodeURIComponent(String(problemId)) },
  });

  return h("tr", {}, [
    h("td", { class: "col-id" }, [detailLink]),
    h("td", { class: "col-title" }, [problemLink]),
    h("td", { text: languageText(item.language) }),
    h("td", {}, [statusBadge(item.status)]),
    h("td", { text: timeText(item.runtime_ms) }),
    h("td", { text: memoryText(item.memory_kb) }),
    h("td", { class: "col-time", text: item.created_at || "—" }),
  ]);
}

// ---------------------------------------------------------------------------
// 路由 / 参数工具
// ---------------------------------------------------------------------------

function buildHash(state) {
  const params = new URLSearchParams();
  if (state.problemId) params.set("problem_id", String(state.problemId));
  if (state.page > 1) params.set("page", String(state.page));
  const qs = params.toString();
  return "/submissions" + (qs ? "?" + qs : "");
}

// 进入提交详情前记录当前历史列表地址（含分页/题目筛选），供详情页返回恢复。
export function goToSubmissionDetail(id) {
  const current =
    typeof location !== "undefined" && location.hash
      ? location.hash
      : "#/submissions";
  saveSubmissionsReturn(current);
  navigate("/submissions/" + encodeURIComponent(String(id)));
}

function parseProblemId(value) {
  const text = String(value === null || value === undefined ? "" : value).trim();
  if (!/^\d+$/.test(text)) return 0;
  const n = parseInt(text, 10);
  if (!Number.isInteger(n) || n < 1) return 0;
  return Math.min(n, Number.MAX_SAFE_INTEGER);
}

function parsePage(value) {
  const n = parseInt(value, 10);
  if (!Number.isInteger(n) || n < 1) return 1;
  return Math.min(n, MAX_PAGE);
}
