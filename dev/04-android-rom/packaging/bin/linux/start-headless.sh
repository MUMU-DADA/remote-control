#!/usr/bin/env bash
# 无头启动这台虚拟机（默认 -no-window，适合服务器/CI）。
#
#   ./bin/start-headless.sh                      # default 实例，端口 5580，全新冷启动
#   ./bin/start-headless.sh --reuse               # 保留上次的数据、已装应用、快照
#   ./bin/start-headless.sh --name vm2 --port 5584
#   ./bin/start-headless.sh --memory 8192 --cores 6
#   ./bin/start-headless.sh --gpu swiftshader_indirect
#   ./bin/start-headless.sh --gui                 # 带窗口（要 X/桌面，调试用）
#   ./bin/start-headless.sh --bridge              # guest 走宿主 br0 上物理 LAN（仅 Linux）
#   ./bin/start-headless.sh --no-wait             # 起了就返回，不等开机
#
# 硬件参数全部来自 templates/config.ini（唯一真源）；命令行只在**显式传参**时覆盖
# —— 这一点很重要：命令行优先于 config.ini，随手写死默认值会让 config.ini 失效。
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

NAME="$DEFAULT_NAME"; PORT=""; GPU=""; MEM=""; CORES=""
REUSE=0; GUI=0; WAIT=1; WIPE=0; BRIDGE=""; ACCEL="auto"; TIMEOUT=300; TEST_INSTANCE=0

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
        --test-instance) TEST_INSTANCE=1 ;;
        --bridge)  BRIDGE=1 ;;
        --nat)     BRIDGE=0 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) die "未知参数：$1（./bin/start-headless.sh --help）" ;;
    esac
    shift
done

require_runtime
require_images
command -v curl >/dev/null 2>&1 || die "找不到 curl（服务就绪检测需要 HTTP 客户端）"

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
    die "端口必须是偶数：$PORT（此端口为 console，port+1 为 ADB）"
fi
OWNED_PORTS="$(instance_names_for_port "$PORT")"
if [ -n "$OWNED_PORTS" ] && [ "$OWNED_PORTS" != "$NAME" ]; then
    die "端口 $PORT 已登记给实例 '$OWNED_PORTS'；不能让 '$NAME' 覆盖它（./bin/status.sh 查看实例）"
fi
if port_listening "$PORT"; then
    die "端口 $PORT 已经有模拟器在跑（./bin/status.sh 看是谁）"
fi
SERIAL="$(serial_for_port "$PORT")"
SYSDIR="$(sysdir_for_port "$PORT")"
DATADIR="$(datadir_for_port "$PORT")"
LOGF="$(logfile_for_port "$PORT")"
REGISTERED_PORT="$(instance_port "$NAME")"
if [ "$REGISTERED_PORT" != "$PORT" ] && { [ -e "$SYSDIR" ] || [ -L "$SYSDIR" ] || [ -e "$DATADIR" ] || [ -L "$DATADIR" ]; }; then
    die "端口 $PORT 的工作目录已存在但不属于实例 '$NAME'：$SYSDIR / $DATADIR；拒绝覆盖未登记数据"
fi

# ---------------------------------------------------------------------------
# 前置自检：这四件事任一不满足，起不来或者起得很痛苦
# ---------------------------------------------------------------------------
if [ "$ACCEL" = auto ]; then
    if kvm_usable; then ACCEL=on; else ACCEL=off; fi
fi
if [ "$ACCEL" = on ] && ! kvm_usable; then
    die "要用 KVM 加速（-accel on）但 /dev/kvm 不可读写。
    裸机请确认已加载 kvm 模块、当前用户在 kvm 组；容器要 --device /dev/kvm。
    想硬着头皮用纯软件模拟（很慢，分钟级起步）： --accel off"
fi
if [ "$ACCEL" = off ]; then
    warn "没有 KVM 加速，走 TCG 纯软件模拟 —— 开机可能要几分钟到几十分钟"
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

# 服务配置来自 release/templates/config.ini；只在 guest 首次创建配置文件时生效。
service_prepare_token "$NAME" "$TEST_INSTANCE" "$REUSE"
service_register "$NAME" "$PORT" "$TEST_INSTANCE" "$REUSE"
build_service_property_args

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
# 「有渲染节点 ≠ 驱动能用」（虚拟 GPU/容器挂进来的 renderD 实测会崩），
# 所以判据是"console 已完成认证"，不是"探测到了"。
GPU_CANDIDATES=("$GPU_MODE")
[ "$GPU_AUTO" = 1 ] && [ "$GPU_MODE" = host ] && GPU_CANDIDATES+=("swiftshader_indirect")

