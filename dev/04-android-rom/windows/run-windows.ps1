<#
.SYNOPSIS
  在 x64 Windows 上启动自编 ROM（WHPX 加速）并做 arm64 应用验收。

.EXAMPLE
  .\run-windows.ps1
  .\run-windows.ps1 -Headless -NoWait
  .\run-windows.ps1 -Verify          # 对已在跑的实例只做验收
  .\run-windows.ps1 -Stop
#>
[CmdletBinding()]
param(
    [string]$EmulatorDir = "",
    [string]$ImagesDir   = "$PSScriptRoot\images",
    [string]$DataDir     = "$PSScriptRoot\data",
    [int]$Port           = 5580,
    # 0 / "" = 从 ..\emulator\config.ini 读（那份才是唯一真源）。
    # ⚠️ 别在这里写死默认值：命令行**优先于** config.ini，
    #    写死了 config.ini 里改什么都没用（Linux 侧 -memory 4096 就是这么
    #    把 hw.ramSize 架空的，两边都改过了）。
    [int]$MemoryMB       = 0,
    [int]$Cores          = 0,
    [string]$Gpu         = "",
    [string]$Apk         = "",
    [switch]$Headless,
    [switch]$NoSnapshot,
    [switch]$WipeData,
    [switch]$NoWait,
    [switch]$Verify,
    [switch]$Stop,
    [switch]$Force,
    [switch]$TestInstance
)

$ErrorActionPreference = "Stop"
$serial = "emulator-$Port"
$LASTEXITCODE = 0

function Find-Adb {
    foreach ($c in @("$PSScriptRoot\sdk\platform-tools\adb.exe",
                     "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe",
                     "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe")) {
        if (Test-Path $c) { return $c }
    }
    $cmd = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "找不到 adb.exe"
}

