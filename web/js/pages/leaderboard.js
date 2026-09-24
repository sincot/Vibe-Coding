// 排行榜页（SPEC RANK-01 / RANK-02 / M4.5）：公开页面，游客可直接访问。
//
// 设计要点：
//   - 数据来自 `GET /api/leaderboard`，只展示后端已聚合的名次、昵称、AC 题目数、
//     总提交次数与首次 AC 时间；前端不根据单次提交结果自行加减排名，也不做本地
//     二次计算；
//   - 分页条件承载于 hash 查询串（`#/leaderboard?page=`），刷新与浏览器前进后退
//     可恢复；读取请求按代次丢弃，快速翻页或离开页面时旧响应不会覆盖当前页面；
//   - 进入页面或主动点击「刷新」时重新获取当前排名，不新增高频轮询或实时推送；
//   - 当前用户高亮仅在已登录且响应含可可靠匹配的 `user_id` 时实现，不通过昵称
//     猜测身份；登录/退出后按最新身份刷新高亮，不残留过时的个人状态；
//   - 昵称等接口文本一律经 `textContent` 纯文本渲染。

import { api } from "../api.js";
import { getUser, isLoggedIn, subscribeAuth } from "../auth.js";
import { ensureLifecycle } from "../lifecycle.js";
import { navigate } from "../router.js";
import { compactPagination, h, setBusy } from "../util.js";

const MAX_PAGE = 1000000;
const DEFAULT_PAGE_SIZE = 20;

export function renderLeaderboard(container, context = {}) {
  document.title = "排行榜 · OJ";
  const lifecycle = ensureLifecycle(context.lifecycle);
  container.replaceChildren();

  const query = context.query || new URLSearchParams();
  const state = { page: parsePage(query.get("page")) };

  container.appendChild(h("h1", { class: "page-title", text: "排行榜" }));
  container.appendChild(
    h("p", {
      class: "page-subtitle",
      text: "按通过题目数降序、总提交次数升序、首次通过时间升序、注册时间升序排名；尚无通过记录显示「—」。",
    })
  );

  const refreshButton = h("button", {
    class: "btn btn-secondary",
    text: "刷新",
    attrs: { type: "button" },
  });
  refreshButton.addEventListener("click", () => load());
  container.appendChild(
    h("div", { class: "leaderboard-toolbar" }, [
      h("span", {
        class: "muted",
        text: "进入页面或点击刷新时获取当前排名。",
      }),
      refreshButton,
    ])
  );

  const listArea = h("div");
  container.appendChild(listArea);

  let inFlight = null;
  // 最近一次成功结果：登录/退出只重绘当前用户高亮，不为高亮重复请求。
  let snapshot = null;

  function apply(patch) {
    navigate(buildHash({ ...state, ...patch }));
  }

  function paint() {
    if (!snapshot) return;
    if (snapshot.total === 0) {
      renderEmpty(listArea);
      return;
    }
    renderResult(listArea, {
      state,
      items: snapshot.items,
      total: snapshot.total,
      totalPages: snapshot.totalPages,
      pageSize: snapshot.pageSize,
      onPage: (page) => apply({ page }),
    });
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
    setBusy(refreshButton, true, "刷新中…");

    const params = new URLSearchParams();
    if (state.page > 1) params.set("page", String(state.page));
    const queryString = params.toString();
    const url = "/api/leaderboard" + (queryString ? "?" + queryString : "");

    let data;
    try {
      data = await api.get(url, {
        auth: false,
        signal: controller ? controller.signal : lifecycle.signal,
      });
    } catch (error) {
      if (error.aborted || !lifecycle.isCurrent(token)) return;
      setBusy(refreshButton, false);
      renderError(listArea, error, load);
      return;
    }
    if (!lifecycle.isCurrent(token)) return;
    setBusy(refreshButton, false);

    const items =
      data && Array.isArray(data.leaderboard) ? data.leaderboard : [];
    const total = Number.isFinite(data && data.total) ? data.total : items.length;
    const totalPages =
      Number.isFinite(data && data.total_pages) && data.total_pages >= 0
        ? data.total_pages
        : 0;
    const pageSize =
      Number.isFinite(data && data.page_size) && data.page_size > 0
        ? data.page_size
        : DEFAULT_PAGE_SIZE;

    // 超出末页：修正到合法页码并重取一次，避免空白或请求循环。
    const targetPage = totalPages >= 1 ? Math.min(state.page, totalPages) : 1;
    if (targetPage !== state.page) {
      navigate(buildHash({ ...state, page: targetPage }), { replace: true });
      return;
    }

    snapshot = { items, total, totalPages, pageSize };
    paint();
  }

  // 登录/退出/切换账号后按最新身份刷新当前用户高亮，不残留过时的个人状态。
  const unsubscribe = subscribeAuth(() => {
    if (lifecycle.disposed) return;
    paint();
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
      h("span", { text: "排行榜加载中…" }),
    ])
  );
}

