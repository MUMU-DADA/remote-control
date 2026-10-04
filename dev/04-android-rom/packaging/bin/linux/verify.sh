#!/usr/bin/env bash
# 验收：这台机器是不是真的在跑这份 ROM，而且 **arm64 应用真的能跑**。
#
#   ./bin/verify.sh                # 验收 default（端口 5580）
#   ./bin/verify.sh --port 5584
#
# 四组检查（和上游项目 scripts/run-linux.sh 的验收口径一致）：
#   1. 镜像身份与 ABI       SDK=31 / device / abilist 含 arm64-v8a
#   2. 翻译层接线           native.bridge / exec / binfmt_misc 注册
#   3. aarch64 机器码在跑   自带的静态 aarch64 ELF 直接执行 → ARM64_OK
#   4. arm64 应用           纯 arm64-v8a 探针 APK：装 → 起 → 映射 arm64 库 → 原生返回值
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

NAME="$DEFAULT_NAME"; PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --name)    NAME="${2:?}"; shift ;;
        --port)    PORT="${2:?}"; shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
    shift
done

if [ -z "$PORT" ]; then
    PORT="$(instance_port "$NAME")"
    [ -n "$PORT" ] || die "实例 '$NAME' 没登记过端口（先 ./bin/start-headless.sh）"
fi
SERIAL="$(serial_for_port "$PORT")"
require_adb
[ -n "$(emu_pid_for_port "$PORT")" ] || die "端口 $PORT 上没有模拟器在跑"

fails=0
chk() {   # chk <描述> <实际值> <期望匹配>
    if printf '%s' "$2" | grep -qE "$3"; then ok "$(printf '%-34s %s' "$1" "$2")"
    else bad "$(printf '%-34s %s（期望匹配 %s）' "$1" "$2" "$3")"; fails=$((fails + 1)); fi
}
# ⚠️ 这两个 helper 一律 `|| true`：`adb shell pidof X` 找不到进程时返回**非 0**，
#    在 set -e 下会把脚本**静默带走**（实测踩过：第 4 组前面的检查全绿，脚本却直接退出、
#    连失败项都没打印）。检查项的失败要由 chk 报出来，不是让脚本死掉。
prop() { "$ADB" -s "$SERIAL" shell getprop "$1" 2>/dev/null | tr -d '\r' | sed -n '1p' || true; }
sh_()  { "$ADB" -s "$SERIAL" shell "$1" 2>/dev/null | tr -d '\r' || true; }

# 探针包名**从设备上发现**，不写死：这个 APK 改过名（org.remotecontrol.arm64probe →
# org.autosnap.arm64probe），写死就会出现"装上了 Success，却 monkey 找不到 Activity"。
probe_pkg() {
    local p
    p="$(sh_ 'pm list packages -3 2>/dev/null | sed "s/^package://" | grep -i "arm64probe$" | sed -n "1p"')"
    printf '%s' "$p"
}

log "等设备就绪"
"$ADB" -s "$SERIAL" wait-for-device >/dev/null 2>&1 || die "设备不在（$SERIAL）"
# `binfmt_misc` 通常只有 root adbd 才能读取。包内 verify 也必须在复用
# 已启动设备时尝试提升权限，否则普通 adbd 会把已注册规则误报成缺失。
# 生产版 adbd 不允许 root 时忽略失败，让实际检查按可见性报告结果。
"$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
"$ADB" -s "$SERIAL" wait-for-device >/dev/null 2>&1 || true

log "验收 1/4：镜像身份与 ABI"
chk "ro.build.version.sdk"   "$(prop ro.build.version.sdk)"   '^31$'
chk "ro.product.device"      "$(prop ro.product.device)"      'remote_control_x64_arm64'
chk "ro.product.cpu.abilist" "$(prop ro.product.cpu.abilist)" 'x86_64,arm64-v8a'

