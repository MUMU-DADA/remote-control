<#
.SYNOPSIS
  从 Linux 构建机把自编 ROM 的镜像拉到 Windows。

.DESCRIPTION
  只拉启动必需的文件 + 构建模式需要的 system\build.prop。
  同名同大小默认跳过，可反复执行（断点续传式）。
#>
[CmdletBinding()]
param(
    [string]$Remote    = "root@192.168.0.108",
    [string]$RemoteDir = "/root/AutoSnapshotAndroid/aosp/out/target/product/autosnap_x64_arm64",
    [string]$Dest      = "$PSScriptRoot\images",
    [switch]$Force,
    [switch]$SkipVerify
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
New-Item -ItemType Directory -Force -Path "$Dest\system" | Out-Null

# 启动必需 + 构建模式必需；大文件（system.img 数 GB）走同一条 scp
$files = @(
    "system.img", "vendor.img", "product.img", "ramdisk.img",
    "kernel-ranchu", "encryptionkey.img", "userdata.img",
    "build.prop", "advancedFeatures.ini", "source.properties"
)

Write-Host "==> 远端：$Remote`:$RemoteDir" -ForegroundColor Cyan
$todo = @()
foreach ($f in $files) {
    $local = Join-Path $Dest $f
    if ((Test-Path $local) -and -not $Force) {
        # 远端大小与本地一致就跳过（省一次几 GB 的传输）
        $rsize = (ssh $Remote "stat -c %s '$RemoteDir/$f' 2>/dev/null")
        if ($rsize -and ([int]$rsize -eq (Get-Item $local).Length)) {
            Write-Host ("    skip {0} ({1:N0} bytes)" -f $f, (Get-Item $local).Length)
            continue
        }
    }
    $todo += "$RemoteDir/$f"
}

if ($todo.Count -gt 0) {
    Write-Host ("==> 拉取 {0} 个文件（可能几 GB，耐心等）" -f $todo.Count) -ForegroundColor Cyan
    & scp @($todo + $Dest)
    if ($LASTEXITCODE -ne 0) { throw "scp 失败（$LASTEXITCODE）" }
}

# system\build.prop：模拟器靠它识别 guest 架构，缺了会退回宿主架构
if (-not (Test-Path "$Dest\system\build.prop")) {
    Write-Host "==> 补 system\build.prop" -ForegroundColor Cyan
    & scp "$Remote`:$RemoteDir/system/build.prop" "$Dest\system\build.prop"
}

# initrd：模拟器 -initrd 指向它
if ((-not (Test-Path "$Dest\initrd")) -and (Test-Path "$Dest\ramdisk.img")) {
    Copy-Item "$Dest\ramdisk.img" "$Dest\initrd" -Force
    Write-Host "==> 已由 ramdisk.img 生成 initrd"
}

Write-Host "==> 完成。清单：" -ForegroundColor Green
Get-ChildItem $Dest -File | Sort-Object Name |
    Format-Table Name, @{N='MB';E={[math]::Round($_.Length/1MB,1)}} -AutoSize

if (-not $SkipVerify) {
    $missing = $files | Where-Object { -not (Test-Path (Join-Path $Dest $_)) }
    if ($missing) { Write-Warning ("缺少：{0}" -f ($missing -join ', ')) }
    else { Write-Host "==> 必需文件齐全 ✓" -ForegroundColor Green }
}
