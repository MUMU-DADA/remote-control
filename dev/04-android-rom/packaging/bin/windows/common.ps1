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
# 模拟器主目录（ANDROID_EMULATOR_HOME / %USERPROFILE%\.android）
#
# ⚠️ 模拟器**不会**自己建这个目录，而它要在这里写 feature flags 的锁文件
#    （emu-last-feature-flags.protobuf.lock）。目录不存在时 CreateFile 返回
#    Win32 error 3（ERROR_PATH_NOT_FOUND），而模拟器**不退出**：它只是无限重试、
#    无限刷同一行 ERROR，对外看起来就是"服务一直不就绪"，日志还会涨到几个 GB。
#    2026-10-05 在 Windows 侧实测踩到过，所以启动前必须先把它建好。
# ---------------------------------------------------------------------------
function Get-EmulatorHome {
    if ($env:ANDROID_EMULATOR_HOME) { return $env:ANDROID_EMULATOR_HOME }
    if ($env:ANDROID_PREFS_ROOT)     { return (Join-Path $env:ANDROID_PREFS_ROOT ".android") }
    if ($env:USERPROFILE)            { return (Join-Path $env:USERPROFILE ".android") }
    if ($env:HOME)                   { return (Join-Path $env:HOME ".android") }
    return ""
}

