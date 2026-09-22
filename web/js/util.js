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
