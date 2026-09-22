// 题目列表页（M1.7 基础版）：展示后端已提供的题目 ID、标题、难度与标签。
// 不显示后端尚未提供的通过人数、本人 AC 状态等数据。
// 覆盖加载中、空列表、加载失败三种状态。

import { api } from "../api.js";
import { isAdmin } from "../auth.js";
import { navigate } from "../router.js";
import {
  difficultyClass,
  difficultyText,
  h,
  tagList,
} from "../util.js";

export async function renderProblems(container) {
  document.title = "题目列表 · OJ";
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
  renderLoading(listArea);

  let data;
  try {
    data = await api.get("/api/problems");
  } catch (error) {
    renderError(listArea, error.message || "加载失败");
    return;
  }

  const problems = data && Array.isArray(data.problems) ? data.problems : [];
  if (problems.length === 0) {
    renderEmpty(listArea);
    return;
  }
  renderTable(listArea, problems);
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

function renderError(area, message) {
  const retry = h("button", {
    class: "btn btn-secondary",
    text: "重试",
    attrs: { type: "button" },
  });
  retry.addEventListener("click", () => renderProblems(area.parentElement));
  area.replaceChildren(
    h("div", { class: "card state" }, [
      h("div", { class: "alert alert-error", text: "题目加载失败：" + message }),
      h("div", { attrs: { style: "margin-top:12px" } }, [retry]),
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
