#!/usr/bin/env bash
# 启动 arm64 模拟器（无显示宿主：-no-window + swiftshader），等到系统真正起来。
#
#   ./run-emulator.sh                  # 前台等 boot_completed（首次 10~40 分钟，TCG）
#   ./run-emulator.sh --no-wait        # 起了就返回，不等开机
#   ./run-emulator.sh --writable-system    # 开 system 可写（想跑 deploy_cuttlefish.sh 时用）
#   ./run-emulator.sh --wipe-data      # 清掉 userdata 重来
#   ./run-emulator.sh --stop           # 停掉本端口的模拟器
#   ./run-emulator.sh --tail           # 跟模拟器控制台日志
#
# 为什么必须用这个脚本而不是直接敲 emulator：
#   1. 用绝对路径调 AOSP 自带的 emulator，它自己会把包内 lib64/qt 加进
#      LD_LIBRARY_PATH；直接调 qemu/linux-x86_64/qemu-system-* 会缺库。
#   2. 传 -sysdir 指向模拟器产物目录（不是 GSI 的 generic_arm64）。
#   3. 默认 -no-window -gpu swiftshader_indirect —— 本机没有 DISPLAY，
#      但 guest 侧仍然会真实合成画面，所以 captureDisplay() 拿得到帧。

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

WAIT=1
WIPE=0
WRITABLE=0
EMU_PID=""
DATADIR="$RUN_DIR/data"

while [ $# -gt 0 ]; do
    case "$1" in
        --no-wait)         WAIT=0 ;;
        --wipe-data)       WIPE=1 ;;
        --writable-system) WRITABLE=1 ;;
        --datadir)         DATADIR="${2:?--datadir 需要参数}"; shift ;;
        --port)            EMULATOR_PORT="${2:?--port 需要参数}"; shift ;;
        --tail)            exec tail -f "$RUN_DIR/emulator-$EMULATOR_PORT.log" ;;
        --stop)
            serial="emulator-$EMULATOR_PORT"
            adb_bin="$(resolve_adb)"
            if "$adb_bin" -s "$serial" emu kill >/dev/null 2>&1; then
                log "已通过 adb 停掉 $serial"
            else
                pkill -f "qemu-system-aarch64.*-port $EMULATOR_PORT" && log "已 pkill 停掉" || warn "没找到在跑的模拟器"
            fi
            exit 0 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) die "未知参数：$1（-h 看用法）" ;;
    esac
    shift
done

SERIAL="emulator-$EMULATOR_PORT"
LOG="$RUN_DIR/emulator-$EMULATOR_PORT.log"

# ---------------------------------------------------------------------------
ADB_BIN="$(resolve_adb)"
[ -x "$EMULATOR_BIN" ] || die "找不到模拟器：$EMULATOR_BIN（容器内 repo sync，同步 prebuilts/android-emulator）"

missing=""
while read -r img; do
    image_ok "$PRODUCT_OUT/$img" || missing="$missing $img"
done < <(required_images)
if [ -n "$missing" ]; then
    die "产物不全，缺：$missing
    目录： $PRODUCT_OUT
    先编： ./build-images.sh          （注意：aosp_arm64 是 GSI，产物在 generic_arm64/，起不了模拟器）"
fi

if "$ADB_BIN" -s "$SERIAL" get-state >/dev/null 2>&1; then
    log "$SERIAL 已经在跑了"
    [ "$WAIT" = 1 ] || exit 0
else
    mkdir -p "$DATADIR"

    args=(
        -sysdir  "$PRODUCT_OUT"
        -datadir "$DATADIR"
        -port    "$EMULATOR_PORT"
        -no-window                     # 宿主无 DISPLAY
        -gpu     "$EMULATOR_GPU"       # swiftshader_indirect：guest 侧真实合成，截图有内容
        -no-snapshot
        -no-boot-anim
        -no-audio
        -accel   off                   # 跨架构本来就没有加速，显式写出来避免误解
        -memory  "$EMULATOR_MEMORY_MB"
        -cores   "$EMULATOR_CORES"
    )
    [ "$WIPE" = 1 ] && args+=(-wipe-data)
    [ "$WRITABLE" = 1 ] && args+=(-writable-system)

    log "启动模拟器（arm64 guest / x86_64 宿主 → TCG 软件模拟，慢是正常的）"
    log "  sysdir : ${PRODUCT_OUT#"$PROJECT_ROOT"/}"
    log "  datadir: ${DATADIR#"$PROJECT_ROOT"/}"
    log "  gpu    : $EMULATOR_GPU    内存: ${EMULATOR_MEMORY_MB}MB    核: $EMULATOR_CORES"
    log "  日志   : ${LOG#"$PROJECT_ROOT"/}"

    setsid nohup "$EMULATOR_BIN" "${args[@]}" > "$LOG" 2>&1 &
    EMU_PID=$!
    log "PID $EMU_PID，等待 adb 出现…"
