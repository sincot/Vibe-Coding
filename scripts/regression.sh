#!/usr/bin/env bash
# =============================================================================
# scripts/regression.sh — 端到端冒烟回归入口（M6.1）
#
# 定位：在【已有构建产物】基础上，一次执行完成「隔离数据准备 -> 测试服务启动
#       -> 真实 HTTP 接口验证 -> 结果汇总 -> 资源清理」的端到端冒烟回归。
#
#       它验证的是一条最小但真实的业务链路：注册 -> 登录 -> 鉴权 -> 取题 ->
#       C++17/C11 已知 AC/WA 提交 -> 判题状态断言 -> 重启后历史持久化。
#       它【不是】全量测试入口：完整单元/集成常规回归请执行
#           ctest --test-dir build --parallel 1 --output-on-failure --timeout 120
#       本脚本通过绝不代表全部功能已通过，只代表该冒烟链路通过。
#
# 前置条件（脚本不会暗中触发构建）：
#   1. 已构建：cmake --build build --parallel 1
#   2. 依赖：curl、python3（可靠 JSON 解析 + 挑选空闲端口）；openssl 可选
#      （仅用于生成随机测试密钥，缺失时退化为仅测试用的常量密钥）。
#      脚本不会临时无提示下载任何工具。
#
# 隔离约定（不影响正式数据）：
#   - 独立临时数据库（mktemp 目录），用后删除；不连接/修改 data/oj.db。
#   - 独立测试 JWT 密钥与测试管理员密码，不写日志、不出现在汇总中。
#   - 独立判题目录，优先复用真实 tmpfs（/opt/oj-tmpfs、/dev/shm）。
#   - 空闲随机端口（可用 OJ_REGRESSION_PORT 覆盖），只清理本轮启动的服务。
#
# 用法：
#   bash scripts/regression.sh
#   OJ_REGRESSION_PORT=18080 bash scripts/regression.sh
#   OJ_REGRESSION_KEEP=1 bash scripts/regression.sh   # 失败排查时保留临时目录
#   OJ_REGRESSION_SERVER=/path/to/oj_server bash scripts/regression.sh
#                                                    # 覆盖被测服务路径（如覆盖率构建）
#
# 仅用于验证脚本自身失败路径（正常运行不要设置）：
#   OJ_REGRESSION_INJECT_FAILURE=1 bash scripts/regression.sh
#     -> 在真实检查之后注入一个必然失败的断言，用于确认脚本会非零退出并清理。
# =============================================================================
set -uo pipefail

# ---------------------------------------------------------------------------
# 颜色（重定向输出时自动退化为纯文本，颜色不代替断言与退出码）
# ---------------------------------------------------------------------------
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  C_GREEN=$'\033[32m'; C_RED=$'\033[31m'; C_YELLOW=$'\033[33m'
  C_BOLD=$'\033[1m'; C_RESET=$'\033[0m'
else
  C_GREEN=''; C_RED=''; C_YELLOW=''; C_BOLD=''; C_RESET=''
fi

