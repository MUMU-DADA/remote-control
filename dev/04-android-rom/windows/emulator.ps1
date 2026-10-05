<#
.SYNOPSIS
  模拟器实例生命周期控制（Windows / WHPX 侧）——
  建 / 起 / 停 / 强杀 / 重启 / 重置 / 删除 / 复制。

.DESCRIPTION
  和 Linux 侧的 scripts/emulator.sh 是**同一套语义、同一份配置**：
  硬件参数全部来自 ..\emulator\config.ini（唯一真源），
  实例名 ↔ 端口一一对应，命令名也一一对应。

.EXAMPLE
  .\emulator.ps1 create  dev2
  .\emulator.ps1 start   dev2
  .\emulator.ps1 start   dev2 -NoWait
  .\emulator.ps1 stop    dev2
  .\emulator.ps1 kill    dev2
  .\emulator.ps1 restart dev2
  .\emulator.ps1 reset   dev2
  .\emulator.ps1 clone   dev2  dev3
  .\emulator.ps1 delete  dev2
  .\emulator.ps1 list
  .\emulator.ps1 status  dev2

.NOTES
  网络：Windows 侧走模拟器默认的用户态 NAT（-net-tap 只在 Linux 侧实现，
  见 ..\docs\10-network-bridge.md）。所以 Linux 侧那个「两台同 MAC 不能同时
  桥接」的限制在这里不存在。
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Command = "",
    [Parameter(Position = 1)][string]$Name    = "",
    [Parameter(Position = 2)][string]$Target  = "",

    [int]$Port      = 0,        # 0 = 自动分配
    [string]$Gpu    = "",       # "" = 按 config.ini 自适应
    [int]$Memory    = 0,        # MB；0 = 按 config.ini
    [int]$Cores     = 0,        # 0 = 按 config.ini
    [switch]$NoWait,
    [switch]$Gui,
    [switch]$Yes,
    [switch]$Force,
    [switch]$TestInstance,
    [int]$TimeoutSec = 60,
    [switch]$Help
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$script:DefaultName   = "default"
$script:PortBase      = 5580
$script:X64Dir        = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$script:ConfigFile    = if ($env:AUTOSNAP_CONFIG) { $env:AUTOSNAP_CONFIG } else { Join-Path $script:X64Dir "emulator\config.ini" }
$script:ImagesDir     = if ($env:AUTOSNAP_IMAGES) { $env:AUTOSNAP_IMAGES } else { Join-Path $PSScriptRoot "images" }
$script:RunDir        = if ($env:AUTOSNAP_RUN_DIR) { $env:AUTOSNAP_RUN_DIR } else { Join-Path $PSScriptRoot ".run" }
$script:InstancesDir  = Join-Path $script:RunDir "instances"

# 这几个文件**不能链接**，必须各实例一份实文件：
#   initrd     —— 模拟器会重写它（把自己的 ramdisk + dtb 合进去），
#                 链接过去会改到共享的 images\ 那份。
#   config.ini —— 要按本项目真源覆盖，链接过去会写穿。
$script:NoLink = @("initrd", "config.ini")

# ⚠️ 下面这些是脚本级状态。StrictMode 会把"读了没赋值的变量"当错误，
#    所以先初始化一遍，别等用到时才第一次出现。
$script:EmuProc = $null
$script:GpuMode = ""
$script:GpuAuto = $true
$script:MemMB   = 0
$script:CoreN   = 0
$script:Service = $null

# 路径/端口相关的运行期残留：clone 之后必须删掉，让模拟器按新路径重建。
# hardware-qemu.ini 里的 disk.*.path 全是**绝对路径**，照抄过去两台机器
# 会读写同一份 userdata —— 那不是复制，是两台机器共用一块硬盘。
$script:StaleAfterClone = @(
    "hardware-qemu.ini", "hardware-qemu.ini.lock", "multiinstance.lock",
    "emu-launch-params.txt", "bootcompleted.ini", "version_num.cache",
    "userdata-qemu.img.qcow2.lock", "cache.img.qcow2.lock"
)

# ---------------------------------------------------------------------------
# 输出
# ---------------------------------------------------------------------------
function Write-Log  { param([string]$Msg) Write-Host "==> $Msg" -ForegroundColor Cyan }
function Write-Warn { param([string]$Msg) Write-Host "[!] $Msg" -ForegroundColor Yellow }
function Write-Ok   { param([string]$Msg) Write-Host "==> $Msg" -ForegroundColor Green }
function Die        { param([string]$Msg) Write-Host "[x] $Msg" -ForegroundColor Red; exit 1 }

function Show-Usage {
    Get-Help $PSCommandPath -Detailed | Out-String | Write-Host
}

