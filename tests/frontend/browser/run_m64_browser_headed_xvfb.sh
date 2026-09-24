#!/usr/bin/env bash
# M6.4 真实浏览器「有头」验收：在私有虚拟 X 显示上运行窗口化 Chromium。
#
# 服务器通常无物理显示。本脚本在**无 root、不改系统**的前提下：
#   1. 用已解包的 Xvfb（见下）在 `unshare -rm` 用户+挂载命名空间内启动一个虚拟 X 显示；
#      Xvfb 需要 /usr/bin/xkbcomp 与 /bin/sh（Ubuntu 上 /bin -> /usr/bin），故在命名空间内
#      把 `/usr/bin` 叠加为一个只含 xkbcomp 与 sh(dash 副本) 的目录，仅对本进程树生效。
#   2. 以 DISPLAY 指向该显示，调用 run_m64_browser.sh 以 PWCLI_HEADED=1 运行真实有头
#      Chromium，执行 M6.4 全流程场景并输出逐项结果与截图。
#
# 前置：已解包 Xvfb 到 XVFB_ROOT（默认 /tmp/opencode/xvfb/root），获取方式：
#   cd /tmp/opencode && mkdir -p xvfb/debs xvfb/root && cd xvfb/debs
#   apt-get download xvfb xserver-xorg-core xserver-common xkb-data xfonts-base \
#     libxfont2 libfontenc1 libxcb1 libxau6 libxdmcp6 libbsd0 libsystemd0 libselinux1 \
#     libpcre2-8-0 libaudit1 libcap-ng0 libgcrypt20 liblz4-1 liblzma5 libzstd1 libmd0 \
#     x11-common x11-xkb-utils
#   for d in ./*.deb; do dpkg-deb -x "$d" /tmp/opencode/xvfb/root; done
#
# 用法：
#   bash tests/frontend/browser/run_m64_browser_headed_xvfb.sh
# 可选：XVFB_ROOT=... M64_XVFB_DISPLAY=:99 BROWSER_ARTIFACTS_DIR=...
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
XVFB_ROOT="${XVFB_ROOT:-/tmp/opencode/xvfb/root}"
DISP="${M64_XVFB_DISPLAY:-:99}"
XDIR="$XVFB_ROOT/xbin"
UNIT="m64xvfb-$$"

[ -x "$XVFB_ROOT/usr/bin/Xvfb" ] || { echo "未找到 $XVFB_ROOT/usr/bin/Xvfb；请按脚本头部说明解包 Xvfb。" >&2; exit 2; }
command -v systemd-run >/dev/null 2>&1 || { echo "需要 systemd-run（user）" >&2; exit 2; }

mkdir -p "$XDIR"
cp -f "$XVFB_ROOT/usr/bin/xkbcomp" "$XDIR/xkbcomp"
cp -f /usr/bin/dash "$XDIR/sh"
cp -f /usr/bin/dash "$XDIR/dash"
chmod +x "$XDIR/xkbcomp" "$XDIR/sh" "$XDIR/dash"
cat > "$XDIR/start_xvfb.sh" <<EOF
#!/bin/bash
exec unshare -rm bash -c '
  mount --bind $XDIR /usr/bin || { echo BIND_FAIL; exit 1; }
  export LD_LIBRARY_PATH=$XVFB_ROOT/usr/lib/x86_64-linux-gnu
  export XKB_CONFIG_ROOT=$XVFB_ROOT/usr/share/X11/xkb
  exec $XVFB_ROOT/usr/bin/Xvfb $DISP -screen 0 1280x800x24 -nolisten tcp -ac -xkbdir $XVFB_ROOT/usr/share/X11/xkb
'
EOF
chmod +x "$XDIR/start_xvfb.sh"

stop_xvfb(){ systemctl --user stop "$UNIT" >/dev/null 2>&1 || true; systemctl --user reset-failed "$UNIT" >/dev/null 2>&1 || true; }
trap stop_xvfb EXIT
stop_xvfb
systemd-run --user --unit="$UNIT" --collect "$XDIR/start_xvfb.sh" >/dev/null 2>&1
for _ in $(seq 1 40); do [ -S "/tmp/.X11-unix/X${DISP#:}" ] && break; sleep 0.25; done
if [ ! -S "/tmp/.X11-unix/X${DISP#:}" ]; then
  echo "虚拟显示 $DISP 启动失败：" >&2
  journalctl --user -u "$UNIT" --no-pager 2>/dev/null | tail -15 >&2
  exit 1
fi
echo "虚拟显示 $DISP 就绪；以有头模式运行 M6.4 场景…"
DISPLAY="$DISP" PWCLI_HEADED=1 \
  PWCLI_BROWSER_LIBS="${PWCLI_BROWSER_LIBS:-/tmp/opencode/browserlibs/root/usr/lib/x86_64-linux-gnu}" \
  bash "$ROOT/tests/frontend/browser/run_m64_browser.sh"
RC=$?
echo "有头运行退出码=$RC"
exit "$RC"
