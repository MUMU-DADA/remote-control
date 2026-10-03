#!/usr/bin/env bash
# 包内公共函数 —— 被 bin/ 下其它脚本 source，不要直接执行。
#
# 这个包是"解压即用"的：所有路径都相对**包根**（bin/ 的上一级）解析，
# 不依赖仓库、不依赖系统里的 Android SDK。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME="${AUTOSNAP_RUNTIME:-$ROOT/runtime}"
IMAGES="${AUTOSNAP_IMAGES:-$ROOT/images}"
TEMPLATES="${AUTOSNAP_TEMPLATES:-$ROOT/templates}"
TOOLS="$ROOT/tools"
RUN_DIR="${AUTOSNAP_RUN_DIR:-$ROOT/.run}"
INSTANCES_DIR="$RUN_DIR/instances"
CONFIG_FILE="${AUTOSNAP_CONFIG:-$TEMPLATES/config.ini}"
EMULATOR="${AUTOSNAP_EMULATOR:-$RUNTIME/emulator/emulator}"
# 自带 adb：这样"解压即用"不要求宿主装 Android SDK。
# 想用系统那份（例如宿主已有 adb server 在跑、不想换版本）：
#   AUTOSNAP_ADB=/usr/bin/adb ./bin/start-headless.sh
ADB="${AUTOSNAP_ADB:-$RUNTIME/platform-tools/adb}"

PORT_BASE="${AUTOSNAP_PORT_BASE:-5580}"
DEFAULT_NAME="default"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }
ok()   { printf '\033[1;32m[✓]\033[0m %s\n' "$*"; }
bad()  { printf '\033[1;31m[✗]\033[0m %s\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# config.ini：硬件参数的唯一真源（templates/config.ini）
# ---------------------------------------------------------------------------
config_get() {   # config_get <键> [默认值]
    local key="$1" def="${2-}" v=""
    if [ -s "$CONFIG_FILE" ]; then
        # 取最后一个匹配：文件里同一个键可能先出现在注释举例里
        v="$(sed -n "s/^[[:space:]]*${key}[[:space:]]*=[[:space:]]*\(.*[^[:space:]]\)[[:space:]]*$/\1/p" \
             "$CONFIG_FILE" | tail -1)"
    fi
    if [ -n "$v" ]; then printf '%s' "$v"; else printf '%s' "$def"; fi
}

# ---------------------------------------------------------------------------
# GPU：探测 + 自适应
#
# ⚠️ 不能只看 /dev/dri 在不在 —— 有渲染节点 **不等于** 驱动能用。
#    实测（VMware 的虚拟 GPU）：renderD128 在、也能读写，选 host 之后模拟器
#    照样 "Could not start renderer!"。所以探测只是第一层，
#    真起不来还得靠 start-headless.sh 里那条回退链。
# ---------------------------------------------------------------------------
host_gpu_available() {
    local d
    for d in /dev/dri/renderD*; do
        [ -c "$d" ] && [ -r "$d" ] && [ -w "$d" ] && return 0
    done
    return 1
}

resolve_gpu_mode() {   # resolve_gpu_mode [config.ini 里的 hw.gpu.mode]
    local want="${1:-auto}"
    case "$want" in
        auto|"") if host_gpu_available; then printf 'host'; else printf 'swiftshader_indirect'; fi ;;
        *)       printf '%s' "$want" ;;
    esac
}

gpu_reason() {
    if host_gpu_available; then
        printf '宿主有可用 GPU 渲染节点（%s）' "$(ls /dev/dri/renderD* 2>/dev/null | sed -n '1p')"
    else
        printf '宿主没有可用的 GPU 渲染节点（/dev/dri/renderD*），用软件渲染'
    fi
}