usage() {
  sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数：$1（使用 --help 查看用法）" >&2; exit 2 ;;
  esac
done

# ---------------------------------------------------------------------------
# 基本路径与依赖
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SERVER="${OJ_REGRESSION_SERVER:-$ROOT/build/oj_server}"
WEB="$ROOT/web"

MISSING=()
[[ -x "$SERVER" ]] || MISSING+=("构建产物 $SERVER 不存在或不可执行")
for tool in curl python3; do
  command -v "$tool" >/dev/null 2>&1 || MISSING+=("缺少依赖命令：$tool")
done
if ((${#MISSING[@]} > 0)); then
  echo "${C_RED}前置条件不满足：${C_RESET}" >&2
  for m in "${MISSING[@]}"; do echo "  - $m" >&2; done
  echo "请先构建：cmake --build \"$ROOT/build\" --parallel 1" >&2
  exit 2
fi

HTTP_TIMEOUT="${OJ_REGRESSION_HTTP_TIMEOUT:-30}"
HEALTH_TIMEOUT="${OJ_REGRESSION_HEALTH_TIMEOUT:-30}"
SERVER_STOP_TIMEOUT="${OJ_REGRESSION_STOP_TIMEOUT:-20}"

# ---------------------------------------------------------------------------
# 隔离资源：临时目录 / 数据库 / 判题工作目录 / 端口 / 密钥
# ---------------------------------------------------------------------------
TMP="$(mktemp -d "${TMPDIR:-/tmp}/oj-regression.XXXXXX")" || {
  echo "无法创建临时目录" >&2; exit 2; }
DB="$TMP/oj.db"
SERVER_LOG="$TMP/server.log"
BODY_FILE="$TMP/body.json"
SUBMIT_FILE="$TMP/submit.json"

# 日志保留目录（build/ 已被 .gitignore 忽略）；完整服务日志在结束时复制至此。
LOG_DIR="${OJ_REGRESSION_LOG_DIR:-$ROOT/build/regression-logs/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$LOG_DIR" || { echo "无法创建日志目录 $LOG_DIR" >&2; exit 2; }
: >"$SERVER_LOG"

PORT="${OJ_REGRESSION_PORT:-}"
if [[ -z "$PORT" ]]; then
  PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')" || {
    echo "无法选择空闲端口" >&2; exit 2; }
fi
if ! [[ "$PORT" =~ ^[0-9]+$ ]] || ((PORT < 1 || PORT > 65535)); then
  echo "非法端口：$PORT" >&2; exit 2
fi
BASE="http://127.0.0.1:$PORT"

# 判题工作目录：优先沿用已有配置，否则自动选择真实 tmpfs，最后退化为开发模式。
if [[ -n "${OJ_JUDGE_WORKSPACE:-}" ]]; then
  : # 使用调用方指定的值（若指向非 tmpfs 且未允许，服务会按设计拒绝启动）
else
  for cand in /opt/oj-tmpfs /dev/shm; do
    if [[ -d "$cand" ]] && [[ "$(stat -f -c %T "$cand" 2>/dev/null || true)" == "tmpfs" ]]; then
      OJ_JUDGE_WORKSPACE="$cand"; break
    fi
  done
fi
if [[ -z "${OJ_JUDGE_WORKSPACE:-}" ]]; then
  OJ_JUDGE_WORKSPACE="$TMP/workspace"
  mkdir -p "$OJ_JUDGE_WORKSPACE"
  export OJ_JUDGE_ALLOW_NON_TMPFS=1
fi
export OJ_JUDGE_WORKSPACE

# 测试密钥：优先随机生成，绝不使用正式密钥，也绝不写入日志/汇总。
if [[ -n "${OJ_JWT_SECRET:-}" ]]; then
  JWT_SECRET="$OJ_JWT_SECRET"
elif command -v openssl >/dev/null 2>&1; then
  JWT_SECRET="$(openssl rand -hex 32)"
else
  JWT_SECRET="regr-test-secret-0123456789abcdef-$(date +%s%N)"
fi
ADMIN_PASSWORD="RegrAdmin_Pw_123!"
USER_PASSWORD="RegrPw_123"
NICKNAME="regr_user_$$"

# ---------------------------------------------------------------------------
# 结果计数与断言辅助
# ---------------------------------------------------------------------------
PASSED=0
FAILED=0

pass() { PASSED=$((PASSED+1)); printf '  %s[PASS]%s %s\n' "$C_GREEN" "$C_RESET" "$1"; }
fail() { FAILED=$((FAILED+1)); printf '  %s[FAIL]%s %s\n' "$C_RED" "$C_RESET" "$1"; }

check_eq() { # <描述> <期望> <实际>
  if [[ "$2" == "$3" ]]; then pass "$1"; else fail "$1（期望='$2' 实际='$3'）"; fi
}
check_match() { # <描述> <扩展正则> <实际>
  if [[ "$3" =~ $2 ]]; then pass "$1"; else fail "$1（应匹配 /$2/，实际='$3'）"; fi
}
check_true() { # <描述> <条件表达式>
  if eval "$2"; then pass "$1"; else fail "$1（条件不成立：$2）"; fi
}

# 可靠 JSON 解析：python3 标准库，失败返回非零，绝不 grep 文本。
json_val() { # <文件> <以 d 为根对象的 python 表达式>
  python3 -c '
import json, sys
try:
    with open(sys.argv[1]) as fh:
        d = json.load(fh)
    v = eval(sys.argv[2], {"__builtins__": __builtins__}, {"d": d})
except Exception:
    sys.exit(3)
if v is None:
    sys.exit(3)
print(v)
' "$1" "$2"
}

# HTTP 调用：返回 HTTP 状态码（stdout），响应体写入 BODY_FILE。
http_code() { # <方法> <路径> [body] [token]
  local method="$1" path="$2" data="${3:-}" token="${4:-}"
  local -a args=(-sS --max-time "$HTTP_TIMEOUT" -o "$BODY_FILE" -w '%{http_code}' -X "$method")
  if [[ -n "$data" ]]; then
    args+=(-H 'Content-Type: application/json' --data-binary "$data")
  fi
  if [[ -n "$token" ]]; then
    args+=(-H "Authorization: Bearer $token")
  fi
  curl "${args[@]}" "$BASE$path"
}

make_submit_body() { # <语言> <源码>
  python3 -c 'import json,sys; print(json.dumps({"language": sys.argv[1], "code": sys.argv[2]}))' "$1" "$2"
}

# ---------------------------------------------------------------------------
# 测试服务生命周期（只管理本轮以 $SERVER_PID 启动的进程）
# ---------------------------------------------------------------------------
SERVER_PID=""

stop_server() {
  [[ -z "${SERVER_PID:-}" ]] && return 0
  if kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    local waited=0
    while kill -0 "$SERVER_PID" 2>/dev/null && (( waited < SERVER_STOP_TIMEOUT * 10 )); do
      sleep 0.1; waited=$((waited + 1))
    done
    if kill -0 "$SERVER_PID" 2>/dev/null; then
      printf '  %s[warn]%s 测试服务未在 %ss 内优雅退出，强制终止本轮 PID=%s\n' \
        "$C_YELLOW" "$C_RESET" "$SERVER_STOP_TIMEOUT" "$SERVER_PID"
      kill -KILL "$SERVER_PID" 2>/dev/null || true
    fi
  fi
  wait "$SERVER_PID" 2>/dev/null || true
  SERVER_PID=""
}

start_server() {
  { echo "===== 启动测试服务 $(date -Is) ====="; } >>"$SERVER_LOG"
  (
    export OJ_JWT_SECRET="$JWT_SECRET"
    export OJ_ADMIN_PASSWORD="$ADMIN_PASSWORD"
    export OJ_JUDGE_WORKSPACE
    if [[ -n "${OJ_JUDGE_ALLOW_NON_TMPFS:-}" ]]; then
      export OJ_JUDGE_ALLOW_NON_TMPFS
    fi
    exec "$SERVER" --host 127.0.0.1 --port "$PORT" --db "$DB" --web "$WEB"
  ) >>"$SERVER_LOG" 2>&1 &
  SERVER_PID=$!
  # 记录准确进程身份（PID/PPID/PGID/命令行），便于审计与排错。
  if command -v ps >/dev/null 2>&1; then
    ps -o pid,ppid,pgid,etime,stat,args -p "$SERVER_PID" >>"$SERVER_LOG" 2>&1 || true
  fi

  # 有截止时间的健康检查；服务提前退出则立即失败，绝不误用端口上的其它服务。
  local deadline=$((SECONDS + HEALTH_TIMEOUT))
  while (( SECONDS < deadline )); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      return 1
    fi
    local code
    code="$(curl -sS --max-time 2 -o "$BODY_FILE" -w '%{http_code}' \
      "$BASE/api/health" 2>/dev/null || true)"
    if [[ "$code" == "200" ]] && python3 -c \
      'import json,sys; sys.exit(0 if json.load(open(sys.argv[1])).get("status")=="ok" else 1)' \
      "$BODY_FILE" 2>/dev/null; then
      return 0
    fi
    sleep 0.3
  done
  return 1
}

# 统一清理：正常结束、失败、中断都走这里；先停本轮服务，再清理临时数据。
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  stop_server
  if [[ -n "${LOG_DIR:-}" ]]; then
    cp -f "$SERVER_LOG" "$LOG_DIR/server.log" 2>/dev/null || true
  fi
  if [[ "${OJ_REGRESSION_KEEP:-0}" == "1" ]]; then
    printf '%s[cleanup]%s 保留临时目录：%s\n' "$C_YELLOW" "$C_RESET" "$TMP"
  else
    rm -rf "$TMP" 2>/dev/null || true
  fi
  exit "$rc"
}
trap 'exit 130' INT
trap 'exit 143' TERM
trap cleanup EXIT

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
printf '%s== OJ 端到端冒烟回归（M6.1）==%s\n' "$C_BOLD" "$C_RESET"
echo "仓库: $ROOT"
echo "端口: $PORT  数据库: $DB（临时）"
WS_FS="$(stat -f -c %T "$OJ_JUDGE_WORKSPACE" 2>/dev/null || true)"
if [[ "$WS_FS" == "tmpfs" ]]; then
  echo "判题工作目录: $OJ_JUDGE_WORKSPACE（tmpfs）"
elif [[ -n "${OJ_JUDGE_ALLOW_NON_TMPFS:-}" ]]; then
  echo "判题工作目录: $OJ_JUDGE_WORKSPACE（非 tmpfs，仅开发/测试）"
else
  echo "判题工作目录: $OJ_JUDGE_WORKSPACE（文件系统 ${WS_FS:-未知}）"
fi

echo
echo "[1/5] 隔离数据准备（导入内置种子题，幂等）"
if ! "$SERVER" --db "$DB" --seed >>"$SERVER_LOG" 2>&1; then
  echo "${C_RED}种子数据导入失败，详见 $SERVER_LOG${C_RESET}" >&2
  tail -n 40 "$SERVER_LOG" >&2
  exit 1
fi
echo "  种子题导入完成"

echo "[2/5] 启动测试服务并等待健康检查"
if ! start_server; then
  echo "${C_RED}测试服务启动失败或健康检查超时（PID=${SERVER_PID:-无}）。${C_RESET}" >&2
  echo "---- 服务日志（末尾 40 行）----" >&2
  tail -n 40 "$SERVER_LOG" >&2
  exit 1
fi
echo "  服务就绪：$BASE（PID=$SERVER_PID）"

echo
echo "[3/5] 认证与鉴权接口验证"
REG_BODY="$(python3 -c 'import json,sys; print(json.dumps({"nickname": sys.argv[1], "password": sys.argv[2]}))' "$NICKNAME" "$USER_PASSWORD")"
code="$(http_code POST /api/register "$REG_BODY")"
check_eq "注册返回 201" "201" "$code"
ACCOUNT="$(json_val "$BODY_FILE" 'd["account"]' || true)"
check_match "返回 10 位纯数字账号" '^[0-9]{10}$' "$ACCOUNT"

code="$(http_code POST /api/register "$REG_BODY")"
check_eq "重复昵称注册被拒绝（409）" "409" "$code"

LOGIN_BAD="$(python3 -c 'import json,sys; print(json.dumps({"account": sys.argv[1], "password": "definitely-wrong"}))' "$ACCOUNT")"
code="$(http_code POST /api/login "$LOGIN_BAD")"
check_eq "错误密码登录返回 401" "401" "$code"

LOGIN_OK="$(python3 -c 'import json,sys; print(json.dumps({"account": sys.argv[1], "password": sys.argv[2]}))' "$ACCOUNT" "$USER_PASSWORD")"
code="$(http_code POST /api/login "$LOGIN_OK")"
check_eq "正确登录返回 200" "200" "$code"
TOKEN="$(json_val "$BODY_FILE" 'd["token"]' || true)"
check_true "登录响应包含非空 token" '[[ -n "$TOKEN" ]]'

code="$(http_code GET /api/me '' '')"
check_eq "未携带 token 访问 /api/me 返回 401" "401" "$code"
code="$(http_code GET /api/me '' "$TOKEN")"
check_eq "携带 token 访问 /api/me 返回 200" "200" "$code"
ME_ACCOUNT="$(json_val "$BODY_FILE" 'd["account"]' || true)"
check_eq "/api/me 账号与登录账号一致" "$ACCOUNT" "$ME_ACCOUNT"

code="$(http_code GET /api/problems '' '')"
check_eq "公开题目列表返回 200" "200" "$code"
PROBLEM_ID="$(json_val "$BODY_FILE" \
  'next((str(p["id"]) for p in d["problems"] if p.get("title")=="A+B Problem"), None)' || true)"
check_match "在种子题中找到 A+B Problem" '^[0-9]+$' "$PROBLEM_ID"

echo
echo "[4/5] 真实判题链路（C++17 / C11 的已知 AC/WA）"
CPP_AC='#include <iostream>
int main(){ long long a,b; if(!(std::cin>>a>>b)) return 0; std::cout<<(a+b)<<"\n"; return 0; }'
C11_AC='#include <stdio.h>
int main(void){ long long a,b; if(scanf("%lld %lld",&a,&b)!=2) return 0; printf("%lld\n", a+b); return 0; }'
CPP_WA='#include <cstdio>
int main(){ long long a,b; if(scanf("%lld %lld",&a,&b)!=2) return 0; printf("0\n"); return 0; }'
C11_WA='#include <stdio.h>
int main(void){ long long a,b; if(scanf("%lld %lld",&a,&b)!=2) return 0; printf("0\n"); return 0; }'

submit_expect() { # <描述> <语言> <源码> <期望判题状态>
  local desc="$1" lang="$2" src="$3" expected="$4"
  local body code status sid
  body="$(make_submit_body "$lang" "$src")"
  code="$(http_code POST "/api/problems/$PROBLEM_ID/submit" "$body" "$TOKEN")"
  if [[ "$code" != "200" ]]; then
    fail "$desc：HTTP 期望 200 实际 $code（请求失败，不等同于判题结果）"
    return
  fi
  cp -f "$BODY_FILE" "$SUBMIT_FILE"
  status="$(json_val "$SUBMIT_FILE" 'd["status"]' || true)"
  check_eq "$desc（HTTP 200 且 JSON status）" "$expected" "$status"
  sid="$(json_val "$SUBMIT_FILE" 'd["id"]' || true)"
  check_match "$desc 返回有效提交 ID" '^[0-9]+$' "$sid"
}

submit_expect "C++17 已知正确程序" cpp17 "$CPP_AC" AC
submit_expect "C11 已知正确程序" c11 "$C11_AC" AC
submit_expect "C++17 已知错误程序" cpp17 "$CPP_WA" WA
submit_expect "C11 已知错误程序" c11 "$C11_WA" WA

UNAUTH_BODY="$(make_submit_body cpp17 "$CPP_AC")"
code="$(http_code POST "/api/problems/$PROBLEM_ID/submit" "$UNAUTH_BODY" '')"
check_eq "未登录提交被拒绝（401）" "401" "$code"

echo
echo "[5/5] 重启后持久化验证"
stop_server
if ! start_server; then
  echo "${C_RED}重启测试服务失败。${C_RESET}" >&2
  tail -n 40 "$SERVER_LOG" >&2
  exit 1
fi
code="$(http_code GET '/api/submissions?mine' '' "$TOKEN")"
check_eq "重启后本人提交历史返回 200" "200" "$code"
TOTAL="$(json_val "$BODY_FILE" 'd.get("total")' || true)"
check_eq "重启后提交历史总数为 4" "4" "$TOTAL"
STATUSES="$(json_val "$BODY_FILE" \
  '",".join(sorted(s.get("status","") for s in d.get("submissions",[])))' || true)"
check_eq "历史中保留 2 个 AC 与 2 个 WA" "AC,AC,WA,WA" "$STATUSES"

# 仅用于验证脚本失败路径：正常运行不设置该变量。
if [[ "${OJ_REGRESSION_INJECT_FAILURE:-0}" == "1" ]]; then
  check_eq "注入的故障路径自检（预期失败）" "expected" "injected-actual"
fi

echo
printf '%s== 冒烟回归汇总 ==%s\n' "$C_BOLD" "$C_RESET"
echo "  通过：$PASSED  失败：$FAILED"
echo "  完整服务日志：$LOG_DIR/server.log"
echo "  说明：本脚本是端到端冒烟回归入口，不等于全量测试；"
echo "        全量常规回归请执行 ctest --test-dir build --parallel 1 --output-on-failure --timeout 120"
if (( FAILED > 0 )); then
  printf '%s冒烟回归失败：%d 项断言未通过%s\n' "$C_RED" "$FAILED" "$C_RESET"
  exit 1
fi
printf '%s冒烟回归通过：%d 项断言全部通过%s\n' "$C_GREEN" "$PASSED" "$C_RESET"
exit 0
