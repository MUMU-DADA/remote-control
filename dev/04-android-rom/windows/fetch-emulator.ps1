<#
.SYNOPSIS
  拿到 Windows 版 emulator.exe（SDK）+ adb（platform-tools）。

.DESCRIPTION
  顺序：本机已有 SDK → 指定 zip → 镜像/官方仓库下载。

  ⚠️ 三个容易踩的点（本脚本已处理）：
    1. 仓库清单里**每个包有按 host-os 分的多个 archive**（linux/windows/macosx），
       直接取第一个会下到 Linux 版；
    2. 渠道不是用名字写的：清单里是 `<channelRef ref="channel-0"/>`，
       名字在 `<channel id="channel-0">Stable</channel>` 里；
    3. **清单的根元素带命名空间前缀**（`sdk:sdk-repository`），
       用 `$doc.sdkRepository` 这种点号访问会得到 $null —— 见下面 Get-Kids 的说明。
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

# ── XML 取值：一律用 XPath 的 local-name() ──────────────────────────────────
#
# ⚠️ 清单的根元素是 `sdk:sdk-repository`（带命名空间前缀），于是
#    PowerShell 的 `$doc.sdkRepository` **直接是 $null** —— 脚本报
#    "清单里没有 emulator 包"，而清单里明明有 317 个包、其中 2 个是 emulator。
#
#    实测（2026-10-08，腾讯镜像 repository2-3.xml）：
#        root = sdk:sdk-repository
#        ($null -eq $doc.sdkRepository) = True
#
#    同一个原因，`$pkg.archives.archive`、`$a.'host-os'`、`$arch.url`
#    这些点号访问**全都取不到值**。而 `host-os` 还是 <archive> 的**子元素**
#    不是属性 —— 就算没有命名空间，按属性取也是错的。
#
#    这里用 `*[local-name()='x']` 形式的 XPath：**与前缀无关，也不需要
#    注册命名空间管理器**。
#
#    （先试过写一个 `Get-Kids` 辅助函数按 LocalName 过滤，实测那在这种
#      非交互调用下取不到值 —— 同样的管道写在脚本主体里就正常，
#      包成函数就空。XPath 绕开了这个问题，也更短。）
function XSelect($node, [string]$local) {
    if ($null -eq $node) { return @() }
    return @($node.SelectNodes("*[local-name()='$local']"))
}

function XSelectOne($node, [string]$local) {
    if ($null -eq $node) { return $null }
    return $node.SelectSingleNode("*[local-name()='$local']")
}

function XText($node) {
    if ($null -eq $node) { return "" }
    return $node.InnerText
}

function Get-Remote([string]$rel, [string]$out) {
    $base = $RepoBase
    $a = @("-fL", "--retry", "2", "-o", $out)
    if ($Proxy) { $a = @("-x", $Proxy) + $a }
    & curl.exe @a "$base/$rel"
    if ($LASTEXITCODE -ne 0) { throw "下载失败：$base/$rel" }
}

function Get-RepoXml([string]$path) {
    if (-not (Test-Path $path)) { Get-Remote "repository2-3.xml" $path }
    return [xml](Get-Content $path -Raw)
}

# 按 host-os 挑 archive（这就是"下到 Linux 版"的根因）
# 按 host-os 挑 archive（这就是"下到 Linux 版"的根因）
function Select-Archive($pkg, [string]$hostOs) {
    $archives = XSelectOne $pkg "archives"
    if ($null -eq $archives) { return $null }
    foreach ($a in (XSelect $archives "archive")) {
        $ho = XText (XSelectOne $a "host-os")
        if (-not $ho) { $ho = "any" }
        if ($ho -eq $hostOs -or $ho -eq "any") { return $a }
    }
    return $null
}

# <archive> 里的 url/size/checksum 都在它的 <complete> 子节点下
function Get-ArchiveComplete($arch) { return (XSelectOne $arch "complete") }

