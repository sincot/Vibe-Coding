#!/usr/bin/env bash
# =============================================================================
# tests/scripts/verify_boot_mount.sh — 重启主机后验证 /opt/oj-tmpfs 开机自动挂载
# 与判题沙箱可用性（M6.4 边界项）。
#
# 背景：TESTING_GUIDE.md §六.4 禁止在开发服务器上擅自重启主机，故“真实重启后
#   systemd mount unit 自动挂载”无法由代理自动执行。本脚本供具备 root/维护窗口的
#   操作者在重启后手工运行，作为开机自动挂载的可复核证据。
#
# 用法（重启后）：
#   bash tests/scripts/verify_boot_mount.sh
#   # 可选：OJ_VERIFY_PORT=18080 bash tests/scripts/verify_boot_mount.sh
#
# 检查项（全部满足才返回 0）：
#   1. systemd mount unit opt-oj\x2dtmpfs.mount 处于 enabled；
#   2. 该 unit 处于 active/mounted（说明本次开机已自动挂载）；
#   3. /opt/oj-tmpfs 文件系统类型为 tmpfs 且容量符合约 512 MiB；
#   4. 以 /opt/oj-tmpfs 为判题工作目录启动隔离服务成功（通过 path_is_tmpfs 与
#      沙箱自检），健康检查 200，随后 SIGINT 优雅停止；
#   5. 结束清理临时库与进程，不影响正式数据库。
# =============================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERVER="$ROOT/build/oj_server"
UNIT='opt-oj\x2dtmpfs.mount'
TARGET=/opt/oj-tmpfs
PASS=0; FAIL=0
pass(){ PASS=$((PASS+1)); echo "  [PASS] $1"; }
fail(){ FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }

echo "== 开机自动挂载验证 =="
state="$(systemctl is-enabled "$UNIT" 2>&1)"
[ "$state" = "enabled" ] && pass "mount unit enabled（开机自动挂载已启用）" || fail "unit 状态为 $state"
active="$(systemctl is-active "$UNIT" 2>&1)"
[ "$active" = "active" ] && pass "mount unit active/mounted" || fail "unit active 状态为 $active"

# 判断当前挂载是否为「本次开机」自动建立：mount unit 的 ActiveEnterTimestamp 应接近本次
# 开机时间。若明显晚于开机时间，则本次只是被手动/安装时启动，不能证明开机自动挂载。
BOOT_CONFIRMED=0
boot_epoch="$(date -d "$(uptime -s)" +%s 2>/dev/null || true)"
active_ts="$(systemctl show "$UNIT" -p ActiveEnterTimestamp --value 2>/dev/null || true)"
active_epoch="$(date -d "$active_ts" +%s 2>/dev/null || true)"
if [ -n "$boot_epoch" ] && [ -n "$active_epoch" ] && [ "$active_epoch" -ge "$boot_epoch" ] 2>/dev/null; then
  delta=$((active_epoch - boot_epoch))
  if [ "$delta" -le 300 ]; then
    pass "mount unit 于本次开机后 ${delta}s 激活（本次开机自动挂载已确认）"
    BOOT_CONFIRMED=1
  else
    echo "  [WARN] 挂载激活于 $active_ts（距本次开机 ${delta}s）；本次为开机后手动/安装时启动，开机自动挂载未得到直接证据"
  fi
else
  echo "  [WARN] 无法比较开机时间与挂载激活时间，开机自动挂载未得到直接证据"
fi

if command -v findmnt >/dev/null 2>&1; then
  fstype="$(findmnt -n -o FSTYPE "$TARGET" 2>/dev/null)"
else
  fstype="$(stat -f -c %T "$TARGET" 2>/dev/null)"
fi
[ "$fstype" = "tmpfs" ] && pass "$TARGET 文件系统为 tmpfs" || fail "$TARGET 文件系统为 ${fstype:-未知}"
size_kb="$(df -k --output=size "$TARGET" 2>/dev/null | tail -1 | tr -d ' ')"
if [ -n "$size_kb" ] && [ "$size_kb" -ge 500000 ] 2>/dev/null; then
  pass "容量约 $((size_kb/1024)) MiB（符合 512 MiB 配置）"
else
  fail "容量异常：${size_kb:-未知} KiB"
fi

[ -x "$SERVER" ] || { echo "未找到 $SERVER，请先构建：cmake --build $ROOT/build --parallel 1" >&2; exit 2; }
TMP="$(mktemp -d "${TMPDIR:-/tmp}/oj-bootverify.XXXXXX")"
DB="$TMP/oj.db"; PORT="${OJ_VERIFY_PORT:-$(( (RANDOM % 1000) + 45000 ))}"
export OJ_JWT_SECRET="boot-verify-secret-0123456789abcdef"
export OJ_ADMIN_PASSWORD="BootVerifyPw123!"
export OJ_JUDGE_WORKSPACE="$TARGET"
SRV=""
cleanup(){ [ -n "$SRV" ] && { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }; rm -rf "$TMP"; }
trap cleanup EXIT
"$SERVER" --db "$DB" --seed >"$TMP/seed.log" 2>&1
"$SERVER" --host 127.0.0.1 --port "$PORT" --db "$DB" --web "$ROOT/web" >"$TMP/server.log" 2>&1 &
SRV=$!
healthy=0
for _ in $(seq 1 80); do
  curl -sf "http://127.0.0.1:$PORT/api/health" >/dev/null 2>&1 && { healthy=1; break; }
  kill -0 "$SRV" 2>/dev/null || break
  sleep 0.25
done
if [ "$healthy" = "1" ]; then
  pass "以 $TARGET 为判题工作目录启动服务成功且健康检查 200（沙箱自检通过）"
else
  fail "服务启动/健康检查失败"; tail -20 "$TMP/server.log" >&2
fi
grep -q "判题工作目录已就绪" "$TMP/server.log" && pass "日志确认 tmpfs 工作目录就绪" || true
if [ -n "$SRV" ] && kill -0 "$SRV" 2>/dev/null; then
  kill -INT "$SRV" 2>/dev/null
  for _ in $(seq 1 60); do kill -0 "$SRV" 2>/dev/null || break; sleep 0.2; done
  kill -0 "$SRV" 2>/dev/null && fail "优雅停止超时" || pass "SIGINT 优雅停止成功"
fi

echo
echo "开机自动挂载验证：通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -ne 0 ]; then
  echo "结论：存在未通过项，开机自动挂载未验证。" >&2; exit 1
fi
if [ "$BOOT_CONFIRMED" = "1" ]; then
  echo "结论：/opt/oj-tmpfs 开机自动挂载已验证。"; exit 0
fi
echo "结论：配置与当前挂载正常，但未能证明“本次开机自动挂载”。" >&2
echo "      请在真实重启主机后再次运行本脚本（重启前运行只能证明 unit 已启用且当前已挂载）。" >&2
exit 3
