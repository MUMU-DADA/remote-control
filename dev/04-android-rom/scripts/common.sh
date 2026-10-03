#!/usr/bin/env bash
# 公共变量 —— 被 dev/04-android-rom/scripts 下其它脚本 source，不要直接执行。

set -euo pipefail

X64_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# 仓库根：从项目目录**向上找到含 aosp/ 的那一层**。
# 这样项目放在仓库根（早期）或放在 dev/ 下（现在）都能用，不必写死层数。
PROJECT_ROOT="$(cd "$X64_DIR" && while [ "$PWD" != "/" ] && [ ! -d "$PWD/aosp" ]; do cd ..; done; pwd)"
AOSP_DIR="$PROJECT_ROOT/aosp"
[ -d "$AOSP_DIR" ] || { echo "[x] 找不到 AOSP 树（向上找 aosp/ 失败）：$AOSP_DIR" >&2; exit 1; }

# 项目内目录
PAYLOAD_DIR="$X64_DIR/payload"          # 从官方镜像提取的翻译层
DEVICE_SRC="$X64_DIR/device"            # 设备树（本项目唯一真源）
ARTIFACTS_DIR="$X64_DIR/artifacts"      # 构建产物软链/清单
RUN_DIR="$X64_DIR/.run"                 # 运行期文件（datadir、日志、截图）

# ⚠️ 把 TMPDIR 指到数据盘。宿主机的 /tmp 是 16 GB 的 tmpfs，很容易被写满；
#    一旦写满：clang 报 "No space left on device"，docker exec 直接失败
#    （runc 要往宿主 /tmp 写进程文件）。踩过一次，见 docs/02-build-traps.md §5。
export TMPDIR="${TMPDIR_OVERRIDE:-$RUN_DIR/tmp}"
mkdir -p "$TMPDIR"

# AOSP 内的落点（由 apply-overlay.sh 同步过去）
DEVICE_DST="$AOSP_DIR/device/remote_control"

# ---------------------------------------------------------------------------
# 产品选择
#
# 两个产品的**本质差别**是"guest 是不是原生 arm64"，它决定一大票分支：
#
#   PRODUCT=x64_arm64（默认）  x86_64 guest + 用户态翻译层（libndk_translation）
#       → 宿主 x86_64：Linux(KVM) / Windows(WHPX) / Intel Mac(HVF，未实测)
#       → 需要 bridge/ 载荷、TARGET_NATIVE_BRIDGE_*、三条 ro.dalvik.vm.* 属性
#
#   PRODUCT=arm64              原生 arm64，无翻译层
#       → 宿主 arm64：Apple Silicon(HVF，待实测) / arm64 Linux(KVM，未实测)
#       → **不要**载荷、**不要**翻译层属性；应用必须自带 arm64-v8a 库
#
# 用法：
#   PRODUCT=arm64 ./scripts/build-rom.sh              # 编 arm64 ROM
#   PRODUCT=arm64 ./scripts/apply-overlay.sh          # 只同步 arm64 设备树
#   ./scripts/build-rom.sh                            # 默认仍是 x64_arm64，行为不变
#
# ⚠️ 产品名与产物目录**不同名**（实测踩过）：
#     lunch 目标 / PRODUCT_NAME  = remote_control_<PRODUCT>
#     产物目录                   = $AOSP_DIR/out/target/product/remote_control_<PRODUCT>
#   当前两个产品恰好同名，所以看不出差异；**换新设备树时务必核对**
#   `PRODUCT_OUT` 是不是真的落在了 lunch 之后 AOSP 打印的那个目录上。
#   参照系：上游 sdk_phone64_arm64 的产物目录是 emulator64_arm64（跟 PRODUCT_DEVICE 走），
#   两者不同名。见 docs/13-macos-port.md §7.0.1。
# ---------------------------------------------------------------------------
PRODUCT="${PRODUCT:-x64_arm64}"

case "$PRODUCT" in
    x64_arm64) HAS_BRIDGE=1 ;;
    arm64)     HAS_BRIDGE=0 ;;
    *) cat >&2 <<EOF
[x] 未知的 PRODUCT：$PRODUCT
    可选： x64_arm64（x86_64 guest + 翻译层，默认） / arm64（原生 arm64）
    用法： PRODUCT=arm64 $0 ...
EOF
       exit 1 ;;
esac

PRODUCT_NAME="remote_control_$PRODUCT"
LUNCH_TARGET="$PRODUCT_NAME-userdebug"
PRODUCT_OUT="${PRODUCT_OUT:-$AOSP_DIR/out/target/product/$PRODUCT_NAME}"
# 想拿别的镜像做彩排（例如官方 google_apis 镜像）时直接覆盖：
#   PRODUCT_OUT=/path/to/sysdir EMULATOR_PORT=5562 ./scripts/run-linux.sh