fi

if [ "$WAIT" != 1 ]; then
    printf '\n后续： %s -s %s shell getprop sys.boot_completed\n' "$ADB_BIN" "$SERIAL"
    exit 0
fi

# ---------------------------------------------------------------------------
# 等 adb + 等开机完成。TCG 下这一步很慢，进度按 30s 打点。
log "等待系统启动（TCG 全软件模拟，首次可能 10~40 分钟；Ctrl-C 不会杀掉模拟器）"
START=$(date +%s)
"$ADB_BIN" -s "$SERIAL" wait-for-device
log "adb 已连上，等 sys.boot_completed…"

while :; do
    el=$(( $(date +%s) - START ))
    if ! kill -0 "${EMU_PID:-0}" 2>/dev/null && ! "$ADB_BIN" -s "$SERIAL" get-state >/dev/null 2>&1; then
        echo
        warn "模拟器进程退出了。日志尾部："
        tail -30 "$LOG" >&2
        die "启动失败（日志：$LOG）"
    fi
    bc="$("$ADB_BIN" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' || true)"
    if [ "$bc" = "1" ]; then
        printf '\n'
        log "开机完成，用时 $((el/60)) 分 $((el%60)) 秒"
        break
    fi
    if [ "$el" -ge "$BOOT_TIMEOUT_S" ]; then
        printf '\n'
        warn "超过 ${BOOT_TIMEOUT_S}s 还没起来（TCG 下偶尔会更久）"
        warn "看日志： tail -50 ${LOG#"$PROJECT_ROOT"/}"
        die "超时"
    fi
    printf '\r  … %02d:%02d 已等待' $((el/60)) $((el%60))
    sleep 30
done

# ---------------------------------------------------------------------------
"$ADB_BIN" -s "$SERIAL" root >/dev/null 2>&1 || warn "adb root 没成功（非 userdebug 镜像？）"
"$ADB_BIN" -s "$SERIAL" wait-for-device

log "设备信息"
printf '  %-22s %s\n' \
    "serial"      "$SERIAL" \
    "型号"        "$("$ADB_BIN" -s "$SERIAL" shell getprop ro.product.model | tr -d '\r')" \
    "Android"     "$("$ADB_BIN" -s "$SERIAL" shell getprop ro.build.version.release | tr -d '\r')" \
    "ABI"         "$("$ADB_BIN" -s "$SERIAL" shell getprop ro.product.cpu.abi | tr -d '\r')" \
    "build type"  "$("$ADB_BIN" -s "$SERIAL" shell getprop ro.build.type | tr -d '\r')" \
    "屏幕"        "$("$ADB_BIN" -s "$SERIAL" shell wm size | tr -d '\r' | awk -F': ' '{print $2}')" \
    "uid"         "$("$ADB_BIN" -s "$SERIAL" shell id -u | tr -d '\r')"

# uinput 是 autod 触控后端的硬依赖，先把结论摆出来
if "$ADB_BIN" -s "$SERIAL" shell 'test -e /dev/uinput' 2>/dev/null; then
    log "/dev/uinput 存在 ✓（autod 的 uinput 后端可用）"
else
    warn "/dev/uinput 不存在，尝试 modprobe"
    "$ADB_BIN" -s "$SERIAL" shell 'modprobe uinput' >/dev/null 2>&1 || true
    if "$ADB_BIN" -s "$SERIAL" shell 'test -e /dev/uinput' 2>/dev/null; then
        log "modprobe 后 /dev/uinput 就绪 ✓"
    else
        warn "仍无 /dev/uinput —— 触控只能等真机验证（截图不受影响）"
    fi
fi

printf '\n下一步： ./smoke-autod.sh\n'
