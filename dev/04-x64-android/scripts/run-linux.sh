#!/usr/bin/env bash
# Linux/x86_64 侧：启动自编 ROM（KVM 加速）并做 arm64 应用验收。
#
#   ./run-linux.sh                 # 启动 + 等开机 + 验收（默认：全新冷启动）
#   ./run-linux.sh --no-wait       # 起了就返回
#   ./run-linux.sh --verify        # 对已在跑的实例只做验收
#   ./run-linux.sh --stop          # 停掉
#   ./run-linux.sh --apk <path>    # 用指定的 arm64 APK 做验收（默认用固定的 F-Droid 包）
#
# 多开一台机器 / 保留状态 / 从快照秒起：
#   ./run-linux.sh --port 5584                 # 再开一台（独立 sysdir / datadir / tap，互不干扰）
#   ./run-linux.sh --port 5584 --reuse         # 保留工作目录：装的应用、快照都留着
#   ./run-linux.sh --snapshot my-snap          # 从快照恢复（隐含 --reuse；实测约 7 秒进系统）
#
# 硬件参数（内存/核数/GPU）默认从 emulator/config.ini 读，命令行可临时覆盖：
#   ./run-linux.sh --memory 8192 --cores 4 --gpu host
#   --gpu auto（默认）= 宿主有 GPU 就用，没有就退软件渲染；host 起不来也会自动退。
#
# 快照：存 `adb -s emulator-<port> emu avd snapshot save <名>`，列 `... emu avd snapshot list`。
#   三个前提（缺一不可）：① config.ini 的 fastboot.forceColdBoot 必须是 no（--snapshot 自动改）
#                        ② hardware-qemu.ini 与存档时逐项一致，改过配置旧快照即作废
#                        ③ 工作目录不能删（--reuse / --snapshot 保证）
# ⚠️ 桥接（-net-tap）时 guest 的 MAC 是 QEMU 默认值，所有实例相同 —— 同时只让一台上物理 LAN。
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

WAIT=1; VERIFY_ONLY=0; APK=""; FORCE_PRODUCT_OUT=0; SHOW_KERNEL=0
REUSE=0; SNAPSHOT=""
# 空 = 从 config.ini 读（见下面 resolve 那段）
GPU_MODE=""; MEM_MB=""; CORES=""
DEFAULT_APK_URL="https://mirrors.tuna.tsinghua.edu.cn/fdroid/repo/com.oF2pks.kalturadeviceinfos_24.apk"
DEFAULT_APK_SHA256="218e213d1014a5ae981160274173bcd5f259defa3805e3465f1c10e4f8ea73d9"

while [ $# -gt 0 ]; do
    case "$1" in
        --no-wait)     WAIT=0 ;;
        --verify)      VERIFY_ONLY=1 ;;
        --stop)        "$ADB" -s "emulator-$EMULATOR_PORT" emu kill >/dev/null 2>&1 && log "已停" || warn "没在跑"; exit 0 ;;
        --apk)         APK="${2:?}"; shift ;;
        --from-product-out) FORCE_PRODUCT_OUT=1 ;;
        --show-kernel) SHOW_KERNEL=1 ;;
        --port)        EMULATOR_PORT="${2:?}"; shift ;;
        # 硬件参数：不传就从 emulator/config.ini 读（那份才是真源）
        --gpu)         GPU_MODE="${2:?}"; shift ;;
        --memory)      MEM_MB="${2:?}"; shift ;;
        --cores)       CORES="${2:?}"; shift ;;
        # 保留工作目录（不 rm -rf）：装了的东西、快照都留着。默认是每次全新冷启动。
        --reuse)       REUSE=1 ;;
        # 从快照秒起（隐含 --reuse）。见文件末尾"快照"一节的三个前提。
        --snapshot)    SNAPSHOT="${2:?}"; REUSE=1; shift ;;
        -h|--help)     sed -n '2,20p' "$0"; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
    shift
done

# 按**最终**端口派生（--port 在上面才生效，所以不能放到 common.sh 里算）：
#   NET_TAP_IF        每实例一个 tap，否则多实例抢同一个 tap0
#   EMULATOR_DATADIR  每实例一个 datadir；共用时 AOSP 自带模拟器会因为目录不存在
#                     直接报 "ERROR: Invalid -datadir directory" 退出（SDK 版容忍，所以一直没暴露）
NET_TAP_IF="${NET_TAP_IF:-tap$EMULATOR_PORT}"
EMULATOR_DATADIR="${EMULATOR_DATADIR:-$RUN_DIR/datadir-$EMULATOR_PORT}"

