#!/usr/bin/env bash
# 公共变量与函数 —— 被 linux-arm64/ 下其它脚本 source，不要直接执行。

set -euo pipefail

EMU_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$EMU_DIR/../../.." && pwd)"
AOSP_DIR="$PROJECT_ROOT/aosp"

# ---------------------------------------------------------------------------
# 目标 ABI —— 本轨道最重要的一组开关
#
#   arm64  = sdk_phone64_arm64-userdebug   → out/target/product/emulator64_arm64
#            与产品一致；跨架构只能 TCG，首次开机 10~40 分钟。
#            **Windows 侧要跑的 ROM 也是它编出来的**，所以迟早要编。
#
#   x86_64 = sdk_phone64_x86_64-userdebug  → out/target/product/emulator64_x86_64
#            同架构，KVM 加速，开机几十秒 —— 开发机日常内循环用它。
#            源码级验证（截图链路 / 触控 / 协议 / 分发）与 arm64 完全等价，
#            只有 ABI 相关的问题才必须回到 arm64。
#
# 切换方式（三选一）：
#   ./run-emulator.sh --fast          # 单次
#   export EMU_ABI=x86_64             # 本次 shell
#   LUNCH_TARGET=... PRODUCT_DEVICE=... ./run-emulator.sh   # 完全自定义
# ---------------------------------------------------------------------------
EMU_ABI="${EMU_ABI:-arm64}"

case "$EMU_ABI" in
    arm64)
        LUNCH_TARGET="${LUNCH_TARGET:-sdk_phone64_arm64-userdebug}"
        PRODUCT_DEVICE="${PRODUCT_DEVICE:-emulator64_arm64}"
        EMULATOR_ACCEL="${EMULATOR_ACCEL:-off}"     # 跨架构，硬件加速用不上
        BOOT_TIMEOUT_S="${BOOT_TIMEOUT_S:-2700}"    # 45 分钟
        ABI_NOTE="arm64 guest / x86_64 宿主 → 纯软件模拟（TCG），首次开机 10~40 分钟"
        ;;
    x86_64)
        LUNCH_TARGET="${LUNCH_TARGET:-sdk_phone64_x86_64-userdebug}"
        PRODUCT_DEVICE="${PRODUCT_DEVICE:-emulator64_x86_64}"
        EMULATOR_ACCEL="${EMULATOR_ACCEL:-on}"      # 同架构，走 KVM
        BOOT_TIMEOUT_S="${BOOT_TIMEOUT_S:-900}"     # 15 分钟足够
        ABI_NOTE="x86_64 guest / x86_64 宿主 → KVM 加速，开机几十秒（日常内循环用这个）"
        ;;
    *)
        printf '\033[1;31m[x]\033[0m EMU_ABI 只能是 arm64 或 x86_64（当前：%s）\n' "$EMU_ABI" >&2
        exit 1 ;;
esac

PRODUCT_OUT="$AOSP_DIR/out/target/product/$PRODUCT_DEVICE"

# 另一个 ABI 的产物目录（check-env 用来提示"你其实已经编过另一个"）
if [ "$EMU_ABI" = arm64 ]; then
    OTHER_PRODUCT_OUT="$AOSP_DIR/out/target/product/emulator64_x86_64"
else
    OTHER_PRODUCT_OUT="$AOSP_DIR/out/target/product/emulator64_arm64"
fi

# AOSP 自带的模拟器（30.8.3），内含 qemu/linux-x86_64/qemu-system-{aarch64,x86_64}
# 可用 EMULATOR_BIN=<别处的 emulator> 覆盖 —— 例如用 SDK 版 37.x 在 Linux 上
# 提前验证"新版模拟器能不能启动 A12 镜像"（等价于 Windows 侧要面对的问题）。
EMULATOR_PREBUILTS="$AOSP_DIR/prebuilts/android-emulator/linux-x86_64"
EMULATOR_BIN="${EMULATOR_BIN:-$EMULATOR_PREBUILTS/emulator}"
EMULATOR_CHECK="${EMULATOR_CHECK:-$EMULATOR_PREBUILTS/emulator-check}"

# 宿主没有 adb，用 AOSP 编出来的（m adb / 全量构建都会产出）
ADB="${ADB:-$AOSP_DIR/out/host/linux-x86/bin/adb}"

# 构建容器（见 docs/07-environment.md）
BUILDER_CONTAINER="${BUILDER_CONTAINER:-autod-builder}"
BUILDER_AOSP_PATH="${BUILDER_AOSP_PATH:-/aosp}"
JOBS="${JOBS:-12}"

# 运行期文件（userdata 覆盖层、控制台日志、截图）放这里，不污染 AOSP 的 out/
RUN_DIR="${RUN_DIR:-$EMU_DIR/.run}"
mkdir -p "$RUN_DIR"

# 模拟器启动参数（都可用环境变量覆盖）
EMULATOR_PORT="${EMULATOR_PORT:-5554}"
EMULATOR_MEMORY_MB="${EMULATOR_MEMORY_MB:-4096}"     # TCG 吃内存，别低于 3072
EMULATOR_CORES="${EMULATOR_CORES:-4}"
EMULATOR_GPU="${EMULATOR_GPU:-swiftshader_indirect}" # 无显示宿主下唯一稳妥的选择

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

# 启动镜像不需要 out/ 里的 obj/、symbols/、testcases/ 等子目录
required_images() {
    printf '%s\n' system.img vendor.img ramdisk.img kernel-ranchu
}
optional_images() {
    printf '%s\n' userdata.img encryptionkey.img cache.img
}

# 镜像存在且非空（软链按指向的真实文件判断）
image_ok() {
    local f
    f="$(readlink -f "$1" 2>/dev/null || printf '%s' "$1")"
    [ -s "$f" ]
}

# 选一个可用的 adb
resolve_adb() {
    if [ -x "$ADB" ]; then
        printf '%s\n' "$ADB"
    elif command -v adb >/dev/null 2>&1; then
        command -v adb
    else
        die "找不到 adb。先编一次 AOSP（容器内 m adb），或 export ADB=/path/to/adb"
    fi
}

# KVM 是否真的可用（x86_64 guest 才有意义）
# emulator-check accel 的输出是 4 行：`accel:` / `<状态码>` / `<说明>` / `accel`，
# 状态码 0 = 可用。注意命令本身的退出码恒为 0，不能用来判断。
kvm_usable() {
    [ -x "$EMULATOR_CHECK" ] || return 1
    "$EMULATOR_CHECK" accel 2>/dev/null | sed -n '2p' | grep -q '^0$'
}
