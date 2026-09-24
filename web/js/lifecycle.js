// 页面生命周期：为每次路由渲染提供统一的「过期/清理」机制。
//
// 语义约定（与 SPEC M4.1 对齐）：
//   - 读取请求（GET）可以随页面切换被取消（AbortController），或通过
//     next()/isCurrent() 忽略过期响应；
//   - 写请求（提交、Rejudge、增删改等）不因页面切换而取消：前端停止等待
//     不等于后端已取消，写操作继续沿用「网络失败时结果无法确认、不自动重试」。
//
// 页面在渲染时从 context.lifecycle 取得实例；路由切换时由 router 调用
// dispose()，页面据此停止写入 DOM、释放事件与 abort 未完成的读取。

export function createLifecycle() {
  const controller = typeof AbortController !== "undefined" ? new AbortController() : null;
  let disposed = false;
  let generation = 0;
  // 页面自定义资源释放回调（如编辑器实例/监听器）。路由在切换页面时统一调用
  // dispose()，异步页面无需要求路由处理其返回的清理函数。
  const disposers = [];

  return {
    // 供 api.get(path, { signal }) 使用；仅在支持 AbortController 的环境下存在。
    get signal() {
      return controller ? controller.signal : undefined;
    },
    get disposed() {
      return disposed;
    },
    // 同一页面内发起新一次读取时递增，用于忽略旧响应。
    next() {
      generation += 1;
      return generation;
    },
    // token 来自 next()；返回 true 表示该响应仍属于当前页面且页面未销毁。
    isCurrent(token) {
      return !disposed && token === generation;
    },
    // 注册随页面销毁执行的清理回调，返回取消注册的函数。
    onDispose(fn) {
      if (typeof fn !== "function") return () => {};
      if (disposed) {
        try {
          fn();
        } catch (error) {
          /* 单个清理回调异常不影响其它清理 */
        }
        return () => {};
      }
      disposers.push(fn);
      return () => {
        const index = disposers.indexOf(fn);
        if (index >= 0) disposers.splice(index, 1);
      };
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      if (controller) {
        try {
          controller.abort();
        } catch (error) {
          /* 已中止或环境不支持，忽略 */
        }
      }
      const pending = disposers.splice(0, disposers.length);
      for (const fn of pending) {
        try {
          fn();
        } catch (error) {
          /* 单个清理回调异常不影响其它清理 */
        }
      }
    },
  };
}

// 兼容没有 context.lifecycle 的调用方（例如单元测试直接调用页面函数）。
export function ensureLifecycle(lifecycle) {
  return lifecycle && typeof lifecycle.dispose === "function"
    ? lifecycle
    : createLifecycle();
}
