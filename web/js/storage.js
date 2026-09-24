// 会话级「登录后返回目标」存储。
//
// 仅保存在当前标签页的 sessionStorage（刷新保留、关标签即弃），且只存放经
// 路由校验通过的站内路径。不存放 token、口令或任何敏感数据。
//
// 与凭证存储分开：退出登录时由 nav 同时清理本目标，避免退出后重新登录又被
// 带回旧的受保护页面。

const PENDING_TARGET_KEY = "oj.pendingTarget";

export function savePendingTarget(target) {
  if (!target) return;
  try {
    sessionStorage.setItem(PENDING_TARGET_KEY, String(target));
  } catch (error) {
    /* sessionStorage 不可用时仅本次跳转内有效 */
  }
}

export function peekPendingTarget() {
  try {
    return sessionStorage.getItem(PENDING_TARGET_KEY) || "";
  } catch (error) {
    return "";
  }
}

export function clearPendingTarget() {
  try {
    sessionStorage.removeItem(PENDING_TARGET_KEY);
  } catch (error) {
    /* 忽略 */
  }
}

export function consumePendingTarget() {
  const value = peekPendingTarget();
  clearPendingTarget();
  return value;
}
