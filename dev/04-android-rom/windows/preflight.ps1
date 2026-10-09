<#
.SYNOPSIS
  Windows 侧跑 ROM 之前的前置检查（一次跑完，缺什么直说）。

.DESCRIPTION
  检查项：虚拟化是否可用（WHPX 的前提）、WHPX/虚拟机监控程序平台是否启用、
  磁盘空间、镜像文件是否齐全、与 Linux 侧交付清单是否一致。
  任何一项不过，run-windows.ps1 都会以更难懂的方式失败——所以先跑这个。
#>
[CmdletBinding()]
param(
    [string]$ImagesDir = "",
    [int]$NeedGB = 80
)

# ⚠️ **不要**把 `$PSScriptRoot` 写在 param 的默认值里。
#    带 `[CmdletBinding()]` 时参数绑定发生在 `$PSScriptRoot` 赋值**之前**，
#    默认值会算成 `\images` 这种丢掉了盘符和目录的路径，而且**不报错**。
#    2026-10-09 实测（同一台机器，只改这一处）：
#        param([string]$X = "$PSScriptRoot\images")            → [\images]   ✗
#        [CmdletBinding()] param([string]$X = "$PSScriptRoot\images") → [\images] ✗
#    于是镜像明明就在 `windows\images\` 里，预检却逐条报"缺 xxx.img"。
#    正确写法：默认值留空，绑定之后再补：
if (-not $ImagesDir) { $ImagesDir = Join-Path $PSScriptRoot "images" }

$ErrorActionPreference = "Continue"
$bad = 0
function Ok($m)   { Write-Host "  [OK] $m" -ForegroundColor Green }
function Warn2($m){ Write-Host "  [!!] $m" -ForegroundColor Yellow; $script:bad++ }
function Info($m) { Write-Host "  [--] $m" -ForegroundColor DarkGray }

Write-Host "==> 1. 虚拟化 / WHPX" -ForegroundColor Cyan
$cs = Get-CimInstance Win32_ComputerSystem
if ($cs.HypervisorPresent) { Ok "HypervisorPresent=True（Hyper-V/WHPX 已在运行或系统已启用虚拟化）" }
else { Warn2 "HypervisorPresent=False：没有任何 hypervisor 在跑。'WHPX 不可用' 与 'AEHD 未安装' 会同时成立，模拟器直接起不来（不是 warning，是硬失败）——到「启用或关闭 Windows 功能」勾选「Hyper-V」并重启（Hyper-V 开着就够；「Windows 虚拟机监控程序平台」可以一起勾，但**不要拿它的状态当判据**），或关掉 Hyper-V 后装 AEHD" }

# 模拟器主目录：缺了它模拟器会报 "Unexpected error while creating ... (error: 3)"
# 并且**不退出**、只是无限重试刷日志（2026-10-05 实测踩过）。
$emuHome = if ($env:ANDROID_EMULATOR_HOME) { $env:ANDROID_EMULATOR_HOME }
           elseif ($env:ANDROID_PREFS_ROOT) { Join-Path $env:ANDROID_PREFS_ROOT ".android" }
           elseif ($env:USERPROFILE) { Join-Path $env:USERPROFILE ".android" }
           elseif ($env:HOME) { Join-Path $env:HOME ".android" }
           else { "" }
if (-not $emuHome) {
    Warn2 "定位不到模拟器主目录（USERPROFILE / HOME 都没设）——用 `$env:ANDROID_EMULATOR_HOME 显式指定一个"
} else {
    if (-not (Test-Path -LiteralPath $emuHome)) {
        Info "模拟器主目录不存在，先建一个：$emuHome"
        New-Item -ItemType Directory -Force -Path $emuHome -ErrorAction SilentlyContinue | Out-Null
    }
    if (Test-Path -LiteralPath $emuHome) { Ok "模拟器主目录存在：$emuHome" }
    else { Warn2 "模拟器主目录不存在且建不出来：$emuHome（模拟器会报 error: 3 并无限重试刷日志）" }
}

$vm = Get-CimInstance Win32_Processor | Select-Object -First 1
if ($vm.VirtualizationFirmwareEnabled) { Ok "CPU 虚拟化已在固件层启用" }
else { Warn2 "CPU 虚拟化未启用（BIOS/UEFI 里打开 VT-x / AMD-V）" }

