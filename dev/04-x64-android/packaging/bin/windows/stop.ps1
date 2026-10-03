<#
.SYNOPSIS
  停掉这台虚拟机（Windows 侧）。

.EXAMPLE
  .\bin\stop.ps1                 # 停 default（端口 5580）
  .\bin\stop.ps1 -Port 5584
  .\bin\stop.ps1 -Force          # 60 秒还没退就强杀

.NOTES
  ⚠️ 为什么不能只 Stop-Process：`adb emu kill` 是**硬断电**，不是关机。
  实测（上游项目 tools/verify-kill-is-hard-poweroff.sh，同一台实例三组对照）：
    写入后 sync 再关 → 数据在
    写入后不 sync 直接关 → **数据没了**
    写入后等 15 秒再关 → **还是没了**
  所以这里一定先 `adb shell sync`。这条别"优化"掉。
#>
[CmdletBinding()]
param(
    [string]$Name = "",
    [int]$Port = 0,
    [int]$TimeoutSec = 60,
    [switch]$Force
)

. (Join-Path $PSScriptRoot "common.ps1")
if ([string]::IsNullOrWhiteSpace($Name)) { $Name = $script:DefaultName }
if ($Port -le 0) { $Port = Get-InstancePort $Name }
if ($Port -le 0) { Die "实例 '$Name' 没登记过端口（.\bin\status.ps1 看有哪些）" }

Assert-Adb
$serial = Get-Serial $Port

if (-not (Test-InstanceRunning $Port)) {
    Write-Warn "端口 $Port 上没有模拟器在跑（实例 '$Name'）"
    exit 0
}

Write-Log "让 guest 把脏页落盘（adb shell sync）"
& $script:Adb -s $serial shell sync 2>$null | Out-Null
Start-Sleep -Seconds 1

Write-Log "请 guest 自己关机（adb emu kill）"
& $script:Adb -s $serial emu kill 2>$null | Out-Null

# ⚠️ 等的是**进程真的退出**，不是"命令返回了"：emu kill 只是递个关机请求，
#    guest 还要走完流程（卸载 /data、收 qcow2）。这中间就重启会撞上
#    multiinstance.lock，第二台报 another emulator instance is currently running。
$waited = 0
while ($waited -lt $TimeoutSec) {
    if (-not (Test-InstanceRunning $Port)) { Write-Ok "已停止：$serial"; exit 0 }
    Start-Sleep -Seconds 2
    $waited += 2
}

Write-Warn "等了 ${TimeoutSec} 秒还没退出 —— 数据已经 sync 过，可以强杀"
if ($Force) {
    foreach ($p in @(Get-EmuProcess $Port)) {
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 2
    if (-not (Test-InstanceRunning $Port)) { Write-Ok "已强杀：$serial"; exit 0 }
    Die "强杀后进程还在"
}
Die "没停掉。重试： .\bin\stop.ps1 -Port $Port -Force"
