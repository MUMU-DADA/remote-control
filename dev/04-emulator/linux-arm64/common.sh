#!/usr/bin/env bash
# 公共变量与函数 —— 被 linux-arm64/ 下其它脚本 source，不要直接执行。

set -euo pipefail

EMU_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$EMU_DIR/../../.." && pwd)"
AOSP_DIR="$PROJECT_ROOT/aosp"

# ---------------------------------------------------------------------------
# Android 12 的 arm64 模拟器目标。
# 注意不是 aosp_arm64 —— 那是 GSI，产物里没有 kernel-ranchu/ramdisk/vendor。
# ---------------------------------------------------------------------------
LUNCH_TARGET="${LUNCH_TARGET:-sdk_phone64_arm64-userdebug}"
PRODUCT_DEVICE="${PRODUCT_DEVICE:-emulator64_arm64}"
PRODUCT_OUT="$AOSP_DIR/out/target/product/$PRODUCT_DEVICE"

# AOSP 自带的模拟器（30.8.3），内含 qemu/linux-x86_64/qemu-system-aarch64
EMULATOR_PREBUILTS="$AOSP_DIR/prebuilts/android-emulator/linux-x86_64"
EMULATOR_BIN="$EMULATOR_PREBUILTS/emulator"
EMULATOR_CHECK="$EMULATOR_PREBUILTS/emulator-check"

# 宿主没有 adb，用 AOSP 编出来的（m adb / m 全量构建都会产出）
ADB="${ADB:-$AOSP_DIR/out/host/linux-x86/bin/adb}"

# 构建容器（见 docs/07-environment.md）
BUILDER_CONTAINER="${BUILDER_CONTAINER:-autod-builder}"
BUILDER_AOSP_PATH="${BUILDER_AOSP_PATH:-/aosp}"
JOBS="${JOBS:-12}"

# 运行期文件（userdata 覆盖层、控制台日志）放这里，不污染 AOSP 的 out/
RUN_DIR="${RUN_DIR:-$EMU_DIR/.run}"
mkdir -p "$RUN_DIR"

# 模拟器启动参数（都可通过环境变量覆盖）
EMULATOR_PORT="${EMULATOR_PORT:-5554}"
EMULATOR_MEMORY_MB="${EMULATOR_MEMORY_MB:-4096}"     # TCG 吃内存，别低于 3072
EMULATOR_CORES="${EMULATOR_CORES:-4}"
EMULATOR_GPU="${EMULATOR_GPU:-swiftshader_indirect}" # 无显示宿主下唯一稳妥的选择
BOOT_TIMEOUT_S="${BOOT_TIMEOUT_S:-2700}"             # TCG 首次启动按 45 分钟给

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