log "验收 2/4：翻译层接线"
chk "ro.dalvik.vm.native.bridge"   "$(prop ro.dalvik.vm.native.bridge)"   'libndk_translation\.so'
chk "ro.enable.native.bridge.exec" "$(prop ro.enable.native.bridge.exec)" '^1$'
# ⚠️ `ls` 是字母序（arm64_dyn 在前），所以拆成两条独立检查，别写成顺序依赖的正则
BF="$(sh_ 'ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | tr "\n" " "')"
chk "binfmt_misc: arm64_exe" "$BF" 'arm64_exe'
chk "binfmt_misc: arm64_dyn" "$BF" 'arm64_dyn'

log "验收 3/4：aarch64 机器码真的在跑"
PROBE="$TOOLS/arm64-probe"
if [ -x "$PROBE" ] || [ -s "$PROBE" ]; then
    "$ADB" -s "$SERIAL" push "$PROBE" /data/local/tmp/arm64-probe >/dev/null 2>&1
    sh_ 'chmod 755 /data/local/tmp/arm64-probe' >/dev/null 2>&1 || true
    chk "aarch64 静态 ELF 执行" "$(sh_ '/data/local/tmp/arm64-probe')" 'ARM64_OK'
else
    # 包里没带探针时的退路：自证 /system/lib64/arm64/libc.so 是 aarch64 ELF（e_machine=0xB7）
    chk "arm64 系统库存在" "$(sh_ 'ls /system/lib64/arm64/libc.so')" '/system/lib64/arm64/libc\.so'
    chk "libc.so 是 aarch64 ELF" "$(sh_ 'head -c 20 /system/lib64/arm64/libc.so | od -An -tx1')" 'b7'
fi

log "验收 4/4：arm64 应用（纯 arm64-v8a 探针 APK）"
APK="$TOOLS/arm64-probe.apk"
if [ -s "$APK" ]; then
    "$ADB" -s "$SERIAL" push "$APK" /data/local/tmp/probe.apk >/dev/null 2>&1 || true
    chk "pm install --abi arm64-v8a" \
        "$("$ADB" -s "$SERIAL" shell 'pm install --abi arm64-v8a -r /data/local/tmp/probe.apk' 2>&1 | tr -d '\r' | tail -1 || true)" \
        'Success'
    PKG="$(probe_pkg)"
    if [ -z "$PKG" ]; then
        bad "装上了但找不到探针包名（pm list packages -3 是空的）"
        fails=$((fails + 1))
    else
        # ⚠️ 启动前先清 logcat，否则会匹配到上一轮残留的 PROBE_RESULT（假绿，踩过）
        "$ADB" -s "$SERIAL" logcat -c >/dev/null 2>&1 || true
        sh_ "monkey -p $PKG -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1
        sleep 6
        PID="$(sh_ "pidof $PKG")"
        chk "进程存活（$PKG）" "${PID:-DEAD}" '^[0-9]+$'
        if [ -n "$PID" ]; then
            chk "映射的 arm64 库条数" "$(sh_ "grep -c '/system/lib64/arm64/' /proc/$PID/maps")" '^[1-9][0-9]*$'
        fi
        chk "primaryCpuAbi" "$(sh_ "pm dump $PKG 2>/dev/null | grep -m1 primaryCpuAbi")" 'arm64-v8a'
        sleep 2
        # 不靠 logcat 的 tag（tag 也可能改）：直接在整个 logcat 里找探针的输出
        chk "探针原生返回值" \
            "$("$ADB" -s "$SERIAL" logcat -d 2>/dev/null | grep -m1 PROBE_RESULT | tr -d '\r' || true)" \
            'arm64-v8a native ok'
    fi
else
    warn "包里没有 tools/arm64-probe.apk，跳过第 4 组"
fi

echo
log "ROM 指纹： $(prop ro.build.fingerprint)"
log "设备：     $(prop ro.product.device)"
if [ "$fails" = 0 ]; then
    ok "验收全部通过"
else
    die "有 $fails 项未通过"
fi