# ---------------------------------------------------------------------------
# config.ini —— 硬件参数的唯一真源
# ---------------------------------------------------------------------------
function Get-ConfigValue {
    param([string]$Key, [string]$Default = "")
    if (-not (Test-Path $script:ConfigFile)) { return $Default }
    # 取**最后一个**匹配：文件里同一个键可能先出现在注释举例里
    $pat = "^\s*" + [regex]::Escape($Key) + "\s*=\s*(.+?)\s*$"
    $hit = Select-String -Path $script:ConfigFile -Pattern $pat -ErrorAction SilentlyContinue |
           Select-Object -Last 1
    if ($hit) { return $hit.Matches[0].Groups[1].Value }
    return $Default
}

# ---------------------------------------------------------------------------
# GPU 自适应
#
# 规则和 Linux 侧一致：宿主有**真**显卡就走 host，否则退软件渲染；
# 而且 host 起不来时还要再退一次（有设备 ≠ 驱动能用 —— Linux 侧实测过：
# VMware 的虚拟 GPU 有 renderD 节点，但模拟器报 Could not start renderer）。
# ---------------------------------------------------------------------------
function Test-HostGpu {
    # "Microsoft Basic Display Adapter" / "Microsoft Remote Display Adapter"
    # 是**没有真驱动**时的兜底适配器，不算 GPU。
    try {
        $gpus = @(Get-CimInstance Win32_VideoController -ErrorAction Stop |
                  Where-Object { $_.Name -and $_.Name -notmatch 'Microsoft\s+(Basic|Remote)' })
        return ($gpus.Count -gt 0)
    } catch {
        return $false
    }
}

function Get-GpuReason {
    if (Test-HostGpu) {
        $n = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -and $_.Name -notmatch 'Microsoft\s+(Basic|Remote)' } |
               Select-Object -First 1 -ExpandProperty Name)
        return "宿主有显卡（$n）"
    }
    return "宿主没有可用的显卡（只有 Microsoft Basic/Remote 适配器）"
}

function Resolve-GpuMode {
    param([string]$Want)
    if ([string]::IsNullOrWhiteSpace($Want)) { $Want = "auto" }
    if ($Want -eq "auto") { if (Test-HostGpu) { return "host" } else { return "swiftshader_indirect" } }
    return $Want
}

# ---------------------------------------------------------------------------
# 工具查找
# ---------------------------------------------------------------------------
function Find-Emulator {
    if ($env:AUTOSNAP_EMULATOR -and (Test-Path -LiteralPath $env:AUTOSNAP_EMULATOR)) { return $env:AUTOSNAP_EMULATOR }
    $cands = @((Join-Path $PSScriptRoot "sdk\emulator\emulator.exe"))
    if ($env:ANDROID_SDK_ROOT) { $cands += Join-Path $env:ANDROID_SDK_ROOT "emulator\emulator.exe" }
    if ($env:ANDROID_HOME) { $cands += Join-Path $env:ANDROID_HOME "emulator\emulator.exe" }
    if ($env:LOCALAPPDATA) { $cands += Join-Path $env:LOCALAPPDATA "Android\Sdk\emulator\emulator.exe" }
    foreach ($c in $cands) { if ($c -and (Test-Path $c)) { return $c } }
    Die "找不到 emulator.exe（先跑 .\fetch-emulator.ps1）"
}

# ---------------------------------------------------------------------------
# 实例登记：名字 ↔ 端口
# ---------------------------------------------------------------------------
function Get-InstanceFile  { param([string]$N) Join-Path $script:InstancesDir "$N.env" }
function Test-Instance     { param([string]$N) Test-Path (Get-InstanceFile $N) }

function Get-InstancePort {
    param([string]$N)
    $f = Get-InstanceFile $N
    if (-not (Test-Path $f)) { return 0 }
    $hit = Select-String -Path $f -Pattern '^PORT=(\d+)' -ErrorAction SilentlyContinue |
           Select-Object -Last 1
    if ($hit) { return [int]$hit.Matches[0].Groups[1].Value }
    return 0
}

function Get-InstanceNames {
    if (-not (Test-Path $script:InstancesDir)) { return @() }
    return @(Get-ChildItem -Path $script:InstancesDir -Filter "*.env" -ErrorAction SilentlyContinue |
             ForEach-Object { $_.BaseName })
}

function Register-Instance {
    param([string]$N, [int]$P)
    New-Item -ItemType Directory -Force -Path $script:InstancesDir | Out-Null
    # ⚠️ 别用 -Encoding ASCII：它会把中文注释整行变成 "?"（实测过）。
    #    UTF8 在 PS 5.1 下会带 BOM，但 BOM 落在第一行注释上，
    #    不影响下面 ^PORT= 的匹配。
    Set-Content -Path (Get-InstanceFile $N) -Encoding UTF8 `
                -Value "# 由 emulator.ps1 维护，手改也行（PORT 一行就够）`nPORT=$P"
}

function Unregister-Instance { param([string]$N) Remove-Item (Get-InstanceFile $N), (Join-Path $script:InstancesDir "$N.token") -Force -ErrorAction SilentlyContinue }

