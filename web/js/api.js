// 统一 API 请求封装（fetch）：
//   - 自动序列化 JSON 请求体、解析 JSON 响应；
//   - 自动携带 Authorization: Bearer <token>（token 只放请求头，绝不放 URL/日志）；
//   - 统一 HTTP 错误与网络异常为 ApiError，保留 status 与业务 code 供页面按
//     接口约定分支处理，不把不同原因压成同一提示，也不依赖中文文案匹配；
//   - 身份失效（401）时清理失效凭证并通知上层跳转登录，避免重复跳转；
//   - 后端返回「必须先改密」时通知上层引导到改密页；
//   - 支持 AbortSignal：页面切换可取消无用的读取请求。写请求不随页面切换取消，
//     前端停止等待不代表后端已取消（沿用网络失败语义）；
//   - 对所有请求设置超时上限：后端不可达/连接被接受却无响应时，不再让界面永久停留
//     在「加载中」，而是抛出明确的超时错误（写请求同样按“结果无法确认”处理、不重试）。

import { clearAuth, getToken } from "./auth.js";

// 请求超时（毫秒）。可通过请求选项 timeoutMs 覆盖（测试用更小值）。
export const DEFAULT_TIMEOUT_MS = 15000;

export class ApiError extends Error {
  constructor(message, options = {}) {
    super(message);
    this.name = "ApiError";
    this.status = options.status || 0;
    this.code = options.code || "";
    this.network = !!options.network;
    this.aborted = !!options.aborted;
    this.timeout = !!options.timeout;
    this.retryable = !!options.retryable;
    this.retryAfterSeconds = options.retryAfterSeconds || 0;
    this.body = options.body || null;
  }

  isTimeout() {
    return this.timeout;
  }

  // 受保护请求认证失效（清理凭证并引导登录）。
  isAuthInvalid() {
    return this.status === 401;
  }

  // 后端要求先完成首次改密。
  isPasswordChangeRequired() {
    return this.status === 403 && this.code === "PASSWORD_CHANGE_REQUIRED";
  }

  // 普通权限不足（非改密要求）。
  isForbidden() {
    return this.status === 403 && !this.isPasswordChangeRequired();
  }

  isNotFound() {
    return this.status === 404;
  }

  isConflict() {
    return this.status === 409;
  }

  isRateLimited() {
    return this.status === 429;
  }

  // 判题队列满载 / 调度器停止等暂时不可用。
  isUnavailable() {
    return this.status === 503;
  }

  isServerError() {
    return this.status >= 500;
  }
}

let unauthorizedHandler = null;
let passwordRequiredHandler = null;

export function setUnauthorizedHandler(handler) {
  unauthorizedHandler = handler;
}

export function setPasswordRequiredHandler(handler) {
  passwordRequiredHandler = handler;
}

// 避免多个并发请求同时 401 时重复触发跳转。
let unauthorizedNotified = false;
export function resetUnauthorizedGuard() {
  unauthorizedNotified = false;
}

function notifyUnauthorized() {
  if (unauthorizedNotified) return;
  unauthorizedNotified = true;
  if (unauthorizedHandler) unauthorizedHandler();
}

// 避免多个并发请求同时要求改密时重复跳转/重复提示。
let passwordChangeNotified = false;
export function resetPasswordChangeGuard() {
  passwordChangeNotified = false;
}

function notifyPasswordChangeRequired() {
  if (passwordChangeNotified) return;
  passwordChangeNotified = true;
  if (passwordRequiredHandler) passwordRequiredHandler();
}

// 解析 Retry-After（秒）。仅接受非负整数；缺失/非法（如 HTTP-date）返回 0，
// 由调用方回退到默认提示，不猜测时间。
function parseRetryAfter(value) {
  if (!value) return 0;
  const seconds = Number.parseInt(String(value).trim(), 10);
  return Number.isFinite(seconds) && seconds > 0 ? seconds : 0;
}

