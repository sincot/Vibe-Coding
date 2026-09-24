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

// 题目列表的返回目标：从列表进入题目详情时记录当前列表地址（含搜索/筛选/页码），
// 详情页的「返回题目列表」据此恢复列表状态（M4.2）。仅保存在当前标签页，随刷新保留、
// 关标签即弃；不使用 consume 语义，以便反复往返。
const PROBLEMS_RETURN_KEY = "oj.problemsReturn";

export function saveProblemsReturn(target) {
  if (!target) return;
  try {
    sessionStorage.setItem(PROBLEMS_RETURN_KEY, String(target));
  } catch (error) {
    /* sessionStorage 不可用时退化为默认列表地址 */
  }
}

export function peekProblemsReturn() {
  try {
    return sessionStorage.getItem(PROBLEMS_RETURN_KEY) || "";
  } catch (error) {
    return "";
  }
}

// 提交历史的返回目标：从历史列表进入提交详情时记录当前列表地址（含分页与题目
// 筛选），详情页的「返回提交历史」据此恢复列表状态（M4.4）。仅保存在当前标签页，
// 随刷新保留、关标签即弃；不使用 consume 语义，以便反复往返。
const SUBMISSIONS_RETURN_KEY = "oj.submissionsReturn";

export function saveSubmissionsReturn(target) {
  if (!target) return;
  try {
    sessionStorage.setItem(SUBMISSIONS_RETURN_KEY, String(target));
  } catch (error) {
    /* sessionStorage 不可用时退化为默认历史地址 */
  }
}

export function peekSubmissionsReturn() {
  try {
    return sessionStorage.getItem(SUBMISSIONS_RETURN_KEY) || "";
  } catch (error) {
    return "";
  }
}