# ---------------------------------------------------------------------------
# 实例：名字 ↔ 端口。登记文件在 .run/instances/<名字>.env
# 端口从 5580 起偶数分配（模拟器要求偶数：console 口 = port+1）。
# ---------------------------------------------------------------------------
instance_file() { printf '%s/%s.env' "$INSTANCES_DIR" "$1"; }
instance_exists() { [ -s "$(instance_file "$1")" ]; }
instance_names() {
    [ -d "$INSTANCES_DIR" ] || return 0
    local f
    for f in "$INSTANCES_DIR"/*.env; do
        [ -s "$f" ] || continue
        basename "$f" .env
    done
}
instance_port() {
    local f; f="$(instance_file "$1")"
    [ -s "$f" ] || return 0
    sed -n 's/^PORT=//p' "$f" | tail -1
}
instance_register() {
    mkdir -p "$INSTANCES_DIR"
    cat > "$(instance_file "$1")" <<EOF
# 实例登记（名字 ↔ 端口）。由 bin/start-headless.sh 维护，手改也行。
PORT=$2
EOF
}
instance_unregister() { rm -f "$(instance_file "$1")"; }

sysdir_for_port()  { printf '%s/sysdir-%s'  "$RUN_DIR" "$1"; }
datadir_for_port() { printf '%s/datadir-%s' "$RUN_DIR" "$1"; }
logfile_for_port() { printf '%s/emulator-%s.log' "$RUN_DIR" "$1"; }
serial_for_port()  { printf 'emulator-%s' "$1"; }

# 找某端口上的模拟器主进程。
#
# ⚠️ 不能用 `pgrep -f "qemu-system.* -port N"`：**它会把调用者自己匹配上**
#    （外层 bash -c 的命令行里就含这段文字），于是 stop 报"还有进程"、
#    instance_running 假阳性，而 kill 有可能去杀一个无辜的进程。
#    所以逐个读 /proc：先看 comm 必须是 qemu-system-*，再看命令行里的端口。
emu_pid_for_port() {
    local port="$1" p comm
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        read -r comm < "$p/comm" 2>/dev/null || continue
        case "$comm" in qemu-system-*) ;; *) continue ;; esac
        # 端口两边要有空格：不加的话 "-port 558" 会匹配到 "-port 5580"
        if tr '\0' ' ' < "$p/cmdline" 2>/dev/null | grep -q " -port $port "; then
            printf '%s\n' "${p#/proc/}"
        fi
    done
}

port_listening() { [ -n "$(emu_pid_for_port "$1")" ]; }
port_taken() {
    local p="$1" n
    for n in $(instance_names); do
        [ "$(instance_port "$n")" = "$p" ] && return 0
    done
    return 1
}
alloc_port() {
    local p="$PORT_BASE"
    while [ "$p" -lt 5700 ]; do
        if ! port_taken "$p" && ! port_listening "$p"; then printf '%s' "$p"; return 0; fi
        p=$((p + 2))
    done
    die "找不到空闲端口（$PORT_BASE..5700 都占着）"
}

# ---------------------------------------------------------------------------
# 前置自检
# ---------------------------------------------------------------------------
require_runtime() {
    [ -x "$EMULATOR" ] || die "找不到模拟器：$EMULATOR
    运行时装在 runtime/ 里；看 runtime/RUNTIME.txt 确认包里带了哪个版本"
    local backend="$RUNTIME/emulator/qemu/linux-x86_64/qemu-system-x86_64-headless"
    [ -x "$backend" ] || die "运行时里没有 x86_64 无头后端：$backend
    本 ROM 是 x86_64 guest，别的架构后端带不动它"
}

REQUIRED_IMAGES="system-qemu.img vendor-qemu.img product-qemu.img ramdisk-qemu.img \
kernel-ranchu encryptionkey.img userdata.img advancedFeatures.ini config.ini"

require_images() {
    [ -d "$IMAGES" ] || die "找不到镜像目录：$IMAGES"
    local missing="" f
    for f in $REQUIRED_IMAGES; do [ -s "$IMAGES/$f" ] || missing="$missing $f"; done
    [ -s "$IMAGES/system/build.prop" ] || missing="$missing system/build.prop"
    [ -n "$missing" ] && die "镜像不全，缺：$missing
    镜像目录：$IMAGES"
    return 0
}

# KVM：能加速就加速，不能就明确告诉用户接下来会很慢（TCG）。
kvm_usable() { [ -r /dev/kvm ] && [ -w /dev/kvm ]; }

require_adb() { [ -x "$ADB" ] || die "找不到 adb：$ADB（可 AUTOSNAP_ADB=/path/to/adb 覆盖）"; }

disk_free_mb() { df -Pk "$1" 2>/dev/null | awk 'NR==2{print int($4/1024)}'; }

# ---------------------------------------------------------------------------
# 工作目录：把 images/ 链进实例自己的 sysdir
#
# ⚠️ 两个文件**不能链**，必须实文件：
#   initrd     —— 模拟器会**透过符号链接**重写它（把自己的 ramdisk + dtb 合进去），
#                 链过去就把 images/ 里那份改了，SHA256SUMS 当场校验失败。
#   config.ini —— 要按包内模板覆盖，链过去会写穿共享的那份。
# 实测踩过（上游项目 docs/11-snapshots-and-multi.md 记录了同一条）。
# ---------------------------------------------------------------------------
NO_LINK="initrd config.ini"

build_sysdir() {   # build_sysdir <端口> fresh|keep
    local port="$1" mode="${2:-fresh}" sysdir; sysdir="$(sysdir_for_port "$port")"
    if [ "$mode" = fresh ]; then rm -rf "$sysdir"; fi
    mkdir -p "$sysdir"
    local item name
    for item in "$IMAGES"/* "$IMAGES"/.[!.]*; do
        [ -e "$item" ] || continue
        name="$(basename "$item")"
        case " $NO_LINK " in *" $name "*) continue ;; esac
        # keep 模式：已有就别动（可能是上次跑留下的快照/状态，删了等于毁机器）
        [ "$mode" = keep ] && [ -e "$sysdir/$name" ] && continue
        ln -s "$item" "$sysdir/$name"
    done
    # config.ini：用包内模板写一份实文件
    [ -s "$CONFIG_FILE" ] || die "模板缺失：$CONFIG_FILE"
    cp -f "$CONFIG_FILE" "$sysdir/config.ini"

    # 收尾核对：images/ 里每一项（除 NO_LINK）都必须在 sysdir 里能看到。
    # "链接没建成但命令没报错"这种失败必须当场暴露，而不是等模拟器起来报文件找不到。
    local missing=""
    for item in "$IMAGES"/*; do
        [ -e "$item" ] || continue
        name="$(basename "$item")"
        case " $NO_LINK " in *" $name "*) continue ;; esac
        [ -e "$sysdir/$name" ] || missing="$missing $name"
    done
    [ -n "$missing" ] && die "工作目录缺文件：$missing"
    return 0
}

# ---------------------------------------------------------------------------
# 等开机
# ---------------------------------------------------------------------------
wait_for_boot() {   # wait_for_boot <端口> [超时秒]
    local port="$1" timeout="${2:-300}" serial; serial="$(serial_for_port "$port")"
    local t0=$SECONDS
    "$ADB" -s "$serial" wait-for-device >/dev/null 2>&1 || true
    while [ $((SECONDS - t0)) -lt "$timeout" ]; do
        if [ "$("$ADB" -s "$serial" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' | sed -n '1p')" = "1" ]; then
            return 0
        fi
        # 进程没了就别再等
        [ -n "$(emu_pid_for_port "$port")" ] || return 1
        sleep 3
    done
    return 1
}

adb_prop() {   # adb_prop <端口> <属性>
    # 末尾 `|| true` 是必须的：设备没起来时 adb 返回非 0，set -e 会把调用者带走
    "$ADB" -s "$(serial_for_port "$1")" shell getprop "$2" 2>/dev/null | tr -d '\r' | sed -n '1p' || true
}

human_mb() {   # human_mb <MB>
    local mb="$1"
    if [ "$mb" -ge 1024 ]; then printf '%d.%d GiB' $((mb / 1024)) $(( (mb % 1024) * 10 / 1024 )); else printf '%d MiB' "$mb"; fi
}
