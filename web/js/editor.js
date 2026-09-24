// CodeMirror 接入（M4.3）：通过 CDN 按需加载固定版本，失败时不影响页面，
// 由调用方保留可编辑的 textarea 作为降级入口。
//
// 设计要点：
//   - 固定使用 CodeMirror 5 的 5.65.21（cdnjs 固定版本地址），不混用 6.x 的 API，
//     也不使用随时变化的 latest 地址；
//   - 采用动态 <script>/<link> 注入并设置加载超时：CDN 不可达或超时不会阻塞首屏
//     渲染，也不会让页面空白（普通 <script> 标签挂起时会拖住整个应用启动）；
//   - 加载成功后用 CodeMirror.fromTextArea 增强既有 textarea，编辑器与同一个
//     textarea 一一对应；加载失败则保留 textarea 原样可编辑；
//   - dispose 时调用 toTextArea 还原、断开 ResizeObserver 与窗口监听，避免重复
//     进入产生重复编辑器或重复快捷键绑定。

export const CODEMIRROR_VERSION = "5.65.21";
const CDN_BASE =
  "https://cdnjs.cloudflare.com/ajax/libs/codemirror/" + CODEMIRROR_VERSION;
const CSS_HREF = CDN_BASE + "/codemirror.min.css";
// 核心 + C/C++（clike）模式 + 基础编辑体验 addon。全部为 CodeMirror 5 的固定
// 版本文件，不加载其它主版本的资源。
const SCRIPT_URLS = [
  CDN_BASE + "/codemirror.min.js",
  CDN_BASE + "/mode/clike/clike.min.js",
  CDN_BASE + "/addon/edit/matchbrackets.min.js",
  CDN_BASE + "/addon/edit/closebrackets.min.js",
  CDN_BASE + "/addon/selection/active-line.min.js",
  CDN_BASE + "/addon/display/placeholder.min.js",
];
const LOAD_TIMEOUT_MS = 8000;

let ready = false;
let fullLoadPromise = null;
// 每个脚本各自缓存 Promise：加载成功不再重复注入，失败后允许下次重新尝试。
const scriptPromises = new Map();

function injectStylesheet() {
  if (typeof document === "undefined") return;
  if (document.querySelector('link[data-oj-codemirror="1"]')) return;
  const link = document.createElement("link");
  link.rel = "stylesheet";
  link.href = CSS_HREF;
  link.setAttribute("data-oj-codemirror", "1");
  document.head.appendChild(link);
}

function loadScript(src) {
  if (scriptPromises.has(src)) return scriptPromises.get(src);
  const promise = new Promise((resolve, reject) => {
    const script = document.createElement("script");
    script.src = src;
    script.async = false;
    let settled = false;
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      reject(new Error("加载超时：" + src));
    }, LOAD_TIMEOUT_MS);
    script.onload = () => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve();
    };
    script.onerror = () => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      reject(new Error("加载失败：" + src));
    };
    document.head.appendChild(script);
  });
  scriptPromises.set(src, promise);
  promise.catch(() => {
    if (scriptPromises.get(src) === promise) scriptPromises.delete(src);
  });
  return promise;
}

// 按需加载 CodeMirror；返回 Promise<window.CodeMirror>。
export function loadCodeMirror() {
  if (ready) return Promise.resolve(window.CodeMirror);
  // 已由外部提供 CodeMirror（例如页面/测试预先注入）时直接复用，不再重复加载。
  if (typeof window !== "undefined" && window.CodeMirror) {
    ready = true;
    return Promise.resolve(window.CodeMirror);
  }
  if (fullLoadPromise) return fullLoadPromise;
  injectStylesheet();
  const attempt = (async () => {
    for (const src of SCRIPT_URLS) {
      await loadScript(src);
    }
    if (!window.CodeMirror) {
      throw new Error("CodeMirror 未正确初始化");
    }
    ready = true;
    return window.CodeMirror;
  })();
  fullLoadPromise = attempt;
  // 失败后清空整体缓存，允许下次进入页面重新尝试；成功则保持不变。
  attempt.catch(() => {
    if (fullLoadPromise === attempt) fullLoadPromise = null;
  });
  return attempt;
}

export function isCodeMirrorReady() {
  return ready;
}