export async function request(method, path, options = {}) {
  const {
    body = null,
    auth = true,
    skipAuthRedirect = false,
    signal = undefined,
    timeoutMs = DEFAULT_TIMEOUT_MS,
  } = options;

  const headers = {};
  let payload;
  if (body !== null && body !== undefined) {
    headers["Content-Type"] = "application/json";
    payload = JSON.stringify(body);
  }

  const token = auth ? getToken() : "";
  if (token) {
    headers["Authorization"] = "Bearer " + token;
  }

  // 请求级超时：与调用方传入的 AbortSignal 合并。超时与“调用方主动取消”区分，
  // 前者提示结果无法确认，后者由页面切换静默忽略。
  const controller =
    typeof AbortController !== "undefined" ? new AbortController() : null;
  let timedOut = false;
  let timer = null;
  if (controller) {
    if (signal) {
      if (signal.aborted) controller.abort();
      else signal.addEventListener("abort", () => controller.abort(), { once: true });
    }
    if (timeoutMs > 0) {
      timer = setTimeout(() => {
        timedOut = true;
        controller.abort();
      }, timeoutMs);
    }
  }

  let response;
  let data = null;
  try {
    response = await fetch(path, {
      method,
      headers,
      body: payload,
      credentials: "same-origin",
      signal: controller ? controller.signal : signal,
    });

    const contentType = response.headers.get("content-type") || "";
    if (response.status !== 204) {
      const text = await response.text();
      if (text && contentType.includes("application/json")) {
        try {
          data = JSON.parse(text);
        } catch (error) {
          data = null;
        }
      }
    }
  } catch (error) {
    if (timedOut) {
      throw new ApiError("请求超时，无法确认结果，请检查网络后重试", {
        network: true,
        timeout: true,
      });
    }
    // 页面切换触发的主动取消：不是网络故障，调用方据此静默忽略。
    if (error && (error.name === "AbortError" || error.code === 20)) {
      throw new ApiError("请求已取消", { aborted: true });
    }
    // 网络中断 / 连接失败：无法得知请求是否到达后端。
    throw new ApiError("网络连接失败，无法确认请求结果，请检查网络后重试", {
      network: true,
    });
  } finally {
    if (timer) clearTimeout(timer);
  }

  if (response.ok) {
    // 成功请求后重置去重标记，便于后续再次失效/再次要求改密时能重新处理。
    unauthorizedNotified = false;
    passwordChangeNotified = false;
    return data;
  }

  const message =
    data && typeof data.error === "string"
      ? data.error
      : `请求失败（HTTP ${response.status}）`;
  const code = data && typeof data.code === "string" ? data.code : "";
  const retryable = !!(data && data.retryable);
  // 限速（429）时后端通过 Retry-After 头返回还需等待的秒数，供界面提示解除时间。
  const retryAfterSeconds = parseRetryAfter(response.headers.get("Retry-After"));
  const apiError = new ApiError(message, {
    status: response.status,
    code,
    retryable,
    retryAfterSeconds,
    body: data,
  });

  // 身份失效：清理本地失效凭证并只触发一次登录跳转。
  // 注意：改密接口的 401 表示「旧密码错误」而非身份失效，调用方通过
  // skipAuthRedirect 明确排除，避免把表单错误误判为登录失效而退出登录。
  if (response.status === 401 && token && !skipAuthRedirect) {
    clearAuth();
    notifyUnauthorized();
  }

  if (response.status === 403 && code === "PASSWORD_CHANGE_REQUIRED") {
    notifyPasswordChangeRequired();
  }

  throw apiError;
}

export const api = {
  get: (path, options) => request("GET", path, options),
  post: (path, body, options) => request("POST", path, { ...options, body }),
  put: (path, body, options) => request("PUT", path, { ...options, body }),
  delete: (path, options) => request("DELETE", path, options),
};
