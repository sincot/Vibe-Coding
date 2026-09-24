# M4.1 真实浏览器验证运行脚本（Windows + 本地已安装的 playwright-cli）。
#
# 使用前提：
#   1. 云服务器上的 oj_server 已启动；本机通过 VS Code/SSH 端口转发访问，例如把
#      远端 127.0.0.1:8080 转发到本机 localhost:8080。
#   2. 本机已安装 playwright-cli（@playwright/cli）及其浏览器。
#
# 用法（在仓库 tests/frontend/browser/ 目录）：
#   powershell -ExecutionPolicy Bypass -File .\run_m41_browser.ps1 -Base http://127.0.0.1:8080 -Headed
#
# 说明：默认无头；-Headed 显示浏览器窗口。截图输出到 .\artifacts\。
param(
  [string]$Base = "http://127.0.0.1:8080",
  [string]$Out = "$PSScriptRoot\artifacts",
  [switch]$Headed
)

$ErrorActionPreference = "Stop"

$scenario = Get-Content -Raw "$PSScriptRoot\m41_browser_scenarios.js"
$scenario = $scenario.Replace("__BASE__", $Base).Replace("__OUT__", ($Out -replace '\\', '/'))
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$run = Join-Path $PSScriptRoot "m41_run.js"
Set-Content -Path $run -Value $scenario -Encoding UTF8

$openArgs = @("open", "about:blank")
if ($Headed) { $openArgs += "--headed" }

playwright-cli @openArgs
playwright-cli run-code --filename="$run"
playwright-cli close

Write-Host "截图输出目录：$Out"