# channelRef.ref -> 渠道名
function Resolve-ChannelId($root, [string]$name) {
    foreach ($c in (XSelect $root "channel")) {
        $text = $c.InnerText
        if ($text -and $text.Trim().ToLower() -eq $name.ToLower()) {
            return $c.GetAttribute("id")
        }
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
$doc  = Get-RepoXml $xmlPath
$root = $doc.DocumentElement

$allPkgs = XSelect $root "remotePackage"
$pkgs = @($allPkgs | Where-Object { $_.GetAttribute("path") -eq "emulator" })
if ($pkgs.Count -eq 0) {
    throw ("清单里没有 emulator 包（根元素 {0}，共 {1} 个 remotePackage）" -f `
           $root.Name, $allPkgs.Count)
}

$chanId = Resolve-ChannelId $root $Channel
$pkg = if ($chanId) {
    # ⚠️ 必须判空：有的包没有 <channelRef>，`(Get-Kid ...).GetAttribute()` 会
    #    炸在 $null 上（"You cannot call a method on a null-valued expression"）。
    $pkgs | Where-Object {
        $cr = XSelectOne $_ "channelRef"
        $cr -and ($cr.GetAttribute("ref") -eq $chanId)
    } | Select-Object -First 1
} else { $null }
if (-not $pkg) {
    Write-Warning "清单里没找到渠道 '$Channel'（可能渠道写法不同），退回第一个 emulator 包"
    $pkg = $pkgs | Select-Object -First 1
}

$arch = Select-Archive $pkg "windows"
if (-not $arch) { throw "该 emulator 包没有 windows 版 archive（清单里可能只有 linux/macosx）" }

$comp     = Get-ArchiveComplete $arch
$archUrl  = XText (XSelectOne $comp "url")
$archSize = [int64](XText (XSelectOne $comp "size"))
$archSum  = XText (XSelectOne $comp "checksum")
if (-not $archUrl) { throw "archive 里没有 url（清单结构变了？）" }

Write-Host ("==> 渠道 {0} → {1}（{2:N0} MiB，sha1 {3}…）" -f `
    $Channel, $archUrl, ($archSize / 1MB), $archSum.Substring(0,12)) -ForegroundColor Cyan

if ($EmulatorZipUrl) {
    $zip = "$Dest\emulator.zip"
    if ($EmulatorZipUrl -match '^https?://') { Get-Remote $EmulatorZipUrl $zip } else { Copy-Item $EmulatorZipUrl $zip -Force }
} else {
    $zip = "$Dest\$([IO.Path]::GetFileName($archUrl))"
    if ($DryRun) { Write-Host "==> DryRun: 将下载 $archUrl → $zip"; exit 0 }
    if ((Test-Path $zip) -and -not $Force) {
        Write-Host "==> 已有 zip，跳过下载"
    } else {
        Get-Remote $archUrl $zip
    }
    # 校验 sha1（清单里给了就一定要对）
    if ($archSum -and -not $DryRun) {
        $got = (Get-FileHash $zip -Algorithm SHA1).Hash.ToLower()
        if ($got -ne $archSum.ToLower()) { throw "sha1 不匹配：期望 $archSum，实得 $got" }
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
    $ptPkg = @($allPkgs |
               Where-Object { $_.GetAttribute("path") -eq "platform-tools" }) |
             Select-Object -First 1
    $a = if ($ptPkg) { Select-Archive $ptPkg "windows" } else { $null }
    if ($a) {
        $ptUrl = XText (XSelectOne (Get-ArchiveComplete $a) "url")
        Write-Host "==> 下载 platform-tools"
        Get-Remote $ptUrl "$Dest\platform-tools.zip"
        Expand-Archive -Path "$Dest\platform-tools.zip" -DestinationPath $Dest -Force
    } else { Write-Warning "platform-tools 没有 windows archive，跳过" }
}

Write-Host "==> 完成：$emuExe" -ForegroundColor Green
