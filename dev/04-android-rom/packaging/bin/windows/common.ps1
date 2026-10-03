<#
.SYNOPSIS
  包内公共函数（Windows 侧）—— 被 bin\*.ps1 点源引入，不要直接执行。

.DESCRIPTION
  与 Linux 侧 bin/linux/lib.sh 是**同一套语义、同一份模板**：
  硬件参数全部来自 templates\config.ini（唯一真源），实例名 ↔ 端口一一对应，
  工作目录布局也一致（.run\sysdir-<端口>、.run\datadir-<端口>）。

  路径全部相对**包根**（bin 的上一级）解析，不依赖宿主装的 Android SDK。
#>

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$script:Root         = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$script:Runtime      = if ($env:AUTOSNAP_RUNTIME) { $env:AUTOSNAP_RUNTIME } else { Join-Path $script:Root "runtime" }
$script:Images       = if ($env:AUTOSNAP_IMAGES)  { $env:AUTOSNAP_IMAGES }  else { Join-Path $script:Root "images" }
$script:Templates    = if ($env:AUTOSNAP_TEMPLATES) { $env:AUTOSNAP_TEMPLATES } else { Join-Path $script:Root "templates" }
$script:Tools        = Join-Path $script:Root "tools"
$script:RunDir       = if ($env:AUTOSNAP_RUN_DIR) { $env:AUTOSNAP_RUN_DIR } else { Join-Path $script:Root ".run" }
$script:InstancesDir = Join-Path $script:RunDir "instances"
$script:ConfigFile   = if ($env:AUTOSNAP_CONFIG) { $env:AUTOSNAP_CONFIG } else { Join-Path $script:Templates "config.ini" }
$script:Emulator     = if ($env:AUTOSNAP_EMULATOR) { $env:AUTOSNAP_EMULATOR } else { Join-Path $script:Runtime "emulator\emulator.exe" }
# 自带 adb：解压即用，不要求宿主装 Android SDK。
# 想用系统那份（例如宿主已有 adb server 在跑）：$env:AUTOSNAP_ADB = "C:\...\adb.exe"
$script:Adb          = if ($env:AUTOSNAP_ADB) { $env:AUTOSNAP_ADB } else { Join-Path $script:Runtime "platform-tools\adb.exe" }

$script:DefaultName = "default"
$script:PortBase    = 5580

# 这几个文件**不能链接**，必须各实例一份实文件：
#   initrd     —— 模拟器会重写它（把自己的 ramdisk + dtb 合进去），
#                 链接过去会改到共享的 images\ 那份，SHA256SUMS 当场对不上。
#   config.ini —— 要按包内模板覆盖，链接过去会写穿。
$script:NoLink = @("initrd", "config.ini")

# 脚本级状态：StrictMode 会把"读了没赋值的变量"当错误，先初始化。
$script:EmuProc = $null

