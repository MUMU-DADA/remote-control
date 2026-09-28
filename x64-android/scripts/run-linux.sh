#!/usr/bin/env bash
# Linux/x86_64 侧：启动自编 ROM（KVM 加速）并做 arm64 应用验收。
#
#   ./run-linux.sh                 # 启动 + 等开机 + 验收
#   ./run-linux.sh --no-wait       # 起了就返回
#   ./run-linux.sh --verify        # 对已在跑的实例只做验收
#   ./run-linux.sh --stop          # 停掉
#   ./run-linux.sh --apk <path>    # 用指定的 arm64 APK 做验收（默认用固定的 F-Droid 包）
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

WAIT=1; VERIFY_ONLY=0; APK=""
DEFAULT_APK_URL="https://mirrors.tuna.tsinghua.edu.cn/fdroid/repo/com.oF2pks.kalturadeviceinfos_24.apk"
DEFAULT_APK_SHA256="218e213d1014a5ae981160274173bcd5f259defa3805e3465f1c10e4f8ea73d9"

while [ $# -gt 0 ]; do
    case "$1" in
        --no-wait)     WAIT=0 ;;
        --verify)      VERIFY_ONLY=1 ;;
        --stop)        "$ADB" -s "emulator-$EMULATOR_PORT" emu kill >/dev/null 2>&1 && log "已停" || warn "没在跑"; exit 0 ;;
        --apk)         APK="${2:?}"; shift ;;
        --port)        EMULATOR_PORT="${2:?}"; shift ;;
        -h|--help)     sed -n '2,12p' "$0"; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
    shift
done

SERIAL="emulator-$EMULATOR_PORT"
mkdir -p "$RUN_DIR"

# 模拟器选择：默认用 SDK 版 37.x（与 Windows 侧同源，便于提前暴露版本问题）
if [ -z "$EMULATOR_BIN" ]; then
    for c in /opt/android/emulator-new/emulator/emulator \
             "$AOSP_DIR/prebuilts/android-emulator/linux-x86_64/emulator"; do
        [ -x "$c" ] && { EMULATOR_BIN="$c"; break; }
    done
fi

