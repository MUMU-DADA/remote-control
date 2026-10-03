<#
.SYNOPSIS
  从 Linux 构建机把自编 ROM 的镜像拉到 Windows，并校验完整性。

.DESCRIPTION
  默认从**打包目录**取（dev/04-android-rom/artifacts/rom-remote_control_x64_arm64/）——它由
  scripts/package-rom.sh 生成，文件集固定、带 SHA256SUMS 与 MANIFEST.txt。
  也可以 -RemoteDir 指到构建产物目录（out/target/product/remote_control_x64_arm64）。

  同名同大小默认跳过，可反复执行。
#>
[CmdletBinding()]
param(
    [string]$Remote    = "root@192.168.0.108",
    [string]$RemoteDir = "/root/AutoSnapshotAndroid/dev/04-android-rom/artifacts/rom-remote_control_x64_arm64",
    [string]$Dest      = "$PSScriptRoot\images",
    [switch]$Force,
    [switch]$SkipVerify
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# 打包目录里已有 system\build.prop；若指向构建产物目录，则 system\ 下文件更多，一并带上
# ⚠️ 必需的是 **-qemu 家族**：
#   system/vendor/product-qemu.img = 带 GPT 分区表的包装版（模拟器靠它做 /dev/block/by-name/*）
#   ramdisk-qemu.img = (cat ramdisk.img vendor_ramdisk.img) 合并版（first-stage 的 fstab 在里面）
# 只拉裸 ext4 的 *.img，模拟器会以 "partition(s) not found in /sys" / "failed to find device
# default fstab" 卡死并无限重启（本地实测，见 docs/02-build-traps.md 坑 8）。
$required = @("system-qemu.img", "vendor-qemu.img", "product-qemu.img", "ramdisk-qemu.img",
              "kernel-ranchu", "encryptionkey.img", "userdata.img",
              "advancedFeatures.ini", "config.ini")
$optional = @("system_ext-qemu.img", "initrd", "system.img", "vendor.img", "product.img",
              "ramdisk.img", "build.prop", "source.properties",
              "dtb.img", "vbmeta.img", "SHA256SUMS", "MANIFEST.txt")

function Remote-Size([string]$rel) {
    (ssh $Remote "stat -c %s '$RemoteDir/$rel' 2>/dev/null") -join ""
}

Write-Host "==> 远端：$Remote`:$RemoteDir" -ForegroundColor Cyan
$todo = @(); $missingRequired = @()
foreach ($f in ($required + $optional)) {
    $local = Join-Path $Dest $f
    $rsize = (Remote-Size $f).Trim()
    if (-not $rsize) {
        if ($required -contains $f) { $missingRequired += $f }
        continue
    }
    if ((Test-Path $local) -and -not $Force -and ([int64]$rsize -eq (Get-Item $local).Length)) {
        Write-Host ("    skip {0} ({1:N0} bytes)" -f $f, (Get-Item $local).Length)
        continue
    }
    $todo += "$RemoteDir/$f"
}
if ($missingRequired.Count -gt 0) {
    throw ("远端缺必需镜像：{0}（先跑构建与打包： scripts/build-rom.sh; scripts/package-rom.sh）" -f ($missingRequired -join ', '))
}
if ($todo.Count -gt 0) {
    Write-Host ("==> 拉取 {0} 个文件（可能几 GB，耐心等）" -f $todo.Count) -ForegroundColor Cyan
    & scp @($todo + $Dest)
    if ($LASTEXITCODE -ne 0) { throw "scp 失败（$LASTEXITCODE）" }
}

# system\build.prop：模拟器靠它识别 guest 架构，缺了会退回宿主架构
New-Item -ItemType Directory -Force -Path "$Dest\system" | Out-Null
if (-not (Test-Path "$Dest\system\build.prop")) {
    Write-Host "==> 补 system\build.prop" -ForegroundColor Cyan
    & scp "$Remote`:$RemoteDir/system/build.prop" "$Dest\system\build.prop"
}

# initrd：模拟器 -initrd 指向它
if ((-not (Test-Path "$Dest\initrd")) -and (Test-Path "$Dest\ramdisk.img")) {
    Copy-Item "$Dest\ramdisk.img" "$Dest\initrd" -Force
    Write-Host "==> 已由 ramdisk.img 生成 initrd"
}

# 自建 arm64 探针 APK（Linux 侧 tools/build-probe-apk.sh 产出）
$probeRemote = "/root/AutoSnapshotAndroid/dev/04-android-rom/artifacts/arm64-probe.apk"
$probeLocal  = Join-Path $Dest "arm64-probe.apk"
if ((Remote-Size "arm64-probe.apk") -or ((ssh $Remote "stat -c %s '$probeRemote' 2>/dev/null") -join "").Trim()) {
    $src = if ((Remote-Size "arm64-probe.apk").Trim()) { "$RemoteDir/arm64-probe.apk" } else { $probeRemote }
    if ((-not (Test-Path $probeLocal)) -or $Force) {
        Write-Host "==> 拉取自建 arm64 探针 APK"
        & scp "$Remote`:$src" "$Dest"
    }
}

# ---------------------------------------------------------------- 校验
if (-not $SkipVerify -and (Test-Path "$Dest\SHA256SUMS")) {
    Write-Host "==> 按 SHA256SUMS 校验" -ForegroundColor Cyan
    $bad = 0; $checked = 0
    foreach ($line in Get-Content "$Dest\SHA256SUMS") {
        if ($line -notmatch '^([0-9a-f]{64})\s+\*?\./(.+)$') { continue }
        $want = $Matches[1]; $rel = $Matches[2] -replace '/', '\'
        $p = Join-Path $Dest $rel
        if (-not (Test-Path $p)) { Write-Warning "缺文件：$rel"; $bad++; continue }
        $got = (Get-FileHash $p -Algorithm SHA256).Hash.ToLower()
        $checked++
        if ($got -ne $want) { Write-Host ("    [!!] {0} 校验不符" -f $rel) -ForegroundColor Red; $bad++ }
    }
    if ($bad -eq 0) { Write-Host ("==> 校验通过 ✓（{0} 个文件）" -f $checked) -ForegroundColor Green }
    else { throw ("校验失败：{0} 个文件有问题" -f $bad) }
}

Write-Host "==> 完成。清单：" -ForegroundColor Green
Get-ChildItem $Dest -File | Sort-Object Name |
    Format-Table Name, @{N='MB';E={[math]::Round($_.Length/1MB,1)}} -AutoSize

$missing = $required | Where-Object { -not (Test-Path (Join-Path $Dest $_)) }
if ($missing) { Write-Warning ("缺少：{0}" -f ($missing -join ', ')) }
else { Write-Host "==> 必需文件齐全 ✓" -ForegroundColor Green }
