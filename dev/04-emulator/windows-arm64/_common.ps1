# windows-arm64 下各脚本共用的小工具。
# 用法： . "$PSScriptRoot\_common.ps1"
#
# 只使用 PowerShell 5.1 就有的语法（Windows 10/11 自带），
# 不用 ?? / 三元 / ForEach-Object -Parallel 这些 7.x 才有的东西。

$ErrorActionPreference = 'Stop'

function Write-Step { param([string]$Message) Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Ok   { param([string]$Message) Write-Host "  [ok] $Message" -ForegroundColor Green }
function Write-Note { param([string]$Message) Write-Host "  [·] $Message"  -ForegroundColor Yellow }
function Write-Fail { param([string]$Message) Write-Host "  [x] $Message"  -ForegroundColor Red }

function Stop-Script { param([string]$Message) Write-Fail $Message; exit 1 }

# dl.google.com 需要 TLS 1.2；PowerShell 5.1 默认可能还在用 TLS 1.0
function Enable-ModernTls {
    try {
        [Net.ServicePointManager]::SecurityProtocol =
            [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    } catch {
        Write-Host "  [!] 无法设置 TLS 1.2：$($_.Exception.Message)" -ForegroundColor Yellow
    }
}

function Get-ProxyParam {
    param([string]$Proxy, [string]$ProxyUser, [string]$ProxyPassword)
    if (-not $Proxy) { return $null }
    $p = New-Object System.Net.WebProxy($Proxy, $true)
    if ($ProxyUser) {
        $p.Credentials = New-Object System.Net.NetworkCredential($ProxyUser, $ProxyPassword)
    }
    return $p
}

# 模拟器安装目录候选：环境变量 → 本脚本目录下的 sdk\ → Android Studio 常见位置
function Get-EmulatorDirCandidates {
    $list = New-Object System.Collections.Generic.List[string]
    if ($env:ANDROID_SDK_ROOT) { $list.Add((Join-Path $env:ANDROID_SDK_ROOT 'emulator')) }
    if ($env:ANDROID_HOME)     { $list.Add((Join-Path $env:ANDROID_HOME     'emulator')) }
    if ($env:LOCALAPPDATA)     { $list.Add((Join-Path $env:LOCALAPPDATA     'Android\Sdk\emulator')) }
    if ($PSScriptRoot)         { $list.Add((Join-Path $PSScriptRoot 'sdk\emulator')) }
    if (${env:ProgramFiles(x86)}) { $list.Add((Join-Path ${env:ProgramFiles(x86)} 'Android\android-sdk\emulator')) }
    if ($env:ProgramFiles)     { $list.Add((Join-Path $env:ProgramFiles 'Android\android-sdk\emulator')) }
    return $list
}

function Get-AdbCandidates {
    $list = New-Object System.Collections.Generic.List[string]
    if ($env:ANDROID_SDK_ROOT) { $list.Add((Join-Path $env:ANDROID_SDK_ROOT 'platform-tools\adb.exe')) }
    if ($env:ANDROID_HOME)     { $list.Add((Join-Path $env:ANDROID_HOME     'platform-tools\adb.exe')) }
    if ($env:LOCALAPPDATA)     { $list.Add((Join-Path $env:LOCALAPPDATA     'Android\Sdk\platform-tools\adb.exe')) }
    if ($PSScriptRoot)         { $list.Add((Join-Path $PSScriptRoot 'sdk\platform-tools\adb.exe')) }
    return $list
}

# 返回模拟器目录（含 emulator.exe），找不到返回 $null
function Resolve-EmulatorDir {
    param([string]$Explicit)
    $cands = New-Object System.Collections.Generic.List[string]
    if ($Explicit) { $cands.Add($Explicit) }
    foreach ($c in (Get-EmulatorDirCandidates)) { $cands.Add($c) }
    foreach ($c in $cands) {
        if ($c -and (Test-Path (Join-Path $c 'emulator.exe'))) { return (Resolve-Path $c).Path }
    }
    return $null
}

# emulator 包里的 arm64 QEMU 后端（跨架构跑 arm64 guest 靠它）
function Get-Arm64QemuBackend {
    param([string]$EmulatorDir)
    $expected = Join-Path $EmulatorDir 'qemu\windows-x86_64\qemu-system-aarch64.exe'
    if (Test-Path $expected) { return $expected }
    $hit = Get-ChildItem -Path $EmulatorDir -Recurse -Filter 'qemu-system-aarch64*.exe' -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

function Resolve-Adb {
    param([string]$Explicit)
    if ($Explicit -and (Test-Path $Explicit)) { return (Resolve-Path $Explicit).Path }
    foreach ($c in (Get-AdbCandidates)) {
        if ($c -and (Test-Path $c)) { return (Resolve-Path $c).Path }
    }
    $cmd = Get-Command 'adb.exe' -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

# 下载：优先 BITS（有进度、可断点），失败退回 WebClient
function Get-RemoteFile {
    param(
        [Parameter(Mandatory=$true)][string]$Url,
        [Parameter(Mandatory=$true)][string]$OutFile,
        [System.Net.WebProxy]$Proxy
    )
    $dir = Split-Path -Parent $OutFile
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }

    Write-Note "下载 $Url"

    $bits = Get-Command Start-BitsTransfer -ErrorAction SilentlyContinue
    if ($bits) {
        try {
            Import-Module BitsTransfer -ErrorAction Stop
            if ($Proxy) {
                Start-BitsTransfer -Source $Url -Destination $OutFile -ProxyUsage Override `
                    -ProxyList $Proxy.Address.AbsoluteUri -ErrorAction Stop
            } else {
                Start-BitsTransfer -Source $Url -Destination $OutFile -ErrorAction Stop
            }
            if (Test-Path $OutFile) { return }
        } catch {
            Write-Note "BITS 失败（$($_.Exception.Message)），改用 WebClient"
        }
    }

    $wc = New-Object System.Net.WebClient
    if ($Proxy) { $wc.Proxy = $Proxy }
    try {
        $wc.DownloadFile($Url, $OutFile)
    } finally {
        $wc.Dispose()
    }
}

function Test-FileHash {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Expected,
        [string]$Algorithm = 'SHA1'
    )
    if (-not (Test-Path $Path)) { return $false }
    $actual = (Get-FileHash -Path $Path -Algorithm $Algorithm).Hash
    return ($actual -ieq $Expected.Trim())
}

function Format-Size {
    param([long]$Bytes)
    if ($Bytes -ge 1GB) { return ('{0:N2} GB' -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ('{0:N1} MB' -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ('{0:N0} KB' -f ($Bytes / 1KB)) }
    return "$Bytes B"
}