// 语言 → CodeMirror 模式。仅使用后端已有的 cpp17 / c11 取值。
export function codeMirrorMode(language) {
  return language === "c11" ? "text/x-csrc" : "text/x-c++src";
}

// 在既有 textarea 上创建源码编辑器。始终以 textarea 作为基础数据源：
//   - 编辑器就绪前：getValue()/setValue() 直接读写 textarea；
//   - 就绪后：改用 CodeMirror 实例，dispose 时还原 textarea。
// ready 为 Promise<{ok:boolean, error?:Error}>，调用方可据此提示降级或失败。
export function createSourceEditor(options = {}) {
  const { textarea, initialLanguage = "cpp17", onShortcut } = options;
  let cm = null;
  let disposed = false;
  let resizeObserver = null;
  let readyResolve = null;
  const ready = new Promise((resolve) => {
    readyResolve = resolve;
  });

  function getValue() {
    return cm ? cm.getValue() : textarea.value;
  }

  function setValue(value) {
    if (cm) cm.setValue(value);
    else textarea.value = value;
  }

  function focus() {
    if (cm) cm.focus();
    else textarea.focus();
  }

  function setLanguage(language) {
    if (cm) cm.setOption("mode", codeMirrorMode(language));
  }

  function refresh() {
    if (cm) cm.refresh();
  }

  function handleWindowResize() {
    refresh();
  }

  async function init() {
    let CodeMirror;
    try {
      CodeMirror = await loadCodeMirror();
    } catch (error) {
      if (!disposed) readyResolve({ ok: false, error });
      return;
    }
    if (disposed) {
      readyResolve({ ok: false, error: new Error("编辑器已销毁") });
      return;
    }
    try {
      cm = CodeMirror.fromTextArea(textarea, {
        mode: codeMirrorMode(initialLanguage),
        lineNumbers: true,
        lineWrapping: false,
        indentUnit: 4,
        tabSize: 4,
        indentWithTabs: false,
        smartIndent: true,
        matchBrackets: true,
        autoCloseBrackets: true,
        styleActiveLine: true,
        viewportMargin: 10,
        // 提交快捷键与按钮共用同一入口；由页面回调统一做登录/源码/提交中检查。
        extraKeys: {
          "Ctrl-Enter": () => {
            if (onShortcut) onShortcut();
          },
          "Cmd-Enter": () => {
            if (onShortcut) onShortcut();
          },
        },
      });
      const wrapper = cm.getWrapperElement();
      wrapper.setAttribute("data-oj-editor", "1");
      // 应用占位提示（依赖 placeholder addon；addon 缺失时该选项被忽略）。
      if (typeof cm.setOption === "function") {
        cm.setOption("placeholder", textarea.getAttribute("placeholder") || "");
      }
      // 布局变化（分屏/窗口缩放/上下排列切换）后刷新尺寸，避免不可操作。
      if (typeof ResizeObserver !== "undefined" && wrapper.parentElement) {
        resizeObserver = new ResizeObserver(() => refresh());
        resizeObserver.observe(wrapper.parentElement);
      }
      window.addEventListener("resize", handleWindowResize);
      if (typeof requestAnimationFrame === "function") {
        requestAnimationFrame(() => refresh());
      } else {
        setTimeout(() => refresh(), 0);
      }
      readyResolve({ ok: true });
    } catch (error) {
      // 初始化异常：尽力还原 textarea（避免被半初始化的编辑器隐藏），
      // 保证降级入口可继续编辑。
      if (cm) {
        try {
          cm.toTextArea();
        } catch (restoreError) {
          /* 忽略 */
        }
      }
      cm = null;
      if (!disposed) readyResolve({ ok: false, error });
    }
  }

  function dispose() {
    if (disposed) return;
    disposed = true;
    window.removeEventListener("resize", handleWindowResize);
    if (resizeObserver) {
      try {
        resizeObserver.disconnect();
      } catch (error) {
        /* 忽略 */
      }
      resizeObserver = null;
    }
    if (cm) {
      try {
        cm.toTextArea();
      } catch (error) {
        /* 忽略 */
      }
      cm = null;
    }
  }

  init();

  return { ready, getValue, setValue, focus, setLanguage, refresh, dispose };
}
