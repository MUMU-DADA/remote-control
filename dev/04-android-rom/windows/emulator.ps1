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
    [switch]$Help
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$script:DefaultName   = "default"
$script:PortBase      = 5580
$script:X64Dir        = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$script:ConfigFile    = Join-Path $script:X64Dir "emulator\config.ini"
$script:ImagesDir     = Join-Path $PSScriptRoot "images"
$script:RunDir        = Join-Path $PSScriptRoot ".run"
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
function Find-Adb {
    $cands = @(
        (Join-Path $PSScriptRoot "sdk\platform-tools\adb.exe"),
        (Join-Path $env:ANDROID_SDK_ROOT "platform-tools\adb.exe"),
        (Join-Path $env:ANDROID_HOME     "platform-tools\adb.exe"),
        (Join-Path $env:LOCALAPPDATA     "Android\Sdk\platform-tools\adb.exe")
    )
    foreach ($c in $cands) { if ($c -and (Test-Path $c)) { return $c } }
    $cmd = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    Die "找不到 adb.exe（先跑 .\fetch-emulator.ps1，或用 Android SDK 的 platform-tools）"
}

function Find-Emulator {
    $cands = @(
        (Join-Path $PSScriptRoot "sdk\emulator\emulator.exe"),
        (Join-Path $env:ANDROID_SDK_ROOT "emulator\emulator.exe"),
        (Join-Path $env:ANDROID_HOME     "emulator\emulator.exe"),
        (Join-Path $env:LOCALAPPDATA     "Android\Sdk\emulator\emulator.exe")
    )
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

function Unregister-Instance { param([string]$N) Remove-Item (Get-InstanceFile $N) -Force -ErrorAction SilentlyContinue }

function Assert-Instance { param([string]$N) if (-not (Test-Instance $N)) { Die "没有叫 '$N' 的实例（.\emulator.ps1 list 看有哪些）" } }

function Get-SysDir  { param([string]$N) Join-Path $script:RunDir "sysdir-$(Get-InstancePort $N)" }
function Get-DataDir { param([string]$N) Join-Path $script:RunDir "datadir-$(Get-InstancePort $N)" }
function Get-LogFile { param([string]$N) Join-Path $script:RunDir "emulator-$(Get-InstancePort $N).log" }
function Get-ErrFile { param([string]$N) Join-Path $script:RunDir "emulator-$(Get-InstancePort $N).err.log" }
function Get-Serial  { param([string]$N) "emulator-$(Get-InstancePort $N)" }

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
    Write-Host ("    数据分区 {0}（实际占用看 qcow2 长到多大）" -f (Get-ConfigValue "disk.dataPartition.size" "32G"))
    Write-Host "    下一步   .\emulator.ps1 start $N"
}

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
    if (-not $Gui) { $a += "-no-window" }

    Remove-Item $logf, $errf -Force -ErrorAction SilentlyContinue
    $script:EmuProc = Start-Process -FilePath $Emu -ArgumentList $a -PassThru -WindowStyle Hidden `
                                    -RedirectStandardOutput $logf -RedirectStandardError $errf
}

function Wait-Adb {
    param([string]$Serial, [int]$TimeoutSec = 60)
    $adb = Find-Adb
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $state = (& $adb -s $Serial get-state 2>$null) -join ""
        if ($state.Trim() -eq "device") { return $true }
        if ($script:EmuProc -and $script:EmuProc.HasExited) { return $false }
        Start-Sleep -Seconds 5
    }
    return $false
}

function Wait-Boot {
    param([string]$Serial, [int]$TimeoutSec = 300)
    $adb = Find-Adb
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $bc = (& $adb -s $Serial shell getprop sys.boot_completed 2>$null) -join ""
        if ($bc.Trim() -eq "1") { return $true }
        Start-Sleep -Seconds 3
    }
    return $false
}

function Invoke-Start {
    param([string]$N)
    if (-not (Test-Instance $N)) { Write-Log "实例 '$N' 不存在，先创建"; Invoke-Create $N }
    if (Test-InstanceRunning $N) { Write-Warn "实例 '$N' 已经在跑（端口 $(Get-InstancePort $N)）"; return }

    $emu = Find-Emulator
    Resolve-Hw
    $serial = Get-Serial $N

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
        if (Wait-Adb $serial 60) { $started = $true; $used = $g; break }
        $tail = (Get-Content (Get-ErrFile $N) -Tail 2 -ErrorAction SilentlyContinue) -join " "
        Write-Warn "没起来（$tail）"
        Stop-Process -Id $script:EmuProc.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
    }
    if (-not $started) { Die "起不来；看 $(Get-LogFile $N) 和 $(Get-ErrFile $N)" }

    Write-Ok "已启动 '$N'（-gpu $used -memory $script:MemMB -cores $script:CoreN，端口 $(Get-InstancePort $N)）"
    if ($NoWait) { Write-Host "后续： adb -s $serial shell getprop sys.boot_completed"; return }

    Write-Log "等开机完成（WHPX 下通常几十秒）"
    if (-not (Wait-Boot $serial 300)) { Write-Warn "等开机超时（300s）—— 看 $(Get-ErrFile $N)"; return }
    $adb = Find-Adb
    & $adb -s $serial root 2>$null | Out-Null
    & $adb -s $serial wait-for-device 2>$null | Out-Null
    Write-Ok "开机完成"
}

# ---------------------------------------------------------------------------
# stop / kill / restart
# ---------------------------------------------------------------------------
function Invoke-Stop {
    param([string]$N)
    Assert-Instance $N
    $p = Get-InstancePort $N
    $procs = @(Get-EmuProcess $p)
    if ($procs.Count -eq 0) { Write-Warn "实例 '$N' 没在跑"; return $true }

    # ⚠️⚠️ 关机前先让 guest 把脏页落盘。
    #
    #    `adb emu kill` **不是优雅关机，是硬断电** —— QEMU 收到信号就立刻终止，
    #    guest 没机会卸载文件系统或提交日志。
    #    实测（Linux 侧 tools/verify-kill-is-hard-poweroff.sh，同一台实例
    #    三组对照）：sync 后再关 → 数据在；不 sync 直接关 → 数据没了；
    #    不 sync 等 15 秒再关 → **还是没了**。
    #    后果不是"丢最后一点"，而是最近写的东西整个没，且毫无征兆 ——
    #    项目早期那条"模拟器 /data 不持久（根因未查明）"就是它。
    $adb = Find-Adb
    Write-Log "让 guest 把脏页落盘（sync）"
    & $adb -s (Get-Serial $N) shell sync 2>$null | Out-Null
    Start-Sleep -Seconds 1

    Write-Log "请 guest 自己关机（adb emu kill）"
    & $adb -s (Get-Serial $N) emu kill 2>$null | Out-Null

    # ⚠️ 等的是**进程真的退出**，不是"命令返回了"。emu kill 只是递个关机请求，
    #    guest 还要走完流程（卸载 /data、收 qcow2）；这中间就重启会撞上
    #    multiinstance.lock，第二台报 another emulator instance is running。
    for ($i = 0; $i -lt 30; $i++) {
        if (@(Get-EmuProcess $p).Count -eq 0) { Write-Ok "已停止 '$N'"; return $true }
        Start-Sleep -Seconds 2
    }
    Write-Warn "'$N' 60 秒还没退（guest 卡住了？）—— 用 kill 强制关闭"
    return $false
}

function Invoke-Kill {
    param([string]$N)
    Assert-Instance $N
    $p = Get-InstancePort $N
    $procs = @(Get-EmuProcess $p)
    if ($procs.Count -eq 0) { Write-Warn "实例 '$N' 没在跑"; return }

    Write-Log "强制关闭 '$N'（Stop-Process -Force）"
    # 先**落盘**、再请它关、最后强杀。emu kill 本身已经是硬断电
    # （见 stop 里那段实测），SIGKILL 只会更狠 —— 不 sync 的话
    # 最近写入的数据会静默消失。多花一秒换数据安全，值。
    $adb = Find-Adb
    & $adb -s (Get-Serial $N) shell sync 2>$null | Out-Null
    & $adb -s (Get-Serial $N) emu kill 2>$null | Out-Null
    Start-Sleep -Seconds 2
    foreach ($pr in @(Get-EmuProcess $p)) { Stop-Process -Id $pr.ProcessId -Force -ErrorAction SilentlyContinue }

    for ($i = 0; $i -lt 15; $i++) {
        if (@(Get-EmuProcess $p).Count -eq 0) { Write-Ok "已强制关闭 '$N'"; return }
        Start-Sleep -Seconds 1
    }
    Die "'$N' 的进程杀不掉，手动看： Get-Process qemu-system-*"
}

function Invoke-Restart {
    param([string]$N)
    Assert-Instance $N
    if (-not (Invoke-Stop $N)) { Invoke-Kill $N }
    Invoke-Start $N
}

# ---------------------------------------------------------------------------
# reset —— 清数据分区，回出厂状态；实例本身留着
# ---------------------------------------------------------------------------
function Invoke-Reset {
    param([string]$N)
    Assert-Instance $N
    if (Test-InstanceRunning $N) { Write-Log "先停掉 '$N'"; if (-not (Invoke-Stop $N)) { Invoke-Kill $N } }

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
    New-Item -ItemType Directory -Force -Path (Get-DataDir $N) | Out-Null
    Write-Ok "已重置 '$N'（下次启动是全新机器）"
}

# ---------------------------------------------------------------------------
# delete
# ---------------------------------------------------------------------------
function Invoke-Delete {
    param([string]$N)
    Assert-Instance $N
    if (Test-InstanceRunning $N) { Write-Log "先停掉 '$N'"; if (-not (Invoke-Stop $N)) { Invoke-Kill $N } }

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
        if (-not (Invoke-Stop $Src)) { Invoke-Kill $Src }
    }

    $p = if ($Port -gt 0) { $Port } else { New-FreePort }
    if (Test-PortTaken $p) { Die "端口 $p 已被别的实例占用" }

    $sSys = Get-SysDir $Src
    Register-Instance $Dst $p
    $dSys = Get-SysDir $Dst

    # 1) 先把**镜像**按新实例的目录重新链一遍（不复制实体）——
    #    共享的只读镜像本来就不该复制，几 GB 白拷。
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
            Copy-Item $item.FullName $dest -Recurse -Force -ErrorAction SilentlyContinue
        } else {
            Copy-Item $item.FullName $dest -Force -ErrorAction SilentlyContinue
        }
        $copied++
    }
    $srcDat = Get-DataDir $Src
    if (Test-Path $srcDat) { Copy-Item "$srcDat\*" (Get-DataDir $Dst) -Recurse -Force -ErrorAction SilentlyContinue }
    else { New-Item -ItemType Directory -Force -Path (Get-DataDir $Dst) | Out-Null }

    # 3) 路径/端口相关的残留全清掉，让模拟器按新路径重建
    foreach ($f in $script:StaleAfterClone) { Remove-Item (Join-Path $dSys $f) -Force -ErrorAction SilentlyContinue }
    if (Test-Path (Join-Path $dSys "snapshots")) {
        Remove-Item (Join-Path $dSys "snapshots") -Recurse -Force -ErrorAction SilentlyContinue
        Write-Warn "源实例的快照没有复制（快照绑定了原来的硬件配置与路径）"
    }
    Copy-Item $script:ConfigFile (Join-Path $dSys "config.ini") -Force

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
    $adb = Find-Adb
    $bc = (& $adb -s "emulator-$p" shell getprop sys.boot_completed 2>$null) -join ""
    if ($bc.Trim() -eq "1") { return "运行中(已开机)" }
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
    "stop"    { [void](Invoke-Stop  $(if ($Name) { $Name } else { $script:DefaultName })) }
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
