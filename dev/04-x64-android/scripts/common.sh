#!/usr/bin/env bash
# 公共变量 —— 被 dev/04-x64-android/scripts 下其它脚本 source，不要直接执行。

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
DEVICE_DST="$AOSP_DIR/device/autosnap"
PRODUCT_NAME="autosnap_x64_arm64"
LUNCH_TARGET="$PRODUCT_NAME-userdebug"
PRODUCT_OUT="${PRODUCT_OUT:-$AOSP_DIR/out/target/product/$PRODUCT_NAME}"
# 想拿别的镜像做彩排（例如官方 google_apis 镜像）时直接覆盖：
#   PRODUCT_OUT=/path/to/sysdir EMULATOR_PORT=5562 ./scripts/run-linux.sh

# 官方镜像（翻译层来源）。API 31 = Android 12，与本 ROM 的 API 级别一致。
BRIDGE_API=31
BRIDGE_TAG=google_apis
BRIDGE_ABI=x86_64

SDK_MIRROR="${SDK_MIRROR:-https://mirrors.cloud.tencent.com/AndroidSDK}"
SDK_REPO="${SDK_REPO:-https://dl.google.com/android/repository}"

# 工具
BUILDER_CONTAINER="${BUILDER_CONTAINER:-autod-builder}"
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
