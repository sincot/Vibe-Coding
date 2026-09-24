#!/usr/bin/env bash
# =============================================================================
# tests/coverage/run_coverage.sh — 可选 C++ 覆盖率采集（gcov + gcovr/lcov → HTML）
#
# 定位：M6.1 之外的可选 QA 增强。使用独立覆盖率构建目录（不改 build/、不改
#       CMakeLists.txt），单并发构建、串行执行全部常规 CTest，再用 gcovr 或
#       lcov/genhtml 生成函数/行/分支覆盖报告。**不注册 CTest，不属于 M6.1
#       完成条件**，不改变 M6.1 的验证结论。
#
# 前置：
#   1. 先构建常规产物（本脚本会自行配置并构建独立的覆盖率目录）。
#   2. 报告工具二选一（见 install_tools_local.sh 免 sudo 安装）：
#        - 系统：sudo apt-get install -y lcov gcovr
#        - 本地：bash tests/coverage/install_tools_local.sh
#
# 用法：
#   bash tests/coverage/run_coverage.sh
#   COV_TOOL=lcov bash tests/coverage/run_coverage.sh
#   COV_WITH_REGRESSION=0 bash tests/coverage/run_coverage.sh   # 跳过 main.cpp 覆盖
#
# 环境变量：
#   COV_BUILD_DIR        覆盖率构建目录，默认 <repo>/build-cov
#   COV_TOOL             auto|gcovr|lcov，默认 auto（优先 gcovr）
#   COV_SCOPE            覆盖率过滤正则，默认 'src/'
#   COV_WITH_REGRESSION  1/0：是否用 instrumented oj_server 跑 regression.sh
#                        （覆盖 src/main.cpp 与服务生命周期），默认 1
#   COVTOOL_DIR          本地工具目录（install_tools_local.sh 安装位置）
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${COV_BUILD_DIR:-$ROOT/build-cov}"
REPORT="$BUILD/coverage"
SCOPE="${COV_SCOPE:-src/}"
WITH_REGRESSION="${COV_WITH_REGRESSION:-1}"
TOOL_PREF="${COV_TOOL:-auto}"

# 提前创建构建目录，供配置日志重定向使用。
mkdir -p "$BUILD" || { echo "无法创建构建目录 $BUILD" >&2; exit 2; }

# ---------------------------------------------------------------------------
# 定位报告工具：优先 PATH，其次本地安装目录
# ---------------------------------------------------------------------------
if [[ -n "${COVTOOL_DIR:-}" && -f "$COVTOOL_DIR/env.sh" ]]; then
  # shellcheck disable=SC1090
  source "$COVTOOL_DIR/env.sh"
  TOOLS_DIR="$COVTOOL_DIR"
elif [[ -f "$SCRIPT_DIR/.tools/env.sh" ]]; then
  # shellcheck disable=SC1090
  source "$SCRIPT_DIR/.tools/env.sh"
  TOOLS_DIR="$SCRIPT_DIR/.tools"
else
  TOOLS_DIR=""
fi

HAVE_GCOVR=0
command -v gcovr >/dev/null 2>&1 && HAVE_GCOVR=1
HAVE_LCOV=0
command -v lcov >/dev/null 2>&1 && command -v genhtml >/dev/null 2>&1 && HAVE_LCOV=1
case "$TOOL_PREF" in
  gcovr) HAVE_LCOV=0 ;;
  lcov)  HAVE_GCOVR=0 ;;
esac

if (( HAVE_GCOVR == 0 && HAVE_LCOV == 0 )); then
  cat >&2 <<'EOF'
未找到覆盖率报告工具（gcovr 和/或 lcov+genhtml）。请任选其一：
  bash tests/coverage/install_tools_local.sh          # 免 sudo，本地解包
  sudo apt-get install -y lcov gcovr                  # 系统安装
EOF
  exit 2
fi

TOOL_DESC=""
(( HAVE_GCOVR )) && TOOL_DESC="gcovr"
(( HAVE_LCOV )) && TOOL_DESC="${TOOL_DESC:+$TOOL_DESC + }lcov/genhtml"

if ! command -v gcov >/dev/null 2>&1; then
  echo "未找到 gcov" >&2; exit 2
fi

echo "== C++ 覆盖率采集（可选）=="
echo "仓库:   $ROOT"
echo "构建:   $BUILD"
echo "报告:   $REPORT"
echo "工具:   $TOOL_DESC${TOOLS_DIR:+（本地 $TOOLS_DIR）}"
echo "范围:   $SCOPE"

# ---------------------------------------------------------------------------
# 1) 配置独立覆盖率构建（命令行传入 --coverage，不改生产 CMake 配置）
# ---------------------------------------------------------------------------
echo
echo "[1/5] 配置覆盖率构建（Debug + --coverage）"
if ! cmake -S "$ROOT" -B "$BUILD" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="--coverage" \
      -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
      > "$BUILD/coverage-configure.log" 2>&1; then
  echo "配置失败，详见 $BUILD/coverage-configure.log" >&2
  tail -n 30 "$BUILD/coverage-configure.log" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# 2) 单并发构建