SERIAL="emulator-$EMULATOR_PORT"
mkdir -p "$RUN_DIR" "$EMULATOR_DATADIR"

# ---------------------------------------------------------------------------
# 硬件参数：默认全部来自 emulator/config.ini
#
# ⚠️ 不要在命令行上写死这些值 —— 命令行**优先于** config.ini，
#    写死了 config.ini 里改什么都没用（以前 -memory 4096 就是这么把
#    hw.ramSize 架空的）。这里只在**显式传参**时才覆盖。
CFG_NCORE="$(config_get hw.cpu.ncore 4)"
CFG_RAM="$(config_get hw.ramSize 4096)"
CFG_GPU="$(config_get hw.gpu.mode auto)"
[ -n "$MEM_MB" ] || MEM_MB="$CFG_RAM"
[ -n "$CORES" ]  || CORES="$CFG_NCORE"
# 内存写 8192 还是 8G 都认
case "$MEM_MB" in *[Gg]) MEM_MB=$(( ${MEM_MB%[Gg]} * 1024 ));; esac
if [ -z "$GPU_MODE" ]; then
    GPU_MODE="$(resolve_gpu_mode "$CFG_GPU")"
    GPU_AUTO=1
else
    GPU_AUTO=0
fi
# host 起不来时自动退软件渲染（见下面候选循环）。显式指定 --gpu 时不退。
GPU_FALLBACK=""
[ "$GPU_AUTO" = 1 ] && [ "$GPU_MODE" = host ] && GPU_FALLBACK="swiftshader_indirect"

# 模拟器选择：默认用 SDK 版 37.x（与 Windows 侧同源，便于提前暴露版本问题）
if [ -z "$EMULATOR_BIN" ]; then
    for c in /opt/android/emulator-new/emulator/emulator \
             "$AOSP_DIR/prebuilts/android-emulator/linux-x86_64/emulator"; do
        [ -x "$c" ] && { EMULATOR_BIN="$c"; break; }
    done
fi

if [ "$GPU_AUTO" = 1 ]; then
    log "硬件参数： -memory $MEM_MB  -cores $CORES  -gpu $GPU_MODE（自适应：$(gpu_mode_reason)）"
else
    log "硬件参数： -memory $MEM_MB  -cores $CORES  -gpu $GPU_MODE（命令行指定）"
fi
log "参数来源： ${EMULATOR_CONFIG#"$PROJECT_ROOT"/}"

