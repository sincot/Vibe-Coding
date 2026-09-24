# 由 tests/coverage/backup_sh_coverage.sh 经 BASH_ENV 注入。
#
# 目的：只对被测脚本 scripts/backup.sh 开启 bash xtrace，并把追踪写入
# $M62_XTRACE_LOG（通过 BASH_XTRACEFD），**不污染被测脚本自身的 stdout/stderr**，
# 也不影响其它非目标脚本。
#
# 该文件仅在显式设置 BASH_ENV 时被 source；未设置时对正常构建/测试/运行零影响。
# 说明：BASH_ENV 在 $0 被设为脚本名之前被 source，无法据此按脚本文件名过滤；
# 因此改为“显式设置 M62_XTRACE_LOG 时对该进程树全部开启 xtrace，收集后再按
# BASH_SOURCE 过滤出 scripts/backup.sh”。BASH_XTRACEFD 保证追踪不写入 stderr。
if [[ -n "${M62_XTRACE_LOG:-}" ]]; then
  exec 19>>"$M62_XTRACE_LOG"
  BASH_XTRACEFD=19
  PS4='+${BASH_SOURCE[0]}:${LINENO}: '
  set -x
fi
