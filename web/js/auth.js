// 认证状态的本地保存。
//
// token 与用户信息保存在 localStorage，由 api.js 在请求时读取并放入
// Authorization 头。token 绝不拼接进 URL、绝不打日志。退出登录或身份失效时
// 调用 clearAuth() 立即清理本地凭证与用户状态。
//
// 注意：清理本地凭证只是前端登出，并不撤销后端已签发的 JWT（SPEC 未要求会话
// 撤销机制），界面文案不声称已完成服务端登出。

const TOKEN_KEY = "oj.token";
const USER_KEY = "oj.user";

let cachedUser = loadUser();

function loadUser() {
  try {
    const raw = localStorage.getItem(USER_KEY);
    return raw ? JSON.parse(raw) : null;
  } catch (error) {
    return null;
  }
}

export function getToken() {
  try {
    return localStorage.getItem(TOKEN_KEY) || "";
  } catch (error) {
    return "";
  }
}

export function getUser() {
  return cachedUser;
}

export function isLoggedIn() {
  return getToken() !== "";
}

export function isAdmin() {
  const user = cachedUser;
  return !!user && user.role === "admin" && !user.reset_pwd_flag;
}

export function requiresPasswordChange() {
  const user = cachedUser;
  return !!user && !!user.reset_pwd_flag;
}

export function setUser(user) {
  cachedUser = user || null;
  try {
    if (cachedUser) {
      localStorage.setItem(USER_KEY, JSON.stringify(cachedUser));
    } else {
      localStorage.removeItem(USER_KEY);
    }
  } catch (error) {
    /* localStorage 不可用时仅保留内存状态 */
  }
}

export function setAuth(token, user) {
  try {
    localStorage.setItem(TOKEN_KEY, token);
  } catch (error) {
    /* 忽略存储异常，仍设置内存用户 */
  }
  setUser(user);
}

export function clearAuth() {
  try {
    localStorage.removeItem(TOKEN_KEY);
    localStorage.removeItem(USER_KEY);
  } catch (error) {
    /* 忽略 */
  }
  cachedUser = null;
}
