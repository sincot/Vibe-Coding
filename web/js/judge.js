// 判题结果渲染：只展示后端本次提交响应中已提供的数据。
//   - 未采集的耗时/内存明确显示为「未采集」，绝不显示为真实的 0；
//   - 区分编译耗时与程序执行耗时，不把等待请求的总时长当作运行耗时；
//   - WA 点的输入/期望输出/实际输出按文本展示并保留空白换行，且区分「空字符串」
//     与「后端未提供」，输出被截断时明确标识；
//   - 编译诊断与程序标准错误分开展示，不混为用户标准输出；
//   - 全局硬上限 / 服务取消 / 内部故障导致的部分结果明确说明未全部执行，
//     未执行点不伪造成通过，也不猜测剩余数量；
//   - 所有内容使用 textContent / pre 元素输出，HTML 特殊字符不会被解释执行。

import { h, memoryText, preBlock, statusBadge, timeText } from "./util.js";

// 后端结构化终止原因 → 可读文案。仅做展示，不据此推断或覆盖后端状态。
const REASON_TEXT = {
  completed: "正常结束",
  non_zero_exit: "非零退出",
  signaled: "被信号终止",
  timed_out: "执行超时",
  memory_exceeded: "超出内存限制",
  cancelled: "服务取消",
  launch_failure: "启动失败",
};

function reasonLabel(reason) {
  if (!reason) return "";
  return REASON_TEXT[reason] || String(reason);
}

function languageLabel(language) {
  if (language === "cpp17") return "C++17";
  if (language === "c11") return "C11";
  return language ? String(language) : "";
}

// 文本字段：区分「缺失」「空字符串」与「有内容」，均原样保留，不做 trim。
function caseField(title, value, options = {}) {
  const { missingText = "（后端未提供）", emptyText = "（空字符串）" } = options;
  const field = h("div", { class: "case-field" }, [h("h5", { text: title })]);
  if (value === undefined || value === null) {
    field.appendChild(h("div", { class: "case-empty muted", text: missingText }));
    return field;
  }
  const text = String(value);
  field.appendChild(preBlock(text));
  if (text === "" && emptyText) {
    field.appendChild(h("div", { class: "case-empty muted", text: emptyText }));
  }
  return field;
}

function truncationNote() {
  return h("div", {
    class: "case-note muted",
    text: "（该输出超过采集上限，已被截断）",
  });
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

  if (status !== "AC" && item.reason) {
    const reason = h("div", { class: "case-reason" }, [
      h("span", { class: "muted", text: "原因：" }),
      h("span", { text: reasonLabel(item.reason) }),
    ]);
    if (item.sanitizer_error) {
      reason.appendChild(h("span", { class: "case-flag", text: "Sanitizer 报告" }));
    }
    if (item.global_deadline_hit) {
      reason.appendChild(h("span", { class: "case-flag", text: "全局时间上限" }));
    }
    children.push(reason);
  }

  if (status === "WA") {
    // WA 点按 SPEC PRB-05 / JUDGE-07 展示输入/期望/实际输出。
    children.push(caseField("输入", item.input));
    children.push(caseField("期望输出", item.expected_output));
    children.push(caseField("你的输出", item.actual_output));
    if (item.output_truncated) children.push(truncationNote());
  } else {
    if (item.message) children.push(caseField("诊断信息", item.message));
    if (item.stderr_output !== undefined && item.stderr_output !== null) {
      children.push(caseField("标准错误", item.stderr_output));
    }
    if (item.actual_output !== undefined && item.actual_output !== null) {
      children.push(caseField("程序输出", item.actual_output));
      if (item.output_truncated) children.push(truncationNote());
    }
    if (typeof item.exit_code === "number" && item.exit_code !== 0) {
      children.push(caseField("退出码", String(item.exit_code)));
    }
    if (typeof item.term_signal === "number" && item.term_signal !== 0) {
      children.push(caseField("终止信号", String(item.term_signal)));
    }
  }
  return h("div", { class: "case-card" }, children);
}

