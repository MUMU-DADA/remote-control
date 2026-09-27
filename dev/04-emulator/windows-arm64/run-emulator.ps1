<#
.SYNOPSIS
    在 Windows x64 上启动 arm64 安卓 ROM（AOSP goldfish/ranchu 模拟器）。

.DESCRIPTION
    Windows 上没有 AOSP 自带的模拟器预编译包（prebuilts/android-emulator 只有
    linux/darwin），所以这里用 SDK 版 emulator.exe —— 它内含
    qemu\windows-x86_64\qemu-system-aarch64.exe，跨架构跑 arm64 guest 就靠它。

    说明：
      * arm64 guest 在 x86_64 宿主上是**纯软件模拟（TCG）**，首次开机 10~40 分钟，
        Windows 的 Hyper-V / WHPX 帮不上忙（那只服务 x86_64 guest）。
      * 默认开快照：第二次启动会快得多。行为异常时用 -WipeData -NoSnapshot 冷启动。
      * 参数名与 emulator 30.8.3 的 -help 逐条核对过，没用这个版本不存在的开关
        （比如 -no-metrics / -product 在 30.8.3 里没有）。

.EXAMPLE
    .\run-emulator.ps1
    .\run-emulator.ps1 -Gpu angle_indirect
    .\run-emulator.ps1 -WipeData -NoSnapshot
    .\run-emulator.ps1 -Headless -NoWait
    .\run-emulator.ps1 -Stop
#>
[CmdletBinding()]
param(
    # 模拟器目录（含 emulator.exe）。省略则自动探测
    [string]$EmulatorDir,

    # fetch-images.ps1 拉下来的镜像目录
    [string]$ImagesDir = (Join-Path $PSScriptRoot 'images'),

    # userdata 覆盖层放哪（别放 AOSP 的 out/ 里）
    [string]$DataDir = (Join-Path $PSScriptRoot 'data'),

    [string]$Adb,

    [int]$MemoryMB = 4096,
    [int]$Cores = 4,
    [int]$Port = 5554,

    # Windows 上 angle_indirect 走 DirectX，可能比 swiftshader 快；不行就退回 swiftshader_indirect
    [ValidateSet('swiftshader_indirect', 'angle_indirect', 'host', 'guest', 'off')]
    [string]$Gpu = 'swiftshader_indirect',

    [int]$BootTimeoutMin = 45,

    [switch]$Headless,        # 不显示模拟器窗口
    [switch]$NoSnapshot,      # 关快照（冷启动，便于复现问题）
    [switch]$WipeData,        # 清空 userdata
    [switch]$WritableSystem,  # 允许 adb remount（要推 /system/bin 时用）
    [switch]$NoWait,          # 起了就返回，不等开机
    [switch]$Stop             # 停掉该端口的模拟器
)

. "$PSScriptRoot\_common.ps1"

$serial = "emulator-$Port"

function Quote-Arg {
    param([string]$Value)
    if ($Value -match '\s') { return '"' + $Value + '"' }
    return $Value
}

# ---------------------------------------------------------------------------
$emulatorDir = Resolve-EmulatorDir -Explicit $EmulatorDir
if (-not $emulatorDir) {
    Stop-Script @"
没找到模拟器（emulator.exe）。

获取方式二选一：
  1. .\fetch-emulator.ps1                          # 自动下载
  2. 装过 Android Studio 的话直接指定：
     .\run-emulator.ps1 -EmulatorDir "`$env:LOCALAPPDATA\Android\Sdk\emulator"
"@
}

$adbExe = Resolve-Adb -Explicit $Adb

if ($Stop) {
    if (-not $adbExe) { Stop-Script '没有 adb，无法优雅停止；请用任务管理器结束 qemu-system-aarch64.exe' }
    & $adbExe -s $serial emu kill | Out-Null
    if ($LASTEXITCODE -eq 0) { Write-Ok "已停止 $serial" } else { Write-Note "adb 没找到 $serial，可能本来就没在跑" }
    exit 0
}

$exe = Join-Path $emulatorDir 'emulator.exe'

if (-not $adbExe) {
    Write-Note '没找到 adb —— 能启动但后面等开机要用它。可先跑 .\fetch-emulator.ps1'
}

Write-Step '检查镜像'
$required = @('system.img', 'vendor.img', 'ramdisk.img', 'kernel-ranchu')
$missing = @($required | Where-Object { -not (Test-Path (Join-Path $ImagesDir $_)) })
if ($missing.Count -gt 0) {
    Stop-Script @"
缺：$($missing -join ', ')
    目录：$ImagesDir

先跑： .\fetch-images.ps1
说明： aosp_arm64-userdebug 在 Android 12 是 GSI，产物里没有 kernel-ranchu/ramdisk，
       必须用开发机上的 sdk_phone64_arm64-userdebug 产物（emulator64_arm64/）。
"@
}
foreach ($f in $required) {
    $p = Join-Path $ImagesDir $f
    Write-Ok "$f  $(Format-Size (Get-Item $p).Length)"
}

$backend = Get-Arm64QemuBackend -EmulatorDir $emulatorDir
if ($backend) {
    Write-Ok "arm64 QEMU 后端：$(Split-Path -Leaf $backend)"
} else {
    Stop-Script "这个模拟器目录里没有 qemu-system-aarch64.exe，无法跑 arm64 镜像：$emulatorDir"
}

if (-not (Test-Path $DataDir)) { New-Item -ItemType Directory -Path $DataDir -Force | Out-Null }