# Reuse the named-instance lifecycle path.  Normal start/stop uses local
# console + HTTP and therefore works even when guest ADB is disabled.
$manager = Join-Path $PSScriptRoot "emulator.ps1"
$instance = "run-$Port"
$previousEnv = @{}
$overrides = @{
    AUTOSNAP_IMAGES = $ImagesDir
    AUTOSNAP_RUN_DIR = (Join-Path $DataDir ".instances")
    AUTOSNAP_DATA_DIR = (Join-Path $DataDir "guest")
}
if ($EmulatorDir) { $overrides["AUTOSNAP_EMULATOR"] = Join-Path $EmulatorDir "emulator.exe" }
try {
    foreach ($key in $overrides.Keys) {
        $previousEnv[$key] = [Environment]::GetEnvironmentVariable($key, "Process")
        [Environment]::SetEnvironmentVariable($key, $overrides[$key], "Process")
    }
    if ($Stop) {
        & $manager stop $instance -Port $Port -Force:$Force
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        exit 0
    }
    if (-not $Verify) {
        if ($WipeData -and (Test-Path (Join-Path $overrides["AUTOSNAP_RUN_DIR"] "instances\$instance.env"))) {
            & $manager reset $instance -Yes -Force:$Force
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
        & $manager start $instance -Port $Port -Memory $MemoryMB -Cores $Cores -Gpu $Gpu `
            -Gui:(-not $Headless) -NoWait:$NoWait -TestInstance:$TestInstance
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
} finally {
    foreach ($key in $overrides.Keys) { [Environment]::SetEnvironmentVariable($key, $previousEnv[$key], "Process") }
}
if (-not $Verify) {
    Write-Host "==> 已启动；用 -Verify 执行需要 ADB 的设备验收"
    exit 0
}
$adb = Find-Adb

# ---------------------------------------------------------------- 验收
function Get-Prop([string]$name) {
    ((& $adb -s $serial shell getprop $name) -join "").Trim()
}
$fails = 0
function Chk([string]$label, [string]$actual, [string]$pattern) {
    if ($actual -match $pattern) { Write-Host ("  [OK] {0,-34} {1}" -f $label, $actual) -ForegroundColor Green }
    else { Write-Host ("  [!!] {0,-34} {1}  (expect {2})" -f $label, $actual, $pattern) -ForegroundColor Red; $script:fails++ }
}

Write-Host "==> 验收 1/4：镜像身份与 ABI"
Chk "ro.build.version.sdk"   (Get-Prop "ro.build.version.sdk")   '^31$'
Chk "ro.product.device"      (Get-Prop "ro.product.device")      'remote_control_x64_arm64'
Chk "ro.product.cpu.abilist" (Get-Prop "ro.product.cpu.abilist") 'x86_64,arm64-v8a'

Write-Host "==> 验收 2/4：翻译层接线"
Chk "native.bridge"        (Get-Prop "ro.dalvik.vm.native.bridge")   'libndk_translation\.so'
Chk "enable.native.bridge.exec" (Get-Prop "ro.enable.native.bridge.exec") '^1$'
$bf = ((& $adb -s $serial shell "ls /proc/sys/fs/binfmt_misc/") -join " ")
Chk "binfmt_misc 注册" $bf 'arm64_exe'

Write-Host "==> 验收 3/4：aarch64 机器码（用设备上的原生库自证）"
$so = ((& $adb -s $serial shell "ls /system/lib64/arm64/libc.so") -join "").Trim()
Chk "arm64 系统库存在" $so '/system/lib64/arm64/libc\.so'
$elf = ((& $adb -s $serial shell "head -c 20 /system/lib64/arm64/libc.so | od -An -tx1") -join " ")
Chk "libc.so 是 aarch64 ELF" $elf 'b7'

Write-Host "==> 验收 4/4：arm64 应用"
$Pkg = ""
if (-not $Apk) {
    $probe = "$ImagesDir\arm64-probe.apk"
    if (-not (Test-Path $probe)) { $probe = "$PSScriptRoot\arm64-probe.apk" }
    if (Test-Path $probe) {
        # 首选项目自建探针：只含 arm64-v8a 一个 ABI
        $Apk = $probe; $Pkg = "org.remotecontrol.arm64probe"
        Write-Host "==> 用自建 arm64 探针 APK"
    } else {
        $Apk = "$PSScriptRoot\com.oF2pks.kalturadeviceinfos_24.apk"; $Pkg = "com.oF2pks.kalturadeviceinfos"
        if (-not (Test-Path $Apk)) {
            Write-Host "==> 下载验收 APK（F-Droid 镜像，312 KB）"
            & curl.exe -fL --retry 2 -o $Apk "https://mirrors.tuna.tsinghua.edu.cn/fdroid/repo/com.oF2pks.kalturadeviceinfos_24.apk"
        }
    }
} else {
    $Pkg = "org.remotecontrol.arm64probe"
}
if (Test-Path $Apk) {
    & $adb -s $serial shell "pm uninstall $Pkg" 2>$null | Out-Null
    & $adb -s $serial push $Apk /data/local/tmp/probe.apk | Out-Null
    $r = ((& $adb -s $serial shell "pm install --abi arm64-v8a -r /data/local/tmp/probe.apk") -join "").Trim()
    Chk "pm install --abi arm64-v8a" $r 'Success'
    & $adb -s $serial shell "monkey -p $Pkg -c android.intent.category.LAUNCHER 1" 2>$null | Out-Null
    Start-Sleep -Seconds 6
    $pid = ((& $adb -s $serial shell "pidof $Pkg") -join "").Trim()
    Chk "进程存活" $pid '^\d+$'
    if ($pid -match '^\d+$') {
        $n = ((& $adb -s $serial shell "grep -c '/system/lib64/arm64/' /proc/$pid/maps") -join "").Trim()
        Chk "映射的 arm64 库条数" $n '^[1-9]\d*$'
    }
    if ($Pkg -eq "org.remotecontrol.arm64probe") {
        Start-Sleep -Seconds 2
        $line = ((& $adb -s $serial logcat -d -s ARM64PROBE) -join " ")
        Chk "探针原生返回值" $line 'arm64-v8a native ok'
    }
} else {
    Write-Host "  [!!] 没有 APK，跳过第 4 项" -ForegroundColor Yellow
}

Write-Host ""
# ⚠️ 这里原来是写成一行的：
#     Write-Host "==> ROM 指纹： $(((& $adb ... ) -join "").Trim())"
#    双引号字符串里的 $( ) 里再出现 ""，PowerShell 的词法分析会当场崩
#    （The string is missing the terminator）—— 也就是说这个脚本
#    **从来没能运行过**。先取到变量再拼字符串就没这个问题。
#    现在 tools/test-windows-emulator.sh 会解析所有 .ps1，这类错不会再溜过去。
$fp = ((& $adb -s $serial shell getprop ro.build.fingerprint) -join "").Trim()
Write-Host "==> ROM 指纹： $fp"
Write-Host "    与 Linux 侧对照这个值 + images\system.img 的 sha256，即可证明是同一份 ROM"
if ($fails -eq 0) { Write-Host "==> 验收全部通过 ✓" -ForegroundColor Green }
else { Write-Host "==> 有 $fails 项未通过" -ForegroundColor Red; exit 1 }