function Assert-Instance { param([string]$N) if (-not (Test-Instance $N)) { Die "没有叫 '$N' 的实例（.\emulator.ps1 list 看有哪些）" } }

function Get-SysDir  { param([string]$N) Join-Path $script:RunDir "sysdir-$(Get-InstancePort $N)" }
function Get-DataDir { param([string]$N) if ($env:AUTOSNAP_DATA_DIR) { return $env:AUTOSNAP_DATA_DIR }; Join-Path $script:RunDir "datadir-$(Get-InstancePort $N)" }
function Get-LogFile { param([string]$N) Join-Path $script:RunDir "emulator-$(Get-InstancePort $N).log" }
function Get-ErrFile { param([string]$N) Join-Path $script:RunDir "emulator-$(Get-InstancePort $N).err.log" }
function Get-Serial  { param([string]$N) "emulator-$(Get-InstancePort $N)" }

# ---------------------------------------------------------------------------
# 模拟器主目录（ANDROID_EMULATOR_HOME / %USERPROFILE%\.android）
#
# ⚠️ 模拟器**不会**自己建这个目录，而它要在这里写 feature flags 的锁文件
#    （emu-last-feature-flags.protobuf.lock）。目录不存在时 CreateFile 返回
#    Win32 error 3（ERROR_PATH_NOT_FOUND），而模拟器**不退出**：它只是无限重试、
#    无限刷同一行 ERROR，看起来就是"服务一直不就绪"。2026-10-05 实测踩到过。
#    （与 packaging/bin/windows/common.ps1 的同名函数保持同一套语义。）
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
# 模拟器日志：致命错误识别 / 最后一条 ERROR
#
# 有一类错误模拟器**不退出、只无限重试**（典型：主目录不在时的 error: 3）。
# 等待循环里每轮扫一次日志尾部，命中就立刻失败并翻译成人话。
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

# 等一个条件成立；同时盯住"日志里出现致命错误"和"进程已经没了"，尽早失败而不是干等到超时。
function Wait-EmulatorCondition {
    param([string]$Name, [string]$LogFile, [scriptblock]$Test, [int]$TimeoutSec = 60, [int]$PollSec = 2)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ($true) {
        $fatal = Get-EmuLogFatal $LogFile
        if ($fatal) { return [pscustomobject]@{ State = "fatal"; Reason = $fatal } }
        if (& $Test) { return [pscustomobject]@{ State = "ready"; Reason = "" } }
        if (-not (Test-InstanceRunning $Name)) { return [pscustomobject]@{ State = "exited"; Reason = "" } }
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
#    Linux 侧踩过同一个坑（pgrep -f 把自己的 bash 匹配上了，
#    结果 kill 有可能去杀一个无辜的进程）。
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
    } catch {
        return @()
    }
}

function Test-PortTaken {
    param([int]$P)
    foreach ($n in Get-InstanceNames) { if ((Get-InstancePort $n) -eq $P) { return $true } }
    if (@(Get-EmuProcess $P).Count -gt 0) { return $true }
    return $false
}

function New-FreePort {
    $p = $script:PortBase
    while ($p -lt 5700) {
        if (-not (Test-PortTaken $p)) { return $p }
        $p += 2
    }
    Die "找不到空闲端口（$($script:PortBase)..5700 都占着）"
}

function Test-InstanceRunning { param([string]$N) return (@(Get-EmuProcess (Get-InstancePort $N)).Count -gt 0) }

