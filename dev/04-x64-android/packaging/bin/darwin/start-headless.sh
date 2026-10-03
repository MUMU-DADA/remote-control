#!/usr/bin/env bash
# 无头启动这台虚拟机（**macOS 版**，默认 -no-window）。
#
#   ./bin/start-headless.sh                      # default 实例，端口 5580，全新冷启动
#   ./bin/start-headless.sh --reuse               # 保留上次的数据、已装应用、快照
#   ./bin/start-headless.sh --name vm2 --port 5584
#   ./bin/start-headless.sh --memory 8192 --cores 6
#   ./bin/start-headless.sh --gpu swiftshader_indirect
#   ./bin/start-headless.sh --gui                 # 带窗口（macOS 原生窗口，调试用）
#   ./bin/start-headless.sh --no-wait             # 起了就返回，不等开机
#
# 硬件参数全部来自 templates/config.ini（唯一真源）；命令行只在**显式传参**时覆盖
# —— 这一点很重要：命令行优先于 config.ini，随手写死默认值会让 config.ini 失效。
#
# 与 bin/linux/start-headless.sh 的三处平台差异：
#   1. **没有 --bridge**：macOS 没有 -net-tap 的等价物（与 Windows 侧一致的能力面）。
#      传 --bridge 会被明确拒绝并说明原因，而不是静默忽略。
#   2. 加速判定用 kern.hv_support（Hypervisor.framework），不是 /dev/kvm。
#   3. 后台化用 `nohup ... &` + disown —— macOS 没有 setsid。
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

NAME="$DEFAULT_NAME"; PORT=""; GPU=""; MEM=""; CORES=""
REUSE=0; GUI=0; WAIT=1; WIPE=0; ACCEL="auto"; TIMEOUT=300

while [ $# -gt 0 ]; do
    case "$1" in
        --name)    NAME="${2:?}"; shift ;;
        --port)    PORT="${2:?}"; shift ;;
        --gpu)     GPU="${2:?}"; shift ;;
        --memory)  MEM="${2:?}"; shift ;;
        --cores)   CORES="${2:?}"; shift ;;
        --timeout) TIMEOUT="${2:?}"; shift ;;
        --accel)   ACCEL="${2:?}"; shift ;;
        --reuse)   REUSE=1 ;;
        --gui)     GUI=1 ;;
        --no-wait) WAIT=0 ;;
        --wipe-data) WIPE=1 ;;
        --bridge)
            die "--bridge 在 macOS 上不可用：macOS 没有 -net-tap 的等价物。
    这不是待修的缺陷 —— windows 包同样没有桥接（见 docs/13-macos-port.md §3.5）。
    guest 走模拟器默认的用户态 NAT；要从宿主访问 guest 的服务，用 adb forward。" ;;
        --nat)     : ;;   # 默认就是 NAT，接受这个参数只为与 linux 侧的调用脚本兼容
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) die "未知参数：$1（./bin/start-headless.sh --help）" ;;
    esac
    shift
done

require_runtime
require_images
require_adb

# ---------------------------------------------------------------------------
# 端口：显式 > 该实例已登记的 > 自动分配
# ---------------------------------------------------------------------------
if [ -z "$PORT" ]; then
    PORT="$(instance_port "$NAME")"
    [ -n "$PORT" ] || PORT="$(alloc_port)"
fi
case "$PORT" in
    ''|*[!0-9]*) die "端口必须是数字：$PORT" ;;
esac
if [ $((PORT % 2)) -ne 0 ]; then
    die "端口必须是偶数：$PORT（模拟器拿 port+1 当 console 口）"
fi
if port_listening "$PORT"; then
    die "端口 $PORT 已经有模拟器在跑（./bin/status.sh 看是谁）"
fi
instance_register "$NAME" "$PORT"

SERIAL="$(serial_for_port "$PORT")"
SYSDIR="$(sysdir_for_port "$PORT")"
DATADIR="$(datadir_for_port "$PORT")"
LOGF="$(logfile_for_port "$PORT")"

# ---------------------------------------------------------------------------
# 前置自检
# ---------------------------------------------------------------------------
if [ "$ACCEL" = auto ]; then
    if hv_usable; then ACCEL=on; else ACCEL=off; fi
fi
if [ "$ACCEL" = on ] && ! hv_usable; then
    die "要用硬件加速（-accel on）但 kern.hv_support 不是 1。
    常见原因：跑在虚拟机里的 macOS（嵌套虚拟化没开）、机型太老。
    裸机请确认： sysctl kern.hv_support
    想硬着头皮用纯软件模拟（很慢，分钟级起步）： --accel off"
fi
if [ "$ACCEL" = off ]; then
    warn "没有硬件虚拟化，走 TCG 纯软件模拟 —— 开机可能要几分钟到几十分钟"
    warn "  量级参照：同架构 KVM 下单 ROM 开机 23.8 秒（上游实测），TCG 是 5–8 分钟量级"
fi

mkdir -p "$RUN_DIR"
FREE="$(disk_free_mb "$RUN_DIR")"
if [ -n "$FREE" ] && [ "$FREE" -lt 8192 ]; then
    warn "工作目录所在分区只剩 $(human_mb "$FREE")。数据分区是 qcow2 覆盖层、会随用量增长，"
    warn "  装大应用前先腾地方：$RUN_DIR"
fi

# ---------------------------------------------------------------------------
# 工作目录：默认每次都是"一台全新机器"；--reuse 才保留状态
# ---------------------------------------------------------------------------
if [ "$REUSE" = 1 ] && [ -d "$SYSDIR" ]; then
    log "复用工作目录：$SYSDIR（保留已装应用、数据、快照）"
    build_sysdir "$PORT" keep
