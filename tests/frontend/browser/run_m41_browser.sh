#!/bin/bash
# M4.1 真实浏览器验证运行脚本（Linux 端；不注册 CTest）。
#
# 启动隔离的 oj_server（临时库 + 种子题 + 随机端口 + 静态托管 web/），再用
# playwright-cli 的真实 Chromium 打开页面并执行 M4.1 场景，输出逐项结果与截图。
#
# 依赖：node + 全局或本地 `playwright-cli`（@playwright/cli）及其浏览器。
#   npm install -g @playwright/cli@latest && playwright install chromium
# 可用 PWCLI 指定可执行文件，如 PWCLI="/path/to/node_modules/.bin/playwright-cli"。
# 若浏览器依赖库在非标准位置，可用 PWCLI_BROWSER_LIBS 指定 LD_LIBRARY_PATH 追加项。
#
# 用法：bash tests/frontend/browser/run_m41_browser.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PWCLI="${PWCLI:-playwright-cli}"
# 若 PWCLI 是相对路径，先转绝对路径，便于稍后切换到临时目录执行。
case "$PWCLI" in
  /*) ;;
  */*) PWCLI="$(cd "$(dirname "$PWCLI")" && pwd)/$(basename "$PWCLI")" ;;
esac
SERVER="$ROOT/build/oj_server"

if [ ! -x "$SERVER" ]; then
  echo "未找到 $SERVER，请先构建：cmake --build $ROOT/build --parallel 1" >&2
  exit 2
fi
if ! command -v "$PWCLI" >/dev/null 2>&1 && [ ! -x "$PWCLI" ]; then
  echo "未找到 playwright-cli；安装：npm install -g @playwright/cli@latest" >&2
  exit 2
fi
if [ -n "${PWCLI_BROWSER_LIBS:-}" ]; then
  export LD_LIBRARY_PATH="${PWCLI_BROWSER_LIBS}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

TMP="$(mktemp -d /tmp/opencode/m41browser.XXXXXX)"
DB="$TMP/oj.db"
PORT="${OJ_BROWSER_PORT:-$(( (RANDOM % 2000) + 21000 ))}"
export OJ_JWT_SECRET="m41-browser-verify-secret-0123456789"
export OJ_ADMIN_PASSWORD="AdminPass123"
export OJ_JUDGE_WORKSPACE="${OJ_JUDGE_WORKSPACE:-/dev/shm}"

SRV=""
cleanup() {
  if [ -n "$SRV" ]; then
    kill "$SRV" 2>/dev/null
    wait "$SRV" 2>/dev/null
  fi
  "$PWCLI" close-all >/dev/null 2>&1 || true
  rm -rf "$TMP"
}
trap cleanup EXIT

"$SERVER" --db "$DB" --seed > "$TMP/seed.log" 2>&1
"$SERVER" --host 127.0.0.1 --port "$PORT" --db "$DB" --web "$ROOT/web" > "$TMP/server.log" 2>&1 &
SRV=$!
for _ in $(seq 1 60); do
  curl -sf "http://127.0.0.1:$PORT/api/health" >/dev/null 2>&1 && break
  sleep 0.2
done
if ! curl -sf "http://127.0.0.1:$PORT/api/health" >/dev/null 2>&1; then
  echo "服务未成功启动，日志：$TMP/server.log" >&2
  tail -20 "$TMP/server.log" >&2
  exit 1
fi

OUT="$TMP/artifacts"
mkdir -p "$OUT"
sed -e "s#__BASE__#http://127.0.0.1:$PORT#g" -e "s#__OUT__#$OUT#g" \
  "$ROOT/tests/frontend/browser/m41_browser_scenarios.js" > "$TMP/scenarios.js"

# 在临时目录执行，避免 playwright-cli 在工作目录生成 .playwright-cli 产物。
cd "$TMP"
OPEN_ARGS=("open" "about:blank" "--browser=chromium")
if [ "${PWCLI_HEADED:-0}" = "1" ]; then
  OPEN_ARGS+=("--headed") # 有头模式（需要可用的显示环境，如本机桌面或 Xvfb）
fi
"$PWCLI" "${OPEN_ARGS[@]}" > "$TMP/open.log" 2>&1
"$PWCLI" run-code --filename="$TMP/scenarios.js" | tee "$TMP/result.log"
RC=${PIPESTATUS[0]}
"$PWCLI" close >/dev/null 2>&1

# 可选：把截图另存到指定目录，保留证据（默认随临时目录清理）。
if [ -n "${BROWSER_ARTIFACTS_DIR:-}" ]; then
  mkdir -p "$BROWSER_ARTIFACTS_DIR"
  cp -f "$OUT"/*.png "$BROWSER_ARTIFACTS_DIR"/ 2>/dev/null || true
  echo "截图已另存：$BROWSER_ARTIFACTS_DIR"
fi
echo "截图目录：$OUT"
if [ "$RC" -ne 0 ]; then
  echo "--- server.log tail ---" >&2
  tail -20 "$TMP/server.log" >&2
fi
exit "$RC"
