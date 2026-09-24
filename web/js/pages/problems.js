// 题目列表页（M1.7 基础版，M4.1 接入统一生命周期）：展示后端提供的题目
// ID、标题、难度与标签。覆盖加载中、空列表、加载失败三种状态。
// 页面切换时通过生命周期取消/忽略过期的读取响应。

import { api } from "../api.js";
import { isAdmin } from "../auth.js";
import { ensureLifecycle } from "../lifecycle.js";
import { navigate } from "../router.js";
import {
  difficultyClass,
  difficultyText,
  h,
  tagList,
} from "../util.js";

export function renderProblems(container, context = {}) {
  document.title = "题目列表 · OJ";
  const lifecycle = ensureLifecycle(context.lifecycle);
  container.replaceChildren();

  const listArea = h("div");
  container.appendChild(h("h1", { class: "page-title", text: "题目列表" }));
  container.appendChild(
    h("p", {
      class: "page-subtitle",
      text: "点击任意题目进入详情；游客可浏览公开题目，提交前需先登录。",
    })
  );
  container.appendChild(listArea);

  async function load() {
    const token = lifecycle.next();
    renderLoading(listArea);

    let data;
    try {
      data = await api.get("/api/problems", { signal: lifecycle.signal });
    } catch (error) {
      if (error.aborted || !lifecycle.isCurrent(token)) return;
      renderError(listArea, error, load);
      return;
    }
    if (!lifecycle.isCurrent(token)) return;

    const problems = data && Array.isArray(data.problems) ? data.problems : [];
    if (problems.length === 0) {
      renderEmpty(listArea);
      return;
    }
    renderTable(listArea, problems);
  }

  load();
  return () => lifecycle.dispose();
}

function renderLoading(area) {
  area.replaceChildren(
    h("div", { class: "state" }, [
      h("span", { class: "spinner" }),
      h("span", { text: "题目加载中…" }),
    ])
  );
}

function renderEmpty(area) {
  area.replaceChildren(
    h("div", { class: "card state", text: "暂无题目。请等待管理员发布题目。" })
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

function renderTable(area, problems) {
  const admin = isAdmin();
  const headCells = [
    h("th", { text: "#" }),
    h("th", { text: "标题" }),
    h("th", { text: "难度" }),
    h("th", { text: "标签" }),
  ];
  if (admin) headCells.push(h("th", { text: "可见性" }));

  const header = h("tr", {}, headCells);
  const body = h("tbody");

  for (const problem of problems) {
    const tags = tagList(problem.tags);
    const cells = [
      h("td", { text: String(problem.id) }),
      h("td", { text: problem.title || "" }),
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
    ];
    if (admin) {
      cells.push(
        h("td", {}, [
          h("span", {
            class: "badge",
            text: problem.visible ? "公开" : "隐藏",
          }),
        ])
      );
    }

    const row = h("tr", { class: "clickable" }, cells);
    row.addEventListener("click", () => navigate("/problems/" + problem.id));
    body.appendChild(row);
  }

  const table = h("table", { class: "data" }, [
    h("thead", {}, [header]),
    body,
  ]);
  area.replaceChildren(h("div", { class: "table-wrap" }, [table]));
}
