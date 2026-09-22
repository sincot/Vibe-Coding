// 统一 API 请求封装（fetch）：
//   - 自动序列化 JSON 请求体、解析 JSON 响应；
//   - 自动携带 Authorization: Bearer <token>（token 只放请求头，绝不放 URL/日志）；
//   - 统一 HTTP 错误与网络异常为 ApiError，页面据此展示可读提示；
//   - 身份失效（401）时清理失效凭证并通知上层跳转登录，避免重复跳转；
//   - 后端返回「必须先改密」时通知上层引导到改密页。

import { clearAuth, getToken } from "./auth.js";

export class ApiError extends Error {
  constructor(message, options = {}) {
    super(message);
    this.name = "ApiError";
    this.status = options.status || 0;
    this.code = options.code || "";
    this.network = !!options.network;
    this.body = options.body || null;
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

export async function request(method, path, options = {}) {
  const { body = null, auth = true, skipAuthRedirect = false } = options;

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

  let response;
  try {
    response = await fetch(path, {
      method,
      headers,
      body: payload,
      credentials: "same-origin",
    });
  } catch (error) {
    // 网络中断 / 连接失败：无法得知请求是否到达后端。
    throw new ApiError("网络连接失败，无法确认请求结果，请检查网络后重试", {
      network: true,
    });
  }

  let data = null;
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

  if (response.ok) {
    // 成功请求后重置 401 去重标记，便于后续再次失效时能重新处理。
    unauthorizedNotified = false;
    return data;
  }

  const message =
    data && typeof data.error === "string"
      ? data.error
      : `请求失败（HTTP ${response.status}）`;
  const code = data && typeof data.code === "string" ? data.code : "";
  const apiError = new ApiError(message, {
    status: response.status,
    code,
    body: data,
  });

  // 身份失效：清理本地失效凭证并只触发一次登录跳转。
  // 注意：改密接口的 401 表示「旧密码错误」而非身份失效，调用方通过
  // skipAuthRedirect 明确排除，避免把表单错误误判为登录失效而退出登录。
  if (response.status === 401 && token && !skipAuthRedirect) {
    clearAuth();
    notifyUnauthorized();
  }

  if (
    response.status === 403 &&
    code === "PASSWORD_CHANGE_REQUIRED" &&
    passwordRequiredHandler
  ) {
    passwordRequiredHandler();
  }

  throw apiError;
}

export const api = {
  get: (path, options) => request("GET", path, options),
  post: (path, body, options) => request("POST", path, { ...options, body }),
  put: (path, body, options) => request("PUT", path, { ...options, body }),
  delete: (path, options) => request("DELETE", path, options),
};
