<#
.SYNOPSIS
    获取 Windows x64 上的 Android 模拟器（QEMU 的 goldfish/ranchu 分支）+ adb。

.DESCRIPTION
    按优先级：
      1. 先找机器上**已有的** SDK（Android Studio / ANDROID_SDK_ROOT / %LOCALAPPDATA%\Android\Sdk），
         有就直接用，什么都不下。
      2. 没有才下载：读 Google 的 repository2-3.xml，取 stable 渠道的
         emulator-windows_x64-*.zip，校验 SHA1 后解包；platform-tools 走固定 URL。

    为什么要下 emulator 而不是 qemu.org 的 qemu-system-aarch64：
    见 ../README.md 硬约束 2 —— 上游 QEMU 没有 goldfish/ranchu 设备，AOSP 镜像跑不起来。

    注意版本差异：AOSP 12 树里自带的是模拟器 30.8.3，而 Google 只保留当前版本，
    老包已经 404（实测 emulator-windows_x64-7595944.zip → HTTP 404）。
    所以 Windows 侧只能用新版模拟器，兼容性预案见 README「模拟器版本」一节。

.EXAMPLE
    .\fetch-emulator.ps1
    .\fetch-emulator.ps1 -Channel beta
    .\fetch-emulator.ps1 -Proxy http://127.0.0.1:10809
    .\fetch-emulator.ps1 -RepoBase https://mirrors.cloud.tencent.com/AndroidSDK
    .\fetch-emulator.ps1 -EmulatorZipUrl D:\pkgs\emulator-windows_x64-15917651.zip
    .\fetch-emulator.ps1 -DryRun
#>
[CmdletBinding()]
param(
    # 下载/解包到哪
    [string]$Dest = (Join-Path $PSScriptRoot 'sdk'),

    # stable / beta / dev / canary
    [ValidateSet('stable', 'beta', 'dev', 'canary')]
    [string]$Channel = 'stable',

    # SDK 仓库根地址（换成国内镜像时改这里）
    [string]$RepoBase = 'https://dl.google.com/android/repository',

    # 仓库清单：默认取 $RepoBase/repository2-3.xml；
    # 也可以直接给本地 XML 文件路径（离线/内网环境用）
    [string]$RepoXmlPath,

    # 直接指定 zip：本地文件路径或 URL（镜像/内网/已下好的包）
    [string]$EmulatorZipUrl,

    [string]$PlatformToolsZipUrl,

    # 例如 http://127.0.0.1:10809（本机有代理工具时用）
    [string]$Proxy,
    [string]$ProxyUser,
    [string]$ProxyPassword,

    [switch]$SkipPlatformTools,
    [switch]$Force,
    [switch]$DryRun
)

. "$PSScriptRoot\_common.ps1"

Enable-ModernTls
$webProxy = Get-ProxyParam -Proxy $Proxy -ProxyUser $ProxyUser -ProxyPassword $ProxyPassword

$channelRefs = @{ stable = 'channel-0'; beta = 'channel-1'; dev = 'channel-2'; canary = 'channel-3' }

# ---------------------------------------------------------------------------
Write-Step '第一步：找机器上已有的 SDK'