export function renderJudgeResult(container, result) {
  container.replaceChildren();
  if (!result) return;

  const status = result.status || "SYSERR";
  const cases = Array.isArray(result.results) ? result.results : [];
  const total = typeof result.total === "number" ? result.total : null;
  const passed = typeof result.passed === "number" ? result.passed : null;
  const compileOk = result.compile_ok !== false;

  const header = h("div", { class: "result-header" }, [
    h("span", { class: "badge", text: `提交 #${result.id}` }),
    statusBadge(status),
  ]);
  if (result.language) {
    header.appendChild(
      h("span", { class: "muted", text: "语言 " + languageLabel(result.language) })
    );
  }
  if (compileOk && passed !== null && total !== null) {
    header.appendChild(
      h("span", { class: "muted", text: `通过 ${passed}/${total} 个测试点` })
    );
  }

  const metaParts = [];
  // 运行耗时只统计程序执行时间（不含排队与编译）；编译耗时单独展示。
  metaParts.push("运行耗时 " + timeText(result.runtime_ms));
  metaParts.push("编译耗时 " + timeText(result.compile_time_ms));
  metaParts.push("峰值内存 " + memoryText(result.memory_kb));
  if (result.created_at) metaParts.push("提交时间 " + result.created_at);

  container.appendChild(h("h3", { text: "判题结果" }));
  container.appendChild(header);
  container.appendChild(h("div", { class: "result-meta", text: metaParts.join(" · ") }));

  if (result.message) {
    container.appendChild(h("div", { class: "alert alert-info", text: result.message }));
  }

  // 部分执行：全局硬上限 / 服务取消 / 内部故障导致未跑完全部测试点。
  // 服务取消与全局硬上限即使发生在编译阶段也要明确说明；编译未通过（普通 CE）时
  // 未执行测试点属预期，不额外提示为异常。
  const totalSuffix = total !== null ? ` / 共 ${total} 个` : "";
  const partialMessages = [];
  if (result.cancelled) {
    partialMessages.push(
      `判题因服务停止被取消，未执行全部测试点（已执行 ${cases.length} 个${totalSuffix}）。`
    );
  } else if (result.global_deadline_hit) {
    partialMessages.push(
      `已达到单次判题全局时间上限，未执行全部测试点（已执行 ${cases.length} 个${totalSuffix}）。`
    );
  } else if (compileOk && status !== "CE" && total !== null && cases.length < total) {
    partialMessages.push(
      `本次仅执行了部分测试点（已执行 ${cases.length} 个 / 共 ${total} 个），未执行点不视为通过。`
    );
  }
  for (const text of partialMessages) {
    container.appendChild(h("div", { class: "alert alert-warn", text }));
  }

  if (result.compile_output) {
    const isError = result.compile_ok === false || status === "CE";
    container.appendChild(h("h4", { text: isError ? "编译错误" : "编译信息" }));
    container.appendChild(preBlock(result.compile_output));
    if (result.compile_output_truncated) {
      container.appendChild(
        h("div", {
          class: "case-note muted",
          text: "（编译诊断超过采集上限，已截断）",
        })
      );
    }
  }

  if (cases.length > 0) {
    const body = h("tbody");
    for (const item of cases) {
      const index = Number.isInteger(item.index) ? item.index + 1 : "?";
      const itemStatus = item.status || "SYSERR";
      let reason = "—";
      if (itemStatus !== "AC" && item.reason) reason = reasonLabel(item.reason);
      body.appendChild(
        h("tr", {}, [
          h("td", { text: String(index) }),
          h("td", {}, [statusBadge(itemStatus)]),
          h("td", { text: timeText(item.time_ms) }),
          h("td", { text: memoryText(item.memory_kb) }),
          h("td", { text: reason }),
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
              h("th", { text: "原因" }),
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
