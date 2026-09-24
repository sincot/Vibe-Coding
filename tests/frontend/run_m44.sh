#!/bin/bash
# M4.4 提交历史与详情页面 DOM 级验证运行脚本（可选，不注册 CTest）。
#
# 真实启动 oj_server（隔离临时库 + 随机端口 + 静态托管 web/），用 sqlite3 预置一组
# 提交记录，再用 jsdom 执行 web/js 模块完成页面级验证。需要 node、jsdom 与 sqlite3；
# 不修改正式数据库。
#
# 用法：bash tests/frontend/run_m44.sh
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
if ! command -v sqlite3 >/dev/null 2>&1; then
  echo "未找到 sqlite3 命令行工具（用于预置测试数据）。" >&2
  exit 2
fi

TMP="$(mktemp -d /tmp/opencode/m44dom.XXXXXX)"
DB="$TMP/oj.db"
PORT="${OJ_FRONTEND_PORT:-$(( (RANDOM % 2000) + 19000 ))}"
export OJ_JWT_SECRET="m44-dom-verification-secret-0123456789"
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

BASE="http://127.0.0.1:$PORT"
for _ in $(seq 1 60); do
  curl -sf "$BASE/api/health" >/dev/null 2>&1 && break
  sleep 0.2
done
if ! curl -sf "$BASE/api/health" >/dev/null 2>&1; then
  echo "服务未成功启动，日志：$TMP/server.log" >&2
  tail -20 "$TMP/server.log" >&2
  exit 1
fi

# 注册一个已知密码的用户，并预置提交记录（不经过判题，聚焦 M4.4 读取页面）。
M44_PASSWORD="UserPass123"
REG="$(curl -s -X POST "$BASE/api/register" -H 'Content-Type: application/json' \
  -d "{\"nickname\":\"m44dom\",\"password\":\"$M44_PASSWORD\"}")"
M44_ACCOUNT="$(printf '%s' "$REG" | sed -n 's/.*"account":"\([0-9]\{10\}\)".*/\1/p')"
if [ -z "$M44_ACCOUNT" ]; then
  echo "注册测试用户失败：$REG" >&2
  exit 1
fi

USER_ID="$(sqlite3 "$DB" "SELECT id FROM users WHERE nickname='m44dom';")"
PROBLEM_ID="$(sqlite3 "$DB" "SELECT id FROM problems ORDER BY id LIMIT 1;")"
if [ -z "$USER_ID" ] || [ -z "$PROBLEM_ID" ]; then
  echo "获取测试用户/题目 ID 失败" >&2
  exit 1
fi

ACC_CASE='[{"index":0,"status":"AC","time_ms":1,"memory_kb":1024}]'
WA_CASE="[{\"index\":0,\"status\":\"WA\",\"time_ms\":2,\"memory_kb\":2048,\"reason\":\"non_zero_exit\",\"actual_output\":\"0\\n\",\"input\":\"1 2\\n\",\"expected_output\":\"3\\n\"}]"

i=0
while [ "$i" -lt 23 ]; do
  mm="$(printf '%02d' "$i")"
  sqlite3 "$DB" "INSERT INTO submissions (user_id,problem_id,language,source_code,status,per_case,compile_msg,runtime_ms,memory_kb,created_at) VALUES ($USER_ID,$PROBLEM_ID,'cpp17','int main(){}','AC','$ACC_CASE','',1,1024,'2026-09-21 12:$mm:00');"
  i=$((i + 1))
done
# 最新一条为 WA，带输入/期望/实际输出，供详情页渲染验证。
sqlite3 "$DB" "INSERT INTO submissions (user_id,problem_id,language,source_code,status,per_case,compile_msg,runtime_ms,memory_kb,created_at) VALUES ($USER_ID,$PROBLEM_ID,'cpp17','int main(){ /* WA source */ return 1; }','WA','$WA_CASE','',2,2048,'2026-09-21 12:59:00');"

SEEDED="$(sqlite3 "$DB" "SELECT COUNT(*) FROM submissions WHERE user_id=$USER_ID;")"
if [ "$SEEDED" != "24" ]; then
  echo "预置提交记录失败：期望 24，实际 $SEEDED（库 $DB）" >&2
  sqlite3 "$DB" "SELECT * FROM submissions WHERE user_id=$USER_ID;" >&2
  exit 1
fi

BASE="$BASE" M44_ACCOUNT="$M44_ACCOUNT" M44_PASSWORD="$M44_PASSWORD" \
  JSDOM_DIR="$JSDOM_DIR" node "$ROOT/tests/frontend/m44_history_dom.mjs"
RC=$?
if [ "$RC" -ne 0 ]; then
  echo "--- server.log tail ---" >&2
  tail -20 "$TMP/server.log" >&2
fi
exit "$RC"