# 桥接：桥不存在就不加 -net-tap（指过去会让模拟器直接起不来）
TAP_ARGS=()
if [ "$BRIDGE" = 1 ]; then
    NET_BRIDGE_IF="${NET_BRIDGE_IF:-br0}"
    if [ -d "/sys/class/net/$NET_BRIDGE_IF/bridge" ]; then
        TAP_ARGS=(-net-tap "tap$PORT" -net-tap-script-up "$TOOLS/net-bridge-ifup.sh")
        warn "桥接模式：guest 的 eth0 落在 $NET_BRIDGE_IF 的二层域里。"
        warn "  guest MAC 是 QEMU 默认值（所有实例相同）——**同时只让一台上物理 LAN**"
    else
        warn "没有桥接口 $NET_BRIDGE_IF（先 sudo tools/net-bridge.sh up），本次走默认 NAT"
    fi
fi

launch() {   # launch <gpu>
    : > "$LOGF"
    # ⚠️ 两个环境变量都要给：
    #   ANDROID_PRODUCT_OUT —— 模拟器靠它进"不用 AVD、直接从镜像目录启动"的模式
    #   ANDROID_BUILD_TOP   —— 少了它 SDK 版模拟器会去找 'kernel-qemu' 并报
    #                          "Your system directory is missing the 'kernel-qemu' image file"
    ANDROID_PRODUCT_OUT="$SYSDIR" ANDROID_BUILD_TOP="$ROOT" setsid nohup "$EMULATOR" \
        -sysdir "$SYSDIR" -datadir "$DATADIR" -port "$PORT" \
        -gpu "$1" -accel "$ACCEL" -memory "$MEM" -cores "$CORES" \
            "${SERVICE_PROPERTY_ARGS[@]}" \
        -no-boot-anim -no-audio -no-snapshot \
        $([ "$GUI" = 1 ] || printf '%s' "-no-window") \
        $([ "$WIPE" = 1 ] && printf '%s' "-wipe-data") \
        "${TAP_ARGS[@]}" \
        >> "$LOGF" 2>&1 < /dev/null &
    EMU_PID=$!
}

console_appears() {   # 最多 40 秒；进程提前退出即判定失败
    local i
    for i in 1 2 3 4 5 6 7 8; do
        console_ready "$PORT" && return 0
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
    if console_appears; then started=1; USED_GPU="$g"; break; fi
    warn "没起来：$(tail -2 "$LOGF" 2>/dev/null | tr '\n' ' ')"
    kill "$EMU_PID" 2>/dev/null || true
    console_kill "$PORT" || true
    sleep 3
done
[ "$started" = 1 ] || die "起不来。日志：$LOGF"

ok "已启动：$SERIAL（-gpu $USED_GPU -memory $MEM -cores $CORES）"
if [ "$GPU_AUTO" = 1 ]; then log "GPU 自适应依据：$(gpu_reason)"; fi

if ! service_create_redirect "$PORT"; then
    die "无法建立 HTTP 转发 127.0.0.1:$SERVICE_HTTP_PORT → guest:$SERVICE_GUEST_PORT（端口可能已占用）"
fi
log "服务地址：http://127.0.0.1:$SERVICE_HTTP_PORT"
[ "$SERVICE_AUTH" != 1 ] || log "访问令牌保存在 $(service_token_file "$NAME")（不在日志中显示）"

if [ "$WAIT" = 0 ]; then
    printf '\n后续： ./bin/status.sh --port %s     ./bin/verify.sh --port %s     ./bin/stop.sh --port %s\n' "$PORT" "$PORT" "$PORT"
    exit 0
fi

log "等 HTTP 服务就绪（不需要 ADB）"
if ! service_wait_ready "$PORT" "$TIMEOUT"; then
    die "HTTP 服务未就绪（${TIMEOUT}s）—— 看 $LOGF；复用实例若修改过服务端口/令牌，请更新实例登记或重新创建"
fi

# 开机耗时那一行是模拟器在 guest 报完之后才写的，可能比 adb 看到 boot_completed 晚一瞬 ——
# 给它三次机会，免得永远打印成空（实测踩过）
BOOTTIME=""
for _ in 1 2 3; do
    BOOTTIME="$(grep -oE '(boot time|Boot completed in) [0-9]+ ms' "$LOGF" 2>/dev/null | tail -1 || true)"
    [ -n "$BOOTTIME" ] && break
    sleep 1
done
ok "服务检测完成 ${BOOTTIME:+（$BOOTTIME）}"
log "设备序列号：$SERIAL"
log "工作目录：  $SYSDIR（镜像在 $IMAGES，只读；状态全在这里）"
log "日志：      $LOGF"
printf '\n下一步： ./bin/verify.sh --port %s      ./bin/stop.sh --port %s\n' "$PORT" "$PORT"