function Assert-EmulatorHome {
    $emuHome = Get-EmulatorHome
    $hint = "    也可以换一个目录：`$env:ANDROID_EMULATOR_HOME = 'D:\emu-home'"
    if (-not $emuHome) { Die "定位不到模拟器主目录（USERPROFILE / HOME 都没设）`n$hint" }
    if (-not (Test-Path -LiteralPath $emuHome)) {
        New-Item -ItemType Directory -Force -Path $emuHome -ErrorAction SilentlyContinue | Out-Null
    }
    if (-not (Test-Path -LiteralPath $emuHome)) {
        Die "模拟器主目录不存在、也建不出来：$emuHome`n    这时模拟器只会报 'Unexpected error while creating: ...lock (error: 3)' 并无限重试。`n    手工建一个空目录后重试： New-Item -ItemType Directory -Force '$emuHome'`n$hint"
    }
    # ⚠️ 变量别叫 $home：PowerShell 变量名不区分大小写，$home 就是只读的自动变量 $HOME
    $probe = Join-Path $emuHome (".autosnap-writetest-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
    $writable = $true
    try { Set-Content -LiteralPath $probe -Value "ok" -ErrorAction Stop } catch { $writable = $false }
    Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
    if (-not $writable) {
        Die "模拟器主目录不可写：$emuHome`n    常见原因：杀软 /「受控文件夹访问」拦截、目录属主是别的账号、或它是指向已失效目标的 junction/符号链接。`n$hint"
    }
    return $emuHome
}

# ---------------------------------------------------------------------------
# 模拟器日志：致命错误识别 / 最后一条 ERROR / 开机耗时
#
# 有一类错误模拟器**不退出、只无限重试**（典型：主目录不在时的 error: 3）。
# 只等 Wait-ConsoleReady / Wait-ServiceReady 会一路等到超时，还把日志刷爆。
# 所以等待循环每轮扫一次日志尾部，命中就立刻失败、并翻译成人话。
# ---------------------------------------------------------------------------
function Get-EmuLogFatal {
    param([string]$LogFile)
    if (-not $LogFile -or -not (Test-Path -LiteralPath $LogFile)) { return "" }
    $tail = @(Get-Content -LiteralPath $LogFile -Tail 40 -ErrorAction SilentlyContinue)
    if ($tail.Count -eq 0) { return "" }
    $text = $tail -join "`n"

    if ($text -match 'Unexpected error while creating:\s*(?<p>.+?)\s*\(error:\s*(?<e>\d+)\)') {
        $path = $Matches['p']
        $code = [int]$Matches['e']
        $why = switch ($code) {
            3       { "路径不存在（ERROR_PATH_NOT_FOUND）：多半是模拟器主目录 $(Get-EmulatorHome) 不在，或它指向了已失效的 junction/符号链接" }
            5       { "拒绝访问（ERROR_ACCESS_DENIED）：目录权限、杀软或「受控文件夹访问」拦截" }
            32      { "文件被占用（ERROR_SHARING_VIOLATION）：这个实例可能已经有另一个模拟器进程在跑" }
            default { "Win32 error $code" }
        }
        return "模拟器创建文件失败：$path`n    原因：$why`n    这类错误模拟器不会退出，只会无限重试刷日志，所以这里直接判死。"
    }
    if ($text -match 'emulation currently requires hardware acceleration' -or
        $text -match 'Android Emulator hypervisor driver is not installed') {
        return "没有可用的硬件加速（WHPX / AEHD 都没生效）`n    修法一：启用或关闭 Windows 功能 → 勾选「Windows 虚拟机监控程序平台」→ 重启；`n    修法二：关掉 Hyper-V 后装 Android Emulator hypervisor driver（AEHD）。详见 windows\README.md"
    }
    if ($text -match 'WHPX is either not available or not installed') {
        return "WHPX 没启用：启用或关闭 Windows 功能 → 勾选「Windows 虚拟机监控程序平台」→ 重启"
    }
    return ""
}

function Get-EmuLastError {
    param([string]$LogFile, [int]$Tail = 200)
    if (-not $LogFile -or -not (Test-Path -LiteralPath $LogFile)) { return "" }
    $hits = @(Get-Content -LiteralPath $LogFile -Tail $Tail -ErrorAction SilentlyContinue |
              Select-String -Pattern 'ERROR\s*\|' -ErrorAction SilentlyContinue)
    if ($hits.Count -eq 0) { return "" }
    return $hits[$hits.Count - 1].Line.Trim()
}

function Get-EmuBootTimeLine {
    param([string[]]$LogFile)
    foreach ($f in @($LogFile)) {
        if (-not $f -or -not (Test-Path -LiteralPath $f)) { continue }
        $hit = Select-String -Path $f -Pattern '(boot time|Boot completed in)\s+\d+\s*ms' -ErrorAction SilentlyContinue |
               Select-Object -Last 1
        # ⚠️ 不能写成 (...).Line：日志里没有 boot time 行时管道给的是 $null，而 StrictMode 2.0
        #    下访问 $null 的属性会抛「在此对象上找不到属性"Line"」并把整个脚本带崩（实测踩过）。
        if ($null -ne $hit -and $hit.PSObject.Properties['Line']) { return $hit.Line.Trim() }
    }
    return ""
}

# 等一个条件成立；同时盯住"日志里出现致命错误"和"进程已经没了"，尽早失败而不是干等到超时。
function Wait-EmulatorCondition {
    param([int]$Port, [scriptblock]$Test, [int]$TimeoutSec = 60, [int]$PollSec = 2)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ($true) {
        $fatal = Get-EmuLogFatal (Get-LogFile $Port)
        if ($fatal) { return [pscustomobject]@{ State = "fatal"; Reason = $fatal } }
        if (& $Test) { return [pscustomobject]@{ State = "ready"; Reason = "" } }
        if (-not (Test-EmulatorPortRunning $Port)) { return [pscustomobject]@{ State = "exited"; Reason = "" } }
        if ((Get-Date) -ge $deadline) { return [pscustomobject]@{ State = "timeout"; Reason = "" } }
        Start-Sleep -Seconds $PollSec
    }
}

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
    if (Get-Command Optimize-ImageStorage -ErrorAction SilentlyContinue) {
        Optimize-ImageStorage $script:Images
    }
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
    if (Get-Command Optimize-ImageStorage -ErrorAction SilentlyContinue) {
        Optimize-ImageStorage $sysdir
    }
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

. (Join-Path $PSScriptRoot "console.ps1")

# storage.ps1 is kept as a small Windows-only provider.  Loading it here makes
# Build-SysDir safe for every entry point while the provider itself avoids
# kernel32 calls on non-Windows PowerShell used by CI.
$storageProvider = Join-Path $PSScriptRoot "storage.ps1"
if ((Test-Path -LiteralPath $storageProvider) -and -not (Get-Command Optimize-ImageStorage -ErrorAction SilentlyContinue)) {
    . $storageProvider
}
