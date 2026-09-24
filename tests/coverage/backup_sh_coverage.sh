#!/usr/bin/env bash
# =============================================================================
# tests/coverage/backup_sh_coverage.sh — scripts/backup.sh 的 shell 行覆盖（可选 QA）
#
# 定位：M6.2 之外的可选增强，**不注册 CTest、不属于 M6.2 完成条件**。gcov/gcovr/lcov
#       只能插桩 C++，无法覆盖 shell 脚本，故用 bash xtrace 收集 scripts/backup.sh
#       执行到的行，按“可执行起始行”统计行覆盖率。
#
# 方法：经 BASH_ENV 注入 `set -x`（PS4=BASH_SOURCE:LINENO，BASH_XTRACEFD 单独 FD），
#       运行 tests/scripts/test_backup_restore.sh 的完整场景集，再按文件过滤。
#       追踪不写入被测脚本的 stdout/stderr，不改变脚本行为。
#
# 局限：仅行覆盖（近似），不是分支覆盖；测试内 `env -i`（T-040 cron 最小环境）不继承
#       BASH_ENV，该次调用不被追踪（其涉及的行在其它场景已覆盖）。
#
# 用法：
#   bash tests/coverage/backup_sh_coverage.sh [输出目录]
# 前置：已构建 build/oj_server（可经 OJ_M62_SERVER 覆盖服务路径）。
# =============================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SUT="$ROOT/scripts/backup.sh"
TEST="$ROOT/tests/scripts/test_backup_restore.sh"
SERVER="${OJ_M62_SERVER:-$ROOT/build/oj_server}"
HOOK="$ROOT/tests/coverage/backup_xtrace_env.sh"
OUTDIR="${1:-$ROOT/build-cov/coverage}"
REPORT="$OUTDIR/backup-sh-coverage.txt"

command -v python3 >/dev/null 2>&1 || { echo "需要 python3" >&2; exit 2; }
[[ -x "$SERVER" ]] || { echo "缺少可执行服务 $SERVER（请先构建）" >&2; exit 2; }
[[ -f "$TEST" && -f "$HOOK" && -f "$SUT" ]] || { echo "缺少被测/测试脚本" >&2; exit 2; }
mkdir -p "$OUTDIR" || { echo "无法创建输出目录 $OUTDIR" >&2; exit 2; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/oj-bshcov.XXXXXX")" || exit 2
TRACE="$TMP/trace.log"
TESTLOG="$TMP/m62.log"
cleanup() { rm -rf -- "$TMP"; }
trap cleanup EXIT

echo "== scripts/backup.sh 行覆盖（xtrace；可选 QA，非 M6.2 完成条件）=="
echo "构建:   $SERVER"
echo "报告:   $REPORT"
echo "运行 M6.2 集成测试场景集以收集追踪（单进程、串行）..."

BASH_ENV="$HOOK" M62_XTRACE_LOG="$TRACE" \
  bash "$TEST" "$ROOT" "$SERVER" >"$TESTLOG" 2>&1
test_rc=$?
grep -E "通过：|失败：" "$TESTLOG" | tail -2 || true
if (( test_rc != 0 )); then
  echo "注意：M6.2 集成测试未通过（退出码 $test_rc），覆盖率仅代表实际执行部分。" >&2
fi

python3 - "$SUT" "$TRACE" "$REPORT" "$test_rc" <<'PY'
import re, sys, os, datetime

sut, trace, report, test_rc = sys.argv[1:5]

exec_lines = set()
if os.path.exists(trace):
    with open(trace, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            for m in re.finditer(r"backup\.sh:(\d+)", line):
                exec_lines.add(int(m.group(1)))

src = open(sut, encoding="utf-8").read().splitlines()
n = len(src)
closers = re.compile(r"^\s*(\}|fi|done|esac|\)|;;)\s*$")
funcdef = re.compile(r"^\s*(function\s+)?[A-Za-z_][A-Za-z0-9_]*\s*\(\)\s*\{?\s*$")

# bash xtrace 对反斜杠续行可能上报起始行或续行；统一映射到命令起始行。
start = [0] * (n + 1)
for i in range(1, n + 1):
    if i > 1 and src[i - 2].rstrip().endswith("\\"):
        start[i] = start[i - 1]
    else:
        start[i] = i

executable = set()
for i, raw in enumerate(src, 1):
    if start[i] != i:            # 续行：归属其起始命令
        continue
    s = raw.strip()
    if not s or s.startswith("#") or closers.match(raw) or funcdef.match(raw):
        continue
    executable.add(i)

covered_set = {start[t] for t in exec_lines if 1 <= t <= n and start[t] in executable}
covered = sorted(covered_set)
missing = sorted(executable - covered_set)
total, cov = len(executable), len(covered)
pct = (100.0 * cov / total) if total else 0.0

out = [
    "# scripts/backup.sh 行覆盖率（xtrace 方式，M6.2）",
    f"# 生成时间: {datetime.datetime.now().isoformat(timespec='seconds')}",
    "# 方法: BASH_ENV 注入 bash xtrace（PS4=BASH_SOURCE:LINENO，BASH_XTRACEFD 独立 FD），",
    "#       运行 tests/scripts/test_backup_restore.sh 场景集；按“可执行起始行”统计。",
    "# 局限: 仅行覆盖（近似），非分支覆盖；T-040 的 `env -i` 调用不继承 BASH_ENV，未被追踪。",
    f"# M6.2 集成测试退出码: {test_rc}",
    "",
    f"可执行起始行: {total}",
    f"已执行行:     {cov}",
    f"行覆盖率:     {cov}/{total} = {pct:.1f}%",
    "",
    "未执行行（命令起始行；需人工区分“分支未走到/结构行 xtrace 不上报/续行归属”）:",
]
if missing:
    for ln in missing:
        out.append(f"  L{ln}: {src[ln - 1]}")
else:
    out.append("  （无）")
out += [
    "",
    "执行到的命令起始行号: " + ",".join(map(str, covered)),
    "",
]
open(report, "w", encoding="utf-8").write("\n".join(out))

print(f"可执行起始行 {total} / 已执行 {cov} = {pct:.1f}%")
print("未执行行: " + (",".join(map(str, missing)) if missing else "（无）"))
print(f"报告: {report}")
PY

echo
echo "完成。注意：行覆盖为可选增强，不代表所有分支已覆盖。"
