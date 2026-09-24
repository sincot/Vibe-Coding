# M6.4 最终验收 A：Windows + 本地 playwright-cli 真实有头浏览器运行脚本。
#
# 使用前提：
#   1. 云服务器上的隔离 oj_server 已启动，且已通过 VS Code/SSH 端口转发把远端端口
#      映射到本机（例如远端 127.0.0.1:18080 -> 本机 localhost:18080）；
#   2. 本机已安装 playwright-cli（@playwright/cli）及其 Chromium。
#
# 用法（在仓库 tests\frontend\browser\ 目录，把两个 .js/.ps1 文件复制到本机）：
#   powershell -ExecutionPolicy Bypass -File .\run_m64_browser.ps1 `
#       -Base http://localhost:18080 -AdminPassword '<服务器启动时的初始管理员密码>' -Headed
#
# 默认无头；-Headed 显示浏览器窗口（真实有头验证）。截图输出到 .\artifacts\。
# 退出码 0 表示全部场景通过。
param(
  [string]$Base = "http://127.0.0.1:18080",
  [string]$Out = "$PSScriptRoot\artifacts",
  [string]$AdminPassword = "",
  [switch]$Headed
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($AdminPassword)) {
  throw "必须用 -AdminPassword 提供服务器首次启动时的 OJ_ADMIN_PASSWORD（首个管理员将在流程中改密）"
}

$scenario = Get-Content -Raw "$PSScriptRoot\m64_browser_scenarios.js"
$scenario = $scenario.Replace("__BASE__", $Base)
$scenario = $scenario.Replace("__OUT__", ($Out -replace '\\', '/'))
$scenario = $scenario.Replace("__ADMIN_PW__", $AdminPassword)
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$run = Join-Path $PSScriptRoot "m64_run.js"
Set-Content -Path $run -Value $scenario -Encoding UTF8

$openArgs = @("open", "about:blank")
if ($Headed) { $openArgs += "--headed" }

playwright-cli @openArgs
$raw = (playwright-cli run-code --filename="$run" | Out-String)
playwright-cli close

Write-Host $raw
$obj = $null
$idx = $raw.IndexOf("### Result")
if ($idx -ge 0) {
  $tail = $raw.Substring($idx + 10)
  # 优先尝试整段解析（可能是单行 JSON 对象或 JSON 字符串）。
  $trimmed = $tail.Trim()
  foreach ($candidate in @($trimmed) + ($tail -split "`r?`n")) {
    $t = $candidate.Trim()
    if (-not $t.StartsWith("{")) { continue }
    try {
      $o = $t | ConvertFrom-Json
      if ($o -is [string]) { $o = $o | ConvertFrom-Json }
      if ($o -ne $null -and $o.results -ne $null) { $obj = $o; break }
    } catch { }
  }
}
if ($obj -eq $null) {
  throw "无法解析场景结果；请把上面的原始输出（尤其 ### Result 之后的内容）贴回。"
}

$results = @($obj.results)
$errors = @($obj.errors)
$failed = @($results | Where-Object { -not $_.ok })
Write-Host ""
Write-Host ("M6.4 Windows 真实浏览器：通过 {0}/{1}" -f ($results.Count - $failed.Count), $results.Count)
foreach ($f in $failed) { Write-Host ("  FAIL: {0} | {1}" -f $f.name, $f.detail) }
if ($errors.Count -gt 0) {
  Write-Host ("脚本错误 {0} 项：" -f $errors.Count)
  $errors | Select-Object -First 8 | ForEach-Object { Write-Host ("  - " + $_) }
}
Write-Host ("截图输出目录：{0}" -f $Out)
if ($failed.Count -gt 0 -or $errors.Count -gt 0) { exit 1 }
exit 0
