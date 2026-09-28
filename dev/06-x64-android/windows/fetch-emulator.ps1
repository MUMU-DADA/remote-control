<#
.SYNOPSIS
  拿到 Windows 版 emulator.exe（SDK）+ adb（platform-tools）。

.DESCRIPTION
  顺序：本机已有 SDK → 指定 zip → 镜像/官方仓库下载。

  ⚠️ 两个容易踩的点（本脚本已处理）：
    1. 仓库清单里**每个包有按 host-os 分的多个 archive**（linux/windows/macosx），
       直接取第一个会下到 Linux 版；
    2. 渠道不是用名字写的：清单里是 `<channelRef ref="channel-0"/>`，
       名字在 `<channel id="channel-0">Stable</channel>` 里。
#>
[CmdletBinding()]
param(
    [string]$Dest = "$PSScriptRoot\sdk",
    [string]$Channel = "Stable",
    [string]$RepoBase = "https://mirrors.cloud.tencent.com/AndroidSDK",
    [string]$EmulatorZipUrl = "",
    [string]$Proxy = "",
    [switch]$SkipPlatformTools,
    [switch]$Force,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

function Get-Remote([string]$rel, [string]$out) {
    $base = $RepoBase
    $args = @("-fL", "--retry", "2", "-o", $out)
    if ($Proxy) { $args = @("-x", $Proxy) + $args }
    & curl.exe @args "$base/$rel"
    if ($LASTEXITCODE -ne 0) { throw "下载失败：$base/$rel" }
}

function Get-RepoXml([string]$path) {
    if (-not (Test-Path $path)) { Get-Remote "repository2-3.xml" $path }
    return [xml](Get-Content $path -Raw)
}

# 按 host-os 挑 archive（这就是"下到 Linux 版"的根因）
function Select-Archive($pkg, [string]$hostOs) {
    foreach ($a in $pkg.archives.archive) {
        $ho = $a.'host-os'
        if (-not $ho) { $ho = "any" }
        if ($ho -eq $hostOs -or $ho -eq "any") { return $a }
    }
    return $null
}

# channelRef.ref -> 渠道名
function Resolve-ChannelId($doc, [string]$name) {
    foreach ($c in $doc.sdkRepository.channel) {
        $text = ($c.'#text'); if (-not $text) { $text = $c.InnerText }
        if ($text -and $text.Trim().ToLower() -eq $name.ToLower()) { return $c.id }
    }
    return $null
}

# 1) 本机已有 SDK？
$cands = @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME, "$env:LOCALAPPDATA\Android\Sdk") |
         Where-Object { $_ -and (Test-Path "$_\emulator\emulator.exe") }
if ($cands -and -not $Force) {
    Write-Host "==> 命中已有 SDK：$($cands[0])" -ForegroundColor Green
    exit 0
}

$emuDir = "$Dest\emulator"
$emuExe = "$emuDir\emulator.exe"
if ((Test-Path $emuExe) -and -not $Force) {
    Write-Host "==> 已有 emulator：$emuExe" -ForegroundColor Green
    exit 0
}

$xmlPath = "$Dest\repository2-3.xml"
$doc = Get-RepoXml $xmlPath

$pkgs = @($doc.sdkRepository.remotePackage | Where-Object { $_.path -eq "emulator" })
if ($pkgs.Count -eq 0) { throw "清单里没有 emulator 包" }

$chanId = Resolve-ChannelId $doc $Channel
$pkg = if ($chanId) { $pkgs | Where-Object { $_.channelRef.ref -eq $chanId } | Select-Object -First 1 } else { $null }
if (-not $pkg) {
    Write-Warning "清单里没找到渠道 '$Channel'（可能渠道写法不同），退回第一个 emulator 包"
    $pkg = $pkgs | Select-Object -First 1
}

$arch = Select-Archive $pkg "windows"
if (-not $arch) { throw "该 emulator 包没有 windows 版 archive（清单里可能只有 linux/macosx）" }

Write-Host ("==> 渠道 {0} → {1}（{2:N0} MiB，sha1 {3}…）" -f `
    $Channel, $arch.url, ([int64]$arch.size / 1MB), $arch.checksum.Substring(0,12)) -ForegroundColor Cyan

if ($EmulatorZipUrl) {
    $zip = "$Dest\emulator.zip"
    if ($EmulatorZipUrl -match '^https?://') { Get-Remote $EmulatorZipUrl $zip } else { Copy-Item $EmulatorZipUrl $zip -Force }
} else {
    $zip = "$Dest\$([IO.Path]::GetFileName($arch.url))"
    if ($DryRun) { Write-Host "==> DryRun: 将下载 $($arch.url) → $zip"; exit 0 }
    if ((Test-Path $zip) -and -not $Force) {
        Write-Host "==> 已有 zip，跳过下载"
    } else {
        Get-Remote $arch.url $zip
    }
    # 校验 sha1（清单里给了就一定要对）
    if ($arch.checksum -and -not $DryRun) {
        $got = (Get-FileHash $zip -Algorithm SHA1).Hash.ToLower()
        if ($got -ne $arch.checksum.ToLower()) { throw "sha1 不匹配：期望 $($arch.checksum)，实得 $got" }
        Write-Host "==> sha1 校验通过 ✓" -ForegroundColor Green
    }
}
if ($DryRun) { exit 0 }

Write-Host "==> 解包"
if (Test-Path $emuDir) { Remove-Item $emuDir -Recurse -Force }
Expand-Archive -Path $zip -DestinationPath $Dest -Force
if (-not (Test-Path $emuExe)) { throw "解包后没有 emulator.exe" }

# platform-tools（adb）
if (-not $SkipPlatformTools -and -not (Test-Path "$Dest\platform-tools\adb.exe")) {
    $pt = @($doc.sdkRepository.remotePackage | Where-Object { $_.path -eq "platform-tools" })[0]
    $a = Select-Archive $pt "windows"
    if ($a) {
        Write-Host "==> 下载 platform-tools"
        Get-Remote $a.url "$Dest\platform-tools.zip"
        Expand-Archive -Path "$Dest\platform-tools.zip" -DestinationPath $Dest -Force
    } else { Write-Warning "platform-tools 没有 windows archive，跳过" }
}

Write-Host "==> 完成：$emuExe" -ForegroundColor Green
