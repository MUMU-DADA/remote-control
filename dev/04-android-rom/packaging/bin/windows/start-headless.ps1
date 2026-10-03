<#
.SYNOPSIS
  无头启动这台虚拟机（Windows / WHPX 侧）。

.DESCRIPTION
  与 Linux 侧 bin/linux/start-headless.sh 同一套语义、同一份 templates\config.ini：
  硬件参数全部来自模板，命令行只在**显式传参**时覆盖
  （命令行优先于 config.ini，随手写死默认值会让模板失效）。

.EXAMPLE
  .\bin\start-headless.ps1                     # default 实例，端口 5580，全新冷启动
  .\bin\start-headless.ps1 -Reuse              # 保留上次的数据、已装应用、快照
  .\bin\start-headless.ps1 -Name vm2 -Port 5584
  .\bin\start-headless.ps1 -Gpu swiftshader_indirect
  .\bin\start-headless.ps1 -Gui                # 带窗口（调试用）
  .\bin\start-headless.ps1 -NoWait             # 起了就返回
#>
[CmdletBinding()]
param(
    [string]$Name = "",
    [int]$Port = 0,
    [string]$Gpu = "",
    [int]$Memory = 0,
    [int]$Cores = 0,
    [int]$TimeoutSec = 300,
    [switch]$Reuse,
    [switch]$Gui,
    [switch]$NoWait,
    [switch]$WipeData,
    [switch]$NoAccel
)

. (Join-Path $PSScriptRoot "common.ps1")

if ([string]::IsNullOrWhiteSpace($Name)) { $Name = $script:DefaultName }

Assert-Emulator
Assert-Images
Assert-Adb

# ---------------------------------------------------------------------------
# 端口：显式 > 该实例已登记的 > 自动分配（偶数，console 口 = port+1）
# ---------------------------------------------------------------------------
if ($Port -le 0) { $Port = Get-InstancePort $Name }
if ($Port -le 0) { $Port = New-FreePort }
if ($Port % 2 -ne 0) { Die "端口必须是偶数：$Port（模拟器拿 port+1 当 console 口）" }
$owners = @(Get-InstanceNamesForPort $Port)
if ($owners.Count -gt 0 -and ($owners.Count -ne 1 -or $owners[0] -ne $Name)) {
    $ownerText = $owners -join ", "
    Die "端口 $Port 已登记给实例 '$ownerText'；不能让 '$Name' 覆盖它（.\bin\status.ps1 查看实例）"
}
if (Test-InstanceRunning $Port) { Die "端口 $Port 已经有模拟器在跑（.\bin\status.ps1 看是谁）" }
$serial  = Get-Serial  $Port
$sysdir  = Get-SysDir  $Port
$datadir = Get-DataDir $Port
$logf    = Get-LogFile $Port
$errf    = Get-ErrFile $Port
$registeredPort = Get-InstancePort $Name
if ($registeredPort -ne $Port -and ((Test-Path $sysdir) -or (Test-Path $datadir))) {
    Die "端口 $Port 的工作目录已存在但不属于实例 '$Name'：$sysdir / $datadir；拒绝覆盖未登记数据"
}
Register-Instance $Name $Port

# ---------------------------------------------------------------------------
# 前置自检
# ---------------------------------------------------------------------------
$accel = if ($NoAccel) { "off" } else { "on" }
if ($NoAccel) { Write-Warn "关了加速（-accel off）走纯软件模拟 —— 开机可能要几分钟到几十分钟" }
else {
    try {
        $cs = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop
        if (-not $cs.HypervisorPresent) {
            Write-Warn "HypervisorPresent=False：WHPX 可能没开。模拟器会报 'WHPX is not installed'。"
            Write-Warn "  修法：启用或关闭 Windows 功能 → 勾选「Windows 虚拟机监控程序平台」→ 重启"
        }
    } catch { Write-Warn "查不到虚拟化状态（非 Windows 或权限不足），跳过这项自检" }
}

$drive = (Get-Item $script:RunDir -ErrorAction SilentlyContinue)
if (-not $drive) { New-Item -ItemType Directory -Force -Path $script:RunDir | Out-Null; $drive = Get-Item $script:RunDir }
$freeGB = (Get-PSDrive $drive.PSDrive.Name).Free / 1GB
if ($freeGB -lt 8) { Write-Warn ("工作目录所在盘只剩 {0:N1} GB —— 数据分区是 qcow2 覆盖层、会随用量增长" -f $freeGB) }

