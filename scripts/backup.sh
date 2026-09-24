#!/usr/bin/env bash
# =============================================================================
# scripts/backup.sh — OJ 数据库逻辑备份（M6.2）
#
# 定位：用 `sqlite3 .dump` 把运行中的 SQLite 数据库导出为单个 SQL 文本文件，
#       默认写入仓库内的 `backup/oj-YYYYMMDD.sql`。用于 cron 定期备份。
#
# 关键约定：
#   - 通过 SQLite 连接读取，WAL 中已提交的数据会被包含；不直接复制主库文件，
#     不删除/移动运行中的 -wal、-shm。
#   - 在单个读事务内导出，多张表来自同一数据库快照（WAL 模式下读事务不阻塞写入，
#     仅可能推迟 WAL 检查点）。
#   - 先写同目录临时文件，校验 sqlite3 退出码与完成条件后再原子替换为正式文件；
#     失败不留下看似可用的正式文件。
#   - 同一备份目录用 flock 互斥，避免并发任务同时写入。
#   - 备份文件权限 0600；backup/ 已被 .gitignore 忽略，不进入版本控制。
#
# 用法：
#   bash scripts/backup.sh
#   bash scripts/backup.sh --db /srv/oj/data/oj.db --out-dir /srv/oj/backup
#   bash scripts/backup.sh --no-replace        # 当天已存在则不覆盖
#   bash scripts/backup.sh --help
#
# 可配置环境变量：
#   OJ_DB                       源数据库路径（默认 <仓库>/data/oj.db）
#   OJ_BACKUP_DIR               备份目录（默认 <仓库>/backup）
#   OJ_BACKUP_TZ                日期所用时区（默认系统本地时区；如 Asia/Shanghai、UTC）
#   OJ_BACKUP_BUSY_TIMEOUT_MS   SQLite 锁等待毫秒数（默认 5000）
#   OJ_BACKUP_TIMEOUT           单次导出总执行超时秒数（默认 600）
#   OJ_BACKUP_LOCK_WAIT         等待备份互斥锁的秒数（默认 30，超时失败）
#   OJ_BACKUP_NO_REPLACE        取 1 时，当天文件已存在则跳过（默认替换）
#   OJ_BACKUP_SKIP_SPACE_CHECK  取 1 时跳过磁盘空间预检
#
# 退出码：0 成功（或按 --no-replace 跳过）；1 备份执行失败；2 用法/前置条件错误。
# 说明：成功返回 0、失败返回非零。调用方若用管道处理输出，需自行启用 pipefail，
#       否则管道退出码可能掩盖本脚本的退出码。
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

umask 077

# ---------------------------------------------------------------------------
# 默认配置（命令行参数优先于环境变量）
# ---------------------------------------------------------------------------
DB="${OJ_DB:-$ROOT/data/oj.db}"
BACKUP_DIR="${OJ_BACKUP_DIR:-$ROOT/backup}"
BUSY_TIMEOUT_MS="${OJ_BACKUP_BUSY_TIMEOUT_MS:-5000}"
EXEC_TIMEOUT="${OJ_BACKUP_TIMEOUT:-600}"
LOCK_WAIT="${OJ_BACKUP_LOCK_WAIT:-30}"
NO_REPLACE="${OJ_BACKUP_NO_REPLACE:-0}"
SKIP_SPACE_CHECK="${OJ_BACKUP_SKIP_SPACE_CHECK:-0}"
SPACE_FACTOR=2

usage() {
  sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//'
}

# ---------------------------------------------------------------------------
# 输出：正常信息走 stdout，错误走 stderr；不打印 SQL/密码哈希/源码/隐藏用例。
# ---------------------------------------------------------------------------
ts() { date '+%Y-%m-%d %H:%M:%S%z'; }
log() { printf '[%s] [backup] %s\n' "$(ts)" "$*"; }
err() { printf '[%s] [backup] 错误：%s\n' "$(ts)" "$*" >&2; }
fail_backup() { err "$*"; exit 1; }
fail_config() { err "$*"; usage >&2; exit 2; }

