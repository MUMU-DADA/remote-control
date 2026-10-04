#!/usr/bin/env bash
# 包内公共函数（**macOS 版**）—— 被 bin/ 下其它脚本 source，不要直接执行。
#
# 与 bin/linux/lib.sh 的语义逐条对齐，只换平台实现。四处**平台相关的重写**：
#
#   1. 进程枚举：macOS 没有 /proc，改用 `ps -eo pid,comm,args`
#      ⚠️ 仍然不能用 `pgrep -f "qemu-system.* -port N"` —— 它会把调用者自己匹配上
#         （外层 bash -c 的命令行里就含这段文字），于是 stop 报"还有进程"、
#         instance_running 假阳性，而 kill 有可能去杀一个无辜的进程。
#         这个坑在 Linux 侧实测踩过，macOS 上同样存在。
#   2. GPU 探测：macOS 没有 /dev/dri，用 `system_profiler SPDisplaysDataType`
#      （对应 Windows 侧的 Get-CimInstance Win32_VideoController）。
#      ⚠️ 不探 GPU 而直接选 host 在 Mac 上通常没问题（Metal 一直有），
#         但**虚拟机里的 macOS** 会没有硬件金属支持 —— 所以还是探一下。
#   3. 虚拟化：macOS 用 Hypervisor.framework，判据是 `sysctl kern.hv_support`（不是 /dev/kvm）。
#   4. 后端架构：按**本机架构**取目录名（darwin-aarch64 / darwin-x64 / linux-x86_64 …），
#      不写死。写死的后果：Apple Silicon 上找不到 x86_64 后端，或者反过来。
#
# ⚠️ 本文件只用 bash 3.2 语法（macOS 自带 /bin/bash 就是 3.2）：
#    没有 declare -A、没有 ${var^^}、没有 mapfile、没有数组 +=。
#
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
#   AUTOSNAP_ADB=/usr/local/bin/adb ./bin/start-headless.sh
ADB="${AUTOSNAP_ADB:-$RUNTIME/platform-tools/adb}"

PORT_BASE="${AUTOSNAP_PORT_BASE:-5580}"
DEFAULT_NAME="default"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }
ok()   { printf '\033[1;32m[✓]\033[0m %s\n' "$*"; }
bad()  { printf '\033[1;31m[✗]\033[0m %s\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# 平台判定（先是 macOS，再定后端目录名）
# ---------------------------------------------------------------------------
HOST_OS="$(uname -s)"
[ "$HOST_OS" = "Darwin" ] || die "这套脚本只用于 macOS（uname -s = $HOST_OS）。
    Linux 用 bin/linux/ 那一套；Windows 用 bin/windows/。"

HOST_MACH="$(uname -m)"
case "$HOST_MACH" in
    arm64)  QEMU_DIR="darwin-aarch64"; GUEST_ABI_HINT="arm64-v8a" ;;
    x86_64) QEMU_DIR="darwin-x64";     GUEST_ABI_HINT="x86_64" ;;
    *)      die "认不出的宿主架构：$HOST_MACH" ;;
esac

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

build_service_property_args() {   # build_service_property_args [test-instance]
    local test_instance="${1:-0}" enabled bind port auth token adb_enabled
    enabled="${SERVICE_ENABLED:-$(config_get service.enabled 1)}"
    bind="${SERVICE_BIND:-$(config_get service.bind 0.0.0.0)}"
    port="${SERVICE_GUEST_PORT:-$(config_get service.port 8088)}"
    auth="${SERVICE_AUTH:-$(config_get service.auth 1)}"
    token="${SERVICE_TOKEN-$(config_get service.token '')}"
    adb_enabled="${SERVICE_ADB:-$(config_get service.adb_enabled 1)}"
    case "$enabled:$auth:$adb_enabled" in
        0:0:0|0:0:1|0:1:0|0:1:1|1:0:0|1:0:1|1:1:0|1:1:1) ;;
        *) die "service.enabled/auth/adb_enabled 只能是 0 或 1" ;;
    esac
    case "$port" in ''|*[!0-9]*) die "service.port 必须是 1-65535 的整数" ;; esac
    [ "$port" -ge 1 ] && [ "$port" -le 65535 ] || die "service.port 必须是 1-65535 的整数"
    [[ "$bind" =~ ^[A-Za-z0-9.:_-]+$ ]] || die "service.bind 格式不受支持：$bind"
    [[ "$token" =~ ^[A-Za-z0-9_-]{0,80}$ ]] || die "service.token 仅支持 80 字节内的字母、数字、下划线和连字符"
    if [ "$test_instance" = 1 ]; then auth=0; token=""; fi
    SERVICE_PROPERTY_ARGS=(
        -prop "qemu.rc.enabled=$enabled"
        -prop "qemu.rc.bind=$bind"
        -prop "qemu.rc.port=$port"
        -prop "qemu.rc.auth=$auth"
        -prop "qemu.rc.adb=$adb_enabled"
    )
    [ -z "$token" ] || SERVICE_PROPERTY_ARGS+=(-prop "qemu.rc.token=$token")
}