# ---------------------------------------------------------------------------
# 工作目录
# ---------------------------------------------------------------------------
if ($Reuse -and (Test-Path $sysdir)) {
    Write-Log "复用工作目录：$sysdir（保留已装应用、数据、快照）"
    Build-SysDir $Port -Keep
} else {
    Build-SysDir $Port
    Remove-Item $datadir -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path $datadir | Out-Null

# ---------------------------------------------------------------------------
# 硬件参数 + 启动
# ---------------------------------------------------------------------------
$gpuWant = if ($Gpu) { $Gpu } else { Get-ConfigValue "hw.gpu.mode" "auto" }
$gpuAuto = (-not $Gpu)
$gpuMode = Resolve-GpuMode $gpuWant
$memMB   = if ($Memory -gt 0) { $Memory } else { [int](Get-ConfigValue "hw.ramSize" "6144") }
$coreN   = if ($Cores  -gt 0) { $Cores }  else { [int](Get-ConfigValue "hw.cpu.ncore" "4") }

# GPU 候选：自适应选的那档起不来就退软件渲染（「有显卡 ≠ 驱动能用」）
$gpuCands = @($gpuMode)
if ($gpuAuto -and $gpuMode -eq "host") { $gpuCands += "swiftshader_indirect" }

function Start-EmuOnce {
    param([string]$GpuMode)
    # 模拟器要 ANDROID_PRODUCT_OUT 才认 -sysdir；少了 ANDROID_BUILD_TOP
    # SDK 版会去找 'kernel-qemu' 并报 "Your system directory is missing the 'kernel-qemu' image file"
    $env:ANDROID_PRODUCT_OUT = $sysdir
    $env:ANDROID_BUILD_TOP   = $script:Root

    # ⚠️ Start-Process 的 -ArgumentList 只是把数组用空格拼起来，**不会**加引号。
    #    路径里有空格（"C:\Program Files\..."、用户名带空格）就会散架，
    #    所以这里自己给路径参数套引号。
    $a = @("-sysdir", "`"$sysdir`"", "-datadir", "`"$datadir`"", "-port", $Port,
           "-gpu", $GpuMode, "-accel", $accel,
           "-memory", "$memMB", "-cores", "$coreN",
           "-no-boot-anim", "-no-audio", "-no-snapshot")
    if (-not $Gui)      { $a += "-no-window" }
    if ($WipeData)      { $a += "-wipe-data" }

    Remove-Item $logf, $errf -Force -ErrorAction SilentlyContinue
    $script:EmuProc = Start-Process -FilePath $script:Emulator -ArgumentList $a -PassThru `
                                    -WindowStyle Hidden `
                                    -RedirectStandardOutput $logf -RedirectStandardError $errf
    Write-Host "    PID $($script:EmuProc.Id)"
}

$started = $false; $usedGpu = ""
foreach ($g in $gpuCands) {
    if ($g -ne $gpuMode) { Write-Warn "上一档（$gpuMode）没起来，退到 $g" }
    Write-Log "启动：-gpu $g -memory $memMB -cores $coreN -accel $accel 端口 $Port"
    Start-EmuOnce $g
    if (Wait-Adb $serial 60) { $started = $true; $usedGpu = $g; break }
    $tail = (Get-Content $errf -Tail 2 -ErrorAction SilentlyContinue) -join " "
    Write-Warn "没起来（$tail）"
    Stop-Process -Id $script:EmuProc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}
if (-not $started) { Die "起不来。日志：$logf / $errf" }

Write-Ok "已启动：$serial（-gpu $usedGpu -memory $memMB -cores $coreN）"
if ($gpuAuto) { Write-Log "GPU 自适应依据：$(Get-GpuReason)" }

if ($NoWait) {
    Write-Host ""
    Write-Host "后续： .\bin\status.ps1 -Port $Port    .\bin\verify.ps1 -Port $Port    .\bin\stop.ps1 -Port $Port"
    exit 0
}

Write-Log "等开机完成（WHPX 下通常几十秒）"
if (-not (Wait-Boot $serial $TimeoutSec)) { Die "等开机超时（${TimeoutSec}s）—— 看 $logf / $errf" }
& $script:Adb -s $serial root 2>$null | Out-Null
& $script:Adb -s $serial wait-for-device 2>$null | Out-Null

$bt = (Select-String -Path $logf -Pattern '(boot time|Boot completed in) \d+ ms' -ErrorAction SilentlyContinue |
       Select-Object -Last 1).Line
# ⚠️ 别把 "$( ... 里面再套 "" ...)" 写进双引号字符串：PowerShell 词法分析会当场崩
#    （run-windows.ps1 踩过，那个脚本因此从来没能运行过）。先取到变量再拼。
$btMsg = ""
if ($bt) { $btMsg = "（" + $bt.Trim() + "）" }
Write-Ok "开机完成$btMsg"
Write-Log "设备序列号：$serial"
Write-Log "工作目录：  $sysdir（镜像在 $($script:Images)，只读；状态全在这里）"
Write-Log "日志：      $logf"
Write-Host ""
Write-Host "下一步： .\bin\verify.ps1 -Port $Port     .\bin\stop.ps1 -Port $Port"
