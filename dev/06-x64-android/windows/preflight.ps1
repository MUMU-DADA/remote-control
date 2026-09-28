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
    [string]$ImagesDir = "$PSScriptRoot\images",
    [int]$NeedGB = 20
)

$ErrorActionPreference = "Continue"
$bad = 0
function Ok($m)   { Write-Host "  [OK] $m" -ForegroundColor Green }
function Warn2($m){ Write-Host "  [!!] $m" -ForegroundColor Yellow; $script:bad++ }
function Info($m) { Write-Host "  [--] $m" -ForegroundColor DarkGray }

Write-Host "==> 1. 虚拟化 / WHPX" -ForegroundColor Cyan
$cs = Get-CimInstance Win32_ComputerSystem
if ($cs.HypervisorPresent) { Ok "HypervisorPresent=True（Hyper-V/WHPX 已在运行或系统已启用虚拟化）" }
else { Info "HypervisorPresent=False：可能是 BIOS 未开虚拟化，或未启用任何 hypervisor" }

$vm = Get-CimInstance Win32_Processor | Select-Object -First 1
if ($vm.VirtualizationFirmwareEnabled) { Ok "CPU 虚拟化已在固件层启用" }
else { Warn2 "CPU 虚拟化未启用（BIOS/UEFI 里打开 VT-x / AMD-V）" }

# WHPX 功能状态（可选功能名：HypervisorPlatform）
$feat = Get-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -ErrorAction SilentlyContinue
if ($feat) {
    if ($feat.State -eq "Enabled") { Ok "Windows Hypervisor Platform 已启用" }
    else { Warn2 "Windows Hypervisor Platform 未启用：启用或关闭 Windows 功能 → 勾选它，然后重启" }
} else {
    Info "查不到 HypervisorPlatform 功能状态（家庭版可能没有该功能名）——若模拟器报 WHPX 不可用，改用 -accel off 或启用 Hyper-V"
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
