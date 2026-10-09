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
    [string]$ImagesDir   = "",
    [string]$DataDir     = "",
    [int]$Port           = 5580,
    # 0 / "" = 从 ..\emulator\config.ini 读（那份才是唯一真源）。
    # ⚠️ 别在这里写死默认值：命令行**优先于** config.ini，
    #    写死了 config.ini 里改什么都没用（Linux 侧 -memory 4096 就是这么
    #    把 hw.ramSize 架空的，两边都改过了）。
    [int]$MemoryMB       = 0,
    [int]$Cores          = 0,
    [string]$Gpu         = "",
    [string]$Accel       = "on",   # on=WHPX；off=纯软件（没装 WHPX 时用，很慢）
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

# ⚠️ **不要**把 `$PSScriptRoot` 写在 param 的默认值里。
#    带 `[CmdletBinding()]` 时参数绑定发生在 `$PSScriptRoot` 赋值**之前**，
#    默认值会算成 `\images` 这种丢掉了盘符和目录的路径，而且**不报错**。
#    2026-10-09 实测（同一台机器，只改这一处）：
#        param([string]$X = "$PSScriptRoot\images")            → [\images]   ✗
#        [CmdletBinding()] param([string]$X = "$PSScriptRoot\images") → [\images] ✗
#    于是镜像明明就在 `windows\images\` 里，预检却逐条报"缺 xxx.img"。
#    正确写法：默认值留空，绑定之后再补：
if (-not $ImagesDir) { $ImagesDir = Join-Path $PSScriptRoot "images" }
if (-not $DataDir) { $DataDir = Join-Path $PSScriptRoot "data" }

