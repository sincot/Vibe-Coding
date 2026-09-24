// 认证状态的本地保存与全局状态机。
//
// 状态区分（SPEC M4.1）：
//   - "unknown"：本地存在 token，但尚未通过 /api/me 核实身份（初始化/刷新期间）；
//   - "guest"：未登录，或 token 无法核实（含失效后清理）；
//   - "authenticated"：已通过 /api/me 核实，currentUser 为服务端最新用户信息。
//
// token 与用户信息保存在 localStorage，由 api.js 在请求时读取并放入
// Authorization 头。token 绝不拼接进 URL、绝不打日志。退出登录或身份失效时
// 调用 clearAuth() 立即清理本地凭证与用户状态，并递增 epoch——使得退出前发起、
// 退出后才返回的旧请求无法重新恢复已退出的身份。
//
// 注意：清理本地凭证只是前端登出，并不撤销后端已签发的 JWT（SPEC 未要求会话
// 撤销机制），界面文案不声称已完成服务端登出。

const TOKEN_KEY = "oj.token";
const USER_KEY = "oj.user";

export const AUTH_UNKNOWN = "unknown";
export const AUTH_GUEST = "guest";
export const AUTH_AUTHENTICATED = "authenticated";

let status = "unknown";
let currentUser = null;
// epoch 在登录/退出等身份切换时递增，用于丢弃跨身份边界的过期响应。
let epoch = 0;
const listeners = new Set();

function readStorage(key) {
  try {
    return localStorage.getItem(key);
  } catch (error) {
    return null;
  }
}

function writeStorage(key, value) {
  try {
    localStorage.setItem(key, value);
  } catch (error) {
    /* localStorage 不可用时仅保留内存状态 */
  }
}

function removeStorage(key) {
  try {
    localStorage.removeItem(key);
  } catch (error) {
    /* 忽略 */
  }
}

function persistUser(user) {
  if (user) writeStorage(USER_KEY, JSON.stringify(user));
  else removeStorage(USER_KEY);
}

function emit() {
  for (const listener of [...listeners]) {
    try {
      listener({ status, user: currentUser });
    } catch (error) {
      /* 单个订阅者异常不影响其它订阅者 */
    }
  }
}

// 初始化：有 token 时进入「待核实」，否则直接视为游客。
if (readStorage(TOKEN_KEY)) {
  status = AUTH_UNKNOWN;
  currentUser = null;
} else {
  status = AUTH_GUEST;
  currentUser = null;
}

export function getToken() {
  return readStorage(TOKEN_KEY) || "";
}

export function hasToken() {
  return getToken() !== "";
}

export function getAuthStatus() {
  return status;
}

export function getUser() {
  return currentUser;
}

export function getAuthEpoch() {
  return epoch;
}

export function isLoggedIn() {
  return status === AUTH_AUTHENTICATED;
}

export function isAdmin() {
  return (
    status === AUTH_AUTHENTICATED &&
    !!currentUser &&
    currentUser.role === "admin" &&
    !currentUser.reset_pwd_flag
  );
}

export function requiresPasswordChange() {
  return status === AUTH_AUTHENTICATED && !!currentUser && !!currentUser.reset_pwd_flag;
}

export function subscribeAuth(listener) {
  if (typeof listener !== "function") return () => {};
  listeners.add(listener);
  return () => listeners.delete(listener);
}

// 登录成功或身份核实成功后调用：切换到已登录状态并记录服务端用户信息。
export function setAuth(token, user) {
  epoch += 1;
  if (token) writeStorage(TOKEN_KEY, token);
  else removeStorage(TOKEN_KEY);
  currentUser = user || null;
  status = token ? AUTH_AUTHENTICATED : AUTH_GUEST;
  persistUser(currentUser);
  emit();
}

// 刷新已登录用户的本地信息（如改密后 /api/me 回查、权限变化刷新）。
export function setUser(user) {
  currentUser = user || null;
  if (user) {
    if (status !== AUTH_AUTHENTICATED) status = AUTH_AUTHENTICATED;
    persistUser(user);
  } else {
    persistUser(null);
  }
  emit();
}

// token 存在但无法核实身份（网络故障等）：按游客处理访问决策，但不主动清除
// token，避免把暂时性故障误判为凭证失效。
export function setUnverified() {
  currentUser = null;
  removeStorage(USER_KEY);
  status = AUTH_GUEST;
  emit();
}

// 退出登录 / 身份失效：清理本地凭证与用户状态。
export function clearAuth() {
  epoch += 1;
  removeStorage(TOKEN_KEY);
  removeStorage(USER_KEY);
  currentUser = null;
  status = AUTH_GUEST;
  emit();
}
