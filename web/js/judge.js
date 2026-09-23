// 判题结果渲染：只展示后端本次提交响应中已提供的数据。
// 未采集的耗时/内存明确显示为「未采集」，绝不显示为真实的 0；
// WA 点的输入/期望输出/实际输出按文本展示，不额外请求隐藏用例。
// 所有内容使用 textContent / pre 元素输出，HTML 特殊字符不会被解释执行。

import { h, memoryText, preBlock, statusBadge, timeText } from "./util.js";

function caseField(title, value) {
  return h("div", { class: "case-field" }, [
    h("h5", { text: title }),
    preBlock(value),
  ]);
}

function buildCaseCard(item) {
  const index = item.index;
  const status = item.status || "SYSERR";
  const header = h("header", {}, [
    h("span", { text: `测试点 #${Number.isInteger(index) ? index + 1 : "?"}` }),
    statusBadge(status),
    h("span", { class: "muted", text: "耗时 " + timeText(item.time_ms) }),
    h("span", { class: "muted", text: "内存 " + memoryText(item.memory_kb) }),
  ]);

  const children = [header];
  if (item.input !== undefined && item.input !== null) {
    children.push(caseField("输入", item.input));
  }
  if (item.expected_output !== undefined && item.expected_output !== null) {
    children.push(caseField("期望输出", item.expected_output));
  }
  if (item.actual_output !== undefined && item.actual_output !== null) {
    children.push(caseField("你的输出", item.actual_output));
  }
  if (item.message) children.push(caseField("诊断信息", item.message));
  if (item.stderr_output) children.push(caseField("标准错误", item.stderr_output));
  if (typeof item.exit_code === "number" && item.exit_code !== 0) {
    children.push(caseField("退出码", String(item.exit_code)));
  }
  if (typeof item.term_signal === "number" && item.term_signal !== 0) {
    children.push(caseField("终止信号", String(item.term_signal)));
  }
  return h("div", { class: "case-card" }, children);
}

export function renderJudgeResult(container, result) {
  container.replaceChildren();
  if (!result) return;

  const status = result.status || "SYSERR";
  const total = typeof result.total === "number" ? result.total : null;
  const passed = typeof result.passed === "number" ? result.passed : null;

  const header = h("div", { class: "result-header" }, [
    h("span", { class: "badge", text: `提交 #${result.id}` }),
    statusBadge(status),
  ]);
  if (passed !== null && total !== null) {
    header.appendChild(
      h("span", { class: "muted", text: `通过 ${passed}/${total} 个测试点` })
    );
  }

  const metaParts = [];
  // 提交级耗时只统计程序执行时间（不含排队与编译）；编译耗时单独展示，
  // 不把排队/编译时间混入运行耗时字段。
  metaParts.push("运行耗时 " + timeText(result.runtime_ms));
  if (typeof result.compile_time_ms === "number") {
    metaParts.push("编译耗时 " + timeText(result.compile_time_ms));
  }
  metaParts.push("峰值内存 " + memoryText(result.memory_kb));
  if (result.created_at) metaParts.push("提交时间 " + result.created_at);

  container.appendChild(h("h3", { text: "判题结果" }));
  container.appendChild(header);
  container.appendChild(h("div", { class: "result-meta", text: metaParts.join(" · ") }));

  if (result.message) {
    container.appendChild(h("div", { class: "alert alert-info", text: result.message }));
  }

  if (result.compile_output) {
    container.appendChild(h("h4", { text: "编译信息" }));
    container.appendChild(preBlock(result.compile_output));
  }

  const cases = Array.isArray(result.results) ? result.results : [];
  if (cases.length > 0) {
    const body = h("tbody");
    for (const item of cases) {
      const index = Number.isInteger(item.index) ? item.index + 1 : "?";
      body.appendChild(
        h("tr", {}, [
          h("td", { text: String(index) }),
          h("td", {}, [statusBadge(item.status || "SYSERR")]),
          h("td", { text: timeText(item.time_ms) }),
          h("td", { text: memoryText(item.memory_kb) }),
        ])
      );
    }
    container.appendChild(h("h4", { text: "逐测试点结果" }));
    container.appendChild(
      h("div", { class: "table-wrap" }, [
        h("table", { class: "data" }, [
          h("thead", {}, [
            h("tr", {}, [
              h("th", { text: "测试点" }),
              h("th", { text: "状态" }),
              h("th", { text: "耗时" }),
              h("th", { text: "内存" }),
            ]),
          ]),
          body,
        ]),
      ])
    );

    const failed = cases.filter((item) => (item.status || "") !== "AC");
    if (failed.length > 0) {
      container.appendChild(h("h4", { text: "失败测试点详情" }));
      const list = h("div", { class: "case-list" });
      for (const item of failed) list.appendChild(buildCaseCard(item));
      container.appendChild(list);
    }
  }
}