# ---------------------------------------------------------------------------
# GPU：探测 + 自适应（macOS：system_profiler）
#
# ⚠️ 判据不是"有没有显卡"那么粗 —— 要看**有没有 Metal 支持**。
#    虚拟机里的 macOS 会列出显示设备但没有 Metal，此时必须退到软件渲染。
#    与 Linux 侧同一条纪律：探测只是第一层，真起不来还得靠 start-headless.sh 的回退链。
# ---------------------------------------------------------------------------
host_gpu_available() {
    local out
    out="$(system_profiler SPDisplaysDataType 2>/dev/null || true)"
    [ -n "$out" ] || return 1
    # 有 Metal 字样或 Metal 支持行才算能用硬件渲染
    if printf '%s' "$out" | grep -qi "Metal"; then return 0; fi
    # 老系统/虚拟机上可能没有 Metal 行；退一步看有没有真实芯片型号
    printf '%s' "$out" | grep -qiE "Chipset Model|VRAM|Apple M[0-9]" && return 0
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
        printf '宿主有可用的 Metal GPU（system_profiler SPDisplaysDataType 有 Metal 支持）'
    else
        printf '宿主没有可用的 Metal GPU（虚拟机里的 macOS 常见），用软件渲染 swiftshader_indirect'
    fi
}

# ---------------------------------------------------------------------------
# 实例：名字 ↔ 端口。登记文件在 .run/instances/<名字>.env
# 端口从 5580 起偶数分配（模拟器要求偶数：console 口 = port，ADB 口 = port+1）。
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
instance_register() {   # instance_register <名字> <端口>
    mkdir -p "$INSTANCES_DIR"
    printf '# 由 bin 下的脚本维护；手改也行（PORT 一行就够）\nPORT=%s\n' "$2" > "$(instance_file "$1")"
}
instance_unregister() { rm -f "$(instance_file "$1")"; }

sysdir_for_port()  { printf '%s/sysdir-%s'  "$RUN_DIR" "$1"; }
datadir_for_port() { printf '%s/datadir-%s' "$RUN_DIR" "$1"; }
logfile_for_port() { printf '%s/emulator-%s.log' "$RUN_DIR" "$1"; }
serial_for_port()  { printf 'emulator-%s' "$1"; }

# 找某端口上的模拟器主进程（macOS 版：ps，不读 /proc）
#
# ⚠️ 端口两边要带空格地比：`-port 558` 不能匹配到 `-port 5580`。
#    这里用 awk 按空白切字段做**精确**比较，而不是 grep 子串 ——
#    子串匹配在 macOS 上更容易误伤（ps 的输出里还有别的参数）。
emu_pid_for_port() {
    local port="$1"
    ps -Ao pid=,comm=,args= 2>/dev/null | awk -v p="$port" '
        {
            pid = $1
            comm = $2
            # 只认 qemu-system-* 的进程名（这一关调用者过不了，见文件头第 1 条）
            n = split(comm, parts, "/")
            if (parts[n] !~ /^qemu-system-/) next
            # 在整条命令行里找精确的 -port <port>
            for (i = 1; i < NF; i++) {
                if ($i == "-port" && $(i+1) == p) { print pid; next }
            }
        }'
}

port_listening() { [ -n "$(emu_pid_for_port "$1")" ]; }
port_taken() {
    local p="$1" n
    for n in $(instance_names); do
        [ "$(instance_port "$n")" = "$p" ] && return 0
    done
    return 1
}
instance_names_for_port() {
    local p="$1" n
    for n in $(instance_names); do
        [ "$(instance_port "$n")" = "$p" ] && printf '%s\n' "$n"
    done
    return 0
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
# 后端路径按**本机架构**拼，不写死（见文件头第 4 条）
backend_path() { printf '%s/emulator/qemu/%s/qemu-system-aarch64-headless' "$RUNTIME" "$QEMU_DIR"; }
backend_path_x64() { printf '%s/emulator/qemu/%s/qemu-system-x86_64-headless' "$RUNTIME" "$QEMU_DIR"; }

require_runtime() {
    [ -x "$EMULATOR" ] || die "找不到模拟器：$EMULATOR
    运行时装在 runtime/ 里；看 runtime/RUNTIME.txt 确认包里带了哪个版本。
    若还没准备： macos/fetch-emulator.sh"
    local backend
    if [ "$HOST_MACH" = "arm64" ]; then
        backend="$(backend_path)"
        [ -x "$backend" ] || die "运行时里没有 arm64 无头后端：$backend
    Apple Silicon 上只有 darwin-aarch64 的包带得动 **arm64 原生 ROM**；
    现有的 x86_64 ROM 在 Apple Silicon 上**没有**可用的后端（不是慢，是没有）。
    详见 docs/13-macos-port.md §7.0.2。"
    else
        backend="$(backend_path_x64)"
        [ -x "$backend" ] || die "运行时里没有 x86_64 无头后端：$backend
    Intel Mac 上要跑现有 x86_64 ROM，需要 darwin-x64 的包。"
    fi
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

# 虚拟化：macOS 是 Hypervisor.framework（对应 Linux 的 /dev/kvm）
hv_usable() { [ "$(sysctl -n kern.hv_support 2>/dev/null || echo 0)" = "1" ]; }

require_adb() { [ -x "$ADB" ] || die "找不到 adb：$ADB（可 AUTOSNAP_ADB=/path/to/adb 覆盖）"; }

# macOS 的 df -k 可用（POSIX）；不用 -P（BSD df 的列语义与 GNU 略有差别，这里只取第 4 列）
disk_free_mb() { df -k "$1" 2>/dev/null | awk 'NR==2{print int($4/1024)}'; }

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
    if declare -F optimize_image_storage >/dev/null; then
        optimize_image_storage "$IMAGES" "$RUNTIME/emulator/qemu-img"
    fi
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

# Host management uses the emulator console and HTTP; ADB remains for verify.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/console.sh"
if [ -f "$(dirname "${BASH_SOURCE[0]}")/storage.sh" ]; then
    source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/storage.sh"
fi