# ---------------------------------------------------------------------------
# 参数解析
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --db|--database)
      [[ $# -ge 2 ]] || fail_config "缺少 --db 的参数"
      DB="$2"; shift 2 ;;
    --out-dir)
      [[ $# -ge 2 ]] || fail_config "缺少 --out-dir 的参数"
      BACKUP_DIR="$2"; shift 2 ;;
    --no-replace) NO_REPLACE=1; shift ;;
    *) fail_config "未知参数：$1" ;;
  esac
done

# ---------------------------------------------------------------------------
# 依赖检查：缺失时明确失败，不静默降级。
# ---------------------------------------------------------------------------
MISSING=()
for tool in sqlite3 flock timeout mktemp stat dirname basename awk; do
  command -v "$tool" >/dev/null 2>&1 || MISSING+=("$tool")
done
if ((${#MISSING[@]} > 0)); then
  err "缺少必要依赖：${MISSING[*]}"
  echo "  提示：sqlite3 的安装见 dependence.md 3.2 节；flock/timeout/mktemp/stat 由" >&2
  echo "        util-linux 与 coreutils 提供（Ubuntu 默认已安装）。" >&2
  exit 2
fi

# 配置数值校验（非法时明确失败，避免超时/等待语义异常）。用 10# 前缀按十进制解析，
# 避免前导 0 被当作八进制；不为空且仅含数字。
for var in BUSY_TIMEOUT_MS EXEC_TIMEOUT LOCK_WAIT; do
  val="${!var}"
  if ! [[ "$val" =~ ^[0-9]+$ ]]; then
    fail_config "$var 必须是非负整数（当前='$val'）"
  fi
  printf -v "$var" '%s' "$((10#$val))"
done
((BUSY_TIMEOUT_MS > 0)) || fail_config "OJ_BACKUP_BUSY_TIMEOUT_MS 必须大于 0"
((EXEC_TIMEOUT > 0)) || fail_config "OJ_BACKUP_TIMEOUT 必须大于 0"

# ---------------------------------------------------------------------------
# 时区与日期：日志中明确时区；默认系统本地时区，可用 OJ_BACKUP_TZ 指定。
# ---------------------------------------------------------------------------
if [[ -n "${OJ_BACKUP_TZ:-}" ]]; then
  export TZ="$OJ_BACKUP_TZ"
fi
DATE="$(date '+%Y%m%d')"
TZ_DESC="$(date '+%Y-%m-%d %H:%M:%S %z (%Z)')"

# ---------------------------------------------------------------------------
# 源数据库前置校验：先确认存在且可读，避免 sqlite3 在路径错误时自动创建空库。
# ---------------------------------------------------------------------------
if [[ ! -e "$DB" ]]; then
  err "源数据库不存在：$DB（拒绝继续，避免 sqlite3 自动创建空库后误报成功）"
  exit 2
fi
if [[ ! -f "$DB" ]]; then
  err "源数据库不是普通文件：$DB"
  exit 2
fi
if [[ ! -r "$DB" ]]; then
  err "源数据库不可读：$DB"
  exit 2
fi
# 解析为绝对路径：既使后续不依赖调用者当前目录，也避免文件名以 '-' 开头被 sqlite3
# 误当作选项。
if ! db_dir="$(cd -- "$(dirname -- "$DB")" 2>/dev/null && pwd)"; then
  err "无法解析源数据库所在目录：$DB"
  exit 2
fi
DB="$db_dir/$(basename -- "$DB")"

# ---------------------------------------------------------------------------
# 备份目录：独立于调用者当前目录；不可创建/不可写时明确失败。
# ---------------------------------------------------------------------------
if ! mkdir -p -- "$BACKUP_DIR" 2>/dev/null; then
  err "无法创建备份目录：$BACKUP_DIR"
  exit 2
fi
if ! BACKUP_DIR="$(cd -- "$BACKUP_DIR" && pwd)"; then
  err "无法解析备份目录绝对路径：$BACKUP_DIR"
  exit 2
fi
if [[ ! -w "$BACKUP_DIR" ]]; then
  err "备份目录不可写：$BACKUP_DIR"
  exit 2
fi

TARGET="$BACKUP_DIR/oj-$DATE.sql"
TMP_FILE=""
ERR_FILE=""
DUMP_PID=""

# ---------------------------------------------------------------------------
# 清理：仅删除本次明确创建的临时文件；不批量删除备份目录中的任何文件。
# 中断/失败/正常退出均走此路径。
# ---------------------------------------------------------------------------
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  # 若导出进程仍在（中断场景），先终止本次启动的导出进程，避免其继续占用读事务；
  # 只终止本脚本记录的子进程 PID，不按进程名批量清理。
  if [[ -n "$DUMP_PID" ]] && kill -0 "$DUMP_PID" 2>/dev/null; then
    kill -TERM "$DUMP_PID" 2>/dev/null || true
    local i
    for i in 1 2 3 4 5; do
      kill -0 "$DUMP_PID" 2>/dev/null || break
      sleep 0.2
    done
    kill -KILL "$DUMP_PID" 2>/dev/null || true
  fi
  if [[ -n "$TMP_FILE" && -e "$TMP_FILE" ]]; then
    rm -f -- "$TMP_FILE" 2>/dev/null || true
  fi
  if [[ -n "$ERR_FILE" && -e "$ERR_FILE" ]]; then
    rm -f -- "$ERR_FILE" 2>/dev/null || true
  fi
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ---------------------------------------------------------------------------
# 目录级互斥：同一备份目录一次只允许一个备份进程（flock 随进程退出自动释放）。
# ---------------------------------------------------------------------------
LOCK_FILE="$BACKUP_DIR/.oj-backup.lock"
exec 9>"$LOCK_FILE" || fail_backup "无法创建锁文件：$LOCK_FILE"
if ! flock -w "$LOCK_WAIT" -x 9; then
  fail_backup "等待备份锁超时（${LOCK_WAIT}s）：另一个备份任务可能正在运行（$LOCK_FILE）"
fi

log "开始备份：db=$DB -> $TARGET"
log "时区：$TZ_DESC（可用 OJ_BACKUP_TZ 指定，如 Asia/Shanghai 或 UTC）"

# ---------------------------------------------------------------------------
# 磁盘空间预检：不足时明确失败，绝不自动清理其它数据腾空间。
# 预检是启发式估计（SQL 文本可能比二进制更大），完成后仍以完成条件与退出码为准。
# ---------------------------------------------------------------------------
if [[ "$SKIP_SPACE_CHECK" != "1" ]]; then
  db_bytes="$(stat -c %s -- "$DB" 2>/dev/null || echo 0)"
  wal_bytes=0
  [[ -f "$DB-wal" ]] && wal_bytes="$(stat -c %s -- "$DB-wal" 2>/dev/null || echo 0)"
  need=$(( (db_bytes + wal_bytes) * SPACE_FACTOR + 1048576 ))
  avail_kb="$(df -Pk "$BACKUP_DIR" 2>/dev/null | awk 'NR==2{print $4}' | tr -dc '0-9')"
  if [[ -n "$avail_kb" ]]; then
    avail_bytes=$(( avail_kb * 1024 ))
    if (( avail_bytes < need )); then
      fail_backup "备份目录可用空间不足：可用约 $((avail_bytes / 1048576)) MiB，" \
        "按源库(含 WAL)约 $(( (db_bytes + wal_bytes) / 1048576 )) MiB 的 ${SPACE_FACTOR} 倍估算需约" \
        "$((need / 1048576)) MiB。请腾出空间或改用其它 --out-dir；本脚本不会自动清理数据。"
    fi
    log "空间预检通过：源库(含 WAL)约 $(( (db_bytes + wal_bytes) / 1024 )) KiB，可用约 $((avail_bytes / 1048576)) MiB"
  else
    log "警告：无法读取备份目录可用空间，跳过空间预检（仍会校验完成条件）"
  fi
fi

# ---------------------------------------------------------------------------
# 创建本次临时文件（0600），与正式文件同目录以保证原子替换。
# 同日重跑：默认写新文件后原子替换；--no-replace 则保留已有文件并跳过。
# ---------------------------------------------------------------------------
if [[ -e "$TARGET" && "$NO_REPLACE" == "1" ]]; then
  log "已存在 $TARGET，按 --no-replace/OJ_BACKUP_NO_REPLACE=1 跳过，不覆盖已有备份"
  exit 0
fi

TMP_FILE="$(mktemp -- "$BACKUP_DIR/.oj-${DATE}.XXXXXX.sql.tmp")" \
  || fail_backup "无法在备份目录创建临时文件：$BACKUP_DIR"
ERR_FILE="$(mktemp "${TMPDIR:-/tmp}/oj-backup-err.XXXXXX")" \
  || fail_backup "无法创建错误日志临时文件"

# ---------------------------------------------------------------------------
# 导出：
#   - 单连接内 `BEGIN;` 建立一致读快照，随后 `.dump`，再 `COMMIT;`；
#     WAL 模式下读事务不阻塞业务写入（仅可能推迟检查点）。
#   - `.timeout` 设置锁等待；外层 `timeout` 限制总执行时间。
#   - stdout 直接流式写入临时文件，不把导出内容读入内存。
#   - `-bail` 保证出错即停并返回非零。
# ---------------------------------------------------------------------------
START_SECONDS=$SECONDS
rc=0
timeout --signal=TERM --kill-after=30 "$EXEC_TIMEOUT" \
  sqlite3 -batch -bail "$DB" \
    ".timeout $BUSY_TIMEOUT_MS" "BEGIN;" ".dump" "COMMIT;" \
  >"$TMP_FILE" 2>"$ERR_FILE" &
DUMP_PID=$!
wait "$DUMP_PID"; rc=$?
DUMP_PID=""

if ((rc != 0)); then
  if ((rc == 124)); then
    reason="导出超时（超过 ${EXEC_TIMEOUT}s）"
  elif ((rc == 137)); then
    reason="导出超时后被强制终止"
  else
    reason="sqlite3 导出失败（退出码 $rc）"
  fi
  detail="$(tail -n 3 "$ERR_FILE" 2>/dev/null | tr '\n' ' ')"
  [[ -n "$detail" ]] && reason="$reason：$detail"
  fail_backup "$reason"
fi

# ---------------------------------------------------------------------------
# 完成条件校验：非空 + 含表结构 + 以 COMMIT 正常收尾。
# 仅“文件非空”不足以证明成功，缺失任一条件即判失败并清理临时文件。
# ---------------------------------------------------------------------------
if [[ ! -s "$TMP_FILE" ]]; then
  fail_backup "导出文件为空（$TMP_FILE）"
fi
if ! grep -qE '^CREATE TABLE ' "$TMP_FILE"; then
  fail_backup "导出内容不含任何表结构；源数据库可能是空库或路径有误（$DB）"
fi
if ! tail -c 256 -- "$TMP_FILE" | grep -qE 'COMMIT;[[:space:]]*$'; then
  fail_backup "导出未正常结束（缺少收尾 COMMIT），文件可能被截断"
fi

chmod 600 -- "$TMP_FILE" 2>/dev/null || true

# 发布：同目录原子 rename；新备份已完整产出后才替换旧文件，失败时旧文件不受影响。
table_count="$(sqlite3 -batch "$DB" \
  "SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%';" 2>/dev/null || echo '?')"
size_bytes="$(stat -c %s -- "$TMP_FILE" 2>/dev/null || echo 0)"
if ! mv -f -- "$TMP_FILE" "$TARGET"; then
  fail_backup "发布备份文件失败：$TARGET"
fi
TMP_FILE=""

elapsed=$((SECONDS - START_SECONDS))
log "备份完成：$TARGET"
log "大小：$size_bytes 字节；表数：$table_count；耗时：${elapsed}s；权限：$(stat -c '%a' -- "$TARGET" 2>/dev/null || echo '600')"
log "说明：本文件含密码哈希、源码与隐藏用例，权限应保持 0600，且不得放入 web/ 或提交到 Git"
exit 0
