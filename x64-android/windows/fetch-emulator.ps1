<#
.SYNOPSIS
  拿到 emulator.exe（SDK 稳定版）+ adb（platform-tools）。

.DESCRIPTION
  顺序：本机已有 SDK → 指定 zip → 腾讯镜像下载。
  Windows 侧要的是 **x86_64 guest**，所以任意现代版本都可以（不用像 arm64 那样钉旧版）。
#>
[CmdletBinding()]
param(
    [string]$Dest = "$PSScriptRoot\sdk",
    [string]$Channel = "stable",
    [string]$RepoBase = "https://mirrors.cloud.tencent.com/AndroidSDK",
    [string]$EmulatorZipUrl = "",
    [string]$Proxy = "",
    [switch]$SkipPlatformTools,
    [switch]$Force,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

function Get-WithMirror([string]$rel, [string]$out) {
    $base = if ($Proxy) { $RepoBase } else { $RepoBase }
    if ($Proxy) {
        Write-Host "==> 经代理下载 $rel"
        & curl.exe -x $Proxy -fL --retry 2 -o $out "$base/$rel"
    } else {
        & curl.exe -fL --retry 2 -o $out "$base/$rel"
    }
    if ($LASTEXITCODE -ne 0) { throw "下载失败：$base/$rel" }
}

# 1) 本机已有 SDK？
$cands = @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME, "$env:LOCALAPPDATA\Android\Sdk") |
         Where-Object { $_ -and (Test-Path "$_\emulator\emulator.exe") }
if ($cands -and -not $Force) {
    Write-Host "==> 命中已有 SDK：$($cands[0])" -ForegroundColor Green
    exit 0
}

# 2) 解析仓库清单，拿 emulator zip 名
$emuDir = "$Dest\emulator"
$emuExe = "$emuDir\emulator.exe"
if ((Test-Path $emuExe) -and -not $Force) {
    Write-Host "==> 已有 emulator：$emuExe" -ForegroundColor Green
} else {
    if ($EmulatorZipUrl) {
        $zip = "$Dest\emulator.zip"
        if ($EmulatorZipUrl -match '^https?://') { Get-WithMirror $EmulatorZipUrl $zip }
        else { Copy-Item $EmulatorZipUrl $zip -Force }
    } else {
        $xml = "$Dest\repository2-3.xml"
        Write-Host "==> 取仓库清单（$Channel 渠道）"
        Get-WithMirror "repository2-3.xml" $xml
        $doc = [xml](Get-Content $xml -Raw)
        $pkg = $doc.sdkRepository.remotePackage |
               Where-Object { $_.path -eq "emulator" -and $_.channelRef.ref -eq $Channel } |
               Select-Object -First 1
        if (-not $pkg) { throw "清单里没有 $Channel 渠道的 emulator" }
        $url = $pkg.archives.archive.url
        $zip = "$Dest\$([IO.Path]::GetFileName($url))"
        if ($DryRun) { Write-Host "==> DryRun: 将下载 $url → $zip"; exit 0 }
        Write-Host "==> 下载 $url"
        Get-WithMirror $url $zip
    }
    if ($DryRun) { exit 0 }
    Write-Host "==> 解包"
    if (Test-Path $emuDir) { Remove-Item $emuDir -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $Dest -Force
    if (-not (Test-Path $emuExe)) { throw "解包后没有 emulator.exe" }
}

# 3) platform-tools（adb）
if (-not $SkipPlatformTools) {
    if (-not (Test-Path "$Dest\platform-tools\adb.exe")) {
        $xml = "$Dest\repository2-3.xml"
        if (-not (Test-Path $xml)) { Get-WithMirror "repository2-3.xml" $xml }
        $doc = [xml](Get-Content $xml -Raw)
        $pt = $doc.sdkRepository.remotePackage | Where-Object { $_.path -eq "platform-tools" } | Select-Object -First 1
        $zip = "$Dest\platform-tools.zip"
        Write-Host "==> 下载 platform-tools"
        Get-WithMirror $pt.archives.archive.url $zip
        Expand-Archive -Path $zip -DestinationPath $Dest -Force
    }
}

Write-Host "==> 完成：$emuExe" -ForegroundColor Green