# ---------------------------------------------------------------------------
$argList = New-Object System.Collections.Generic.List[string]
foreach ($a in @(
    '-sysdir',   (Quote-Arg $ImagesDir),
    '-datadir',  (Quote-Arg $DataDir),
    '-port',     "$Port",
    '-gpu',      "$Gpu",
    '-memory',   "$MemoryMB",
    '-cores',    "$Cores",
    '-accel',    'off',            # arm64 guest 在 x86_64 宿主上只能 TCG
    '-no-boot-anim',
    '-no-audio'
)) { $argList.Add($a) }

if ($Headless)       { $argList.Add('-no-window') }
if ($NoSnapshot)     { $argList.Add('-no-snapshot') }
if ($WipeData)       { $argList.Add('-wipe-data') }
if ($WritableSystem) { $argList.Add('-writable-system') }

$outLog = Join-Path $DataDir "emulator-$Port.out.log"
$errLog = Join-Path $DataDir "emulator-$Port.err.log"

Write-Step "启动模拟器"
Write-Host "  $exe $($argList -join ' ')" -ForegroundColor DarkGray
Write-Note "arm64 guest / x86_64 宿主 → 纯软件模拟，首次开机 10~40 分钟（不是卡住了）"
if (-not $Headless) { Write-Note '首次运行 Windows 防火墙可能弹窗，允许即可' }

$proc = Start-Process -FilePath $exe -ArgumentList $argList -PassThru `
        -RedirectStandardOutput $outLog -RedirectStandardError $errLog `
        -WorkingDirectory $PSScriptRoot

Write-Ok "PID $($proc.Id)   日志：$outLog"

if ($NoWait) {
    Write-Host ''
    Write-Host '后续手动查看：' -ForegroundColor Cyan
    Write-Host "  `"$adbExe`" -s $serial shell getprop sys.boot_completed"
    exit 0
}

if (-not $adbExe) { Stop-Script '需要 adb 才能等开机；先装 platform-tools（fetch-emulator.ps1）后加 -Adb 参数重跑' }

# ---------------------------------------------------------------------------
Write-Step '等待设备上线'
& $adbExe -s $serial wait-for-device

Write-Step "等待开机完成（最多 $BootTimeoutMin 分钟）"
$deadline = (Get-Date).AddMinutes($BootTimeoutMin)
$booted = $false

while ((Get-Date) -lt $deadline) {
    if ($proc.HasExited) {
        Write-Fail "模拟器进程已退出（exit $($proc.ExitCode)）。日志尾部："
        if (Test-Path $errLog) { Get-Content $errLog -Tail 20 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray } }
        if (Test-Path $outLog) { Get-Content $outLog -Tail 20 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray } }
        Stop-Script '启动失败'
    }
    $bc = (& $adbExe -s $serial shell getprop sys.boot_completed 2>$null | Out-String).Trim()
    if ($bc -eq '1') { $booted = $true; break }
    $elapsed = [int]((Get-Date) - $proc.StartTime).TotalSeconds
    Write-Host -NoNewline ("`r  已等待 {0:mm\:ss} …" -f ([TimeSpan]::FromSeconds($elapsed)))
    Start-Sleep -Seconds 15
}
Write-Host ''

if (-not $booted) {
    Stop-Script "超过 $BootTimeoutMin 分钟还没起来。看日志：$outLog（TCG 下偶尔更久，可加 -BootTimeoutMin 90 重试）"
}

Write-Ok '开机完成'

# ---------------------------------------------------------------------------
& $adbExe -s $serial root 2>$null | Out-Null
& $adbExe -s $serial wait-for-device

Write-Step '设备信息'
$rows = @(
    @('serial',     $serial),
    @('型号',       (& $adbExe -s $serial shell getprop ro.product.model | Out-String).Trim()),
    @('Android',    (& $adbExe -s $serial shell getprop ro.build.version.release | Out-String).Trim()),
    @('ABI',        (& $adbExe -s $serial shell getprop ro.product.cpu.abi | Out-String).Trim()),
    @('build type', (& $adbExe -s $serial shell getprop ro.build.type | Out-String).Trim()),
    @('屏幕',       (& $adbExe -s $serial shell wm size | Out-String).Trim()),
    @('uid',        (& $adbExe -s $serial shell id -u | Out-String).Trim())
)
foreach ($r in $rows) { Write-Host ("  {0,-12} {1}" -f $r[0], $r[1]) }

$uinput = (& $adbExe -s $serial shell 'test -e /dev/uinput && echo yes || echo no' | Out-String).Trim()
if ($uinput -eq 'yes') {
    Write-Ok '/dev/uinput 存在 —— autod 的 uinput 触控后端可用'
} else {
    & $adbExe -s $serial shell 'modprobe uinput' 2>$null | Out-Null
    $uinput = (& $adbExe -s $serial shell 'test -e /dev/uinput && echo yes || echo no' | Out-String).Trim()
    if ($uinput -eq 'yes') { Write-Ok 'modprobe 后 /dev/uinput 就绪' }
    else { Write-Note '没有 /dev/uinput —— 截图不受影响，触控只能等真机验证' }
}

Write-Host ''
Write-Host '常用命令：' -ForegroundColor Cyan
Write-Host "  `"$adbExe`" -s $serial shell"
Write-Host "  `"$adbExe`" -s $serial logcat -s autod:*"
Write-Host "  .\run-emulator.ps1 -Stop"
