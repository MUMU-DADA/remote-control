<#
.SYNOPSIS
  验收：这台机器是不是真的在跑这份 ROM，而且 **arm64 应用真的能跑**（Windows 侧）。

.DESCRIPTION
  四组检查（与 Linux 侧 bin/linux/verify.sh、上游 run-windows.ps1 同一口径）：
    1. 镜像身份与 ABI       SDK=31 / device / abilist 含 arm64-v8a
    2. 翻译层接线           native.bridge / exec / binfmt_misc 注册
    3. aarch64 机器码在跑   自带静态 aarch64 ELF 直接执行 → ARM64_OK
    4. arm64 应用           纯 arm64-v8a 探针 APK：装 → 起 → 映射 arm64 库 → 原生返回值

.EXAMPLE
  .\bin\verify.ps1
  .\bin\verify.ps1 -Port 5584
#>
[CmdletBinding()]
param(
    [string]$Name = "",
    [int]$Port = 0
)

. (Join-Path $PSScriptRoot "common.ps1")
if ([string]::IsNullOrWhiteSpace($Name)) { $Name = $script:DefaultName }
if ($Port -le 0) { $Port = Get-InstancePort $Name }
if ($Port -le 0) { Die "实例 '$Name' 没登记过端口（先 .\bin\start-headless.ps1）" }

Assert-Adb
$serial = Get-Serial $Port
if (-not (Test-InstanceRunning $Port)) { Die "端口 $Port 上没有模拟器在跑" }

$fails = 0
function Chk {
    param([string]$Label, [string]$Actual, [string]$Pattern)
    if ($Actual -match $Pattern) {
        Write-Host ("  [OK] {0,-34} {1}" -f $Label, $Actual) -ForegroundColor Green
    } else {
        Write-Host ("  [!!] {0,-34} {1}  (expect {2})" -f $Label, $Actual, $Pattern) -ForegroundColor Red
        $script:fails++
    }
}
function Sh {
    param([string]$Cmd)
    return ((& $script:Adb -s $serial shell $Cmd 2>$null) -join "").Trim()
}

Write-Log "等设备就绪"
& $script:Adb -s $serial wait-for-device 2>$null | Out-Null

Write-Log "验收 1/4：镜像身份与 ABI"
Chk "ro.build.version.sdk"   (Get-Prop $serial "ro.build.version.sdk")   '^31$'
Chk "ro.product.device"      (Get-Prop $serial "ro.product.device")      'remote_control_x64_arm64'
Chk "ro.product.cpu.abilist" (Get-Prop $serial "ro.product.cpu.abilist") 'x86_64,arm64-v8a'

Write-Log "验收 2/4：翻译层接线"
Chk "ro.dalvik.vm.native.bridge"   (Get-Prop $serial "ro.dalvik.vm.native.bridge")   'libndk_translation\.so'
Chk "ro.enable.native.bridge.exec" (Get-Prop $serial "ro.enable.native.bridge.exec") '^1$'
# ⚠️ `ls` 是字母序（arm64_dyn 在前），拆成两条独立检查，别写成顺序依赖的正则
$bf = Sh 'ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | tr "\n" " "'
Chk "binfmt_misc: arm64_exe" $bf 'arm64_exe'
Chk "binfmt_misc: arm64_dyn" $bf 'arm64_dyn'

Write-Log "验收 3/4：aarch64 机器码真的在跑"
$probe = Join-Path $script:Tools "arm64-probe"
if (Test-Path $probe) {
    & $script:Adb -s $serial push $probe /data/local/tmp/arm64-probe 2>$null | Out-Null
    Sh "chmod 755 /data/local/tmp/arm64-probe" | Out-Null
    Chk "aarch64 静态 ELF 执行" (Sh "/data/local/tmp/arm64-probe") 'ARM64_OK'
} else {
    # 包里没带探针时的退路：自证 /system/lib64/arm64/libc.so 是 aarch64 ELF（e_machine=0xB7）
    Chk "arm64 系统库存在"     (Sh "ls /system/lib64/arm64/libc.so") '/system/lib64/arm64/libc\.so'
    Chk "libc.so 是 aarch64 ELF" (Sh "head -c 20 /system/lib64/arm64/libc.so | od -An -tx1") 'b7'
}

Write-Log "验收 4/4：arm64 应用（纯 arm64-v8a 探针 APK）"
# 探针包名**从设备上发现**，不写死：这个 APK 改过名
# （org.remotecontrol.arm64probe → org.autosnap.arm64probe），写死就会出现
# "装上了 Success，却 monkey 找不到 Activity"（Linux 侧实测踩过）。
function Get-ProbePackage {
    $p = Sh 'pm list packages -3 2>/dev/null | sed "s/^package://" | grep -i probe | sed -n "1p"'
    if (-not $p) { $p = Sh 'pm list packages -3 2>/dev/null | sed "s/^package://" | sed -n "1p"' }
    return $p
}
$apk = Join-Path $script:Tools "arm64-probe.apk"
if (Test-Path $apk) {
    $old = Get-ProbePackage
    if ($old) { & $script:Adb -s $serial shell "pm uninstall $old" 2>$null | Out-Null }
    & $script:Adb -s $serial push $apk /data/local/tmp/probe.apk 2>$null | Out-Null
    Chk "pm install --abi arm64-v8a" (Sh "pm install --abi arm64-v8a -r /data/local/tmp/probe.apk") 'Success'
    $pkg = Get-ProbePackage
    if (-not $pkg) {
        Chk "探针包名（pm list packages -3）" "" '.+'
    } else {
        # ⚠️ 启动前先清 logcat，否则会匹配到上一轮残留的 PROBE_RESULT（假绿，踩过）
        & $script:Adb -s $serial logcat -c 2>$null | Out-Null
        Sh "monkey -p $pkg -c android.intent.category.LAUNCHER 1" | Out-Null
        Start-Sleep -Seconds 6
        $appPid = Sh "pidof $pkg"
        Chk "进程存活（$pkg）" $appPid '^[0-9]+$'
        if ($appPid -match '^[0-9]+$') {
            Chk "映射的 arm64 库条数" (Sh "grep -c '/system/lib64/arm64/' /proc/$appPid/maps") '^[1-9][0-9]*$'
        }
        Chk "primaryCpuAbi" (Sh "pm dump $pkg 2>/dev/null | grep -m1 primaryCpuAbi") 'arm64-v8a'
        Start-Sleep -Seconds 2
        # 不靠 logcat 的 tag（tag 也可能改）：直接在整个 logcat 里找探针的输出
        $line = ((& $script:Adb -s $serial logcat -d 2>$null) -join " ")
        $hit = ($line -split "`n" | Where-Object { $_ -match 'PROBE_RESULT' } | Select-Object -First 1)
        Chk "探针原生返回值" ([string]$hit) 'arm64-v8a native ok'
    }
} else {
    Write-Warn "包里没有 tools\arm64-probe.apk，跳过第 4 组"
}

Write-Host ""
# ⚠️ 先取到变量再拼字符串（双引号里套 $() 再套引号会让 PowerShell 词法分析当场崩）
$fp = Get-Prop $serial "ro.build.fingerprint"
$dv = Get-Prop $serial "ro.product.device"
Write-Log "ROM 指纹： $fp"
Write-Log "设备：     $dv"
if ($fails -eq 0) { Write-Ok "验收全部通过" }
else { Die "有 $fails 项未通过" }