# ── 硬件加速到底能不能用 ────────────────────────────────────────────────
#
# ⚠️ **不要拿 `Get-WindowsOptionalFeature HypervisorPlatform` 的状态当判据。**
#
#    2026-10-09 在真机上实测到的反例：
#
#        Microsoft-Hyper-V-All        Enabled
#        HypervisorPlatform           **Disabled**
#        HypervisorPresent            True
#        emulator -accel-check        **WHPX(10.0.19044) is installed and usable.**
#
#    也就是说：**Hyper-V 开着就够用**，`HypervisorPlatform` 那个勾显示 Disabled
#    并不代表加速不可用。旧版本的这里把 Disabled 当成"硬项"，于是把一台**本来
#    就能跑**的机器判成跑不了，还让人去勾一个不必勾的开关。
#    （模拟器自己的文档也只要求"有 hypervisor 在跑 + WHPX API 可用"。）
#
#    判据按可靠性排序，从上往下取第一个能给出结论的：
#      1. 模拟器自己的 `-accel-check` —— 最权威，它说 usable 就是能用
#      2. `HypervisorPresent` —— 有 hypervisor 在跑
#      3. 功能状态 —— Hyper-V（任意一种）或 HypervisorPlatform 任一 Enabled 即可
$accelChecked = $false
$emuExe = Join-Path $PSScriptRoot "sdk\emulator\emulator.exe"
if (Test-Path -LiteralPath $emuExe) {
    $accelOut = & $emuExe -accel-check 2>&1 | Out-String
    if ($accelOut -match "is installed and usable") {
        Ok ("模拟器自检：硬件加速可用（" + ($accelOut -replace "\s+", " ").Trim() + "）")
        $accelChecked = $true
    } elseif ($accelOut -match "not installed|not usable|Unable") {
        Warn2 ("模拟器自检说硬件加速不可用：" + ($accelOut -replace "\s+", " ").Trim() +
               " —— 启用在「启用或关闭 Windows 功能」里的 Hyper-V **或**「Windows 虚拟机监控程序平台」，然后重启")
        $accelChecked = $true
    }
}
if (-not $accelChecked) {
    $hv = @("Microsoft-Hyper-V-All", "Microsoft-Hyper-V", "HypervisorPlatform") |
          ForEach-Object { Get-WindowsOptionalFeature -Online -FeatureName $_ -ErrorAction SilentlyContinue } |
          Where-Object { $_ -and $_.State -eq "Enabled" }
    if ($hv) {
        Ok ("Hyper-V / 虚拟机监控程序平台已启用（" + ($hv.FeatureName -join ", ") + "）")
    } elseif ($cs.HypervisorPresent) {
        Info "功能状态读不到，但 HypervisorPresent=True（有 hypervisor 在跑）——多半够用，跑一次 run-windows.ps1 便知"
    } else {
        Warn2 "没有任何 hypervisor 在跑，且 Hyper-V / 虚拟机监控程序平台都没启用 —— 模拟器起不来。到「启用或关闭 Windows 功能」勾选 Hyper-V（或「Windows 虚拟机监控程序平台」）并重启"
    }
}

Write-Host "==> 2. 磁盘空间" -ForegroundColor Cyan
$drive = (Resolve-Path $ImagesDir -ErrorAction SilentlyContinue).Drive.Name
if (-not $drive) { $drive = (Get-Item $PSScriptRoot).PSDrive.Name }
$free = (Get-PSDrive $drive).Free / 1GB
if ($free -ge $NeedGB) { Ok ("{0}: 可用 {1:N1} GB" -f $drive, $free) }
else { Warn2 ("{0}: 可用仅 {1:N1} GB，建议 ≥ {2} GB" -f $drive, $free, $NeedGB) }

Write-Host "==> 3. 镜像文件" -ForegroundColor Cyan
# -qemu 家族是必需的：GPT 包装 + 合并 ramdisk，缺了 first-stage 起不来
$required = @("system-qemu.img","vendor-qemu.img","product-qemu.img","ramdisk-qemu.img",
              "kernel-ranchu","encryptionkey.img","userdata.img",
              "advancedFeatures.ini","config.ini","system\build.prop")
$missing = @()
foreach ($f in $required) {
    $p = Join-Path $ImagesDir $f
    if (Test-Path $p) { Ok ("{0,-24} {1:N1} MB" -f $f, ((Get-Item $p).Length/1MB)) }
    else { Warn2 "缺 $f"; $missing += $f }
}
if (-not (Test-Path (Join-Path $ImagesDir "initrd"))) {
    Info "没有 initrd：模拟器会自己从 ramdisk-qemu.img 生成（实测可接受）"
}

Write-Host "==> 4. 与 Linux 交付清单比对" -ForegroundColor Cyan
$sum = Join-Path $ImagesDir "SHA256SUMS"
if (Test-Path $sum) {
    $bad2 = 0; $n = 0
    foreach ($line in Get-Content $sum) {
        if ($line -notmatch '^([0-9a-f]{64})\s+\*?\./(.+)$') { continue }
        $want = $Matches[1]; $rel = $Matches[2] -replace '/', '\'
        $p = Join-Path $ImagesDir $rel
        if (-not (Test-Path $p)) { Warn2 "缺文件 $rel"; continue }
        $n++
        if ((Get-FileHash $p -Algorithm SHA256).Hash.ToLower() -ne $want) { Warn2 "$rel 校验不符"; $bad2++ }
    }
    if ($bad2 -eq 0 -and $n -gt 0) { Ok "SHA256SUMS 校验通过（$n 个文件）" }
} else { Info "没有 SHA256SUMS（非打包目录？）——跳过一致性校验" }

$man = Join-Path $ImagesDir "MANIFEST.txt"
if (Test-Path $man) {
    Write-Host "==> 交付清单里的指纹（与 Linux 侧对照）" -ForegroundColor Cyan
    Select-String -Path $man -Pattern "fingerprint|sha256" | ForEach-Object { Info $_.Line.Trim() }
}

Write-Host ""
if ($bad -eq 0) { Write-Host "==> 前置检查通过 ✓  下一步： .\run-windows.ps1" -ForegroundColor Green }
else { Write-Host "==> 有 $bad 项需要处理（见上）" -ForegroundColor Yellow; exit 1 }
