<#
.SYNOPSIS
  看这台机器（或全部实例）现在什么状态，以及这个包里的 ROM 是哪一份。

.EXAMPLE
  .\bin\status.ps1
  .\bin\status.ps1 -Port 5580
#>
[CmdletBinding()]
param(
    [string]$Name = "",
    [int]$Port = 0
)

. (Join-Path $PSScriptRoot "common.ps1")
if ([string]::IsNullOrWhiteSpace($Name)) { $Name = $script:DefaultName }

function Show-Head { param([string]$T) Write-Host "── $T" -ForegroundColor Cyan }
function Show-Row  { param([string]$K, [string]$V) Write-Host ("  {0,-16} {1}" -f $K, $V) }

function Get-BootState {
    param([int]$P)
    if (Test-ServiceReady $P) { return "服务已就绪" }
    return "服务未就绪"
}

if ($Port -gt 0 -or (Test-Instance $Name)) {
    if ($Port -le 0) { $Port = Get-InstancePort $Name }
    if ($Port -le 0) { Die "实例 '$Name' 没登记过端口" }
    $sysdir = Get-SysDir $Port

    Show-Head "实例 $Name（端口 $Port，序列号 $(Get-Serial $Port)）"
    if (Test-InstanceRunning $Port) {
        $procs = @(Get-EmuProcess $Port)
        Show-Row "进程" ("在跑（PID " + (($procs | ForEach-Object { $_.ProcessId }) -join ",") + "）")
        # 先取变量再拼字符串：双引号里再套 $() 与引号是 PowerShell 的经典坑
        $svcReady = Test-ServiceReady $Port
        $svcText = if ($svcReady) { "服务已就绪" } else { "服务未就绪" }
        Show-Row "服务状态" $svcText
        Show-Row "管理地址" ("http://127.0.0.1:" + (Get-ServiceHttpPort $Port))
        if (-not $svcReady) {
            # 服务不就绪时最有用的是日志尾巴：有一类错误模拟器只刷日志、不退出
            $logNow = Get-LogFile $Port
            $lastErr = Get-EmuLastError $logNow
            if ($lastErr) { Write-Host ("  {0,-16} {1}" -f "日志最后一条", $lastErr) -ForegroundColor Yellow }
            $fatal = Get-EmuLogFatal $logNow
            if ($fatal) { Write-Host ("  [!] " + $fatal) -ForegroundColor Yellow }
        }
    } else {
        Show-Row "进程" "没在跑"
    }
    Show-Row "工作目录" $sysdir
    Show-Row "日志" (Get-LogFile $Port)
    $emuHome = Get-EmulatorHome
    if ($emuHome -and (Test-Path -LiteralPath $emuHome)) {
        Show-Row "模拟器主目录" $emuHome
    } else {
        Write-Host ("  {0,-16} {1}" -f "模拟器主目录", "$emuHome（不存在！模拟器会报 error: 3 并无限重试刷日志）") -ForegroundColor Yellow
    }
    $hw = Join-Path $sysdir "hardware-qemu.ini"
    if (Test-Path $hw) {
        $vals = @(Select-String -Path $hw -Pattern '^(hw\.ramSize|hw\.cpu\.ncore|hw\.lcd\.width|hw\.lcd\.height)\s*=' |
                  ForEach-Object { $_.Line.Trim() })
        if ($vals.Count -gt 0) { Show-Row "上次生效的硬件" ($vals -join "  ") }
    }
    $snap = Join-Path $sysdir "snapshots"
    if (Test-Path $snap) {
        Show-Row "快照" ((Get-ChildItem $snap -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }) -join " ")
    }
    Write-Host ""
}

if ($Port -le 0) {
    Show-Head "全部实例"
    $names = @(Get-InstanceNames)
    if ($names.Count -eq 0) { Write-Host "  （还没有实例；.\bin\start-headless.ps1 建一台）" }
    foreach ($n in $names) {
        $p = Get-InstancePort $n
        $state = if (Test-InstanceRunning $p) { "在跑（$(Get-BootState $p)）" } else { "没在跑" }
        Write-Host ("  {0,-12} 端口 {1,-6} {2}" -f $n, $p, $state)
    }
    Write-Host ""
}

Show-Head "这个包里的 ROM"
$man = Join-Path $script:Images "MANIFEST.txt"
if (Test-Path $man) {
    Select-String -Path $man -Pattern '^(ro\.system\.build\.fingerprint|system\.img sha256)\s*=' |
        ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
} else {
    Write-Host "  （images\MANIFEST.txt 不在，无法显示指纹）"
}
$rel = Join-Path $script:Root "RELEASE.json"
if (Test-Path $rel) {
    try {
        $j = Get-Content $rel -Raw | ConvertFrom-Json
        Show-Row "发布版本" ("$($j.version) / $($j.platform)")
        Show-Row "运行时" ("$($j.runtime.package)（$($j.runtime.version) build $($j.runtime.buildId)）")
        Show-Row "无头后端" $j.runtime.backend
    } catch { Write-Host "  （RELEASE.json 解析失败）" }
}

Show-Head "本机模板（templates\config.ini）"
Show-Row "hw.lcd.width/height" ("$(Get-ConfigValue 'hw.lcd.width' '?')/$(Get-ConfigValue 'hw.lcd.height' '?')")
Show-Row "hw.lcd.density" (Get-ConfigValue "hw.lcd.density" "?")
Show-Row "hw.ramSize / ncore" ("$(Get-ConfigValue 'hw.ramSize' '?') MB / $(Get-ConfigValue 'hw.cpu.ncore' '?')")
Show-Row "disk.dataPartition" (Get-ConfigValue "disk.dataPartition.size" "?")
$gpuWant = Get-ConfigValue "hw.gpu.mode" "auto"
Show-Row "hw.gpu.mode" ("$gpuWant（自适应 → $(Resolve-GpuMode $gpuWant)）")
