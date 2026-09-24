#!/bin/bash
# M4.5 排行榜页面 DOM 级验证运行脚本（可选，不注册 CTest）。
#
# 真实启动 oj_server（隔离临时库 + 随机端口 + 静态托管 web/），用 sqlite3 预置一组
# 用户与做题状态（24 名，触发分页），再用 jsdom 执行 web/js 模块完成页面级验证。
# 需要 node、jsdom 与 sqlite3；不修改正式数据库。
#
# 用法：bash tests/frontend/run_m45.sh
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

TMP="$(mktemp -d /tmp/opencode/m45dom.XXXXXX)"
DB="$TMP/oj.db"
PORT="${OJ_FRONTEND_PORT:-$(( (RANDOM % 2000) + 21000 ))}"
export OJ_JWT_SECRET="m45-dom-verification-secret-0123456789"
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

# 注册一个已知密码的用户（用于当前用户高亮与登录），并预置做题状态。
M45_PASSWORD="UserPass123"
REG="$(curl -s -X POST "$BASE/api/register" -H 'Content-Type: application/json' \
  -d "{\"nickname\":\"m45dom\",\"password\":\"$M45_PASSWORD\"}")"
M45_ACCOUNT="$(printf '%s' "$REG" | sed -n 's/.*"account":"\([0-9]\{10\}\)".*/\1/p')"
if [ -z "$M45_ACCOUNT" ]; then
  echo "注册测试用户失败：$REG" >&2
  exit 1
fi

KNOWN_ID="$(sqlite3 "$DB" "SELECT id FROM users WHERE nickname='m45dom';")"
PROBLEM_ID="$(sqlite3 "$DB" "SELECT id FROM problems ORDER BY id LIMIT 1;")"
PROBLEM_ID2="$(sqlite3 "$DB" "SELECT id FROM problems ORDER BY id LIMIT 1 OFFSET 1;")"
if [ -z "$KNOWN_ID" ] || [ -z "$PROBLEM_ID" ] || [ -z "$PROBLEM_ID2" ]; then
  echo "获取测试用户/题目 ID 失败" >&2
  exit 1
fi

# 已知用户：两道题已 AC（ac=2，提交 3，首次 AC 2026-02-01），用于排名第 1 与高亮。
sqlite3 "$DB" "INSERT INTO user_problem_status (user_id,problem_id,status,first_ac_at,submit_count) VALUES ($KNOWN_ID,$PROBLEM_ID,'accepted','2026-02-01 00:00:00',2);"
sqlite3 "$DB" "INSERT INTO user_problem_status (user_id,problem_id,status,first_ac_at,submit_count) VALUES ($KNOWN_ID,$PROBLEM_ID2,'accepted','2026-02-02 00:00:00',1);"

# 另 23 名用户（均无 AC、提交 1），触发分页；无 AC 的首次 AC 显示「—」。
i=0
while [ "$i" -lt 23 ]; do
  acct="$(printf '9%09d' "$i")"
  created="$(printf '2026-03-%02d 00:00:00' "$((i + 1))")"
  sqlite3 "$DB" "INSERT INTO users (account,nickname,password_hash,role,created_at) VALUES ('$acct','m45u$i','x','user','$created');"
  uid="$(sqlite3 "$DB" "SELECT id FROM users WHERE nickname='m45u$i';")"
  sqlite3 "$DB" "INSERT INTO user_problem_status (user_id,problem_id,status,first_ac_at,submit_count) VALUES ($uid,$PROBLEM_ID,'none',NULL,1);"
  i=$((i + 1))
done

COUNT="$(sqlite3 "$DB" "SELECT COUNT(*) FROM user_problem_status;")"
if [ "$COUNT" -lt 24 ]; then
  echo "预置状态失败：期望至少 24，实际 $COUNT" >&2
  exit 1
fi

BASE="$BASE" M45_ACCOUNT="$M45_ACCOUNT" M45_PASSWORD="$M45_PASSWORD" \
  JSDOM_DIR="$JSDOM_DIR" node "$ROOT/tests/frontend/m45_leaderboard_dom.mjs"
RC=$?
if [ "$RC" -ne 0 ]; then
  echo "--- server.log tail ---" >&2
  tail -20 "$TMP/server.log" >&2
fi
exit "$RC"