# 这个产品的 ROM 跑在哪种宿主上（只用于提示与文档口径，不参与构建）
#   x64_arm64 → x86_64 宿主；arm64 → arm64 宿主
PRODUCT_HOST_ARCH=$([ "$HAS_BRIDGE" = 1 ] && printf 'x86_64' || printf 'arm64')

# 官方镜像（翻译层来源）。API 31 = Android 12，与本 ROM 的 API 级别一致。
# ⚠️ 只有 x64_arm64 产品用得上；arm64 产品下 fetch-payload.sh / apply-overlay.sh
#    会**整段跳过**（翻译层在原生 arm64 上不存在，见 docs/13-macos-port.md §2.2）。
BRIDGE_API=31
BRIDGE_TAG=google_apis
BRIDGE_ABI=x86_64

SDK_MIRROR="${SDK_MIRROR:-https://mirrors.cloud.tencent.com/AndroidSDK}"
SDK_REPO="${SDK_REPO:-https://dl.google.com/android/repository}"

# 工具
BUILDER_CONTAINER="${BUILDER_CONTAINER:-remote-control-builder}"
BUILDER_AOSP_PATH="${BUILDER_AOSP_PATH:-/aosp}"
ADB="${ADB:-$AOSP_DIR/out/host/linux-x86/bin/adb}"
EMULATOR_BIN="${EMULATOR_BIN:-}"
EMULATOR_PORT="${EMULATOR_PORT:-5580}"

# 模拟器运行配置（屏幕尺寸/密度等）。**本项目唯一真源** —— ROM 构建产物里那份
# config.ini 是 goldfish 的 1440x2960@560，由 run-linux.sh / package-rom.sh /
# run-windows.ps1 三处统一覆盖成这里的内容。不参与 AOSP 构建，改完无需重编 ROM。
EMULATOR_CONFIG="${EMULATOR_CONFIG:-$X64_DIR/emulator/config.ini}"

# 桥接（-net-tap）：宿主机上已就绪的桥接口名。留空 = 用模拟器默认的用户态 NAT。
#   ./tools/net-bridge.sh up   建 br0（把 ens33 桥进去）→ 之后启动即自动走桥接
# ⚠️ 这里用 ${VAR-default} 而**不是** ${VAR:-default}：`:-` 对"已设置但为空"也替换成默认值，
#    那样 `NET_BRIDGE_IF= ./run-linux.sh` 就关不掉桥接了（实测踩过：想关桥却照样加了
#    -net-tap，第二台实例去抢同一个 tap0 → "could not configure /dev/net/tun (tap0):
#    Device or resource busy"）。
NET_BRIDGE_IF="${NET_BRIDGE_IF-br0}"
# 注意：NET_TAP_IF 与 EMULATOR_DATADIR 都**按端口派生**，在 run-linux.sh 解析完 --port
# 之后才算（见那里）。放在这里会因为端口还没被覆盖而算成 5580 的值 —— 多实例时
# 一个抢 tap、一个共用不存在的 datadir。
JOBS="${JOBS:-12}"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 从 emulator/config.ini 读"硬件"参数
#
# ⚠️ 这些值以前是**写死在 run-linux.sh 的命令行上**的（-memory 4096 -cores 4
#    -gpu swiftshader_indirect），而**命令行优先于 config.ini** ——
#    于是改 config.ini 里的 hw.ramSize 一点用都没有：hardware-qemu.ini 里
#    照样是命令行那个值。现在命令行不再写死，统一从这里读，只有一处真源。
config_get() {   # config_get <键> [默认值]
    local key="$1" def="${2-}" v=""
    if [ -s "$EMULATOR_CONFIG" ]; then
        # 取最后一个匹配：文件里同一键可能先举例后赋值
        v="$(sed -n "s/^[[:space:]]*${key}[[:space:]]*=[[:space:]]*\(.*[^[:space:]]\)[[:space:]]*$/\1/p" \
             "$EMULATOR_CONFIG" | tail -1)"
    fi
    if [ -n "$v" ]; then printf '%s' "$v"; else printf '%s' "$def"; fi
}

# 宿主有没有**可用的** GPU 渲染节点。
#
# ⚠️ 不能只看 /dev/dri 目录在不在 —— 容器里经常挂着一个空目录。
#    要看 renderD* 是真字符设备、而且当前用户能打开（否则 -gpu host 会
#    起不来，或者起来之后画面全黑）。
host_gpu_available() {
    local d
    for d in /dev/dri/renderD*; do
        [ -c "$d" ] && [ -r "$d" ] && [ -w "$d" ] && return 0
    done
    return 1
}