function Write-Log  { param([string]$Msg) Write-Host "==> $Msg" -ForegroundColor Cyan }
function Write-Warn { param([string]$Msg) Write-Host "[!] $Msg" -ForegroundColor Yellow }
function Write-Ok   { param([string]$Msg) Write-Host "==> $Msg" -ForegroundColor Green }
function Die        { param([string]$Msg) Write-Host "[x] $Msg" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------------------
# config.ini：硬件参数的唯一真源（templates\config.ini）
# ---------------------------------------------------------------------------
function Get-ConfigValue {
    param([string]$Key, [string]$Default = "")
    if (-not (Test-Path $script:ConfigFile)) { return $Default }
    $pat = "^\s*" + [regex]::Escape($Key) + "\s*=\s*(.+?)\s*$"
    $hit = Select-String -Path $script:ConfigFile -Pattern $pat -ErrorAction SilentlyContinue |
           Select-Object -Last 1
    if ($hit) { return $hit.Matches[0].Groups[1].Value }
    return $Default
}

# ---------------------------------------------------------------------------
# GPU：探测 + 自适应。
# ⚠️ 有显卡 ≠ 驱动能用，所以判据是"真的起来了"（start-headless.ps1 里带回退链）。
# ---------------------------------------------------------------------------
function Test-HostGpu {
    try {
        $g = @(Get-CimInstance Win32_VideoController -ErrorAction Stop |
               Where-Object { $_.Name -and $_.Name -notmatch 'Microsoft\s+(Basic|Remote)' })
        return ($g.Count -gt 0)
    } catch { return $false }
}
function Resolve-GpuMode {
    param([string]$Want)
    if ([string]::IsNullOrWhiteSpace($Want) -or $Want -eq "auto") {
        if (Test-HostGpu) { return "host" } else { return "swiftshader_indirect" }
    }
    return $Want
}
function Get-GpuReason {
    if (Test-HostGpu) { return "宿主有可用显卡（WHPX/桌面 GPU）" }
    return "宿主没有可用显卡，用软件渲染（swiftshader_indirect）"
}

# ---------------------------------------------------------------------------
# 实例登记：.run\instances\<名字>.env
# ---------------------------------------------------------------------------
function Get-InstanceFile { param([string]$N) Join-Path $script:InstancesDir "$N.env" }
function Test-Instance    { param([string]$N) Test-Path (Get-InstanceFile $N) }
function Get-InstancePort {
    param([string]$N)
    $f = Get-InstanceFile $N
    if (-not (Test-Path $f)) { return 0 }
    foreach ($line in Get-Content $f) {
        if ($line -match '^\s*PORT\s*=\s*(\d+)') { return [int]$Matches[1] }
    }
    return 0
}
function Get-InstanceNames {
    if (-not (Test-Path $script:InstancesDir)) { return @() }
    return @(Get-ChildItem $script:InstancesDir -Filter *.env -ErrorAction SilentlyContinue |
             ForEach-Object { $_.BaseName })
}
function Register-Instance {
    param([string]$N, [int]$P)
    New-Item -ItemType Directory -Force -Path $script:InstancesDir | Out-Null
    Set-Content -Path (Get-InstanceFile $N) -Encoding ASCII -Value @(
        "# 实例登记（名字 ↔ 端口）。由 bin\start-headless.ps1 维护，手改也行。",
        "PORT=$P")
}

function Get-SysDir  { param([int]$P) Join-Path $script:RunDir "sysdir-$P" }
function Get-DataDir { param([int]$P) Join-Path $script:RunDir "datadir-$P" }
function Get-LogFile { param([int]$P) Join-Path $script:RunDir "emulator-$P.log" }
function Get-ErrFile { param([int]$P) Join-Path $script:RunDir "emulator-$P.err.log" }
function Get-Serial  { param([int]$P) "emulator-$P" }

# ---------------------------------------------------------------------------
# 找某端口上的模拟器进程
#
# ⚠️ 不能只按命令行匹配 —— 那样**会把调用者自己匹配上**（本脚本的命令行里
#    就含着 "-port 5582" 这段文字）。所以先用 Name 卡死只有
#    qemu-system-*.exe / emulator.exe 才进入候选，再匹配端口。
#    Linux 侧踩过同一个坑（pgrep -f 把自己匹配上了，kill 可能杀到无辜进程）。
# ---------------------------------------------------------------------------
function Get-EmuProcess {
    param([int]$P)
    if ($P -le 0) { return @() }
    try {
        return @(Get-CimInstance Win32_Process -ErrorAction Stop |
                 Where-Object {
                     $_.ProcessId -ne $PID -and
                     $_.Name -match '^(qemu-system-.*|emulator)\.exe$' -and
                     $_.CommandLine -and
                     $_.CommandLine -match "-port\s+$P(\s|$)"
                 })
    } catch { return @() }
}
function Test-InstanceRunning { param([int]$P) return (@(Get-EmuProcess $P).Count -gt 0) }
function Test-PortTaken {
    param([int]$P)
    foreach ($n in Get-InstanceNames) { if ((Get-InstancePort $n) -eq $P) { return $true } }
    return (Test-InstanceRunning $P)
}
function Get-InstanceNamesForPort {
    param([int]$P)
    foreach ($n in Get-InstanceNames) {
        if ((Get-InstancePort $n) -eq $P) { $n }
    }
}
function New-FreePort {
    $p = $script:PortBase
    while ($p -lt 5700) {
        if (-not (Test-PortTaken $p)) { return $p }
        $p += 2
    }
    Die "找不到空闲端口（$($script:PortBase)..5700 都占着）"
}

# ---------------------------------------------------------------------------
# 工作目录：把 images\ 链进实例自己的 sysdir
#
# Windows 上建符号链接要管理员或开发者模式，所以按能力降级：
#   目录：junction（不需要管理员）→ 符号链接 → 复制
#   文件：符号链接 → 硬链接（不需要管理员，同卷）→ 复制
#
# ⚠️ 每一档都要**读回确认**，不能"命令没报错就当成功"。
#    实测：在非 Windows 的 PowerShell 上 `New-Item -ItemType Junction`
#    **既不报错也不建东西** —— 命令返回成功、Test-Path 却是 false。
#    静默少一个目录，要等模拟器起来报"文件找不到"才发现。
# ---------------------------------------------------------------------------
function New-DirLink {
    param([string]$Source, [string]$Dest)
    try { New-Item -ItemType Junction -Path $Dest -Target $Source -ErrorAction Stop | Out-Null } catch { }
    if (Test-Path $Dest) { return "junction" }
    try { New-Item -ItemType SymbolicLink -Path $Dest -Target $Source -ErrorAction Stop | Out-Null } catch { }
    if (Test-Path $Dest) { return "symlink" }
    Copy-Item -Path $Source -Destination $Dest -Recurse -Force
    return "copy"
}
function New-FileLink {
    param([string]$Source, [string]$Dest)
    try { New-Item -ItemType SymbolicLink -Path $Dest -Target $Source -ErrorAction Stop | Out-Null } catch { }
    if (Test-Path $Dest) { return "symlink" }
    try { New-Item -ItemType HardLink -Path $Dest -Target $Source -ErrorAction Stop | Out-Null } catch { }
    if (Test-Path $Dest) { return "hardlink" }
    Copy-Item -Path $Source -Destination $Dest -Force
    return "copy"
}

function Build-SysDir {
    param([int]$P, [switch]$Keep)
    if (-not (Test-Path (Join-Path $script:Images "system-qemu.img"))) {
        Die "镜像目录不完整：$($script:Images)（缺 system-qemu.img）"
    }
    $sysdir = Get-SysDir $P
    if (-not $Keep) { Remove-Item $sysdir -Recurse -Force -ErrorAction SilentlyContinue }
    New-Item -ItemType Directory -Force -Path $sysdir | Out-Null

    $kinds = @{}
    foreach ($item in Get-ChildItem -Path $script:Images -Force) {
        if ($script:NoLink -contains $item.Name) { continue }
        $dest = Join-Path $sysdir $item.Name
        if (Test-Path $dest) { continue }
        $k = if ($item.PSIsContainer) { New-DirLink $item.FullName $dest }
             else                     { New-FileLink $item.FullName $dest }
        if ($kinds.ContainsKey($k)) { $kinds[$k]++ } else { $kinds[$k] = 1 }
    }
    # config.ini 用包内模板写一份实文件（不能用链接，见 $script:NoLink）
    Copy-Item $script:ConfigFile (Join-Path $sysdir "config.ini") -Force

    # 收尾核对：images\ 里每一项（除 NoLink）都必须在 sysdir 里出现。
    $missing = @()
    foreach ($item in Get-ChildItem -Path $script:Images -Force) {
        if ($script:NoLink -contains $item.Name) { continue }
        if (-not (Test-Path (Join-Path $sysdir $item.Name))) { $missing += $item.Name }
    }
    if ($missing.Count -gt 0) { Die "工作目录缺文件：$($missing -join ', ')（镜像链接没建成）" }
    $how = if ($kinds.Count -gt 0) { ($kinds.Keys | Sort-Object) -join "/" } else { "（无）" }
    Write-Log "镜像已就位（$how）"
}

function Assert-Emulator {
    if (-not (Test-Path $script:Emulator)) {
        Die "找不到模拟器：$($script:Emulator)`n    看 runtime\RUNTIME.txt 确认包里带了哪个版本"
    }
    $backend = Join-Path $script:Runtime "emulator\qemu\windows-x86_64\qemu-system-x86_64.exe"
    if (-not (Test-Path $backend)) {
        Die "运行时里没有 x86_64 后端：$backend`n    本 ROM 是 x86_64 guest，别的架构后端带不动它"
    }
}
function Assert-Images {
    $need = @("system-qemu.img","vendor-qemu.img","product-qemu.img","ramdisk-qemu.img",
              "kernel-ranchu","encryptionkey.img","userdata.img","advancedFeatures.ini","config.ini")
    $missing = @()
    foreach ($f in $need) { if (-not (Test-Path (Join-Path $script:Images $f))) { $missing += $f } }
    if (-not (Test-Path (Join-Path $script:Images "system\build.prop"))) { $missing += "system\build.prop" }
    if ($missing.Count -gt 0) { Die "镜像不全，缺：$($missing -join ', ')`n    镜像目录：$($script:Images)" }
}
function Assert-Adb {
    if (-not (Test-Path $script:Adb)) { Die "找不到 adb：$($script:Adb)（可用 `$env:AUTOSNAP_ADB 覆盖）" }
}

# ---------------------------------------------------------------------------
# 等开机 / 读属性
# ---------------------------------------------------------------------------
function Wait-Adb {
    param([string]$Serial, [int]$TimeoutSec = 60)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $state = (& $script:Adb -s $Serial get-state 2>$null) -join ""
        if ($state.Trim() -eq "device") { return $true }
        if ($script:EmuProc -and $script:EmuProc.HasExited) { return $false }
        Start-Sleep -Seconds 5
    }
    return $false
}
function Wait-Boot {
    param([string]$Serial, [int]$TimeoutSec = 300)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $bc = (& $script:Adb -s $Serial shell getprop sys.boot_completed 2>$null) -join ""
        if ($bc.Trim() -eq "1") { return $true }
        Start-Sleep -Seconds 3
    }
    return $false
}
function Get-Prop {
    param([string]$Serial, [string]$Name)
    return ((& $script:Adb -s $Serial shell getprop $Name 2>$null) -join "").Trim()
}