# ---------------------------------------------------------------------------
if [ "$VERIFY_ONLY" = 0 ]; then
    [ -x "$EMULATOR_BIN" ] || die "找不到模拟器（export EMULATOR_BIN=...）"

    # 优先验收**打包后的交付目录**：
    #   1. 它带 source.properties，SDK 版模拟器（Windows 侧同源）更认；
    #   2. 它才是真正要交付的那份东西（含 SHA256SUMS / MANIFEST.txt）。
    # 打包目录不存在或不全时，回落到构建产物目录。
    PACKAGED="$ARTIFACTS_DIR/rom-$PRODUCT_NAME"
    if [ "$FORCE_PRODUCT_OUT" = 0 ] && [ -s "$PACKAGED/system-qemu.img" ] && [ -s "$PACKAGED/ramdisk-qemu.img" ] && [ "$PACKAGED" != "$PRODUCT_OUT" ]; then
        # ⚠️ 不能直接对着交付目录启动：模拟器会往 sysdir 里写状态文件
        #    （hardware-qemu.ini / userdata-qemu.img / cache.img / build.avd ...），
        #    把"要交付的那份"弄脏。这里用**符号链接**搭一个工作目录，
        #    镜像仍是同一份（只读），状态文件落在 .run/ 下。
        SCRATCH="$RUN_DIR/sysdir-$EMULATOR_PORT"
        # 实例状态全在 sysdir 里（build.avd/、*.qcow2、snapshots/），所以默认的
        # "每次 rm -rf" 等价于"每次都是一台全新机器"。--reuse / --snapshot 时不删。
        if [ "$REUSE" = 1 ] && [ -d "$SCRATCH" ]; then
            log "复用工作目录（保留已装应用与快照）：.run/sysdir-$EMULATOR_PORT"
        else
            rm -rf "$SCRATCH"
        fi
        mkdir -p "$SCRATCH"
        for f in "$PACKAGED"/*; do
            b="$(basename "$f")"
            # initrd 不链接：模拟器会**重写**它（把自己的 ramdisk-qemu + dtb 写进去），
            # 链接过去会改到交付目录里的文件（实测过，SHA256SUMS 会对不上）。
            # 让它落在工作目录里即可——模拟器本来就会自己生成。
            #
            # config.ini 同理不链接：下面要用 emulator/config.ini 覆盖它，
            # 写穿符号链接会直接改到交付目录里那份。
            case "$b" in initrd|config.ini) continue ;; esac
            ln -sf "$f" "$SCRATCH/$b"
        done
        log "验收对象：打包目录 ${PACKAGED#"$PROJECT_ROOT"/}（工作目录用符号链接：.run/sysdir-$EMULATOR_PORT）"
        PRODUCT_OUT="$SCRATCH"
    fi

    # ---------------------------------------------------------------------------
    # 屏幕尺寸/密度：用本项目 emulator/config.ini 覆盖 ROM 自带那份。
    # ROM 里装的是 goldfish 的 config.ini.xl（1440x2960 @560dpi，Pixel 3 XL 尺寸），
    # 配 swiftshader_indirect 纯软件光栅化是这套配置里最贵的一项；这里统一成
    # 720x1280 @320dpi（像素量约 1/4.6，逻辑尺寸 360x640 dp）。
    # ⚠️ 必须先 rm 再 cp：交付目录那条路径下 config.ini 是符号链接，直接 cp 会写穿。
    if [ -s "$EMULATOR_CONFIG" ]; then
        rm -f "$PRODUCT_OUT/config.ini"
        cp -f "$EMULATOR_CONFIG" "$PRODUCT_OUT/config.ini"
        log "显示配置： $(grep -E '^(skin\.name|hw\.lcd\.density)=' "$EMULATOR_CONFIG" | paste -sd' ' -)  （源：${EMULATOR_CONFIG#"$PROJECT_ROOT"/}）"
    else
        warn "没有 $EMULATOR_CONFIG，沿用 ROM 自带的显示配置"
    fi

    # 快照的前提之一：ROM 上游 config.ini 里的 `fastboot.forceColdBoot = yes` 含义是
    # "永远冷启动、忽略快照"。不改它，-snapshot 会被模拟器直接丢掉
    # （实测日志：ignoring -snapshot option due to the use of -no-snapshot）。
    if [ -n "$SNAPSHOT" ] && [ -s "$PRODUCT_OUT/config.ini" ]; then
        sed -i 's/^fastboot\.forceColdBoot.*/fastboot.forceColdBoot = no/' "$PRODUCT_OUT/config.ini"
        log "快照模式：config.ini 的 fastboot.forceColdBoot → no"
        warn "快照要求 hardware-qemu.ini 与存档时**逐项一致**：改过 config.ini/hw.* 之后再加载旧快照会被拒（The emulator hardware cannot load snapshot）"
    fi
    for img in system.img vendor.img ramdisk.img kernel-ranchu; do
        [ -s "$PRODUCT_OUT/$img" ] || die "产物缺失：$PRODUCT_OUT/$img
    先构建： ./build-rom.sh   （--status 看进度）"
    done
    # 模拟器候选：先 SDK 版（与 Windows 侧同源），失败再回退 AOSP 自带版
    CANDIDATES=("$EMULATOR_BIN")
    AOSP_EMU="$AOSP_DIR/prebuilts/android-emulator/linux-x86_64/emulator"
    [ -x "$AOSP_EMU" ] && [ "$AOSP_EMU" != "$EMULATOR_BIN" ] && CANDIDATES+=("$AOSP_EMU")

    launch() {   # $1 = emulator, $2 = -gpu 取值
        # 桥接模式：宿主侧那座桥就绪时（tools/net-bridge.sh up），给模拟器挂 TAP 网卡，
        # guest 的 eth0 就直接落在局域网的二层域里、从真实 DHCP 拿 IP。
        # 桥没起时**不加**参数 —— 指向不存在的桥会让模拟器直接起不来，
        # 所以这里按"桥在不在"自动决定，而不是靠开关。
        local tap_args=() snap_args=()
        if [ -n "$NET_BRIDGE_IF" ] && [ -d "/sys/class/net/$NET_BRIDGE_IF/bridge" ]; then
            tap_args=(-net-tap "$NET_TAP_IF"
                      -net-tap-script-up "$X64_DIR/tools/net-bridge-ifup.sh")
            log "桥接模式：$NET_TAP_IF → $NET_BRIDGE_IF（guest 的 eth0 走物理局域网）"
        fi
        # --snapshot <名> 从快照秒起；否则明确 -no-snapshot（每次真冷启动，
        # 验收要的是确定性，不能被上一次的状态污染）。
        if [ -n "$SNAPSHOT" ]; then
            snap_args=(-snapshot "$SNAPSHOT")
            log "快照模式：从 '$SNAPSHOT' 恢复（工作目录复用：.run/sysdir-$EMULATOR_PORT）"
        else
            snap_args=(-no-snapshot)
        fi
        : > "$RUN_DIR/emulator-$EMULATOR_PORT.log"
        # ⚠️ 两个变量都要给：
        #   ANDROID_PRODUCT_OUT —— 模拟器靠它进"不用 AVD、直接从构建产物启动"的模式
        #   ANDROID_BUILD_TOP   —— 少了它 SDK 版模拟器会去找 'kernel-qemu' 并报
        #                          "Your system directory is missing the 'kernel-qemu' image file"
        #                          （彩排时踩到，见 docs/02-build-traps.md §7）
        ANDROID_PRODUCT_OUT="$PRODUCT_OUT" ANDROID_BUILD_TOP="$AOSP_DIR" setsid nohup "$1" \
            -sysdir "$PRODUCT_OUT" -datadir "$EMULATOR_DATADIR" -port "$EMULATOR_PORT" \
            -no-window -gpu "$2" -no-boot-anim -no-audio \
            "${snap_args[@]}" \
            -accel on -memory "$MEM_MB" -cores "$CORES" \
            "${tap_args[@]}" \
            $([ "$SHOW_KERNEL" = 1 ] && printf '%s' "-show-kernel") \
            >> "$RUN_DIR/emulator-$EMULATOR_PORT.log" 2>&1 < /dev/null &
        EMU_PID=$!
    }
    adb_appears() {   # 最多等 40 秒；进程提前退出即判定失败
        for _ in $(seq 1 8); do
            "$ADB" -s "$SERIAL" get-state >/dev/null 2>&1 && return 0
            kill -0 "$EMU_PID" 2>/dev/null || return 1
            sleep 5
        done
        return 1
    }

    # GPU 候选：默认那一档起不来就退软件渲染。
    #
    # ⚠️ 为什么需要回退：-gpu auto 选 host 只看"有没有渲染节点"，
    #    而**有节点 ≠ 驱动能用**（VMware/VirtualBox 的虚拟 GPU、
    #    容器里挂进来的 renderD、驱动版本不匹配……都会让 host 起来就崩）。
    #    这时画面全黑或进程直接退出，而软件渲染一定能用 —— 所以自适应
    #    要落到"真的起来了"为止，而不是"探测到了就算数"。
    GPU_CANDIDATES=("$GPU_MODE")
    [ -n "$GPU_FALLBACK" ] && GPU_CANDIDATES+=("$GPU_FALLBACK")

    started=0
    for c in "${CANDIDATES[@]}"; do
        for g in "${GPU_CANDIDATES[@]}"; do
            log "启动自编 ROM（KVM）：$c  -gpu $g  -memory $MEM_MB -cores $CORES"
            [ "$g" = "$GPU_MODE" ] || warn "上一档（$GPU_MODE）没起来，退到 $g"
            launch "$c" "$g"
            if adb_appears; then started=1; EMULATOR_BIN="$c"; GPU_MODE="$g"; break 2; fi
            warn "这个组合没起来（$(tail -2 "$RUN_DIR/emulator-$EMULATOR_PORT.log" | tr '\n' ' ')）"
            kill "$EMU_PID" 2>/dev/null || true
            "$ADB" -s "$SERIAL" emu kill >/dev/null 2>&1 || true
            sleep 3
        done
    done
    [ "$started" = 1 ] || die "所有模拟器/GPU 组合都没能启动设备；看 $RUN_DIR/emulator-$EMULATOR_PORT.log"
    log "实际使用：-gpu $GPU_MODE -memory $MEM_MB -cores $CORES"

    if [ "$WAIT" = 1 ]; then
        log "等开机完成"
        for i in $(seq 1 40); do
            [ "$("$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break
            sleep 5
        done
        "$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
        "$ADB" -s "$SERIAL" wait-for-device
        log "开机完成： $(grep -oE '(boot time|Boot completed in) [0-9]+ ms' "$RUN_DIR/emulator-$EMULATOR_PORT.log" 2>/dev/null | tail -1 || true)"
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
# 注意：ls 按字母序输出（arm64_dyn 在 arm64_exe 之前），别写成顺序依赖的正则
bf=$("$ADB" -s "$SERIAL" shell 'ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | tr "\n" " "' | tr -d '\r')
chk "binfmt: arm64_exe"      "$bf" 'arm64_exe'
chk "binfmt: arm64_dyn"      "$bf" 'arm64_dyn'

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
PROBE_APK="$X64_DIR/artifacts/arm64-probe.apk"
PKG=""
if [ -z "$APK" ]; then
    if [ -s "$PROBE_APK" ]; then
        # 首选项目自建的探针 APK：只含 arm64-v8a 一个 ABI，能装能跑就是翻译层在工作
        APK="$PROBE_APK"; PKG=org.autosnap.arm64probe
        log "  用自建探针 APK（tools/build-probe-apk.sh 产出，纯 arm64-v8a）"
    else
        APK="$RUN_DIR/com.oF2pks.kalturadeviceinfos_24.apk"; PKG=com.oF2pks.kalturadeviceinfos
        if [ ! -s "$APK" ]; then
            log "  下载验收用 APK（F-Droid 镜像，312 KB）"
            curl -fsSL -o "$APK" "$DEFAULT_APK_URL" || warn "APK 下载失败（可用 --apk 指定本地文件）"
        fi
    fi
fi
if [ -s "$APK" ] && [ -n "$PKG" ]; then
    got=$(sha256sum "$APK" | cut -d' ' -f1)
    if [ "$PKG" = com.oF2pks.kalturadeviceinfos ] && [ "$got" != "$DEFAULT_APK_SHA256" ]; then
        warn "APK sha256 与内置值不同（$got）——若是自己指定的包可忽略"
    fi
    "$ADB" -s "$SERIAL" shell "pm uninstall $PKG" >/dev/null 2>&1 || true
    "$ADB" -s "$SERIAL" push "$APK" /data/local/tmp/probe.apk >/dev/null
    chk "pm install --abi arm64-v8a" "$("$ADB" -s "$SERIAL" shell 'pm install --abi arm64-v8a -r /data/local/tmp/probe.apk' 2>&1 | tr -d '\r' | tail -1)" 'Success'
    # 先清 logcat，否则会匹配到上一轮残留的 PROBE_RESULT（彩排时踩过）
    "$ADB" -s "$SERIAL" logcat -c >/dev/null 2>&1 || true
    "$ADB" -s "$SERIAL" shell "monkey -p $PKG -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1 || true
    sleep 6
    pid=$("$ADB" -s "$SERIAL" shell pidof "$PKG" | tr -d '\r')
    chk "进程存活"              "${pid:-DEAD}" '^[0-9]+$'
    if [ -n "$pid" ]; then
        chk "映射的 arm64 库条数" "$("$ADB" -s "$SERIAL" shell "grep -c '/system/lib64/arm64/' /proc/$pid/maps" | tr -d '\r')" '^[1-9][0-9]*$'
    fi
    chk "primaryCpuAbi"         "$("$ADB" -s "$SERIAL" shell "pm dump $PKG 2>/dev/null | grep -m1 primaryCpuAbi" | tr -d '\r')" 'arm64-v8a'
    # 自建探针：再从 logcat 里确认原生方法真的返回了结果
    if [ "$PKG" = org.autosnap.arm64probe ]; then
        sleep 2
        chk "探针原生返回值"    "$("$ADB" -s "$SERIAL" logcat -d -s ARM64PROBE 2>/dev/null | grep -m1 PROBE_RESULT | tr -d '\r')" 'arm64-v8a native ok'
    fi
else
    warn "没有可用的 APK，跳过第 4 项"
fi

echo
log "ROM 指纹： $("$ADB" -s "$SERIAL" shell getprop ro.build.fingerprint | tr -d '\r')"
log "设备：     $("$ADB" -s "$SERIAL" shell getprop ro.product.device | tr -d '\r')  开机： $(grep -oE '(boot time|Boot completed in) [0-9]+ ms' "$RUN_DIR/emulator-$EMULATOR_PORT.log" 2>/dev/null | tail -1 || true)"
if [ "$fails" = 0 ]; then log "验收全部通过 ✓  （ROM：$PRODUCT_OUT）"
else die "有 $fails 项未通过"; fi