# ---------------------------------------------------------------------------
# 工作目录：把 images\ 链进实例自己的 sysdir
#
# Windows 上建**符号链接**要管理员或开发者模式，所以按能力降级：
#   符号链接 → 硬链接（不需要管理员，但要求同一个卷）→ 直接复制
# 目录用 junction（不需要管理员）。
# ---------------------------------------------------------------------------
# ⚠️ 每一档都要**读回确认**，不能"命令没报错就当成功"。
#
#    实测：在非 Windows 的 PowerShell 上 `New-Item -ItemType Junction`
#    **既不报错也不建东西** —— 命令返回成功、Test-Path 却是 false。
#    原来那版直接 return "junction" 就走了，结果目录**静默缺失**，
#    要等模拟器起来报"文件找不到"才发现。这个坑是本项目的
#    tools/test-windows-emulator.sh 在 Linux 上跑出来的。
function New-DirLink {
    param([string]$Source, [string]$Dest)
    # Windows：junction 不需要管理员，优先。
    try { New-Item -ItemType Junction -Path $Dest -Target $Source -ErrorAction Stop | Out-Null } catch { }
    if (Test-Path $Dest) { return "junction" }
    # 非 Windows（本项目的验证环境拿 pwsh 在 Linux 上跑）退到符号链接
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
    param([string]$N)
    if (-not (Test-Path (Join-Path $script:ImagesDir "system-qemu.img"))) {
        Die "镜像目录不完整：$($script:ImagesDir)`n    先拉镜像： .\fetch-images.ps1"
    }
    $sysdir = Get-SysDir $N
    Optimize-ImageStorage $script:ImagesDir
    New-Item -ItemType Directory -Force -Path $sysdir | Out-Null

    $kinds = @{}
    foreach ($item in Get-ChildItem -Path $script:ImagesDir -Force) {
        $dest = Join-Path $sysdir $item.Name
        if ($script:NoLink -contains $item.Name) {
            # 这两个必须实文件，理由见 $script:NoLink 的注释
            if (-not (Test-Path $dest)) { Copy-Item $item.FullName $dest -Force }
            continue
        }
        if (Test-Path $dest) { continue }
        if ($item.PSIsContainer) { $k = New-DirLink  $item.FullName $dest }
        else                     { $k = New-FileLink $item.FullName $dest }
        if ($kinds.ContainsKey($k)) { $kinds[$k]++ } else { $kinds[$k] = 1 }
    }
    # config.ini 用当前真源覆盖（可能是上次 clone 带过来的旧值）
    Copy-Item $script:ConfigFile (Join-Path $sysdir "config.ini") -Force

    # 收尾核对：images\ 里每一项都必须在 sysdir 里出现。
    # 「链接没建成但没报错」这种失败必须在这里当场暴露，而不是等模拟器起来。
    $missing = @()
    foreach ($item in Get-ChildItem -Path $script:ImagesDir -Force) {
        if (-not (Test-Path (Join-Path $sysdir $item.Name))) { $missing += $item.Name }
    }
    if ($missing.Count -gt 0) { Die "工作目录缺文件：$($missing -join ', ')（镜像链接没建成）" }

    $how = if ($kinds.Count -gt 0) { ($kinds.Keys | Sort-Object) -join "/" } else { "（无）" }
    Optimize-ImageStorage $sysdir
    Write-Log "镜像已就位（$how）"
}

# ---------------------------------------------------------------------------
# create
# ---------------------------------------------------------------------------
function Invoke-Create {
    param([string]$N)
    if (Test-Instance $N) { Die "实例 '$N' 已经存在（换个名字，或先 delete）" }
    $p = if ($Port -gt 0) { $Port } else { New-FreePort }
    if (Test-PortTaken $p) { Die "端口 $p 已被别的实例占用" }

    Register-Instance $N $p
    Build-SysDir $N
    New-Item -ItemType Directory -Force -Path (Get-DataDir $N) | Out-Null

    $w = Get-ConfigValue "hw.lcd.width" "1280"
    $h = Get-ConfigValue "hw.lcd.height" "720"
    Write-Ok "已创建实例 '$N'"
    Write-Host "    端口     $p（adb -s emulator-$p）"
    Write-Host "    工作目录 .run\sysdir-$p"
    Write-Host ("    显示     {0}x{1} @{2}dpi  {3}" -f $w, $h, (Get-ConfigValue "hw.lcd.density" "320"),
                $(if ([int]$w -gt [int]$h) { "横屏" } else { "竖屏" }))
    Write-Host ("    内存/核  {0} MB / {1} 核" -f (Get-ConfigValue "hw.ramSize" "6144"), (Get-ConfigValue "hw.cpu.ncore" "4"))
    Write-Host ("    数据分区 {0}（实际占用看 qcow2 长到多大）" -f (Get-ConfigValue "disk.dataPartition.size" "64G"))
    Write-Host "    下一步   .\emulator.ps1 start $N"
}

# 常规生命周期共享 release 的 console/HTTP helper，实例文件函数仍使用名字。
. (Join-Path $script:X64Dir "packaging\bin\windows\console.ps1")
. (Join-Path $script:X64Dir "packaging\bin\windows\storage.ps1")

# ---------------------------------------------------------------------------
# start
# ---------------------------------------------------------------------------
function Resolve-Hw {
    $cfgGpu  = Get-ConfigValue "hw.gpu.mode" "auto"
    $script:GpuMode = if ($Gpu) { $Gpu } else { Resolve-GpuMode $cfgGpu }
    $script:GpuAuto = (-not $Gpu)
    $script:MemMB   = if ($Memory -gt 0) { $Memory } else { [int](Get-ConfigValue "hw.ramSize" "6144") }
    $script:CoreN   = if ($Cores  -gt 0) { $Cores }  else { [int](Get-ConfigValue "hw.cpu.ncore" "4") }
}