# ⚠️ 本脚本 `$ErrorActionPreference = "Stop"`，所以**所有原生命令都要把 stderr 一起并走**：
#    `2>&1 | Out-Null`，不能只写 `| Out-Null`。
#
#    `| Out-Null` 只吃 stdout；而 `adb push` 把进度、`pm uninstall` 把失败信息写到
#    **stderr**，PowerShell 5.1 会把它变成 ErrorRecord —— 在 Stop 下就是
#    **终止性 NativeCommandError**：脚本**就地静默退出**，一行提示都没有。
#
#    2026-10-09 实测：验收走到 4/4 就没了，日志停在"用自建 arm64 探针 APK"，
#    看着像卡住，其实是死在 `adb push`（它打印的 "1 file pushed ..." 走的正是 stderr）。
#    加了 try/catch 把 ScriptStackTrace 打出来才看见。
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
            -Accel $Accel `
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

# ⚠️ `/proc/sys/fs/binfmt_misc/` **只有 root adbd 读得到**（非 root 是
#    `ls: Permission denied`）。Linux 侧的 run-linux.sh 同样是先 `adb root`
#    再读，那里有注释记着这件事。
#
#    2026-10-09 在 Windows 上第一次真跑验收时，就是因为这里没提权：
#    2/4 的第三条读不到目录，脚本既拿不到 arm64_exe、也没把话说清楚。
#    提权后实测：`arm64_dyn arm64_exe arm_dyn arm_exe register status` ✓
$null = & $adb -s $serial root 2>&1
Start-Sleep -Seconds 5
$null = & $adb -s $serial wait-for-device 2>&1
$rooted = ((& $adb -s $serial shell id) -join "").Trim()
if ($rooted -notmatch 'uid=0') {
    Write-Host ("  [--] adbd 不是 root（$rooted）—— binfmt_misc 读不到，这一条会误报") -ForegroundColor DarkGray
}
$bf = ((& $adb -s $serial shell "ls /proc/sys/fs/binfmt_misc/ 2>/dev/null") -join " ")
Chk "binfmt_misc 注册" $bf 'arm64_exe'

Write-Host "==> 验收 3/4：aarch64 机器码（用设备上的原生库自证）"
$so = ((& $adb -s $serial shell "ls /system/lib64/arm64/libc.so") -join "").Trim()
Chk "arm64 系统库存在" $so '/system/lib64/arm64/libc\.so'
$elf = ((& $adb -s $serial shell "head -c 20 /system/lib64/arm64/libc.so | od -An -tx1") -join " ")
Chk "libc.so 是 aarch64 ELF" $elf 'b7'

Write-Host "==> 验收 4/4：arm64 应用"
$Pkg = ""
if (-not $Apk) {
    # ⚠️ **0 字节不算"有"**。`Test-Path` 只看存在性：一个拉取失败留下的
    #    空文件会让脚本以为探针就绪，然后去 `pm install` 一个空包。
    #    2026-10-09 真机上就这么卡住的（images\arm64-probe.apk = 0 字节）。
    $probe = "$ImagesDir\arm64-probe.apk"
    if (-not (Test-Path $probe) -or (Get-Item $probe).Length -eq 0) {
        $probe = "$PSScriptRoot\arm64-probe.apk"
    }
    if ((Test-Path $probe) -and (Get-Item $probe).Length -gt 0) {
        # 首选项目自建探针：只含 arm64-v8a 一个 ABI
        $Apk = $probe; $Pkg = "org.remotecontrol.arm64probe"
        Write-Host "==> 用自建 arm64 探针 APK"
    } else {
        $Apk = "$PSScriptRoot\com.oF2pks.kalturadeviceinfos_24.apk"; $Pkg = "com.oF2pks.kalturadeviceinfos"
        if ((-not (Test-Path $Apk)) -or (Get-Item $Apk -ErrorAction SilentlyContinue).Length -eq 0) {
            Write-Host "==> 下载验收 APK（F-Droid 镜像，312 KB）"
            & curl.exe -fL --retry 2 -o $Apk "https://mirrors.tuna.tsinghua.edu.cn/fdroid/repo/com.oF2pks.kalturadeviceinfos_24.apk"
        }
    }
} else {
    $Pkg = "org.remotecontrol.arm64probe"
}
if ((Test-Path $Apk) -and (Get-Item $Apk).Length -gt 0) {
    # ⚠️ 这一段里全是**原生命令**（adb push / pm install / monkey），它们会往 stderr 写
    #    进度和失败信息。本脚本开头是 `$ErrorActionPreference = "Stop"`，而
    #    PowerShell 5.1 会把原生 stderr 变成 **终止性 NativeCommandError** ——
    #    脚本就地静默退出。（`2>&1 | Out-Null` **不够**：记录类型仍是 error。）
    #    实测：`adb push` 打印 "1 file pushed ..."（走 stderr）就把脚本打死在
    #    这一行，日志停在"用自建 arm64 探针 APK"，看着像卡住。
    $eapSaved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    # 两个历史包名都卸掉再装 —— 否则"发现包名"可能发现到**上一轮残留的那个**，
    # 于是验收测的是旧包（Linux 侧 run-linux.sh 同样是两个都删）。
    foreach ($old in @("org.remotecontrol.arm64probe", "org.autosnap.arm64probe")) {
        & $adb -s $serial shell "pm uninstall $old" 2>&1 | Out-Null
    }
    & $adb -s $serial push $Apk /data/local/tmp/probe.apk 2>&1 | Out-Null
    $r = ((& $adb -s $serial shell "pm install --abi arm64-v8a -r /data/local/tmp/probe.apk") -join "").Trim()
    Chk "pm install --abi arm64-v8a" $r 'Success'

    # ⚠️ **装完从设备实际清单里发现包名**，别信脚本里写死的那个。
    #    探针包名改过不止一次（org.remotecontrol.arm64probe →
    #    org.autosnap.arm64probe），而交付目录里的 APK 可能是**不同一轮**构建出来的。
    #    Linux 侧的 run-linux.sh 早就这么做，还留了注释；Windows 侧一直是写死的，
    #    结果 monkey 找错包 → 第 4 项永远过不了（2026-10-09 实测）。
    $found = ((& $adb -s $serial shell "pm list packages -3 2>/dev/null | sed 's/^package://' | grep -i arm64probe") -join " ").Trim()
    if ($found) {
        $Pkg = ($found -split '\s+')[0]
        Write-Host ("  设备上实际包名：" + $Pkg)
    }
    # ⚠️ **先清空 logcat 缓冲**再启动。否则下面那条断言会被**上一次运行**
    #    留下的 "arm64-v8a native ok" 满足 —— 这一次哪怕 JNI 报 satisfiedLinkError
    #    也照样"通过"。2026-10-09 实测撞到过：新包里 JNI 符号是错的、
    #    日志里明明写着 satisfiedLinkError，验收却是绿的。
    & $adb -s $serial logcat -c 2>&1 | Out-Null
    & $adb -s $serial shell "monkey -p $Pkg -c android.intent.category.LAUNCHER 1" 2>&1 | Out-Null
    Start-Sleep -Seconds 6
    # ⚠️ 变量不能叫 `$pid` —— PowerShell 里 `$PID` 是**只读自动变量**（大小写不敏感），
    #    赋值会抛 VariableNotWritable。2026-10-09 实测：走到这里直接终止，
    #    所以第 4 项 **从来没能跑完过**。
    $probePid = ((& $adb -s $serial shell "pidof $Pkg") -join "").Trim()
    Chk "进程存活" $probePid '^\d+$'
    if ($probePid -match '^\d+$') {
        $n = ((& $adb -s $serial shell "grep -c '/system/lib64/arm64/' /proc/$probePid/maps") -join "").Trim()
        Chk "映射的 arm64 库条数" $n '^[1-9]\d*$'
    }
    if ($Pkg -eq "org.remotecontrol.arm64probe") {
        Start-Sleep -Seconds 2
        $line = ((& $adb -s $serial logcat -d -s ARM64PROBE) -join " ")
        Chk "探针原生返回值" $line 'arm64-v8a native ok'
    }
    $ErrorActionPreference = $eapSaved
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
