#!/bin/bash
# M4.3 前端覆盖率采集运行脚本（可选，不注册 CTest）。
#
# 在与 run_m43.sh 相同的隔离服务下执行 m43_problem_page_dom.mjs，并用 c8 基于 Node
# 内置 V8 覆盖率采集被执行的 web/js 模块的行/函数覆盖率（前端无构建、无测试框架，
# c8 仅作为本地报告工具，不属于项目运行依赖）。
#
# 依赖：node + c8（例如：npm install --prefix /tmp/opencode/covtool c8）。
# 用法：bash tests/frontend/run_m43_coverage.sh
# 可选环境变量：C8（c8 可执行文件，默认 /tmp/opencode/covtool/node_modules/.bin/c8）、
#   JSDOM_DIR、OJ_FRONTEND_PORT、FRONTEND_COVERAGE_REPORT（把 text 报告另存到该文件）。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JSDOM_DIR="${JSDOM_DIR:-/tmp/opencode/domtest}"
SERVER="$ROOT/build/oj_server"
C8="${C8:-}"
if [ -z "$C8" ]; then
  if [ -x /tmp/opencode/covtool/node_modules/.bin/c8 ]; then
    C8=/tmp/opencode/covtool/node_modules/.bin/c8
  else
    C8=c8
  fi
fi

if [ ! -x "$SERVER" ]; then
  echo "未找到可执行文件 $SERVER，请先构建：cmake --build $ROOT/build --parallel 1" >&2
  exit 2
fi
if ! command -v node >/dev/null 2>&1; then
  echo "未找到 node，请先安装 Node.js。" >&2
  exit 2
fi
if [ ! -d "$JSDOM_DIR/node_modules/jsdom" ]; then
  echo "未找到 jsdom（$JSDOM_DIR/node_modules/jsdom）。" >&2
  echo "请安装：npm install --prefix $JSDOM_DIR jsdom" >&2
  exit 2
fi
if ! command -v "$C8" >/dev/null 2>&1 && [ ! -x "$C8" ]; then
  echo "未找到 c8；安装：npm install --prefix /tmp/opencode/covtool c8" >&2
  exit 2
fi

TMP="$(mktemp -d /tmp/opencode/m43cov.XXXXXX)"
DB="$TMP/oj.db"
PORT="${OJ_FRONTEND_PORT:-$(( (RANDOM % 2000) + 21000 ))}"
export OJ_JWT_SECRET="m43-cov-verification-secret-0123456789"
export OJ_ADMIN_PASSWORD="AdminPass123"
export OJ_JUDGE_WORKSPACE="${OJ_JUDGE_WORKSPACE:-/dev/shm}"

SRV=""
cleanup() {
  if [ -n "$SRV" ]; then
    kill "$SRV" 2>/dev/null
    wait "$SRV" 2>/dev/null
  fi
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

cd "$ROOT"
BASE="http://127.0.0.1:$PORT" JSDOM_DIR="$JSDOM_DIR" \
  "$C8" --include 'web/js/**' --reporter=text --reporter=text-summary \
  --reports-dir "$TMP/cov" \
  node "$ROOT/tests/frontend/m43_problem_page_dom.mjs" 2>&1 | tee "$TMP/cov.log"
RC=${PIPESTATUS[0]}

if [ -n "${FRONTEND_COVERAGE_REPORT:-}" ]; then
  cp -f "$TMP/cov.log" "$FRONTEND_COVERAGE_REPORT" 2>/dev/null || true
  echo "覆盖率报告已另存：$FRONTEND_COVERAGE_REPORT"
fi

if [ "$RC" -ne 0 ]; then
  echo "--- server.log tail ---" >&2
  tail -20 "$TMP/server.log" >&2
fi
exit "$RC"
