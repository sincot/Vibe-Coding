// 会话核实：通过 /api/me 用已有 token 核实当前用户，确保后台访问权限基于
// 服务端最新角色与首次改密标记，而不是本地保存的旧角色（SPEC M4.1）。
//
// 设计要点：
//   - 单飞（single-flight）：并发调用共享同一个 /api/me 请求，避免重复核实；
//   - epoch 校验：核实返回时若已发生登录/退出（epoch 变化）或 token 已更换，
//     丢弃该结果，避免旧请求恢复已退出的身份；
//   - 401 由 api.js 全局处理器清理凭证；其它失败按「未核实」处理，不清 token。

import { api } from "./api.js";
import {
  AUTH_UNKNOWN,
  getAuthEpoch,
  getAuthStatus,
  getToken,
  isLoggedIn,
  setAuth,
  setUnverified,
} from "./auth.js";

let inflight = null;
let lastCheckedToken = null;

// 已登录且不强制刷新时直接返回；否则用 token 核实一次。
export function ensureAuth(options = {}) {
  const force = !!options.force;
  if (isLoggedIn() && !force) return Promise.resolve(true);

  const token = getToken();
  if (!token) {
    if (getAuthStatus() !== "guest") setUnverified();
    lastCheckedToken = null;
    return Promise.resolve(false);
  }

  if (inflight) return inflight;
  if (!force && token === lastCheckedToken && getAuthStatus() !== AUTH_UNKNOWN) {
    return Promise.resolve(isLoggedIn());
  }

  lastCheckedToken = token;
  const startEpoch = getAuthEpoch();

  inflight = api
    .get("/api/me")
    .then((me) => {
      if (startEpoch !== getAuthEpoch() || getToken() !== token) {
        return isLoggedIn();
      }
      setAuth(token, me);
      return true;
    })
    .catch(() => {
      // 401 已由全局处理器清理；其它错误仅标记未核实，不主动清除 token。
      if (startEpoch === getAuthEpoch() && getToken() === token) {
        setUnverified();
      }
      return false;
    })
    .finally(() => {
      inflight = null;
    });

  return inflight;
}

// 登录/退出后重置单飞状态，使下一次 ensureAuth 重新核实。
export function resetSessionVerification() {
  inflight = null;
  lastCheckedToken = null;
}
