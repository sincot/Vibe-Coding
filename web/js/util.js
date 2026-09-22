// 通用工具：DOM 构建、文本安全渲染与展示格式化。
// 全部通过 textContent / createElement 写入，绝不使用 innerHTML 拼接后端数据，
// 从而保证题面、昵称、编译信息与程序输出中的 HTML 特殊字符被当作纯文本显示。

// 创建元素。props 支持 class / text / attrs / on（事件）/ dataset。
export function h(tag, props = {}, children = []) {
  const node = document.createElement(tag);
  if (props.class) node.className = props.class;
  if (props.text !== undefined && props.text !== null) {
    node.textContent = String(props.text);
  }
  if (props.attrs) {
    for (const [key, value] of Object.entries(props.attrs)) {
      if (value !== null && value !== undefined) {
        node.setAttribute(key, String(value));
      }
    }
  }
  if (props.on) {
    for (const [event, handler] of Object.entries(props.on)) {
      node.addEventListener(event, handler);
    }
  }
  for (const child of [].concat(children)) {
    if (child === null || child === undefined || child === false) continue;
    node.appendChild(typeof child === "string" ? document.createTextNode(child) : child);
  }
  return node;
}

export function clear(node) {
  while (node.firstChild) {
    node.removeChild(node.firstChild);
  }
}

// 文本块：保留换行与空白，始终作为纯文本显示。
export function preBlock(text, extraClass = "") {
  return h("pre", {
    class: `code-block ${extraClass}`.trim(),
    text: text === null || text === undefined ? "" : String(text),
  });
}

// 后端标签为字符串（逗号分隔）或数组，统一为数组。
export function tagList(tags) {
  if (Array.isArray(tags)) {
    return tags.map((t) => String(t)).filter(Boolean);
  }
  if (typeof tags === "string") {
    return tags
      .split(",")
      .map((t) => t.trim())
      .filter(Boolean);
  }
  return [];
}

export function difficultyText(difficulty) {
  switch (difficulty) {
    case "easy":
      return "易";
    case "medium":
      return "中";
    case "hard":
      return "难";
    default:
      return difficulty || "未知";
  }
}

export function difficultyClass(difficulty) {
  switch (difficulty) {
    case "easy":
      return "difficulty-easy";
    case "medium":
      return "difficulty-medium";
    case "hard":
      return "difficulty-hard";
    default:
      return "";
  }
}

export function limitText(ms) {
  if (ms === null || ms === undefined) return "未提供";
  if (ms % 1000 === 0) return `${ms / 1000} s`;
  return `${ms} ms`;
}

export function memoryText(kb) {
  if (kb === null || kb === undefined) return "未采集";
  return `${kb} KB`;
}

export function timeText(ms) {
  if (ms === null || ms === undefined) return "—";
  return `${ms} ms`;
}

// 统一的轻提示（不阻塞操作）。
let toastTimer = null;
export function showToast(message) {
  const node = document.getElementById("toast");
  if (!node) return;
  node.textContent = message;
  node.classList.add("show");
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => node.classList.remove("show"), 3200);
}

// 表单提示区：kind ∈ error / success / info / warn。
export function setMessage(node, kind, text) {
  if (!node) return;
  clear(node);
  if (text) {
    node.appendChild(h("div", { class: `alert alert-${kind}`, text }));
  }
}

// 表单字段：标签 + 控件 + 可选说明。
export function field(labelText, input, hintText) {
  const children = [];
  const labelAttrs = input.id ? { for: input.id } : {};
  children.push(h("label", { text: labelText, attrs: labelAttrs }));
  children.push(input);
  if (hintText) children.push(h("span", { class: "hint", text: hintText }));
  return h("div", { class: "field" }, children);
}

export function statusBadge(status) {
  const safe = String(status || "未知");
  return h("span", { class: `badge status-${safe}`, text: safe });
}

// 通用模态框：把内容节点覆盖在页面上，返回 close()。
// 用于需要明确确认或需要输入（如重置密码）的写操作，避免依赖 window.confirm/prompt。
export function openModal(contentNode, options = {}) {
  const overlay = h("div", { class: "modal-overlay" });
  const dialog = h(
    "div",
    { class: "modal", attrs: { role: "dialog", "aria-modal": "true" } },
    [contentNode]
  );
  overlay.appendChild(dialog);
  document.body.appendChild(overlay);

  let closed = false;
  function close() {
    if (closed) return;
    closed = true;
    document.removeEventListener("keydown", onKey);
    overlay.remove();
    if (typeof options.onClose === "function") options.onClose();
  }
  function onKey(event) {
    if (event.key === "Escape" && options.dismissible !== false) close();
  }
  document.addEventListener("keydown", onKey);
  if (options.dismissible !== false) {
    overlay.addEventListener("mousedown", (event) => {
      if (event.target === overlay) close();
    });
  }
  return { close, overlay, dialog };
}

// 确认对话框：返回 Promise<boolean>。body 可为字符串或 DOM 节点。
export function confirmDialog(options = {}) {
  const {
    title = "请确认",
    body = "",
    confirmText = "确认",
    cancelText = "取消",
    danger = false,
  } = options;
  return new Promise((resolve) => {
    let result = false;
    const bodyNode = h("div", { class: "modal-body" });
    if (typeof body === "string") bodyNode.textContent = body;
    else if (body) bodyNode.appendChild(body);

    const cancel = h("button", {
      class: "btn btn-secondary",
      text: cancelText,
      attrs: { type: "button" },
    });
    const confirm = h("button", {
      class: "btn " + (danger ? "btn-danger" : "btn-primary"),
      text: confirmText,
      attrs: { type: "button" },
    });
    const content = h("div", {}, [
      h("h3", { class: "modal-title", text: title }),
      bodyNode,
      h("div", { class: "modal-actions" }, [cancel, confirm]),
    ]);

    const { close } = openModal(content, { onClose: () => resolve(result) });
    cancel.addEventListener("click", () => {
      result = false;
      close();
    });
    confirm.addEventListener("click", () => {
      result = true;
      close();
    });
    setTimeout(() => confirm.focus(), 0);
  });
}

// 分页控件：page 从 1 开始。返回元素，页码回调由 onChange 提供。
export function pagination(page, totalPages, onChange) {
  const wrap = h("div", { class: "pagination" });
  const safeTotal = Number.isFinite(totalPages) && totalPages > 0 ? totalPages : 1;
  const current = Math.min(Math.max(1, page), safeTotal);

  const prev = h("button", {
    class: "btn btn-secondary btn-sm",
    text: "上一页",
    attrs: { type: "button" },
  });
  prev.disabled = current <= 1;
  prev.addEventListener("click", () => onChange(current - 1));

  const next = h("button", {
    class: "btn btn-secondary btn-sm",
    text: "下一页",
    attrs: { type: "button" },
  });
  next.disabled = current >= safeTotal;
  next.addEventListener("click", () => onChange(current + 1));

  wrap.appendChild(prev);
  wrap.appendChild(
    h("span", { class: "pagination-info", text: `第 ${current} / ${safeTotal} 页` })
  );
  wrap.appendChild(next);
  return wrap;
}

export function setBusy(button, busy, busyText = "处理中…", idleText = null) {
  if (!button) return;
  if (busy) {
    if (!button.dataset.idleText) {
      button.dataset.idleText = idleText || button.textContent;
    }
    button.disabled = true;
    button.textContent = busyText;
  } else {
    button.disabled = false;
    button.textContent = idleText || button.dataset.idleText || button.textContent;
  }
}