function Start-Emu {
    param([string]$N, [string]$Emu, [string]$GpuMode)
    $sysdir  = Get-SysDir  $N
    $datadir = Get-DataDir $N
    $logf    = Get-LogFile $N
    $errf    = Get-ErrFile $N
    $p       = Get-InstancePort $N
    New-Item -ItemType Directory -Force -Path $datadir | Out-Null

    # 模拟器要 ANDROID_PRODUCT_OUT 才认 -sysdir；少了 ANDROID_BUILD_TOP
    # SDK 版会去找 kernel-qemu 并报 "system directory is missing ..."
    $env:ANDROID_PRODUCT_OUT = $sysdir
    $env:ANDROID_BUILD_TOP   = $script:X64Dir   # 只为让 SDK 版模拟器不去找 kernel-qemu

    # ⚠️ Start-Process 的 -ArgumentList 只是把数组用空格拼起来，**不会**加引号。
    #    路径里有空格（"C:\Program Files\..."、用户名带空格）就会散架，
    #    所以这里自己给路径参数套引号。
    $a = @("-sysdir", "`"$sysdir`"", "-datadir", "`"$datadir`"", "-port", $p,
           "-gpu", $GpuMode, "-accel", "on",
           "-memory", "$script:MemMB", "-cores", "$script:CoreN",
           "-no-boot-anim", "-no-audio", "-no-snapshot")
    if ($script:Service) { $a += $script:Service.Properties }
    if (-not $Gui) { $a += "-no-window" }

    Remove-Item $logf, $errf -Force -ErrorAction SilentlyContinue
    $script:EmuProc = Start-Process -FilePath $Emu -ArgumentList $a -PassThru -WindowStyle Hidden `
                                    -RedirectStandardOutput $logf -RedirectStandardError $errf
}

function Invoke-Start {
    param([string]$N)
    if (-not (Test-Instance $N)) { Write-Log "实例 '$N' 不存在，先创建"; Invoke-Create $N }
    if (Test-InstanceRunning $N) { Write-Warn "实例 '$N' 已经在跑（端口 $(Get-InstancePort $N)）"; return }

    # 模拟器不会自己建主目录，缺了它会无限重试刷日志（见本文件开头 Get-EmulatorHome 的说明）
    [void](Assert-EmulatorHome)

    $emu = Find-Emulator
    Resolve-Hw
    $p = Get-InstancePort $N
    $reuseService = -not [string]::IsNullOrWhiteSpace((Get-InstanceValue $N "SERVICE_AUTH"))
    $script:Service = Initialize-ServiceInstance $N $p -Reuse:$reuseService -TestInstance:$TestInstance

    if ($script:GpuAuto) {
        Write-Log ("硬件参数： -memory $script:MemMB  -cores $script:CoreN  -gpu $script:GpuMode（自适应：$(Get-GpuReason)）")
    } else {
        Write-Log ("硬件参数： -memory $script:MemMB  -cores $script:CoreN  -gpu $script:GpuMode（命令行指定）")
    }

    # GPU 回退链：自适应选的那一档起不来就退软件渲染。
    # 「有设备 ≠ 驱动能用」—— Linux 侧实测过：虚拟 GPU 有 renderD 节点，
    # 模拟器照样 Could not start renderer。所以判据是"真的起来了"。
    $gpus = @($script:GpuMode)
    if ($script:GpuAuto -and $script:GpuMode -eq "host") { $gpus += "swiftshader_indirect" }

    $started = $false; $used = ""
    foreach ($g in $gpus) {
        if ($g -ne $script:GpuMode) { Write-Warn "上一档（$script:GpuMode）没起来，退到 $g" }
        Start-Emu $N $emu $g
        Write-Host "    PID $($script:EmuProc.Id)"
        $wait = Wait-EmulatorCondition -Name $N -LogFile (Get-LogFile $N) -Test { Test-ConsoleReady $p } -TimeoutSec 60
        if ($wait.State -eq "ready") { $started = $true; $used = $g; break }
        if ($wait.State -eq "fatal") {
            # 这类错误换 GPU 档位也好不了，别浪费时间再试一遍
            Stop-Process -Id $script:EmuProc.Id -Force -ErrorAction SilentlyContinue
            Die "模拟器起不来（-gpu $g）：`n    $($wait.Reason)`n    日志：$(Get-LogFile $N)"
        }
        $tail = (Get-Content (Get-ErrFile $N) -Tail 2 -ErrorAction SilentlyContinue) -join " "
        if (-not $tail) { $tail = Get-EmuLastError (Get-LogFile $N) }
        Write-Warn "没起来（$tail）"
        Stop-Process -Id $script:EmuProc.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
    }
    if (-not $started) { Die "起不来；看 $(Get-LogFile $N) 和 $(Get-ErrFile $N)" }

    Write-Ok "已启动 '$N'（-gpu $used -memory $script:MemMB -cores $script:CoreN，端口 $(Get-InstancePort $N)）"
    if (-not (Add-ConsoleRedirect $p $script:Service.HttpPort $script:Service.GuestPort)) {
        Die "无法建立服务端口转发：宿主 $($script:Service.HttpPort) → guest $($script:Service.GuestPort)"
    }
    if ($NoWait) { Write-Host "后续： .\emulator.ps1 status $N"; return }

    Write-Log "等 remote-control 服务就绪（WHPX 下通常几十秒）"
    $svcWait = Wait-EmulatorCondition -Name $N -LogFile (Get-LogFile $N) -Test { Test-ServiceReady $p } -TimeoutSec 300
    if ($svcWait.State -eq "ready") { Write-Ok "开机完成"; return }
    if ($svcWait.State -eq "fatal") {
        Stop-Process -Id $script:EmuProc.Id -Force -ErrorAction SilentlyContinue
        Die "模拟器起不来：`n    $($svcWait.Reason)`n    日志：$(Get-LogFile $N)"
    }
    Die "等服务超时（300s）—— 看 $(Get-ErrFile $N)"
}

# ---------------------------------------------------------------------------
# stop / kill / restart
# ---------------------------------------------------------------------------
function Invoke-Stop {
    param([string]$N)
    Assert-Instance $N
    $p = Get-InstancePort $N
    if (Stop-Instance $p $TimeoutSec -Force:$Force) { Write-Ok "已停止 '$N'"; return $true }
    Write-Warn "'$N' 没停掉；检查服务令牌或显式 -Force 后重试"
    return $false
}

function Invoke-Kill {
    param([string]$N)
    Assert-Instance $N
    $p = Get-InstancePort $N
    if (Stop-Instance $p 0 -Force) { Write-Ok "已强制关闭 '$N'"; return }
    Die "'$N' 的进程杀不掉"
}

function Invoke-Restart {
    param([string]$N)
    Assert-Instance $N
    if (-not (Invoke-Stop $N)) { Die "实例未停止；未修改数据，请显式 -Force 后重试" }
    Invoke-Start $N
}

# ---------------------------------------------------------------------------
# reset —— 清数据分区，回出厂状态；实例本身留着
# ---------------------------------------------------------------------------
function Invoke-Reset {
    param([string]$N)
    Assert-Instance $N
    if (Test-InstanceRunning $N) { Write-Log "先停掉 '$N'"; if (-not (Invoke-Stop $N)) { Die "实例未停止；未修改数据，请显式 -Force 后重试" } }

    if (-not $Yes) {
        Write-Host "[!] 重置 '$N' 会清空数据分区（已装应用 / 应用数据 / sdcard / 快照）" -ForegroundColor Yellow
        $ans = Read-Host "    输入 yes 继续"
        if ($ans -ne "yes") { Die "已取消" }
    }

    $sysdir = Get-SysDir $N
    Write-Log "清掉数据分区与状态：.run\sysdir-$(Get-InstancePort $N)"
    # 只按名字精确删 —— 镜像在 sysdir 里是链接，别用通配去 rm，
    # 免得手滑删到共享的 images\。
    $state = @("userdata-qemu.img", "userdata-qemu.img.qcow2", "userdata-qemu.img.qcow2.lock",
               "cache.img", "cache.img.qcow2", "cache.img.qcow2.lock",
               "encryptionkey.img.qcow2", "bootcompleted.ini", "hardware-qemu.ini",
               "hardware-qemu.ini.lock", "multiinstance.lock", "emu-launch-params.txt",
               "version_num.cache", "read-snapshot.txt")
    foreach ($f in $state) { Remove-Item (Join-Path $sysdir $f) -Force -ErrorAction SilentlyContinue }
    foreach ($d in @("build.avd", "snapshots", "tmpAdbCmds")) {
        Remove-Item (Join-Path $sysdir $d) -Recurse -Force -ErrorAction SilentlyContinue
    }
    $datadir = Get-DataDir $N
    $dataItem = Get-Item -LiteralPath $datadir -Force -ErrorAction SilentlyContinue
    if ($dataItem -and ($dataItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        [IO.Directory]::Delete($dataItem.FullName, $false)
    } elseif ($dataItem) {
        Remove-Item -LiteralPath $datadir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $datadir | Out-Null
    Remove-Item -LiteralPath (Get-ServiceTokenFile $N) -Force -ErrorAction SilentlyContinue
    Register-Instance $N (Get-InstancePort $N)
    Write-Ok "已重置 '$N'（下次启动是全新机器）"
}

# ---------------------------------------------------------------------------
# delete
# ---------------------------------------------------------------------------
function Invoke-Delete {
    param([string]$N)
    Assert-Instance $N
    if (Test-InstanceRunning $N) { Write-Log "先停掉 '$N'"; if (-not (Invoke-Stop $N)) { Die "实例未停止；未修改数据，请显式 -Force 后重试" } }

    if (-not $Yes) {
        Write-Host "[!] 删除 '$N' 会删掉整个工作目录（含已装应用与快照）" -ForegroundColor Yellow
        $ans = Read-Host "    输入 yes 继续"
        if ($ans -ne "yes") { Die "已取消" }
    }

    $sysdir = Get-SysDir $N
    $p = Get-InstancePort $N
    # ⚠️ 用 -Recurse 删链接目录时要小心：junction 的 Remove-Item -Recurse
    #    在旧版 PowerShell 上会**跟着链接删到目标**。先显式摘掉 junction。
    foreach ($d in (Get-ChildItem $sysdir -Force -ErrorAction SilentlyContinue |
                    Where-Object { $_.LinkType -in @("Junction", "SymbolicLink") -and $_.PSIsContainer })) {
        [System.IO.Directory]::Delete($d.FullName, $false)
    }
    Remove-Item $sysdir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Get-DataDir $N) -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Get-LogFile $N) -Force -ErrorAction SilentlyContinue
    Remove-Item (Get-ErrFile $N) -Force -ErrorAction SilentlyContinue
    Unregister-Instance $N
    Write-Ok "已删除实例 '$N'"
}

# ---------------------------------------------------------------------------
# clone —— 连已装应用和状态一起复制出一台新的
# ---------------------------------------------------------------------------
function Invoke-Clone {
    param([string]$Src, [string]$Dst)
    if (-not $Src -or -not $Dst) { Die "用法： .\emulator.ps1 clone <源实例> <新实例>" }
    Assert-Instance $Src
    if (Test-Instance $Dst) { Die "实例 '$Dst' 已经存在" }
    if (Test-InstanceRunning $Src) {
        Write-Log "复制前先停掉源实例 '$Src'（跑着的时候 qcow2 还在写，抄出来是脏的）"
        if (-not (Invoke-Stop $Src)) { Die "源实例未停止；未复制数据，请显式 -Force 后重试" }
    }

    $p = if ($Port -gt 0) { $Port } else { New-FreePort }
    if (Test-PortTaken $p) { Die "端口 $p 已被别的实例占用" }

    $sSys = Get-SysDir $Src
    $dSys = Join-Path $script:RunDir "sysdir-$p"
    $dData = Join-Path $script:RunDir "datadir-$p"
    if ((Test-Path $dSys) -or (Test-Path $dData)) {
        Die "端口 $p 的目标目录已存在但没有实例登记：$dSys / $dData；请先检查并处理残留数据"
    }

    Register-Instance $Dst $p
    try {
        # 1) 先把**镜像**按新实例的目录重新链一遍（不复制实体）。
        Build-SysDir $Dst

        # 2) 再把**状态**文件抄过去。镜像同名的不抄（上面已经链好了）。
        Write-Log "复制状态（含已装应用）"
        New-Item -ItemType Directory -Force -Path $dSys | Out-Null
        $copied = 0
        foreach ($item in Get-ChildItem $sSys -Force) {
            if ($script:NoLink -contains $item.Name) { continue }   # initrd 等由模拟器自己重建
            if (Test-Path (Join-Path $script:ImagesDir $item.Name)) { continue }  # 镜像是链接，跳过
            $dest = Join-Path $dSys $item.Name
            if ($item.PSIsContainer) {
                Copy-Item $item.FullName $dest -Recurse -Force -ErrorAction Stop
            } else {
                Copy-Item $item.FullName $dest -Force -ErrorAction Stop
            }
            $copied++
        }
        $srcDat = Get-DataDir $Src
        New-Item -ItemType Directory -Force -Path $dData | Out-Null
        if (Test-Path $srcDat) {
            foreach ($item in Get-ChildItem $srcDat -Force) {
                Copy-Item $item.FullName $dData -Recurse -Force -ErrorAction Stop
            }
        }

        # 3) 路径/端口相关的残留全清掉，让模拟器按新路径重建
        foreach ($f in $script:StaleAfterClone) {
            $stale = Join-Path $dSys $f
            if (Test-Path $stale) { Remove-Item $stale -Force -ErrorAction Stop }
        }
        $snapshots = Join-Path $dSys "snapshots"
        if (Test-Path $snapshots) {
            Remove-Item $snapshots -Recurse -Force -ErrorAction Stop
            Write-Warn "源实例的快照没有复制（快照绑定了原来的硬件配置与路径）"
        }
        Copy-Item $script:ConfigFile (Join-Path $dSys "config.ini") -Force -ErrorAction Stop
        # /data contains the persisted daemon configuration too.  The clone's
        # host token must match it; only the host redirection port is new.
        $srcGuestPort = Get-InstanceValue $Src "SERVICE_PORT"
        if ($srcGuestPort) {
            Set-InstanceService $Dst (18088 + [int](($p - $script:PortBase) / 2)) ([int]$srcGuestPort) `
                (Get-InstanceValue $Src "SERVICE_ENABLED") (Get-InstanceValue $Src "SERVICE_AUTH") `
                (Get-InstanceValue $Src "SERVICE_BIND") (Get-InstanceValue $Src "SERVICE_ADB")
            if (Test-Path -LiteralPath (Get-ServiceTokenFile $Src)) {
                Copy-Item -LiteralPath (Get-ServiceTokenFile $Src) -Destination (Get-ServiceTokenFile $Dst) -Force
                Protect-ServiceTokenFile (Get-ServiceTokenFile $Dst)
            }
        }
    } catch {
        $copyError = $_
        try {
            Remove-Item $dSys, $dData -Recurse -Force -ErrorAction SilentlyContinue
            Unregister-Instance $Dst
        } catch {
            Write-Warn "复制失败后的目标清理也未完成，请检查 $dSys / $dData 与实例登记"
        }
        throw $copyError
    }

    Write-Ok "已复制： '$Src' → '$Dst'（端口 $p，$copied 项状态）"
    Write-Host "    下一步   .\emulator.ps1 start $Dst"
}

# ---------------------------------------------------------------------------
# list / status
# ---------------------------------------------------------------------------
function Get-InstanceState {
    param([string]$N)
    $p = Get-InstancePort $N
    if (@(Get-EmuProcess $p).Count -eq 0) { return "已停止" }
    if (Test-ServiceReady $p) { return "运行中(服务已就绪)" }
    return "运行中(启动中)"
}

function Get-DirSizeMB {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return "-" }
    try {
        $b = (Get-ChildItem $Path -Recurse -Force -ErrorAction SilentlyContinue |
              Measure-Object -Property Length -Sum).Sum
        return ("{0:N1}G" -f ($b / 1GB))
    } catch { return "-" }
}

function Invoke-List {
    $names = @(Get-InstanceNames)
    if ($names.Count -eq 0) {
        Write-Host "（还没有实例）"
        Write-Host "建一台： .\emulator.ps1 create $($script:DefaultName)"
        return
    }
    Write-Host ("{0,-12} {1,-7} {2,-16} {3,-9} {4}" -f "名字", "端口", "状态", "占用", "工作目录")
    Write-Host ("{0,-12} {1,-7} {2,-16} {3,-9} {4}" -f "------------", "-------", "----------------", "---------", "--------")
    foreach ($n in $names) {
        $p = Get-InstancePort $n
        Write-Host ("{0,-12} {1,-7} {2,-16} {3,-9} {4}" -f $n, $p, (Get-InstanceState $n),
                    (Get-DirSizeMB (Get-SysDir $n)), ".run\sysdir-$p")
    }
}

function Invoke-Status {
    param([string]$N)
    if (-not $N) { $N = $script:DefaultName }
    Assert-Instance $N
    $p = Get-InstancePort $N
    Write-Host "实例       $N"
    Write-Host "端口       $p"
    Write-Host "状态       $(Get-InstanceState $N)"
    Write-Host "串口       adb -s emulator-$p"
    Write-Host "工作目录   $(Get-SysDir $N)"
    Write-Host "日志       $(Get-LogFile $N)"
    Write-Host "错误日志   $(Get-ErrFile $N)"
    Write-Host ""
    Write-Host "config.ini（唯一真源 emulator\config.ini）："
    $keys = @("hw.lcd.width", "hw.lcd.height", "hw.lcd.density", "hw.cpu.ncore",
              "hw.ramSize", "hw.gpu.mode", "disk.dataPartition.size")
    foreach ($k in $keys) { Write-Host ("  {0,-24} {1}" -f $k, (Get-ConfigValue $k "-")) }

    $hw = Join-Path (Get-SysDir $N) "hardware-qemu.ini"
    if (Test-Path $hw) {
        Write-Host ""
        Write-Host "上次启动**实际生效**的（hardware-qemu.ini）："
        foreach ($k in $keys) {
            $hit = Select-String -Path $hw -Pattern "^$([regex]::Escape($k))\s*=\s*(.+?)\s*$" -ErrorAction SilentlyContinue |
                   Select-Object -Last 1
            $v = if ($hit) { $hit.Matches[0].Groups[1].Value } else { "-" }
            Write-Host ("  {0,-24} {1}" -f $k, $v)
        }
    }
}

# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------
if ($Help -or -not $Command) { Show-Usage; if (-not $Command) { exit 1 } else { exit 0 } }

New-Item -ItemType Directory -Force -Path $script:RunDir, $script:InstancesDir | Out-Null
if (-not (Test-Path $script:ConfigFile)) { Write-Warn "找不到 $($script:ConfigFile)，将用内置默认值" }

switch ($Command.ToLower()) {
    "create"  { Invoke-Create  $(if ($Name) { $Name } else { $script:DefaultName }) }
    "start"   { Invoke-Start   $(if ($Name) { $Name } else { $script:DefaultName }) }
    "stop"    { if (-not (Invoke-Stop $(if ($Name) { $Name } else { $script:DefaultName }))) { exit 1 } }
    "kill"    { Invoke-Kill    $(if ($Name) { $Name } else { $script:DefaultName }) }
    "restart" { Invoke-Restart $(if ($Name) { $Name } else { $script:DefaultName }) }
    "reset"   { Invoke-Reset   $(if ($Name) { $Name } else { $script:DefaultName }) }
    "delete"  { Invoke-Delete  $(if ($Name) { $Name } else { $script:DefaultName }) }
    "clone"   { Invoke-Clone $Name $Target }
    "list"    { Invoke-List }
    "ls"      { Invoke-List }
    "status"  { Invoke-Status $Name }
    default   { Die "未知命令：$Command（-Help 看用法）" }
}