else
    build_sysdir "$PORT" fresh
    rm -rf "$DATADIR"
fi
mkdir -p "$DATADIR"

# ---------------------------------------------------------------------------
# 硬件参数
# ---------------------------------------------------------------------------
GPU_WANT="${GPU:-$(config_get hw.gpu.mode auto)}"
GPU_AUTO=0
[ -n "$GPU" ] || GPU_AUTO=1
GPU_MODE="$(resolve_gpu_mode "$GPU_WANT")"
[ -n "$MEM" ]   || MEM="$(config_get hw.ramSize 6144)"
[ -n "$CORES" ] || CORES="$(config_get hw.cpu.ncore 4)"
case "$MEM" in *[Gg]) MEM=$(( ${MEM%[Gg]} * 1024 )) ;; esac    # 允许写 6G
case "$MEM" in ''|*[!0-9]*) die "内存必须是 MB 数（或 6G 这种写法）：$MEM" ;; esac
case "$CORES" in ''|*[!0-9]*) die "核数必须是数字：$CORES" ;; esac

# GPU 候选：自适应选的那档起不来就退软件渲染。
# 「探测到 GPU ≠ 驱动能用」（虚拟机里的 macOS 就是有设备无 Metal），
# 所以判据是"adb 真的看得见设备"，不是"探测到了"。
GPU_CANDIDATES=("$GPU_MODE")
[ "$GPU_AUTO" = 1 ] && [ "$GPU_MODE" = host ] && GPU_CANDIDATES+=("swiftshader_indirect")

launch() {   # launch <gpu>
    : > "$LOGF"
    # ⚠️ 两个环境变量都要给：
    #   ANDROID_PRODUCT_OUT —— 模拟器靠它进"不用 AVD、直接从镜像目录启动"的模式
    #   ANDROID_BUILD_TOP   —— 少了它 SDK 版模拟器会去找 'kernel-qemu' 并报
    #                          "Your system directory is missing the 'kernel-qemu' image file"
    #
    # macOS 没有 setsid，用 nohup + & + disown；父 shell 退出后模拟器继续跑。
    ANDROID_PRODUCT_OUT="$SYSDIR" ANDROID_BUILD_TOP="$ROOT" \
        nohup "$EMULATOR" \
            -sysdir "$SYSDIR" -datadir "$DATADIR" -port "$PORT" \
            -gpu "$1" -accel "$ACCEL" -memory "$MEM" -cores "$CORES" \
            -no-boot-anim -no-audio -no-snapshot \
            $([ "$GUI" = 1 ] || printf '%s' "-no-window") \
            $([ "$WIPE" = 1 ] && printf '%s' "-wipe-data") \
            >> "$LOGF" 2>&1 < /dev/null &
    EMU_PID=$!
    disown 2>/dev/null || true
}

adb_appears() {   # 最多 40 秒；进程提前退出即判定失败
    local i
    for i in 1 2 3 4 5 6 7 8; do
        "$ADB" -s "$SERIAL" get-state >/dev/null 2>&1 && return 0
        kill -0 "$EMU_PID" 2>/dev/null || return 1
        sleep 5
    done
    return 1
}

started=0; USED_GPU=""
for g in "${GPU_CANDIDATES[@]}"; do
    [ "$g" = "$GPU_MODE" ] || warn "上一档（$GPU_MODE）没起来，退到 $g"
    log "启动：$EMULATOR -gpu $g -memory $MEM -cores $CORES -accel $ACCEL 端口 $PORT"
    launch "$g"
    if adb_appears; then started=1; USED_GPU="$g"; break; fi
    warn "没起来：$(tail -2 "$LOGF" 2>/dev/null | tr '\n' ' ')"
    kill "$EMU_PID" 2>/dev/null || true
    "$ADB" -s "$SERIAL" emu kill >/dev/null 2>&1 || true
    sleep 3
done
[ "$started" = 1 ] || die "起不来。日志：$LOGF"
ok "已启动：$SERIAL（-gpu $USED_GPU -memory $MEM -cores $CORES）"
if [ "$GPU_AUTO" = 1 ]; then log "GPU 自适应依据：$(gpu_reason)"; fi

if [ "$WAIT" = 0 ]; then
    printf '\n后续： ./bin/status.sh --port %s     ./bin/verify.sh --port %s     ./bin/stop.sh --port %s\n' "$PORT" "$PORT" "$PORT"
    exit 0
fi

log "等开机完成（硬件加速下通常 20~60 秒；TCG 下是分钟级）"
if ! wait_for_boot "$PORT" "$TIMEOUT"; then
    warn "等开机超时（${TIMEOUT}s）—— 看 $LOGF"
    die "设备没能进入 sys.boot_completed=1"
fi
"$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
"$ADB" -s "$SERIAL" wait-for-device >/dev/null 2>&1 || true

# 开机耗时那一行是模拟器在 guest 报完之后才写的，可能比 adb 看到 boot_completed 晚一瞬 ——
# 给它三次机会，免得永远打印成空（实测踩过）
BOOTTIME=""
for _ in 1 2 3; do
    BOOTTIME="$(grep -oE '(boot time|Boot completed in) [0-9]+ ms' "$LOGF" 2>/dev/null | tail -1 || true)"
    [ -n "$BOOTTIME" ] && break
    sleep 1
done
ok "开机完成 ${BOOTTIME:+（$BOOTTIME）}"
log "设备序列号：$SERIAL"
log "工作目录：  $SYSDIR（镜像在 $IMAGES，只读；状态全在这里）"
log "日志：      $LOGF"
printf '\n下一步： ./bin/verify.sh --port %s      ./bin/stop.sh --port %s\n' "$PORT" "$PORT"
