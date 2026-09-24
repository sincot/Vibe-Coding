// M4.3 真实浏览器场景（playwright-cli run-code 脚本）。
//
// 用法（在已 `playwright-cli open` 的会话中）：
//   playwright-cli run-code --filename=m43_browser_scenarios.js
// 运行前把 __BASE__（后端根 URL）与 __OUT__（截图输出目录）替换为实际值。
//
// 返回 JSON：{ results:[{name,ok,detail}], errors:[...] }。
//
// 真实 Chromium 验证 M4.3：真实 CodeMirror 从 CDN 加载与 C/C++ 高亮、行号、语言切换
// 更新模式且保留源码、ResizeObserver 触发 refresh 的实际布局调整、窄视口单列布局与
// 可操作性、离开页面释放编辑器（toTextArea）。
async (page) => {
  const BASE = "__BASE__";
  const OUT = "__OUT__";
  const results = [];
  const errors = [];
  const ck = (name, cond, detail) =>
    results.push({
      name,
      ok: !!cond,
      detail: detail === undefined ? "" : String(detail).slice(0, 240),
    });

  page.on("pageerror", (e) => errors.push("pageerror: " + (e && e.message)));
  page.on("console", (m) => {
    if (m.type() !== "error") return;
    const t = m.text();
    if (/Failed to load resource|server responded with a status|net::ERR/.test(t)) return;
    errors.push("console: " + t);
  });

  const noOverflow = () =>
    page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1);
  const layoutCols = () =>
    page.evaluate(() => {
      const el = document.querySelector(".problem-layout");
      if (!el) return null;
      return getComputedStyle(el).gridTemplateColumns.trim().split(/\s+/).filter(Boolean).length;
    });
  // CodeMirror 5 在包装元素上保存实例：document.querySelector('.CodeMirror').CodeMirror。
  const cmInfo = () =>
    page.evaluate(() => {
      const w = document.querySelector(".CodeMirror");
      const cm = w && w.CodeMirror;
      const host = document.querySelector(".code-editor-host");
      const ta = document.getElementById("source-code");
      const btn = document.querySelector(".editor-actions button");
      return {
        hasWrapper: !!w,
        hasInstance: !!cm,
        value: cm ? cm.getValue() : null,
        mode: cm ? cm.getOption("mode") : null,
        width: w ? Math.round(w.getBoundingClientRect().width) : -1,
        height: w ? Math.round(w.getBoundingClientRect().height) : -1,
        hostWidth: host ? Math.round(host.getBoundingClientRect().width) : -1,
        linenumbers: document.querySelectorAll(".CodeMirror-linenumber").length,
        keywords: document.querySelectorAll(".cm-keyword").length,
        meta: document.querySelectorAll(".cm-meta").length,
        textareaDisplay: ta ? getComputedStyle(ta).display : "",
        fallbackShown: !!document.querySelector(".editor-status .alert-warn"),
        submitRight: btn ? Math.round(btn.getBoundingClientRect().right) : -1,
        refreshCount: window.__refreshCount,
        toTextAreaCalled: window.__toTextAreaCalled,
      };
    });
  async function waitForCm(seedId, timeout = 25000) {
    await page.goto(BASE + "/#/problems/" + seedId, { waitUntil: "domcontentloaded" });
    await page.waitForSelector(".CodeMirror", { timeout });
    await page.waitForFunction(
      () => {
        const w = document.querySelector(".CodeMirror");
        return !!(w && w.CodeMirror);
      },
      null,
      { timeout }
    );
    await page.waitForTimeout(150);
  }

  async function scenario(name, fn) {
    try {
      await fn();
    } catch (e) {
      ck(name + " (no exception)", false, (e && e.message) || String(e));
    }
  }

  await page.goto(BASE + "/?bust=" + Date.now() + "#/problems", { waitUntil: "domcontentloaded" });
  await page.waitForSelector("#app table.data tbody tr", { timeout: 10000 });
  const seedId = await page.evaluate(() =>
    document.querySelector("#app table.data tbody tr a").getAttribute("href").split("/").pop()
  );

  // B43-01 真实 CodeMirror 从 CDN 加载、行号与 C/C++ 高亮
  await scenario("B43-01 真实 CodeMirror 加载与高亮", async () => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await waitForCm(seedId);
    await page.click(".CodeMirror");
    await page.keyboard.type("#include <cstdio>\nint main(){ return 0; }");
    await page.waitForTimeout(200);
    const info = await cmInfo();
    ck("CodeMirror 实例已加载（真实 CDN）", info.hasInstance);
    ck("显示行号", info.linenumbers >= 1, "linenumbers=" + info.linenumbers);
    ck("C/C++ 语法高亮生效", info.meta >= 1 && info.keywords >= 1, `meta=${info.meta} kw=${info.keywords}`);
    ck("textarea 被编辑器接管", info.textareaDisplay === "none", info.textareaDisplay);
    ck("未降级到 textarea", !info.fallbackShown);
    ck("桌面无横向溢出", await noOverflow());
    ck("编辑器有实际高度", info.height > 100, "h=" + info.height);
    await page.screenshot({ path: OUT + "/m43-desktop-editor.png", fullPage: true });
  });

  // B43-02 语言切换更新编辑器模式且保留源码
  await scenario("B43-02 语言切换更新模式且保留源码", async () => {
    const before = await cmInfo();
    ck("初始模式为 C++", before.mode === "text/x-c++src", before.mode);
    await page.selectOption("#submit-language", "c11");
    await page.waitForTimeout(200);
    const afterC = await cmInfo();
    ck("切换 C11 更新模式", afterC.mode === "text/x-csrc", afterC.mode);
    ck("切换语言保留源码", afterC.value === before.value, (afterC.value || "").slice(0, 40));
    await page.selectOption("#submit-language", "cpp17");
    await page.waitForTimeout(150);
    const afterCpp = await cmInfo();
    ck("切回 C++17 更新模式", afterCpp.mode === "text/x-c++src", afterCpp.mode);
  });

  // B43-03 ResizeObserver 在真实布局变化时触发 refresh，并切换为单列
  await scenario("B43-03 真实布局变化触发 refresh", async () => {
    const installed = await page.evaluate(() => {
      const w = document.querySelector(".CodeMirror");
      const cm = w && w.CodeMirror;
      if (!cm) return false;
      window.__refreshCount = 0;
      const orig = cm.refresh.bind(cm);
      cm.refresh = (...args) => {
        window.__refreshCount += 1;
        return orig(...args);
      };
      return true;
    });
    ck("安装 refresh 计数探针", installed);
    const wide = await cmInfo();
    // 1280→1000 仍为两列，容器真正变窄，验证 ResizeObserver 触发的 refresh 调整尺寸。
    await page.setViewportSize({ width: 1000, height: 800 });
    await page.waitForTimeout(500);
    const mid = await cmInfo();
    ck("布局变化后触发 refresh", mid.refreshCount > 0, "refresh=" + mid.refreshCount);
    ck("同布局下容器变窄", mid.hostWidth < wide.hostWidth, `${wide.hostWidth}→${mid.hostWidth}`);
    ck("编辑器随容器收缩", mid.width <= mid.hostWidth + 2, `w=${mid.width} host=${mid.hostWidth}`);
    // 再降到 ≤900 触发单列布局。
    await page.setViewportSize({ width: 640, height: 900 });
    await page.waitForTimeout(500);
    const narrow = await cmInfo();
    ck("640 宽题面为单列", (await layoutCols()) === 1);
    ck("640 宽无横向溢出", await noOverflow());
    ck("单列下编辑器贴合容器", narrow.width <= narrow.hostWidth + 2, `w=${narrow.width} host=${narrow.hostWidth}`);
    await page.screenshot({ path: OUT + "/m43-640-editor.png", fullPage: true });
  });

  // B43-04 窄视口布局与可操作性
  for (const [vw, vh] of [[360, 640], [390, 844]]) {
    await scenario(`B43-窄视口 ${vw}x${vh}`, async () => {
      await page.setViewportSize({ width: vw, height: vh });
      await waitForCm(seedId);
      const cols = await layoutCols();
      ck(`${vw}x${vh} 题面为单列`, cols === 1, "cols=" + cols);
      ck(`${vw}x${vh} 无横向溢出`, await noOverflow());
      const info = await cmInfo();
      ck(`${vw}x${vh} 编辑器可见`, info.width > 0 && info.height > 0, `w=${info.width} h=${info.height}`);
      ck(`${vw}x${vh} 提交按钮在视口内`, info.submitRight <= vw, "right=" + info.submitRight);
      await page.screenshot({ path: `${OUT}/m43-narrow-${vw}x${vh}.png`, fullPage: true });
    });
  }

  // B43-05 离开页面释放编辑器（toTextArea）
  await scenario("B43-05 离开页面释放编辑器", async () => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await waitForCm(seedId);
    const installed = await page.evaluate(() => {
      const w = document.querySelector(".CodeMirror");
      const cm = w && w.CodeMirror;
      if (!cm) return false;
      window.__toTextAreaCalled = false;
      const orig = cm.toTextArea.bind(cm);
      cm.toTextArea = (...args) => {
        window.__toTextAreaCalled = true;
        return orig(...args);
      };
      return true;
    });
    ck("安装 toTextArea 探针", installed);
    await page.goto(BASE + "/#/problems", { waitUntil: "domcontentloaded" });
    await page.waitForSelector("#app table.data tbody tr", { timeout: 8000 });
    const called = await page.evaluate(() => window.__toTextAreaCalled === true);
    ck("离开页面调用 toTextArea 释放编辑器", called);
  });

  return JSON.stringify({ results, errors });
}
