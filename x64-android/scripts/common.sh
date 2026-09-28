#!/usr/bin/env bash
# 公共变量 —— 被 x64-android/scripts 下其它脚本 source，不要直接执行。

set -euo pipefail

X64_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_ROOT="$(cd "$X64_DIR/.." && pwd)"
AOSP_DIR="$PROJECT_ROOT/aosp"

# 项目内目录
PAYLOAD_DIR="$X64_DIR/payload"          # 从官方镜像提取的翻译层
DEVICE_SRC="$X64_DIR/device"            # 设备树（本项目唯一真源）
ARTIFACTS_DIR="$X64_DIR/artifacts"      # 构建产物软链/清单
RUN_DIR="$X64_DIR/.run"                 # 运行期文件（datadir、日志、截图）

# AOSP 内的落点（由 apply-overlay.sh 同步过去）
DEVICE_DST="$AOSP_DIR/device/autosnap"
PRODUCT_NAME="autosnap_x64_arm64"
LUNCH_TARGET="$PRODUCT_NAME-userdebug"
PRODUCT_OUT="$AOSP_DIR/out/target/product/$PRODUCT_NAME"

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
JOBS="${JOBS:-12}"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }
