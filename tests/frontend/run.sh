#!/bin/bash
# M2.5 后台管理页面 DOM 级验证的运行脚本（可选，不注册 CTest）。
#
# 真实启动 oj_server（隔离临时库 + 随机端口 + 静态托管 web/），再用 jsdom 执行
# web/js 模块完成页面级验证。需要 node 与 jsdom；不修改正式数据库。
#
# 用法：bash tests/frontend/run.sh
# 可选环境变量：JSDOM_DIR（含 node_modules/jsdom 的目录，默认 /tmp/opencode/domtest）
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JSDOM_DIR="${JSDOM_DIR:-/tmp/opencode/domtest}"
SERVER="$ROOT/build/oj_server"

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

TMP="$(mktemp -d /tmp/opencode/m25dom.XXXXXX)"
DB="$TMP/oj.db"
PORT="${OJ_FRONTEND_PORT:-$(( (RANDOM % 2000) + 18000 ))}"
export OJ_JWT_SECRET="m25-dom-verification-secret-0123456789"
export OJ_ADMIN_PASSWORD="AdminPass123"

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

BASE="http://127.0.0.1:$PORT" JSDOM_DIR="$JSDOM_DIR" \
  node "$ROOT/tests/frontend/admin_pages_dom.mjs"
RC=$?
if [ "$RC" -ne 0 ]; then
  echo "--- server.log tail ---" >&2
  tail -20 "$TMP/server.log" >&2
fi
exit "$RC"