# ---------------------------------------------------------------------------
if [ "$VERIFY_ONLY" = 0 ]; then
    [ -x "$EMULATOR_BIN" ] || die "找不到模拟器（export EMULATOR_BIN=...）"
    for img in system.img vendor.img ramdisk.img kernel-ranchu; do
        [ -s "$PRODUCT_OUT/$img" ] || die "产物缺失：$PRODUCT_OUT/$img
    先构建： ./build-rom.sh   （--status 看进度）"
    done
    log "启动自编 ROM（KVM）：$EMULATOR_BIN"
    ANDROID_PRODUCT_OUT="$PRODUCT_OUT" setsid nohup "$EMULATOR_BIN" \
        -sysdir "$PRODUCT_OUT" -datadir "$RUN_DIR/datadir" -port "$EMULATOR_PORT" \
        -no-window -gpu swiftshader_indirect -no-snapshot -no-boot-anim -no-audio \
        -accel on -memory 4096 -cores 4 \
        > "$RUN_DIR/emulator-$EMULATOR_PORT.log" 2>&1 < /dev/null &

    if [ "$WAIT" = 1 ]; then
        log "等开机完成"
        "$ADB" -s "$SERIAL" wait-for-device
        for i in $(seq 1 40); do
            [ "$("$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break
            sleep 5
        done
        "$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
        "$ADB" -s "$SERIAL" wait-for-device
        log "开机完成： $(grep -o 'boot time [0-9]* ms' "$RUN_DIR/emulator-$EMULATOR_PORT.log" | tail -1)"
    else
        printf '\n后续： ./run-linux.sh --verify\n'
        exit 0
    fi
fi

# ---------------------------------------------------------------------------
# 验收
fails=0
chk() {  # chk <描述> <实际值> <期望匹配>
    if printf '%s' "$2" | grep -qE "$3"; then printf '  [✓] %-34s %s\n' "$1" "$2"
    else printf '  [✗] %-34s %s（期望匹配 %s）\n' "$1" "$2" "$3"; fails=$((fails+1)); fi
}

log "验收 1/4：镜像身份与 ABI"
chk "ro.build.version.sdk"   "$("$ADB" -s "$SERIAL" shell getprop ro.build.version.sdk | tr -d '\r')"      '^31$'
chk "ro.product.device"      "$("$ADB" -s "$SERIAL" shell getprop ro.product.device | tr -d '\r')"         'autosnap_x64_arm64'
chk "ro.product.cpu.abilist" "$("$ADB" -s "$SERIAL" shell getprop ro.product.cpu.abilist | tr -d '\r')"    'x86_64,arm64-v8a'

log "验收 2/4：翻译层接线"
chk "native.bridge"          "$("$ADB" -s "$SERIAL" shell getprop ro.dalvik.vm.native.bridge | tr -d '\r')" 'libndk_translation.so'
chk "enable.native.bridge.exec" "$("$ADB" -s "$SERIAL" shell getprop ro.enable.native.bridge.exec | tr -d '\r')" '^1$'
chk "binfmt_misc 注册"       "$("$ADB" -s "$SERIAL" shell 'ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | tr "\n" " "' | tr -d '\r')" 'arm64_exe.*arm64_dyn'

log "验收 3/4：aarch64 机器码真的在跑"
NDK_DIR="${NDK_DIR:-/opt/android/android-ndk-r26d}"
CLANG="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang"
if [ -x "$CLANG" ]; then
    probe="$RUN_DIR/arm64-probe"
    cat > "$probe.c" <<'EOF'
#include <stdio.h>
#include <sys/utsname.h>
int main(void){ struct utsname u; uname(&u);
  volatile unsigned long a=0; for (unsigned long i=0;i<50000000UL;i++) a+=i;
  printf("ARM64_OK machine=%s a=%lu\n", u.machine, a); return 0; }
EOF
    "$CLANG" -static -O2 "$probe.c" -o "$probe" >/dev/null 2>&1 || warn "arm64 探针编译失败"
    if [ -x "$probe" ]; then
        file -b "$probe" | grep -q aarch64 && log "  探针：$(file -b "$probe" | cut -c1-50)"
        "$ADB" -s "$SERIAL" push "$probe" /data/local/tmp/arm64-probe >/dev/null
        "$ADB" -s "$SERIAL" shell 'chmod 755 /data/local/tmp/arm64-probe'
        chk "aarch64 ELF 执行"  "$("$ADB" -s "$SERIAL" shell /data/local/tmp/arm64-probe | tr -d '\r')" 'ARM64_OK'
    fi
else
    warn "没有 NDK（$NDK_DIR），跳过探针"
fi

log "验收 4/4：arm64 应用（APK）"
if [ -z "$APK" ]; then
    APK="$RUN_DIR/com.oF2pks.kalturadeviceinfos_24.apk"
    if [ ! -s "$APK" ]; then
        log "  下载验收用 APK（F-Droid 镜像，312 KB）"
        curl -fsSL -o "$APK" "$DEFAULT_APK_URL" || warn "APK 下载失败（可用 --apk 指定本地文件）"
    fi
fi
if [ -s "$APK" ]; then
    got=$(sha256sum "$APK" | cut -d' ' -f1)
    [ "$got" = "$DEFAULT_APK_SHA256" ] || warn "APK sha256 与内置值不同（$got）——若是自己指定的包可忽略"
    PKG=com.oF2pks.kalturadeviceinfos
    "$ADB" -s "$SERIAL" shell "pm uninstall $PKG" >/dev/null 2>&1 || true
    "$ADB" -s "$SERIAL" push "$APK" /data/local/tmp/probe.apk >/dev/null
    chk "pm install --abi arm64-v8a" "$("$ADB" -s "$SERIAL" shell 'pm install --abi arm64-v8a -r /data/local/tmp/probe.apk' 2>&1 | tr -d '\r' | tail -1)" 'Success'
    "$ADB" -s "$SERIAL" shell "monkey -p $PKG -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1 || true
    sleep 6
    pid=$("$ADB" -s "$SERIAL" shell pidof "$PKG" | tr -d '\r')
    chk "进程存活"              "${pid:-DEAD}" '^[0-9]+$'
    if [ -n "$pid" ]; then
        chk "映射的 arm64 库条数" "$("$ADB" -s "$SERIAL" shell "grep -c '/system/lib64/arm64/' /proc/$pid/maps" | tr -d '\r')" '^[1-9][0-9]*$'
    fi
    chk "primaryCpuAbi"         "$("$ADB" -s "$SERIAL" shell "pm dump $PKG 2>/dev/null | grep -m1 primaryCpuAbi" | tr -d '\r')" 'arm64-v8a'
else
    warn "没有可用的 APK，跳过第 4 项"
fi

echo
if [ "$fails" = 0 ]; then log "验收全部通过 ✓  （ROM：$PRODUCT_OUT）"
else die "有 $fails 项未通过"; fi
