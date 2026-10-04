<#
.SYNOPSIS
  停掉这台虚拟机（Windows 侧）。

.EXAMPLE
  .\bin\stop.ps1                 # 停 default（端口 5580）
  .\bin\stop.ps1 -Port 5584
  .\bin\stop.ps1 -Force          # 60 秒还没退就强杀

.NOTES
  常规停机经 HTTP /api/v1/power 让 Android 完成应用通知、卸载文件系统与落盘。
  只有显式 -Force 才允许 console kill / Stop-Process 强制断电。
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

if (-not (Test-InstanceRunning $Port)) {
    Write-Warn "端口 $Port 上没有模拟器在跑（实例 '$Name'）"
    exit 0
}

if (Stop-Instance $Port $TimeoutSec -Force:$Force) {
    Write-Ok "已停止：emulator-$Port"
    exit 0
}
if ($Force) { Die "强杀后进程还在" }
Die "没停掉。服务未接受优雅关机；确认令牌或重试： .\bin\stop.ps1 -Port $Port -Force"
