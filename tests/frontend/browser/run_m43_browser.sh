#!/bin/bash
# M4.3 真实浏览器验证运行脚本（Linux 端；不注册 CTest）。
#
# 启动隔离的 oj_server（临时库 + 种子题 + 随机端口 + 静态托管 web/），再用
# playwright-cli 的真实 Chromium 打开页面并执行 M4.3 场景（真实 CodeMirror CDN 加载与
# C/C++ 高亮、语言切换、ResizeObserver 触发的 refresh、窄视口布局与可操作性、
# 离开页面释放编辑器），输出逐项结果与截图。
#
# 依赖：node + `playwright-cli`（@playwright/cli）及其 Chromium。
#   npm install -g @playwright/cli@latest && playwright install chromium
# 可用 PWCLI 指定可执行文件；浏览器依赖库在非标准位置时用 PWCLI_BROWSER_LIBS 指定。
#
# 用法：bash tests/frontend/browser/run_m43_browser.sh
# 可选：PWCLI_HEADED=1（有头，需显示环境）、BROWSER_ARTIFACTS_DIR（保留截图）、
#       OJ_BROWSER_PORT（固定端口）。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PWCLI="${PWCLI:-}"
if [ -z "$PWCLI" ]; then
  if [ -x /tmp/opencode/pwcli/node_modules/.bin/playwright-cli ]; then
    PWCLI=/tmp/opencode/pwcli/node_modules/.bin/playwright-cli
  else
    PWCLI=playwright-cli
  fi
fi
case "$PWCLI" in
  /*) ;;
  */*) PWCLI="$(cd "$(dirname "$PWCLI")" && pwd)/$(basename "$PWCLI")" ;;
esac
if [ -z "${PWCLI_BROWSER_LIBS:-}" ] && [ -d /tmp/opencode/browserlibs/root/usr/lib/x86_64-linux-gnu ]; then
  PWCLI_BROWSER_LIBS=/tmp/opencode/browserlibs/root/usr/lib/x86_64-linux-gnu
fi
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

TMP="$(mktemp -d /tmp/opencode/m43browser.XXXXXX)"
DB="$TMP/oj.db"
PORT="${OJ_BROWSER_PORT:-$(( (RANDOM % 2000) + 23000 ))}"
export OJ_JWT_SECRET="m43-browser-verify-secret-0123456789"
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
  "$ROOT/tests/frontend/browser/m43_browser_scenarios.js" > "$TMP/scenarios.js"

cd "$TMP"
OPEN_ARGS=("open" "about:blank" "--browser=chromium")
if [ "${PWCLI_HEADED:-0}" = "1" ]; then
  OPEN_ARGS+=("--headed")
fi
"$PWCLI" "${OPEN_ARGS[@]}" > "$TMP/open.log" 2>&1
"$PWCLI" run-code --filename="$TMP/scenarios.js" > "$TMP/result.log" 2>&1
RC=$?

node -e '
const fs = require("fs");
const text = fs.readFileSync(process.argv[1], "utf8");
const parts = text.split(/^### Result\s*$/m);
if (parts.length < 2) {
  console.error("未找到场景返回结果，原始输出：");
  console.error(text.slice(0, 800));
  process.exit(1);
}
const line = parts[1].trim().split("\n")[0];
let value;
try {
  value = JSON.parse(line);
  if (typeof value === "string") value = JSON.parse(value);
} catch (e) {
  console.error("解析场景结果失败：" + e.message);
  console.error(line.slice(0, 400));
  process.exit(1);
}
const results = value.results || [];
const errors = value.errors || [];
const failed = results.filter((r) => !r.ok);
console.log(`M4.3 真实浏览器：通过 ${results.length - failed.length}/${results.length}`);
for (const f of failed) console.log("  FAIL: " + f.name + (f.detail ? " | " + f.detail : ""));
if (errors.length) {
  console.log(`脚本错误 ${errors.length} 项：`);
  for (const e of errors.slice(0, 8)) console.log("  - " + e);
}
process.exit(failed.length === 0 && errors.length === 0 ? 0 : 1);
' "$TMP/result.log"
PARSE_RC=$?

if [ -n "${BROWSER_ARTIFACTS_DIR:-}" ]; then
  mkdir -p "$BROWSER_ARTIFACTS_DIR"
  cp -f "$OUT"/*.png "$BROWSER_ARTIFACTS_DIR"/ 2>/dev/null || true
  echo "截图已另存：$BROWSER_ARTIFACTS_DIR"
fi
if [ "$PARSE_RC" -ne 0 ]; then
  echo "--- result.log tail ---" >&2
  tail -40 "$TMP/result.log" >&2
  echo "--- server.log tail ---" >&2
  tail -20 "$TMP/server.log" >&2
  exit "$PARSE_RC"
fi
if [ "$RC" -ne 0 ]; then
  echo "playwright-cli 退出码 $RC" >&2
  exit "$RC"
fi
exit 0
