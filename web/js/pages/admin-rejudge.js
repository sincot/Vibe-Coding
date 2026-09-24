// 后台重判页面：按提交 ID 发起重判，展示结果与诊断。
// 提交历史/详情页（M4.4）已提供详情展示，成功重判后给出跳转入口。

import { api } from "../api.js";
import { renderJudgeResult } from "../judge.js";
import {
  adminShell,
  errorBlock,
  handleRevoked,
  loadingBlock,
  renderRequestError,
} from "./admin-common.js";
import {
  confirmDialog,
  h,
  setBusy,
  setMessage,
  showToast,
} from "../util.js";

export function renderAdminRejudge(container) {
  const area = adminShell(container, {
    title: "重判",
    subtitle:
      "输入提交 ID，使用当前题目配置与测试用例重新判题。重判会更新原提交结果并联动刷新该用户该题的状态，不增加提交次数。",
    active: "rejudge",
  });

  const idInput = h("input", {
    attrs: { type: "text", placeholder: "例如 42", id: "rejudge-id" },
  });

  const message = h("div");
  const submit = h("button", {
    class: "btn btn-primary",
    text: "确认重判",
    attrs: { type: "submit" },
  });

  const form = h("form", { class: "form admin-form" }, [
    h("div", { class: "field" }, [
      h("label", { text: "提交 ID", attrs: { for: "rejudge-id" } }),
      idInput,
      h("span", {
        class: "hint",
        text: "仅接受正整数；重判将使用该提交保存的源码与语言，按当前题目配置重新判题。",
      }),
    ]),
    h("div", { class: "field" }, [
      h("span", { class: "hint", text: "说明：" }),
      h("ul", { class: "hint-list" }, [
        h("li", { text: "不新增提交记录，不增加该用户的 submit_count。" }),
        h("li", { text: "重判后该题状态按当前所有提交重新计算：仍有 AC 则保留并取最早时间，无 AC 则清空。" }),
        h("li", { text: "若该提交正在重判中，需等待完成后再次发起。" }),
      ]),
    ]),
    message,
    h("div", { class: "editor-actions" }, [submit]),
  ]);

  // 状态区与结果区分离：renderJudgeResult 会 replaceChildren 结果区，
  // 若把成功提示放进结果区会被清掉，故用独立的状态区承载进行中/成功提示。
  const statusArea = h("div");
  const resultArea = h("div");
  area.appendChild(form);
  area.appendChild(statusArea);
  area.appendChild(resultArea);

  let running = false;

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (running) return;

    const raw = idInput.value.trim();
    const id = parseInt(raw, 10);
    if (!/^\d+$/.test(raw) || id <= 0) {
      setMessage(message, "error", "提交 ID 须为正整数");
      return;
    }

    setMessage(message, "info", "");
    statusArea.replaceChildren();
    resultArea.replaceChildren();

    const ok = await confirmDialog({
      title: "确认重判提交？",
      body: `即将对提交 #${id} 发起重判。原提交结果将被覆盖为该次重判结果，该用户该题的做题状态将按最新结果重新计算，且不增加提交次数。`,
      confirmText: "开始重判",
    });
    if (!ok) return;

    running = true;
    setBusy(submit, true, "重判中…");
    statusArea.appendChild(loadingBlock("正在重判，请稍候…"));

    try {
      const result = await api.post(
        "/api/admin/submissions/" + encodeURIComponent(id) + "/rejudge",
        {}
      );
      statusArea.replaceChildren();
      statusArea.appendChild(
        h("div", { class: "alert alert-success", text: "重判完成" })
      );
      statusArea.appendChild(
        h("div", { class: "state-actions" }, [
          h("a", {
            class: "btn btn-secondary btn-sm",
            text: "查看提交详情",
            attrs: { href: "#/submissions/" + encodeURIComponent(String(id)) },
          }),
        ])
      );
      renderJudgeResult(resultArea, result);
      showToast("重判完成：" + (result.status || "未知"));
    } catch (error) {
      statusArea.replaceChildren();
      resultArea.replaceChildren();
      if (error.status === 409 && error.code === "REJUDGE_IN_PROGRESS") {
        setMessage(
          message,
          "warn",
          "该提交正在重判中，请等待当前重判完成后再试。"
        );
      } else if (error.status === 503) {
        setMessage(
          message,
          "warn",
          error.message || "判题队列已满，请稍后重试。"
        );
      } else {
        const text =
          error.network
            ? "网络连接失败，无法确认重判结果，请检查网络后自行确认。"
            : error.message || "重判失败";
        resultArea.appendChild(errorBlock(text));
        handleRevoked(error);
      }
    } finally {
      running = false;
      setBusy(submit, false, null, "确认重判");
    }
  });
}