# ---------------------------------------------------------------------------
echo "[2/5] 单并发构建（cmake --build --parallel 1）"
if ! cmake --build "$BUILD" --parallel 1 > "$BUILD/coverage-build.log" 2>&1; then
  echo "构建失败，详见 $BUILD/coverage-build.log" >&2
  tail -n 30 "$BUILD/coverage-build.log" >&2
  exit 1
fi
echo "  构建完成"

# 清理旧计数，避免上次残留数据污染本次报告。
find "$BUILD" -name '*.gcda' -delete 2>/dev/null || true

# ---------------------------------------------------------------------------
# 3) 串行运行全部常规 CTest
# ---------------------------------------------------------------------------
echo "[3/5] 串行运行全部常规 CTest"
if ! ctest --test-dir "$BUILD" --parallel 1 --output-on-failure --timeout 120 \
      > "$BUILD/coverage-ctest.log" 2>&1; then
  echo "常规测试未全部通过，覆盖率仅代表实际执行部分，详见 $BUILD/coverage-ctest.log" >&2
else
  echo "  常规测试全部通过"
fi
grep -E "tests passed|tests failed" "$BUILD/coverage-ctest.log" || true

# ---------------------------------------------------------------------------
# 4) 可选：用 instrumented 服务跑 regression.sh，覆盖 main.cpp/服务生命周期
# ---------------------------------------------------------------------------
if [[ "$WITH_REGRESSION" == "1" && -x "$BUILD/oj_server" ]]; then
  echo "[4/5] 用 instrumented oj_server 运行 regression.sh（覆盖 main.cpp）"
  if OJ_REGRESSION_SERVER="$BUILD/oj_server" \
       bash "$ROOT/scripts/regression.sh" > "$BUILD/coverage-regression.log" 2>&1; then
    echo "  冒烟回归通过"
  else
    echo "  冒烟回归未通过，详见 $BUILD/coverage-regression.log（不影响其余覆盖率数据）" >&2
  fi
else
  echo "[4/5] 跳过 regression.sh（COV_WITH_REGRESSION=$WITH_REGRESSION，或无 instrumented 服务）"
fi

# ---------------------------------------------------------------------------
# 5) 生成报告
# ---------------------------------------------------------------------------
echo "[5/5] 生成覆盖率报告"
mkdir -p "$REPORT"
REPORTED=0

# gcovr：行/分支 HTML 明细
if (( HAVE_GCOVR )); then
  if gcovr -r "$ROOT" --filter "$SCOPE" \
        --gcov-ignore-parse-errors \
        --html-details "$REPORT/index.html" \
        --txt > "$REPORT/summary.txt" 2> "$REPORT/gcovr.err"; then
    echo "  gcovr 行/分支 HTML：$REPORT/index.html"
    gcovr -r "$ROOT" --filter "$SCOPE" --print-summary 2>/dev/null \
      | tee "$REPORT/summary.txt" || true
    REPORTED=1
  else
    echo "  gcovr 生成失败，详见 $REPORT/gcovr.err" >&2
    tail -n 20 "$REPORT/gcovr.err" >&2
  fi
fi

# lcov/genhtml：函数/行/分支 汇总与含函数覆盖的 HTML
if (( HAVE_LCOV )); then
  lcov --capture --directory "$BUILD" --output-file "$REPORT/lcov.info" \
       --rc lcov_branch_coverage=1 > "$REPORT/lcov.err" 2>&1
  lcov --extract "$REPORT/lcov.info" "*/${SCOPE}*" \
       -o "$REPORT/lcov.scope.info" --rc lcov_branch_coverage=1 \
       >> "$REPORT/lcov.err" 2>&1
  if lcov --summary "$REPORT/lcov.scope.info" --rc lcov_branch_coverage=1 \
        > "$REPORT/lcov-summary.txt" 2>&1; then
    echo "  lcov 函数/行/分支 汇总："
    sed 's/^/    /' "$REPORT/lcov-summary.txt"
    if genhtml "$REPORT/lcov.scope.info" -o "$REPORT/html-lcov" \
          --branch-coverage >> "$REPORT/lcov.err" 2>&1; then
      echo "  含函数覆盖的 HTML：$REPORT/html-lcov/index.html"
    fi
    REPORTED=1
  else
    echo "  lcov 处理失败，详见 $REPORT/lcov.err（lcov 1.14 对 gcov 11 兼容性有限）" >&2
    tail -n 10 "$REPORT/lcov.err" >&2
  fi
fi

if (( REPORTED == 0 )); then
  echo "未能生成任何报告" >&2
  exit 1
fi

echo
echo "完成。注意：覆盖率采集为可选增强，不属于 M6.1 完成条件，也不代表所有分支已覆盖。"
