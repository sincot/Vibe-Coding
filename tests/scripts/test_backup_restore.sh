#!/usr/bin/env bash
# =============================================================================
# tests/scripts/test_backup_restore.sh — M6.2 备份/恢复脚本集成测试
#
# 用法：bash tests/scripts/test_backup_restore.sh <源码根> <oj_server 可执行路径>
#   （CI：由 CTest m62_backup_restore 调用；两参数均可省略，脚本会尝试推断）
#
# 隔离约定：
#   - 所有数据（源库、恢复库、备份目录、临时目录）都在 mktemp 临时目录内，用后删除。
#   - 不接触仓库 data/oj.db，不使用真实密钥/密码/端口。
#   - 全程串行，单进程，无长期后台任务；结束时清理 FIFO 与写入连接。
#
# 覆盖：备份正常路径、SQL/输出安全、一致性结构；恢复到新库并核对完整性/外键/
#       关键业务数据/样例与隐藏用例/源码/索引与 CHECK/序列/在途任务；WAL 已提交
#       数据包含且不动 -wal/-shm；错误路径（源库缺失/目录/非库/空库/非法参数/
#       非法环境/不可写目录/磁盘不足/并发锁）；同日替换与 --no-replace；含空格路径
#       与脱离 cwd；时区；清理边界；忽略规则；文档与勾选。
# =============================================================================
set -uo pipefail

SOURCE_DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
SERVER="${2:-$SOURCE_DIR/build/oj_server}"
SUT="$SOURCE_DIR/scripts/backup.sh"

PASSED=0
FAILED=0
declare -a FAILURES=()
pass() { PASSED=$((PASSED + 1)); printf '  [PASS] %s\n' "$1"; }
fail() { FAILED=$((FAILED + 1)); FAILURES+=("$1"); printf '  [FAIL] %s\n' "$1"; }
check_eq() { # <desc> <expected> <actual>
  if [[ "$2" == "$3" ]]; then pass "$1"; else fail "$1（期望='$2' 实际='$3'）"; fi
}
check_true() { # <desc> <condition-cmd...>
  local desc="$1"; shift
  if "$@"; then pass "$desc"; else fail "$desc"; fi
}
check_contains() { # <desc> <file> <fixed-string>
  if grep -qF -- "$3" "$2"; then pass "$1"; else fail "$1（未找到：$3）"; fi
}
check_not_contains() { # <desc> <file> <fixed-string>
  if grep -qF -- "$3" "$2"; then fail "$1（不应包含：$3）"; else pass "$1"; fi
}
check_match() { # <desc> <extended-regex> <actual>
  if [[ "$3" =~ $2 ]]; then pass "$1"; else fail "$1（应匹配 /$2/ 实际='$3'）"; fi
}

for t in bash sqlite3 flock timeout mktemp stat awk mkfifo git sha256sum curl python3; do
  command -v "$t" >/dev/null 2>&1 || { echo "前置条件不满足：缺少 $t" >&2; exit 2; }
