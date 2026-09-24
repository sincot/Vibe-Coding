#!/bin/bash
# 开发/调试启动 oj_server 的辅助脚本。
#
# 目的：避免因判题工作目录不是 tmpfs 而让服务启动即退出（退出码 1），导致端口转发
# 后浏览器一直空白/加载。脚本会自动为判题选择可用的 tmpfs，并在启动前打印监听地址。
#
# 用法：
#   OJ_JWT_SECRET="$(openssl rand -hex 32)" OJ_ADMIN_PASSWORD='AdminPass123' \
#     bash scripts/run_dev_server.sh
#
# 可选环境变量：
#   OJ_DB / OJ_HOST / OJ_PORT / OJ_WEB / OJ_JUDGE_WORKSPACE / OJ_JUDGE_ALLOW_NON_TMPFS
#   OJ_SEED=1            启动前先 --seed 导入种子题（幂等）
#   OJ_GEN_SECRET=1      未提供 OJ_JWT_SECRET 时自动生成（重启后旧 token 失效，仅开发）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="$ROOT/build/oj_server"
DB="${OJ_DB:-$ROOT/data/oj.db}"
HOST="${OJ_HOST:-0.0.0.0}"
PORT="${OJ_PORT:-8080}"
WEB="${OJ_WEB:-$ROOT/web}"

if [ ! -x "$SERVER" ]; then
  echo "[run_dev_server] 未找到 $SERVER，请先构建：cmake --build $ROOT/build --parallel 1" >&2
  exit 2
fi

is_tmpfs() {
  [ -d "$1" ] && [ "$(stat -f -c %T "$1" 2>/dev/null || true)" = "tmpfs" ]
}

# 判题工作目录：优先已配置的值；否则依次尝试 /opt/oj-tmpfs、/dev/shm；都没有则显式
# 允许非 tmpfs（仅开发验证，会打印告警）。
if [ -z "${OJ_JUDGE_WORKSPACE:-}" ] && [ -z "${OJ_JUDGE_ALLOW_NON_TMPFS:-}" ]; then
  if is_tmpfs /opt/oj-tmpfs; then
    export OJ_JUDGE_WORKSPACE=/opt/oj-tmpfs
  elif is_tmpfs /dev/shm; then
    export OJ_JUDGE_WORKSPACE=/dev/shm
    echo "[run_dev_server] 提示：/opt/oj-tmpfs 未挂载，判题工作目录改用 /dev/shm（tmpfs）" >&2
  else
    export OJ_JUDGE_ALLOW_NON_TMPFS=1
    echo "[run_dev_server] 警告：未找到可用 tmpfs，已设置 OJ_JUDGE_ALLOW_NON_TMPFS=1（仅限开发）" >&2
  fi
fi

if [ -z "${OJ_JWT_SECRET:-}" ]; then
  if [ "${OJ_GEN_SECRET:-0}" = "1" ]; then
    OJ_JWT_SECRET="$(openssl rand -hex 32)"
    export OJ_JWT_SECRET
    echo "[run_dev_server] 警告：已自动生成随机 OJ_JWT_SECRET（重启后旧 token 失效，仅限开发）" >&2
  else
    echo "[run_dev_server] 错误：请设置 OJ_JWT_SECRET（长度 ≥ 16 字节），或设 OJ_GEN_SECRET=1 自动生成" >&2
    exit 2
  fi
fi

if [ ! -f "$DB" ] && [ -z "${OJ_ADMIN_PASSWORD:-}" ]; then
  echo "[run_dev_server] 错误：数据库 $DB 尚不存在，首次初始化需要 OJ_ADMIN_PASSWORD 设置初始管理员密码" >&2
  exit 2
fi

if [ "${OJ_SEED:-0}" = "1" ]; then
  echo "[run_dev_server] 导入种子题（幂等）..." >&2
  "$SERVER" --db "$DB" --seed
fi

echo "[run_dev_server] 监听 http://$HOST:$PORT/（web=$WEB, db=$DB, workspace=${OJ_JUDGE_WORKSPACE:-非 tmpfs 开发模式}）" >&2
echo "[run_dev_server] 自检命令：curl -sS --max-time 5 http://127.0.0.1:$PORT/api/health" >&2
exec "$SERVER" --host "$HOST" --port "$PORT" --db "$DB" --web "$WEB"