# 把 config.ini 里的 hw.gpu.mode 解析成模拟器真正认的那个 -gpu 值。
#
# 规则：
#   · auto / 空  → 宿主有 GPU 就 host，否则 swiftshader_indirect  ← 自适应
#   · 其余取值    → 原样透传（想钉死某一档就写死，比如 swiftshader_indirect）
resolve_gpu_mode() {   # resolve_gpu_mode [config.ini 里的值]
    local want="${1:-auto}"
    case "$want" in
        auto|"") if host_gpu_available; then printf 'host'
                 else printf 'swiftshader_indirect'; fi ;;
        *)       printf '%s' "$want" ;;
    esac
}

gpu_mode_reason() {   # 给日志用的一句话解释
    if host_gpu_available; then
        printf '宿主有 GPU 渲染节点（%s）' "$(ls /dev/dri/renderD* 2>/dev/null | head -1)"
    else
        printf '宿主没有可用的 GPU 渲染节点（/dev/dri/renderD*）'
    fi
}

# ---------------------------------------------------------------------------
# 实例：名字 ↔ 端口。状态文件在 .run/instances/<名字>.env
#
# 端口从 5580 起偶数分配（模拟器要求偶数：console 口 = port+1）。
# 工作目录沿用 run-linux.sh 的按端口派生规则，两边可以混用：
#   .run/sysdir-<port>/  .run/datadir-<port>/  .run/emulator-<port>.log
INSTANCES_DIR="$RUN_DIR/instances"
EMULATOR_PORT_BASE="${EMULATOR_PORT_BASE:-5580}"

instance_file() { printf '%s/%s.env' "$INSTANCES_DIR" "$1"; }

instance_exists() { [ -s "$(instance_file "$1")" ]; }

instance_port() {   # 没登记就返回空
    local f; f="$(instance_file "$1")"
    [ -s "$f" ] || return 0
    sed -n 's/^PORT=//p' "$f" | tail -1
}

instance_names() {
    [ -d "$INSTANCES_DIR" ] || return 0
    local f
    for f in "$INSTANCES_DIR"/*.env; do
        [ -s "$f" ] || continue
        basename "$f" .env
    done
}

port_taken() {   # 已被登记的实例占用？
    local p="$1" n
    for n in $(instance_names); do
        [ "$(instance_port "$n")" = "$p" ] && return 0
    done
    return 1
}

port_listening() {   # 真有进程在用（含没登记的野实例）
    [ -n "$(emu_pid_for_port "$1")" ]
}

alloc_port() {
    local p="$EMULATOR_PORT_BASE"
    while [ "$p" -lt 5700 ]; do
        if ! port_taken "$p" && ! port_listening "$p"; then printf '%s' "$p"; return 0; fi
        p=$((p + 2))
    done
    die "找不到空闲端口（$EMULATOR_PORT_BASE..5700 都占着）"
}

instance_register() {   # instance_register <名字> <端口>
    mkdir -p "$INSTANCES_DIR"
    cat > "$(instance_file "$1")" <<EOF
# 由 scripts/emulator.sh 维护，手改也行（PORT 一行就够）
PORT=$2
EOF
}

instance_unregister() { rm -f "$(instance_file "$1")"; }

# 找某个端口上的模拟器主进程。
#
# ⚠️ 不能用 `pgrep -f "qemu-system.* -port N"` —— **它会把调用者自己匹配上**：
#    外层 `bash -c '... pgrep -f "qemu-system.* -port 5582" ...'` 的命令行里
#    就含这段文字，正则照样匹配。实测踩到：stop 明明成功了，
#    pgrep 却报"还有进程"；反过来 instance_running 会假阳性，
#    而 kill 有可能**去杀一个无辜的进程**。
#
#    所以这里逐个读 /proc：先看 comm（进程名）必须是 qemu-system-*，
#    再看命令行里有没有 " -port <端口> "。名字这一关调用者过不了。
emu_pid_for_port() {
    local port="$1" p comm
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        read -r comm < "$p/comm" 2>/dev/null || continue
        case "$comm" in qemu-system-*) ;; *) continue ;; esac
        # ⚠️ 端口两边要有空格：不加的话 "-port 558" 会匹配到 "-port 5580"
        if tr '\0' ' ' < "$p/cmdline" 2>/dev/null | grep -q " -port $port "; then
            printf '%s\n' "${p#/proc/}"
        fi
    done
}

instance_running() { [ -n "$(emu_pid_for_port "$(instance_port "$1")")" ]; }