$existing = Resolve-EmulatorDir
if ($existing -and -not $Force -and -not $EmulatorZipUrl) {
    $backend = Get-Arm64QemuBackend -EmulatorDir $existing
    Write-Ok "已有模拟器：$existing"
    if ($backend) {
        Write-Ok "arm64 后端：$(Split-Path -Leaf $backend)"
    } else {
        Write-Note "这个目录里没找到 qemu-system-aarch64.exe，可能装的是不完整包"
    }
    $adb = Resolve-Adb
    if ($adb) { Write-Ok "adb：$adb" } else { Write-Note "没找到 adb，可加 -SkipPlatformTools:$false 重新跑本脚本" }
    Write-Host ''
    Write-Host '不用下载。下一步：' -ForegroundColor Cyan
    Write-Host "  .\fetch-images.ps1"
    Write-Host "  .\run-emulator.ps1 -EmulatorDir `"$existing`""
    exit 0
}

# ---------------------------------------------------------------------------
Write-Step "第二步：解析 SDK 仓库清单（$Channel 渠道）"

$emuUrl = $null; $emuSize = 0; $emuSha1 = $null

if ($EmulatorZipUrl) {
    $emuUrl = $EmulatorZipUrl
    Write-Note "使用指定的模拟器包：$emuUrl"
} else {
    $xmlUrl = "$RepoBase/repository2-3.xml"
    $xmlPath = Join-Path $env:TEMP 'dsh-repository2-3.xml'

    if ($RepoXmlPath -and (Test-Path $RepoXmlPath -PathType Leaf)) {
        $xmlPath = (Resolve-Path $RepoXmlPath).Path
        Write-Note "使用本地仓库清单：$xmlPath"
    } else {
    if ($RepoXmlPath) { $xmlUrl = $RepoXmlPath }
    try {
        Get-RemoteFile -Url $xmlUrl -OutFile $xmlPath -Proxy $webProxy
    } catch {
        Stop-Script @"
下载仓库清单失败：$xmlUrl
    $($_.Exception.Message)

三种处理方式：
  1. 本机有代理工具： .\fetch-emulator.ps1 -Proxy http://127.0.0.1:10809
  2. 换国内镜像：     .\fetch-emulator.ps1 -RepoBase <镜像地址>
  3. 手工下好 zip 后： .\fetch-emulator.ps1 -EmulatorZipUrl <本地zip或URL>
"@
        }
    }

    try {
        [xml]$repo = Get-Content -Path $xmlPath -Raw -Encoding UTF8
    } catch {
        $msg = $_.Exception.Message
        if ($msg.Length -gt 300) { $msg = $msg.Substring(0, 300) + ' …(截断)' }
        Stop-Script "仓库清单不是合法 XML：$msg"
    }

    $ref = $channelRefs[$Channel]
    $node = $repo.SelectSingleNode("//remotePackage[@path='emulator'][channelRef[@ref='$ref']]/archives/archive[host-os='windows' and host-arch='x64']")
    if (-not $node) {
        Write-Note "按渠道 $Channel($ref) 没匹配到，列出清单里所有 emulator 包供排查："
        foreach ($pkg in $repo.SelectNodes("//remotePackage[@path='emulator']")) {
            $r = $pkg.SelectSingleNode('revision')
            $c = $pkg.SelectSingleNode('channelRef')
            $rev = if ($r) { "$($r.major).$($r.minor).$($r.micro)" } else { '?' }
            $cref = if ($c) { $c.ref } else { '-' }
            Write-Host "    rev=$rev channel=$cref"
        }
        Stop-Script '解析失败 —— 可用 -EmulatorZipUrl 直接指定包'
    }

    $emuUrl  = "$RepoBase/" + $node.SelectSingleNode('complete/url').InnerText.Trim()
    $emuSize = [long]$node.SelectSingleNode('complete/size').InnerText
    $emuSha1 = $node.SelectSingleNode('complete/checksum').InnerText.Trim()

    $revNode = $repo.SelectSingleNode("//remotePackage[@path='emulator'][channelRef[@ref='$ref']]/revision")
    if ($revNode) {
        Write-Ok "模拟器版本 $($revNode.major).$($revNode.minor).$($revNode.micro)（$Channel 渠道）"
    }
    Write-Ok "$(Split-Path -Leaf $emuUrl)  $(Format-Size $emuSize)  sha1=$($emuSha1.Substring(0,12))…"
}

if (-not $PlatformToolsZipUrl) {
    $PlatformToolsZipUrl = "$RepoBase/platform-tools-latest-windows.zip"
}

if ($DryRun) {
    Write-Host ''
    Write-Step 'DryRun：以下是将要下载的内容'
    Write-Host "  emulator      : $emuUrl"
    if ($emuSize -gt 0) { Write-Host "  emulator 大小 : $(Format-Size $emuSize)" }
    if ($emuSha1)       { Write-Host "  emulator sha1 : $emuSha1" }
    if (-not $SkipPlatformTools) { Write-Host "  platform-tools: $PlatformToolsZipUrl" }
    Write-Host "  解包到        : $Dest"
    exit 0
}

# ---------------------------------------------------------------------------
Write-Step '第三步：下载并解包'

if (-not (Test-Path $Dest)) { New-Item -ItemType Directory -Path $Dest -Force | Out-Null }

$emuZip = Join-Path $Dest (Split-Path -Leaf ([uri]$emuUrl).AbsolutePath)
if ((Test-Path $emuZip) -and -not $Force) {
    Write-Ok "已有安装包，跳过下载：$(Split-Path -Leaf $emuZip)"
} elseif (Test-Path $emuUrl -PathType Leaf) {
    Write-Note "从本地文件复制：$emuUrl"
    Copy-Item -Path $emuUrl -Destination $emuZip -Force
} else {
    Get-RemoteFile -Url $emuUrl -OutFile $emuZip -Proxy $webProxy
}

$actualSize = (Get-Item $emuZip).Length
Write-Ok "安装包 $(Format-Size $actualSize)"
if ($emuSize -gt 0 -and $actualSize -ne $emuSize) {
    Write-Note "大小与清单不一致（清单 $(Format-Size $emuSize)），可能是镜像上的版本不同，继续"
}
if ($emuSha1) {
    if (Test-FileHash -Path $emuZip -Expected $emuSha1 -Algorithm SHA1) {
        Write-Ok 'SHA1 校验通过'
    } else {
        Stop-Script "SHA1 校验失败：$emuZip`n    期望 $emuSha1`n    删除该文件后重试，或换 -RepoBase / -EmulatorZipUrl"
    }
}

$emuDir = Join-Path $Dest 'emulator'
if ((Test-Path $emuDir) -and $Force) { Remove-Item $emuDir -Recurse -Force }
if (-not (Test-Path (Join-Path $emuDir 'emulator.exe'))) {
    Write-Note "解包到 $emuDir（400MB 左右，要一会儿）"
    Expand-Archive -Path $emuZip -DestinationPath $Dest -Force
} else {
    Write-Ok 'emulator 已解包'
}

if (-not $SkipPlatformTools) {
    $ptZip = Join-Path $Dest 'platform-tools-latest-windows.zip'
    if ((Test-Path $ptZip) -and -not $Force) {
        Write-Ok '已有 platform-tools 安装包，跳过下载'
    } else {
        Get-RemoteFile -Url $PlatformToolsZipUrl -OutFile $ptZip -Proxy $webProxy
    }
    if (-not (Test-Path (Join-Path $Dest 'platform-tools\adb.exe'))) {
        Write-Note '解包 platform-tools'
        Expand-Archive -Path $ptZip -DestinationPath $Dest -Force
    } else {
        Write-Ok 'platform-tools 已解包'
    }
}

# ---------------------------------------------------------------------------
Write-Step '第四步：自检'

$ok = $true
$exe = Join-Path $emuDir 'emulator.exe'
if (Test-Path $exe) { Write-Ok "emulator.exe  $(Format-Size (Get-Item $exe).Length)" } else { Write-Fail '缺 emulator.exe'; $ok = $false }

$backend = Get-Arm64QemuBackend -EmulatorDir $emuDir
if ($backend) { Write-Ok "arm64 QEMU 后端：$(Split-Path -Leaf $backend)" }
else { Write-Fail '没找到 qemu-system-aarch64.exe —— 跨架构跑 arm64 guest 就靠它'; $ok = $false }

$adb = Resolve-Adb
if ($adb) { Write-Ok "adb：$adb" } else { Write-Note '没找到 adb（可后面单独装 platform-tools）' }

$ver = ''
try { $ver = (Get-Item $exe).VersionInfo.ProductVersion } catch { }
if ($ver) { Write-Note "emulator.exe 版本信息：$ver" }

Write-Host ''
if ($ok) {
    Write-Host '完成。下一步：' -ForegroundColor Cyan
    Write-Host '  .\fetch-images.ps1'
    Write-Host '  .\run-emulator.ps1'
} else {
    Stop-Script '自检未通过，见上面的 [x]'
}
