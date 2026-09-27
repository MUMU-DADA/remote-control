<#
.SYNOPSIS
    从开发机拉取 arm64 镜像到 Windows，供 run-emulator.ps1 使用。

.DESCRIPTION
    用 OpenSSH（Windows 10 1809+ 自带 ssh.exe / scp.exe）从开发机拉
    out/target/product/emulator64_arm64/ 下的顶层镜像文件：
      system.img vendor.img ramdisk.img kernel-ranchu … 以及 product/system_ext/super 等
    同名同大小的文件默认跳过（可重复执行），拉完做一次 md5 比对。

    认证：优先免密（把本机公钥加到开发机 root 的 authorized_keys）。
    只有密码认证时，脚本会把所有文件放进**一次** scp 调用，所以只提示一次密码。

.EXAMPLE
    .\fetch-images.ps1
    .\fetch-images.ps1 -Remote root@192.168.0.108 -Force
    .\fetch-images.ps1 -SkipVerify
#>
[CmdletBinding()]
param(
    [string]$Remote = 'root@192.168.0.108',

    # 开发机上的产物目录（注意是 emulator64_arm64，不是 generic_arm64）
    [string]$RemoteDir = '/root/AutoSnapshotAndroid/aosp/out/target/product/emulator64_arm64',

    [string]$Dest = (Join-Path $PSScriptRoot 'images'),
    [int]$Port = 22,
    [switch]$Force,
    [switch]$SkipVerify
)

. "$PSScriptRoot\_common.ps1"

foreach ($tool in @('ssh.exe', 'scp.exe')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Stop-Script "$tool 不存在。Windows 10 1809+ 自带 OpenSSH 客户端；`n    设置 → 应用 → 可选功能 → 添加功能 → OpenSSH 客户端"
    }
}

# 非交互场景别卡在 host key 确认上
$sshOpts = @('-o', 'StrictHostKeyChecking=accept-new', '-o', 'ConnectTimeout=15', '-p', "$Port")

function Invoke-Remote {
    param([string]$Command)
    $out = & ssh.exe @sshOpts $Remote $Command 2>&1
    if ($LASTEXITCODE -ne 0) {
        return $null
    }
    return $out
}

# ---------------------------------------------------------------------------
Write-Step "列出开发机上的镜像： $Remote`:$RemoteDir"

$listing = Invoke-Remote "ls -1 $RemoteDir"
if (-not $listing) {
    Stop-Script @"
连不上或目录不存在： $Remote`:$RemoteDir

排查：
  1. 开发机上先编出来： cd dev/04-emulator/linux-arm64 && ./build-images.sh
  2. 手动确认：        ssh $Remote "ls -1 $RemoteDir"
  3. 开发机地址/用户不同： .\fetch-images.ps1 -Remote root@<ip>
"@
}

# 模拟器启动需要的镜像/固件：只取顶层文件，跳过 obj/ symbols/ testcases/ 等目录
$keep = '^(system|vendor|product|system_ext|odm|ramdisk|userdata|cache|encryptionkey|super|super_empty|vbmeta.*)\.img$|^kernel-|\.ini$|^source\.properties$'
$files = @($listing | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_ -match $keep })

if ($files.Count -eq 0) {
    Stop-Script "目录里没有找到镜像文件（pattern: $keep）"
}
Write-Ok "候选文件 $($files.Count) 个"

# ---------------------------------------------------------------------------
Write-Step '读取远端文件大小（用于跳过已拉取的文件）'

$remoteSizes = @{}
$statOut = Invoke-Remote "cd $RemoteDir && stat -c '%n|%s' *"
if ($statOut) {
    foreach ($line in $statOut) {
        $parts = $line.Trim() -split '\|'
        if ($parts.Count -eq 2) { $remoteSizes[$parts[0]] = [long]$parts[1] }
    }
}

if (-not (Test-Path $Dest)) { New-Item -ItemType Directory -Path $Dest -Force | Out-Null }

$toGet = New-Object System.Collections.Generic.List[string]
foreach ($f in $files) {
    $local = Join-Path $Dest $f
    $rSize = 0
    if ($remoteSizes.ContainsKey($f)) { $rSize = $remoteSizes[$f] }

    if ((Test-Path $local) -and -not $Force) {
        $lSize = (Get-Item $local).Length
        if ($rSize -gt 0 -and $lSize -eq $rSize) {
            Write-Ok "$f 已存在且大小一致（$(Format-Size $lSize)），跳过"
            continue
        }
        Write-Note "$f 大小不一致（本地 $(Format-Size $lSize) / 远端 $(Format-Size $rSize)），重拉"
    }
    $toGet.Add($f)
}

if ($toGet.Count -eq 0) {
    Write-Ok '所有文件都是最新的，无需下载'
} else {
    Write-Step "拉取 $($toGet.Count) 个文件（一次 scp 调用，最多提示一次密码）"
    $sources = @($toGet | ForEach-Object { "${Remote}:${RemoteDir}/$_" })
    & scp.exe @sshOpts @sources $Dest
    if ($LASTEXITCODE -ne 0) {
        Stop-Script 'scp 失败。检查网络/认证，或用 -Force 重试'
    }
}

# ---------------------------------------------------------------------------
if (-not $SkipVerify) {
    Write-Step 'md5 校验'
    $names = @($files | Where-Object { Test-Path (Join-Path $Dest $_) })
    $md5Out = Invoke-Remote "cd $RemoteDir && md5sum $($names -join ' ')"
    if (-not $md5Out) {
        Write-Note '拿不到远端 md5，跳过校验'
    } else {
        $bad = 0
        foreach ($line in $md5Out) {
            $parts = ($line.Trim() -split '\s+')
            if ($parts.Count -lt 2) { continue }
            $remoteHash = $parts[0]; $name = $parts[-1]
            $local = Join-Path $Dest $name
            if (-not (Test-Path $local)) { continue }
            $localHash = (Get-FileHash -Path $local -Algorithm MD5).Hash.ToLower()
            if ($localHash -eq $remoteHash.ToLower()) {
                Write-Ok "$name  ✓"
            } else {
                Write-Fail "$name  md5 不一致（远端 $remoteHash / 本地 $localHash）"
                $bad++
            }
        }
        if ($bad -gt 0) { Stop-Script "$bad 个文件校验失败，删掉后加 -Force 重拉" }
    }
}

# ---------------------------------------------------------------------------
Write-Step '自检'

$required = @('system.img', 'vendor.img', 'ramdisk.img', 'kernel-ranchu')
$missing = @($required | Where-Object { -not (Test-Path (Join-Path $Dest $_)) })
foreach ($f in $required) {
    $p = Join-Path $Dest $f
    if (Test-Path $p) { Write-Ok "$f  $(Format-Size (Get-Item $p).Length)" } else { Write-Fail "缺 $f" }
}
foreach ($f in $files) {
    if ($required -contains $f) { continue }
    $p = Join-Path $Dest $f
    if (Test-Path $p) { Write-Note "$f  $(Format-Size (Get-Item $p).Length)" }
}

$total = (Get-ChildItem $Dest -File | Measure-Object -Property Length -Sum).Sum
Write-Host ''
Write-Host "  合计 $(Format-Size $total)  →  $Dest" -ForegroundColor Cyan

if ($missing.Count -gt 0) {
    Stop-Script "缺必需文件：$($missing -join ', ')  —— 开发机上重新 ./build-images.sh"
}

Write-Host ''
Write-Host '完成。下一步：' -ForegroundColor Cyan
Write-Host "  .\run-emulator.ps1"
