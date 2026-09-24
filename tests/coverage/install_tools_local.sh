#!/usr/bin/env bash
# =============================================================================
# tests/coverage/install_tools_local.sh — 免 sudo 本地安装覆盖率报告工具
#
# 用 `apt-get download`（无需 root）+ `dpkg-deb -x` 把 gcovr 与 lcov/genhtml
# 及其 Python/Perl 依赖解包到本地目录，不修改系统、不需要密码。仅用于可选的
# C++ 覆盖率采集（不属于 M6.1 完成条件）。
#
# 用法：
#   bash tests/coverage/install_tools_local.sh
#   COVTOOL_DIR=/path/to/tools bash tests/coverage/install_tools_local.sh
#
# 安装后可直接使用 run_coverage.sh；它会自动 source 本目录下的 env.sh。
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOLS_DIR="${COVTOOL_DIR:-$SCRIPT_DIR/.tools}"

if ! command -v apt-get >/dev/null 2>&1 || ! command -v dpkg-deb >/dev/null 2>&1; then
  echo "需要 apt-get 与 dpkg-deb（Debian/Ubuntu）" >&2
  exit 2
fi

mkdir -p "$TOOLS_DIR/debs" "$TOOLS_DIR/root" || exit 2
cd "$TOOLS_DIR/debs" || exit 2

PKGS=(
  gcovr lcov
  python3-babel python3-jinja2 python3-lxml python3-markupsafe
  python3-pygments python3-tz python-babel-localedata
  libjson-perl libperlio-gzip-perl
)

echo "[coverage-tools] 下载：${PKGS[*]}"
if ! apt-get download "${PKGS[@]}"; then
  echo "[coverage-tools] 下载失败（检查网络与软件源）" >&2
  exit 1
fi

shopt -s nullglob
for d in ./*.deb; do
  if ! dpkg-deb -x "$d" "$TOOLS_DIR/root"; then
    echo "[coverage-tools] 解包失败：$d" >&2
    exit 1
  fi
done
shopt -u nullglob

PYDIR="$TOOLS_DIR/root/usr/lib/python3/dist-packages"
PERLDIRS="$(find "$TOOLS_DIR/root/usr" -type d -path '*perl*' 2>/dev/null | tr '\n' ':')"

cat > "$TOOLS_DIR/env.sh" <<EOF
# 由 install_tools_local.sh 生成，source 后即可使用 gcovr/lcov/genhtml。
export COVTOOL_DIR="$TOOLS_DIR"
export PYTHONPATH="$PYDIR\${PYTHONPATH:+:\$PYTHONPATH}"
export PERL5LIB="$PERLDIRS\${PERL5LIB:+:\$PERL5LIB}"
export PATH="$TOOLS_DIR/root/usr/bin:\$PATH"
EOF

# shellcheck disable=SC1090
source "$TOOLS_DIR/env.sh"
echo "[coverage-tools] gcovr: $(gcovr --version 2>/dev/null | head -1)"
echo "[coverage-tools] lcov : $(lcov --version 2>/dev/null | head -1)"
echo "[coverage-tools] 已安装到 $TOOLS_DIR"