function renderEmpty(area) {
  area.replaceChildren(
    h("div", { class: "card state" }, [
      h("div", {
        text: "暂时没有可展示的排名，等有同学提交后再来看看。",
      }),
    ])
  );
}

function renderError(area, error, retry) {
  const message = error.network
    ? "网络连接失败，无法加载排行榜。"
    : error.message || "加载失败";
  const retryButton = h("button", {
    class: "btn btn-secondary",
    text: "重试",
    attrs: { type: "button" },
  });
  retryButton.addEventListener("click", retry);
  area.replaceChildren(
    h("div", { class: "card state" }, [
      h("div", { class: "alert alert-error", text: "排行榜加载失败：" + message }),
      h("div", { class: "state-actions" }, [retryButton]),
    ])
  );
}

function renderResult(area, options) {
  const { state, items, total, totalPages, pageSize, onPage } = options;
  const countText =
    totalPages > 1
      ? `共 ${total} 名用户　·　第 ${state.page} / ${totalPages} 页（每页 ${pageSize} 条）`
      : `共 ${total} 名用户`;

  const nodes = [h("div", { class: "muted list-count", text: countText })];
  nodes.push(buildTable(items));
  if (totalPages > 1) {
    nodes.push(compactPagination(state.page, totalPages, onPage));
  }
  area.replaceChildren(...nodes);
}

function buildTable(items) {
  const header = h("tr", {}, [
    h("th", { text: "名次" }),
    h("th", { text: "昵称" }),
    h("th", { text: "AC 题目数" }),
    h("th", { text: "总提交次数" }),
    h("th", { text: "首次 AC 时间" }),
  ]);
  const body = h("tbody");

  const user = isLoggedIn() ? getUser() : null;
  const currentUserId =
    user && user.id !== undefined && user.id !== null ? Number(user.id) : null;
  for (const item of items) {
    body.appendChild(buildRow(item, currentUserId));
  }

  const table = h("table", { class: "data leaderboard-table" }, [
    h("thead", {}, [header]),
    body,
  ]);
  return h("div", { class: "table-wrap" }, [table]);
}

function buildRow(item, currentUserId) {
  const isCurrent =
    currentUserId !== null &&
    Number.isFinite(currentUserId) &&
    Number(item.user_id) === currentUserId;
  const row = h("tr", { class: isCurrent ? "current-user" : "" }, [
    h("td", { class: "col-rank", text: String(item.rank) }),
    h("td", { class: "col-nick", text: item.nickname ? String(item.nickname) : "—" }),
    h("td", { class: "col-ac", text: formatCount(item.ac_count) }),
    h("td", { class: "col-submits", text: formatCount(item.submit_count) }),
    h("td", {
      class: "col-time",
      text: item.first_ac_at ? String(item.first_ac_at) : "—",
    }),
  ]);
  if (isCurrent) {
    row.setAttribute("aria-current", "true");
  }
  return row;
}

function formatCount(value) {
  return value === null || value === undefined ? "—" : String(value);
}

// ---------------------------------------------------------------------------
// 路由 / 参数工具
// ---------------------------------------------------------------------------

function buildHash(state) {
  const params = new URLSearchParams();
  if (state.page > 1) params.set("page", String(state.page));
  const qs = params.toString();
  return "/leaderboard" + (qs ? "?" + qs : "");
}

function parsePage(value) {
  const n = parseInt(value, 10);
  if (!Number.isInteger(n) || n < 1) return 1;
  return Math.min(n, MAX_PAGE);
}