done
if [[ ! -f "$SUT" ]]; then echo "缺少被测脚本：$SUT" >&2; exit 2; fi
if [[ ! -x "$SERVER" ]]; then echo "缺少可执行服务：$SERVER" >&2; exit 2; fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/oj-m62.XXXXXX")"
export TMPDIR="$TMP/tmp"      # 让脚本的临时错误文件也落在受控目录，便于检查残留
mkdir -p "$TMPDIR"
WAL_PID=""
SERVER_PID=""
RWS_DIR=""
cleanup() {
  trap - EXIT INT TERM
  [[ -n "$WAL_PID" ]] && kill "$WAL_PID" 2>/dev/null || true
  if [[ -n "$SERVER_PID" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  [[ -n "$RWS_DIR" ]] && rm -rf -- "$RWS_DIR" 2>/dev/null || true
  rm -rf -- "$TMP" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

OUT="$TMP/out.txt"
ERR="$TMP/err.txt"
last_rc=0
run_sut() { # env ... -- then args are passed as one env invocation
  env "$@" >"$OUT" 2>"$ERR"; last_rc=$?
}
count_of() { sqlite3 --readonly "$1" "SELECT count(*) FROM $2;" 2>/dev/null; }
free_port() {
  python3 -c 'import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

printf '== M6.2 备份与恢复测试 ==\n'
printf '源码根: %s\n被测: %s\n\n' "$SOURCE_DIR" "$SUT"

# ---------------------------------------------------------------------------
# 准备：真实 schema + 种子题（隔离库），并注入业务数据
# ---------------------------------------------------------------------------
SRC_DIR="$TMP/work"
mkdir -p "$SRC_DIR"
SRC_DB="$SRC_DIR/oj.db"
if ! "$SERVER" --db "$SRC_DB" --seed >"$OUT" 2>"$ERR"; then
  echo "准备失败：oj_server --seed 未成功（详见下方）" >&2
  cat "$ERR" >&2
  exit 2
fi
check_eq "T-000 种子库创建（WAL）" "wal" \
  "$(sqlite3 "$SRC_DB" 'PRAGMA journal_mode;')"

sqlite3 "$SRC_DB" <<'SQL' 2>"$ERR"
PRAGMA foreign_keys=ON;
INSERT INTO users(id,account,nickname,password_hash,role,reset_pwd_flag,created_at)
  VALUES(4242,'0000000042','m62_user','NOT_A_REAL_HASH','user',0,'2026-01-02 03:04:05');
INSERT INTO submissions(user_id,problem_id,language,source_code,status,per_case,compile_msg,runtime_ms,memory_kb,created_at)
  SELECT id,(SELECT MIN(id) FROM problems),'cpp17','int main(){/*M62_MARKER_SRC*/}','AC','M62_MARKER_PER_CASE','',7,123,'2026-01-02 03:04:06'
  FROM users WHERE nickname='m62_user';
INSERT INTO user_problem_status(user_id,problem_id,status,first_ac_at,submit_count)
  SELECT id,(SELECT MIN(id) FROM problems),'accepted','2026-01-02 03:04:06',1
  FROM users WHERE nickname='m62_user';
INSERT INTO in_flight_tasks(task_id,user_id,problem_id,language,source_code,submitted_at,state,owner,claimed_at,reason)
  SELECT 'm62-task-0001',id,(SELECT MIN(id) FROM problems),'c11','int main(){/*M62_INFLIGHT_SRC*/}','2026-01-02 03:04:07','pending','',NULL,''
  FROM users WHERE nickname='m62_user';
CREATE TABLE wal_probe(id INTEGER PRIMARY KEY AUTOINCREMENT, note TEXT NOT NULL);
SQL
[[ $? -eq 0 ]] && pass "T-000 注入业务数据与 WAL 探测表" || fail "T-000 注入业务数据失败"

SRC_USERS="$(count_of "$SRC_DB" users)"
SRC_PROBLEMS="$(count_of "$SRC_DB" problems)"
SRC_CASES="$(count_of "$SRC_DB" testcases)"
SRC_CASE_HIDDEN="$(sqlite3 --readonly "$SRC_DB" "SELECT count(*) FROM testcases WHERE is_sample=0;")"
SRC_CASE_SAMPLE="$(sqlite3 --readonly "$SRC_DB" "SELECT count(*) FROM testcases WHERE is_sample=1;")"
SRC_SUBS="$(count_of "$SRC_DB" submissions)"
SRC_STATUS="$(count_of "$SRC_DB" user_problem_status)"
SRC_FLIGHT="$(count_of "$SRC_DB" in_flight_tasks)"
SRC_SEQ="$(sqlite3 --readonly "$SRC_DB" "SELECT seq FROM sqlite_sequence WHERE name='users';")"
check_true "T-000 源库含隐藏用例与公开样例" test "$SRC_CASE_HIDDEN" -gt 0 -a "$SRC_CASE_SAMPLE" -gt 0

# ---------------------------------------------------------------------------
# 正常备份
# ---------------------------------------------------------------------------
BK="$TMP/backup"
mkdir -p "$BK"
DECOY="$BK/keep-me.txt"; echo "decoy" > "$DECOY"
EXPECT_DATE="$(TZ=UTC date +%Y%m%d)"
TARGET="$BK/oj-$EXPECT_DATE.sql"
run_sut OJ_BACKUP_TZ=UTC bash "$SUT" --db "$SRC_DB" --out-dir "$BK"
check_eq "T-001 正常备份退出码 0" "0" "$last_rc"
check_true "T-001 生成备份文件 oj-YYYYMMDD.sql" test -f "$TARGET"
check_eq "T-001 备份文件权限 0600" "600" "$(stat -c %a "$TARGET" 2>/dev/null)"
check_true "T-001 备份文件非空" test -s "$TARGET"
check_true "T-001 备份以 COMMIT; 收尾" bash -c "tail -c 256 -- '$TARGET' | grep -qE 'COMMIT;[[:space:]]*\$'"
check_contains "T-002 导出为 SQL 文本（CREATE TABLE）" "$TARGET" "CREATE TABLE "
check_true "T-002 导出非二进制数据库文件" bash -c "! head -c 16 '$TARGET' | grep -q 'SQLite format'"
check_not_contains "T-002 stdout 不泄露 SQL（CREATE TABLE）" "$OUT" "CREATE TABLE"
check_not_contains "T-002 stdout 不泄露 SQL（INSERT INTO）" "$OUT" "INSERT INTO"
check_not_contains "T-002 stdout 不泄露 PRAGMA" "$OUT" "PRAGMA foreign_keys"
check_contains "T-002 stdout 报告完成" "$OUT" "[backup] 备份完成"
check_contains "T-003 一致读事务结构 BEGIN" "$TARGET" "BEGIN TRANSACTION;"
check_true "T-029 备份目录中无关文件未被删除" test -f "$DECOY"
check_true "T-030 成功后无 .tmp 残留" bash -c "! find '$BK' -maxdepth 1 -name '*.tmp' | grep -q ."
check_true "T-030 成功后无错误临时文件残留" bash -c "! find '$TMPDIR' -name 'oj-backup-err.*' | grep -q ."

# ---------------------------------------------------------------------------
# 恢复：导入全新隔离库并核对
# ---------------------------------------------------------------------------
RESTORE_DB="$TMP/restore/restore.db"
mkdir -p "$(dirname "$RESTORE_DB")"
set -o pipefail
sqlite3 -bail "$RESTORE_DB" <"$TARGET" >"$OUT" 2>"$ERR"; rc=$?
check_eq "T-004 备份导入新库退出码 0" "0" "$rc"
check_eq "T-005 恢复库 integrity_check=ok" "ok" "$(sqlite3 --readonly "$RESTORE_DB" 'PRAGMA integrity_check;')"
FK_OUT="$(sqlite3 --readonly "$RESTORE_DB" 'PRAGMA foreign_key_check;')"
check_eq "T-006 恢复库 foreign_key_check 无输出" "" "$FK_OUT"
check_eq "T-007 users 计数一致" "$SRC_USERS" "$(count_of "$RESTORE_DB" users)"
check_eq "T-007 problems 计数一致" "$SRC_PROBLEMS" "$(count_of "$RESTORE_DB" problems)"
check_eq "T-007 testcases 计数一致" "$SRC_CASES" "$(count_of "$RESTORE_DB" testcases)"
check_eq "T-007 submissions 计数一致" "$SRC_SUBS" "$(count_of "$RESTORE_DB" submissions)"
check_eq "T-007 user_problem_status 计数一致" "$SRC_STATUS" "$(count_of "$RESTORE_DB" user_problem_status)"
check_eq "T-007 in_flight_tasks 计数一致" "$SRC_FLIGHT" "$(count_of "$RESTORE_DB" in_flight_tasks)"
check_eq "T-008 隐藏用例计数一致" "$SRC_CASE_HIDDEN" "$(sqlite3 --readonly "$RESTORE_DB" 'SELECT count(*) FROM testcases WHERE is_sample=0;')"
check_eq "T-008 公开样例计数一致" "$SRC_CASE_SAMPLE" "$(sqlite3 --readonly "$RESTORE_DB" 'SELECT count(*) FROM testcases WHERE is_sample=1;')"
check_eq "T-009 用户行原样恢复" "m62_user|user|0" "$(sqlite3 --readonly "$RESTORE_DB" "SELECT nickname||'|'||role||'|'||reset_pwd_flag FROM users WHERE id=4242;")"
check_eq "T-009 提交源码/状态/时间原样恢复" "AC|2026-01-02 03:04:06" \
  "$(sqlite3 --readonly "$RESTORE_DB" "SELECT status||'|'||created_at FROM submissions WHERE instr(source_code,'M62_MARKER_SRC')>0;")"
check_eq "T-009 逐点结果 JSON 原样恢复" "M62_MARKER_PER_CASE" \
  "$(sqlite3 --readonly "$RESTORE_DB" "SELECT per_case FROM submissions WHERE instr(source_code,'M62_MARKER_SRC')>0;")"
check_eq "T-010 索引保留（idx_submissions_user_created）" "1" \
  "$(sqlite3 --readonly "$RESTORE_DB" "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='idx_submissions_user_created';")"
check_eq "T-010 索引保留（idx_problems_seed_key）" "1" \
  "$(sqlite3 --readonly "$RESTORE_DB" "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='idx_problems_seed_key';")"
CHECK_ERR="$TMP/check-err.txt"
sqlite3 "$RESTORE_DB" "INSERT INTO users(account,nickname,password_hash,role,reset_pwd_flag) VALUES('x1','x1','x','superuser',0);" >"$OUT" 2>"$CHECK_ERR"
check_rc=$?
check_true "T-010 CHECK 约束保留（可写连接非法 role 被拒）" test "$check_rc" -ne 0
check_contains "T-010 拒绝原因为 CHECK 约束" "$CHECK_ERR" "CHECK"
check_eq "T-011 AUTOINCREMENT 序列一致" "$SRC_SEQ" \
  "$(sqlite3 --readonly "$RESTORE_DB" "SELECT seq FROM sqlite_sequence WHERE name='users';")"
check_eq "T-012 在途任务原样恢复" "m62-task-0001|pending|1" \
  "$(sqlite3 --readonly "$RESTORE_DB" "SELECT task_id||'|'||state||'|'||(instr(source_code,'M62_INFLIGHT_SRC')>0) FROM in_flight_tasks;")"

# ---------------------------------------------------------------------------
# WAL：只存在于 WAL 的已提交数据必须被包含，且不动 -wal/-shm
# ---------------------------------------------------------------------------
WALBK="$TMP/walbk"; mkdir -p "$WALBK"
WAL_FIFO="$TMP/wal.fifo"; mkfifo "$WAL_FIFO"
sqlite3 "$SRC_DB" <"$WAL_FIFO" >/dev/null 2>&1 &
WAL_PID=$!
exec 8>"$WAL_FIFO"
printf "INSERT INTO wal_probe(note) VALUES('wal-committed-probe');\n" >&8
ok=0
for _ in $(seq 1 50); do
  if [[ "$(sqlite3 --readonly "$SRC_DB" "SELECT count(*) FROM wal_probe WHERE note='wal-committed-probe';" 2>/dev/null)" == "1" ]]; then ok=1; break; fi
  sleep 0.1
done
check_eq "T-013 探测行已提交（WAL 可见）" "1" "$ok"
WAL_SIZE="$(stat -c %s "$SRC_DB-wal" 2>/dev/null || echo 0)"
check_true "T-013 备份前 -wal 非空" test "${WAL_SIZE:-0}" -gt 0
WAL_INODE_BEFORE="$(stat -c %i "$SRC_DB-wal" 2>/dev/null || echo '')"
MAIN_INODE_BEFORE="$(stat -c %i "$SRC_DB" 2>/dev/null || echo '')"
run_sut bash "$SUT" --db "$SRC_DB" --out-dir "$WALBK"
check_eq "T-013 WAL 场景备份退出码 0" "0" "$last_rc"
WAL_TARGET="$(ls "$WALBK"/oj-*.sql 2>/dev/null | head -n1)"
check_contains "T-013 dump 包含仅存于 WAL 的行" "$WAL_TARGET" "wal-committed-probe"
check_true "T-014 备份后 -wal 仍存在" test -f "$SRC_DB-wal"
check_true "T-014 备份后 -shm 仍存在" test -f "$SRC_DB-shm"
check_eq "T-014 -wal inode 未变（未删除/重建）" "$WAL_INODE_BEFORE" "$(stat -c %i "$SRC_DB-wal" 2>/dev/null || echo '')"
check_eq "T-014 主库文件 inode 未变（未复制替换）" "$MAIN_INODE_BEFORE" "$(stat -c %i "$SRC_DB" 2>/dev/null || echo '')"
WAL_RESTORE="$TMP/restore/wal-restore.db"
sqlite3 -bail "$WAL_RESTORE" <"$WAL_TARGET" >/dev/null 2>&1
check_eq "T-013 恢复库含 WAL 中提交的行" "1" \
  "$(sqlite3 --readonly "$WAL_RESTORE" "SELECT count(*) FROM wal_probe WHERE note='wal-committed-probe';")"
exec 8>&-
wait "$WAL_PID" 2>/dev/null || true
WAL_PID=""
rm -f "$WAL_FIFO"

# ---------------------------------------------------------------------------
# 错误路径
# ---------------------------------------------------------------------------
# T-015 源库不存在
MISSING_DB="$TMP/does-not-exist.db"
run_sut bash "$SUT" --db "$MISSING_DB" --out-dir "$TMP/e15"
check_eq "T-015 源库不存在退出码 2" "2" "$last_rc"
check_true "T-015 未在错误路径创建空库" test ! -e "$MISSING_DB"
check_true "T-015 未产生正式备份文件" bash -c "! ls '$TMP/e15'/oj-*.sql 2>/dev/null | grep -q ."

# T-016 源库为目录
run_sut bash "$SUT" --db "$SRC_DIR" --out-dir "$TMP/e16"
check_eq "T-016 源库为目录退出码 2" "2" "$last_rc"

# T-017 非数据库文件
NOTDB="$TMP/not-a-db.db"; printf 'this is not sqlite\n' > "$NOTDB"
run_sut bash "$SUT" --db "$NOTDB" --out-dir "$TMP/e17"
check_eq "T-017 非数据库文件退出码 1" "1" "$last_rc"
check_true "T-017 未留下正式文件" bash -c "! ls '$TMP/e17'/oj-*.sql 2>/dev/null | grep -q ."
check_true "T-017 临时文件已清理" bash -c "! find '$TMP/e17' -maxdepth 1 -name '*.tmp' | grep -q ."

# T-018 空库（无表）
EMPTY_DB="$TMP/empty.db"; : >"$EMPTY_DB"
run_sut bash "$SUT" --db "$EMPTY_DB" --out-dir "$TMP/e18"
check_eq "T-018 空库退出码 1" "1" "$last_rc"
check_contains "T-018 空库提示明确" "$ERR" "不含任何表结构"

# T-019 参数与帮助
run_sut bash "$SUT" --bogus
check_eq "T-019 未知参数退出码 2" "2" "$last_rc"
run_sut bash "$SUT" --help
check_eq "T-019 --help 退出码 0" "0" "$last_rc"
check_contains "T-019 --help 输出用法" "$OUT" "用法："

# T-020 非法数值环境
run_sut OJ_BACKUP_TIMEOUT=abc bash "$SUT" --db "$SRC_DB" --out-dir "$TMP/e20"
check_eq "T-020 非法数值环境退出码 2" "2" "$last_rc"

# T-021 输出目录不可写
RO_DIR="$TMP/ro"; mkdir -p "$RO_DIR"; chmod 500 "$RO_DIR"
run_sut bash "$SUT" --db "$SRC_DB" --out-dir "$RO_DIR"
check_eq "T-021 输出目录不可写退出码 2" "2" "$last_rc"
chmod 700 "$RO_DIR"

# T-022 磁盘空间不足（df 桩）
STUB="$TMP/stub-bin"; mkdir -p "$STUB"
printf '#!/bin/sh\necho "Filesystem 1024-blocks Used Available Capacity Mounted on"\necho "/dev/fake 1000 999 1 99%% /fake"\n' >"$STUB/df"
chmod +x "$STUB/df"
run_sut PATH="$STUB:$PATH" bash "$SUT" --db "$SRC_DB" --out-dir "$TMP/e22"
check_eq "T-022 磁盘不足退出码 1" "1" "$last_rc"
check_contains "T-022 磁盘不足提示明确" "$ERR" "空间不足"
check_true "T-022 磁盘不足不产生正式文件" bash -c "! ls '$TMP/e22'/oj-*.sql 2>/dev/null | grep -q ."

# T-023 并发锁
LOCKDIR="$TMP/lockdir"; mkdir -p "$LOCKDIR"
exec 7>"$LOCKDIR/.oj-backup.lock"
flock -x 7
run_sut OJ_BACKUP_LOCK_WAIT=1 bash "$SUT" --db "$SRC_DB" --out-dir "$LOCKDIR"
check_eq "T-023 锁被占用时退出码 1" "1" "$last_rc"
check_contains "T-023 锁超时提示明确" "$ERR" "等待备份锁超时"
flock -u 7; exec 7>&-

# T-024 失败保留已有成功备份
PRESERVE="$TMP/preserve"; mkdir -p "$PRESERVE"
run_sut bash "$SUT" --db "$SRC_DB" --out-dir "$PRESERVE"
PRESERVE_TARGET="$(ls "$PRESERVE"/oj-*.sql 2>/dev/null | head -n1)"
H1="$(sha256sum "$PRESERVE_TARGET" | awk '{print $1}')"
run_sut bash "$SUT" --db "$NOTDB" --out-dir "$PRESERVE"
check_eq "T-024 损坏源退出码 1" "1" "$last_rc"
check_eq "T-024 已有成功备份内容不变" "$H1" "$(sha256sum "$PRESERVE_TARGET" | awk '{print $1}')"
check_true "T-024 失败后无 .tmp 残留" bash -c "! find '$PRESERVE' -maxdepth 1 -name '*.tmp' | grep -q ."

# ---------------------------------------------------------------------------
# 同日重跑语义
# ---------------------------------------------------------------------------
# T-025 默认替换
sqlite3 "$SRC_DB" "INSERT INTO wal_probe(note) VALUES('second-generation-marker');" >/dev/null 2>&1
run_sut bash "$SUT" --db "$SRC_DB" --out-dir "$PRESERVE"
check_eq "T-025 同日默认重跑退出码 0" "0" "$last_rc"
check_contains "T-025 同日默认重跑内容更新" "$PRESERVE_TARGET" "second-generation-marker"

# T-026 --no-replace 保留
H2="$(sha256sum "$PRESERVE_TARGET" | awk '{print $1}')"
run_sut bash "$SUT" --db "$SRC_DB" --out-dir "$PRESERVE" --no-replace
check_eq "T-026 --no-replace 退出码 0" "0" "$last_rc"
check_eq "T-026 --no-replace 内容不变" "$H2" "$(sha256sum "$PRESERVE_TARGET" | awk '{print $1}')"
check_contains "T-026 --no-replace 提示跳过" "$OUT" "跳过"

# T-035 中断：导出进行中收到 SIGTERM 时清理临时文件并释放锁
BIG_DB="$TMP/big.db"
sqlite3 "$BIG_DB" "PRAGMA journal_mode=WAL; CREATE TABLE big(id INTEGER PRIMARY KEY, pad TEXT); INSERT INTO big(pad) SELECT hex(randomblob(80)) FROM (WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<300000) SELECT x FROM c);" >/dev/null 2>&1
check_true "T-035 大库准备（用于制造足够长的导出窗口）" test -s "$BIG_DB"
INTR="$TMP/intr"; mkdir -p "$INTR"
bash "$SUT" --db "$BIG_DB" --out-dir "$INTR" >"$OUT" 2>"$ERR" &
BK_PID=$!
seen_tmp=0
for _ in $(seq 1 500); do
  if find "$INTR" -maxdepth 1 -name '.oj-*.sql.tmp' 2>/dev/null | grep -q .; then seen_tmp=1; break; fi
  kill -0 "$BK_PID" 2>/dev/null || break
  sleep 0.02
done
kill -TERM "$BK_PID" 2>/dev/null || true
wait "$BK_PID" 2>/dev/null; intr_rc=$?
check_eq "T-035 中断时已进入导出阶段（观察到临时文件）" "1" "$seen_tmp"
check_eq "T-035 以 SIGTERM 约定的非零码退出" "143" "$intr_rc"
check_true "T-035 中断后无 .tmp 残留" bash -c "! find '$INTR' -maxdepth 1 -name '*.tmp' | grep -q ."
run_sut bash "$SUT" --db "$SRC_DB" --out-dir "$INTR"
check_eq "T-035 中断已释放锁（后续备份成功）" "0" "$last_rc"
gone=0
for _ in $(seq 1 50); do
  if ! pgrep -f "$BIG_DB" >/dev/null 2>&1; then gone=1; break; fi
  sleep 0.1
done
check_true "T-035 中断后导出进程已回收" test "$gone" -eq 1

# ---------------------------------------------------------------------------
# T-036 备份进行中并发高频写入：不得被长时间阻塞（WAL 读事务不阻塞写入）
# ---------------------------------------------------------------------------
CONC_DIR="$TMP/conc"; mkdir -p "$CONC_DIR"
bash "$SUT" --db "$BIG_DB" --out-dir "$CONC_DIR" >"$OUT" 2>"$ERR" &
CONC_BK=$!
for _ in $(seq 1 500); do
  find "$CONC_DIR" -maxdepth 1 -name '.oj-*.sql.tmp' 2>/dev/null | grep -q . && break
  kill -0 "$CONC_BK" 2>/dev/null || break
  sleep 0.02
done
CONC_WRITES=0; CONC_FAILS=0; CONC_MAX_MS=0; CONC_DURING=0
while kill -0 "$CONC_BK" 2>/dev/null; do
  t0="$(date +%s%N)"
  if sqlite3 -cmd '.timeout 2000' "$BIG_DB" "INSERT INTO big(pad) VALUES('conc-write');" >/dev/null 2>&1; then
    CONC_WRITES=$((CONC_WRITES + 1)); CONC_DURING=1
  else
    CONC_FAILS=$((CONC_FAILS + 1))
  fi
  t1="$(date +%s%N)"
  dt=$(( (t1 - t0) / 1000000 )); (( dt > CONC_MAX_MS )) && CONC_MAX_MS=$dt
  (( CONC_WRITES >= 50 )) && break
done
wait "$CONC_BK"; conc_rc=$?
check_eq "T-036 备份与并发写入并存（备份退出码 0）" "0" "$conc_rc"
check_true "T-036 备份存活期间至少完成一次写入（实际写入 ${CONC_WRITES} 次）" test "$CONC_DURING" -eq 1
check_eq "T-036 并发写入无失败" "0" "$CONC_FAILS"
check_true "T-036 写入未长时间阻塞（峰值 ${CONC_MAX_MS}ms < 2000ms）" test "$CONC_MAX_MS" -lt 2000

# ---------------------------------------------------------------------------
# T-039 进程崩溃持久性：提交后 SIGKILL 写连接，重开仍可读（WAL 恢复；非断电）
# ---------------------------------------------------------------------------
DUR_DB="$TMP/dur.db"
sqlite3 "$DUR_DB" "PRAGMA journal_mode=WAL; CREATE TABLE d(x TEXT);" >/dev/null 2>&1
DUR_FIFO="$TMP/dur.fifo"; mkfifo "$DUR_FIFO"
sqlite3 "$DUR_DB" <"$DUR_FIFO" >/dev/null 2>&1 &
DUR_PID=$!
exec 6>"$DUR_FIFO"
printf "INSERT INTO d(x) VALUES('crash-durable');\n" >&6
dok=0
for _ in $(seq 1 50); do
  [[ "$(sqlite3 --readonly "$DUR_DB" "SELECT count(*) FROM d WHERE x='crash-durable';" 2>/dev/null)" == "1" ]] && { dok=1; break; }
  sleep 0.1
done
check_eq "T-039 提交已可见（准备）" "1" "$dok"
kill -9 "$DUR_PID" 2>/dev/null || true
exec 6>&-
wait "$DUR_PID" 2>/dev/null || true
check_eq "T-039 SIGKILL 后源库完整性 ok" "ok" "$(sqlite3 --readonly "$DUR_DB" 'PRAGMA integrity_check;')"
check_eq "T-039 SIGKILL 后已提交数据仍可读" "1" \
  "$(sqlite3 --readonly "$DUR_DB" "SELECT count(*) FROM d WHERE x='crash-durable';")"

# ---------------------------------------------------------------------------
# T-037 SIGKILL 备份进程：不产生正式文件、源库完整、锁自动释放
# 说明：SIGKILL 不可捕获，父进程无法清理其导出的子进程/临时文件；本测试断言脚本
# 可保证的部分（无正式文件、源库完好、锁已释放），并按唯一临时库路径清理残留子进程。
# ---------------------------------------------------------------------------
SKK="$TMP/sigkill"; mkdir -p "$SKK"
bash "$SUT" --db "$BIG_DB" --out-dir "$SKK" >"$OUT" 2>"$ERR" &
SK_PID=$!
for _ in $(seq 1 500); do
  find "$SKK" -maxdepth 1 -name '.oj-*.sql.tmp' 2>/dev/null | grep -q . && break
  kill -0 "$SK_PID" 2>/dev/null || break
  sleep 0.02
done
kill -9 "$SK_PID" 2>/dev/null || true
wait "$SK_PID" 2>/dev/null; sk_rc=$?
check_eq "T-037 SIGKILL 备份进程退出码 137" "137" "$sk_rc"
check_true "T-037 SIGKILL 未产生正式备份文件" bash -c "! ls '$SKK'/oj-*.sql 2>/dev/null | grep -q ."
check_eq "T-037 SIGKILL 后源库完整性 ok" "ok" "$(sqlite3 --readonly "$BIG_DB" 'PRAGMA integrity_check;')"
run_sut bash "$SUT" --db "$SRC_DB" --out-dir "$SKK"
check_eq "T-037 SIGKILL 后锁自动释放（后续备份成功）" "0" "$last_rc"
# 清理 SIGKILL 遗留、仍以本测试唯一临时库路径为参数的导出子进程（非按进程名批量清理）
for _ in $(seq 1 50); do
  pids="$(pgrep -f "$BIG_DB" 2>/dev/null | grep -v "^$$\$" || true)"
  [[ -z "$pids" ]] && break
  kill $pids 2>/dev/null || true
  sleep 0.1
done

# ---------------------------------------------------------------------------
# T-038 从备份恢复的库启动服务：M3.7 启动恢复重新入队并结算 pending 在途任务
# （隔离库 + 随机端口 + 测试密钥 + tmpfs 判题目录；仅本轮、不接触正式数据）
# ---------------------------------------------------------------------------
RECOV_DB="$TMP/restore/recovery.db"
sqlite3 -bail "$RECOV_DB" <"$TARGET" >/dev/null 2>&1; recov_import_rc=$?
check_eq "T-038 恢复库准备（备份导入退出 0）" "0" "$recov_import_rc"
before_flight="$(sqlite3 --readonly "$RECOV_DB" "SELECT count(*) FROM in_flight_tasks WHERE state IN ('pending','claimed');")"
before_subs="$(count_of "$RECOV_DB" submissions)"
check_eq "T-038 恢复库含 1 个未结算在途任务" "1" "$before_flight"
RECOV_ALLOW_NON_TMPFS=0
if stat -f -c %T /dev/shm 2>/dev/null | grep -q tmpfs; then
  RWS_DIR="/dev/shm/oj-m62-$$"
else
  RWS_DIR="$TMP/ws"; RECOV_ALLOW_NON_TMPFS=1
fi
mkdir -p "$RWS_DIR"
RECOV_PORT="$(free_port)"
if (( RECOV_ALLOW_NON_TMPFS )); then
  OJ_JWT_SECRET="m62-test-secret-0123456789abcdef" OJ_ADMIN_PASSWORD="M62_Test_Pw_123" \
  OJ_JUDGE_WORKSPACE="$RWS_DIR" OJ_JUDGE_ALLOW_NON_TMPFS=1 \
  "$SERVER" --host 127.0.0.1 --port "$RECOV_PORT" --db "$RECOV_DB" --web "$SOURCE_DIR/web" \
    >"$TMP/recov-server.log" 2>&1 &
else
  OJ_JWT_SECRET="m62-test-secret-0123456789abcdef" OJ_ADMIN_PASSWORD="M62_Test_Pw_123" \
  OJ_JUDGE_WORKSPACE="$RWS_DIR" \
  "$SERVER" --host 127.0.0.1 --port "$RECOV_PORT" --db "$RECOV_DB" --web "$SOURCE_DIR/web" \
    >"$TMP/recov-server.log" 2>&1 &
fi
SERVER_PID=$!
recov_up=0
for _ in $(seq 1 100); do
  kill -0 "$SERVER_PID" 2>/dev/null || break
  code="$(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$RECOV_PORT/api/health" 2>/dev/null || true)"
  [[ "$code" == "200" ]] && { recov_up=1; break; }
  sleep 0.3
done
check_eq "T-038 恢复库服务启动并就绪" "1" "$recov_up"
if [[ "$recov_up" == "1" ]]; then
  settled=0
  for _ in $(seq 1 200); do
    fl="$(sqlite3 --readonly "$RECOV_DB" "SELECT count(*) FROM in_flight_tasks WHERE state IN ('pending','claimed');" 2>/dev/null || echo 9)"
    sb="$(count_of "$RECOV_DB" submissions)"
    if [[ "$fl" == "0" && "$sb" == "$((before_subs + 1))" ]]; then settled=1; break; fi
    sleep 0.3
  done
  check_eq "T-038 启动恢复重新入队并结算（在途清空且提交 +1）" "1" "$settled"
  recov_status="$(sqlite3 --readonly "$RECOV_DB" "SELECT status FROM submissions WHERE instr(source_code,'M62_INFLIGHT_SRC')>0 ORDER BY id DESC LIMIT 1;" 2>/dev/null)"
  check_true "T-038 结算结果为确定终态（实际=${recov_status:-无}）" bash -c "echo '$recov_status' | grep -Eq '^(AC|WA|CE|TLE|RE|MLE|SYSERR)$'"
fi
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
rm -rf -- "$RWS_DIR"; RWS_DIR=""

# ---------------------------------------------------------------------------
# T-040 cron 最小环境调用（模拟 cron 执行环境：env -i、绝对路径、非仓库 cwd）
# ---------------------------------------------------------------------------
CRON_MIN="$TMP/cron-min"; mkdir -p "$CRON_MIN"
CRON_MIN_LOG="$TMP/cron-min.log"
( cd / && env -i SHELL=/bin/bash HOME="${HOME:-/tmp}" \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    OJ_BACKUP_TZ=UTC \
    bash -c "bash '$SUT' --db '$SRC_DB' --out-dir '$CRON_MIN' >>'$CRON_MIN_LOG' 2>&1" )
cron_min_rc=$?
check_eq "T-040 最小 cron 环境调用退出码 0" "0" "$cron_min_rc"
check_true "T-040 最小 cron 环境生成备份" bash -c "ls '$CRON_MIN'/oj-*.sql 2>/dev/null | grep -q ."
check_contains "T-040 最小 cron 环境日志含完成" "$CRON_MIN_LOG" "[backup] 备份完成"

# ---------------------------------------------------------------------------
# 含空格路径 + 脱离 cwd；时区
# ---------------------------------------------------------------------------
SPACE_DB="$TMP/dir with space/oj.db"; SPACE_BK="$TMP/backup with space"
mkdir -p "$(dirname "$SPACE_DB")"
"$SERVER" --db "$SPACE_DB" --seed >/dev/null 2>&1
( cd / && run_sut bash "$SUT" --db "$SPACE_DB" --out-dir "$SPACE_BK" )
check_eq "T-027 含空格路径且从 / 调用退出码 0" "0" "$last_rc"
check_true "T-027 空格目录生成备份" bash -c "ls '$SPACE_BK'/oj-*.sql 2>/dev/null | grep -q ."

TZ_DIR="$TMP/tz"; mkdir -p "$TZ_DIR"
EXP_TZ_DATE="$(TZ='Etc/GMT-14' date +%Y%m%d)"
run_sut OJ_BACKUP_TZ='Etc/GMT-14' bash "$SUT" --db "$SRC_DB" --out-dir "$TZ_DIR"
check_eq "T-028 指定时区备份退出码 0" "0" "$last_rc"
check_true "T-028 文件名使用指定时区日期" test -f "$TZ_DIR/oj-$EXP_TZ_DATE.sql"
check_match "T-028 日志打印指定时区偏移" '\+1400' "$(cat "$OUT")"

# ---------------------------------------------------------------------------
# 忽略规则与文档
# ---------------------------------------------------------------------------
check_true "T-031 backup/.gitkeep 存在" test -f "$SOURCE_DIR/backup/.gitkeep"
if git -C "$SOURCE_DIR" check-ignore -q "backup/oj-20200101.sql"; then pass "T-031 备份文件被忽略"; else fail "T-031 备份文件被忽略"; fi
if git -C "$SOURCE_DIR" check-ignore -q "backup/.oj-backup.lock"; then pass "T-031 锁文件被忽略"; else fail "T-031 锁文件被忽略"; fi
if git -C "$SOURCE_DIR" check-ignore -q "backup/.oj-x.sql.tmp"; then pass "T-031 临时文件被忽略"; else fail "T-031 临时文件被忽略"; fi
if git -C "$SOURCE_DIR" check-ignore -q "backup/.gitkeep"; then fail "T-031 .gitkeep 不应被忽略"; else pass "T-031 .gitkeep 不被忽略"; fi

README="$SOURCE_DIR/README.md"; SPEC="$SOURCE_DIR/SPEC.md"
check_contains "T-032 README 含 cron 示例" "$README" "30 3 * * *"
check_contains "T-032 README 含恢复步骤 integrity_check" "$README" "PRAGMA integrity_check"
check_contains "T-032 README 含恢复步骤 foreign_key_check" "$README" "foreign_key_check"
check_contains "T-032 README 含隔离恢复只读检查" "$README" "sqlite3 --readonly"
check_contains "T-032 README 含 M3.7 恢复行为说明" "$README" "重新入队判题"
check_contains "T-032 README 含 --no-replace 与权限说明" "$README" "--no-replace"
check_contains "T-032 SPEC 已勾选 backup.sh" "$SPEC" "- [x] 编写 \`scripts/backup.sh\`"
check_contains "T-032 SPEC 已勾选 cron 说明" "$SPEC" "- [x] 编写 cron 配置说明。"

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------
printf '\n== 汇总 ==\n'
printf '通过：%d  失败：%d\n' "$PASSED" "$FAILED"
if ((FAILED > 0)); then
  printf '失败项：\n'
  for f in "${FAILURES[@]}"; do printf '  - %s\n' "$f"; done
  exit 1
fi
printf 'M6.2 备份与恢复测试全部通过\n'
exit 0
