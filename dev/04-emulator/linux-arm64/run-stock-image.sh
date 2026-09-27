#!/usr/bin/env bash
# 启动 Google 成品镜像（构建模式，不需要 AVD、也不需要 SDK 目录布局）。
#
#   ./run-stock-image.sh                 # 等价于 run-emulator.sh，但目标是成品镜像
#   ./run-stock-image.sh --no-wait
#   ./run-stock-image.sh --stop
#
# 为什么单独一个入口：成品镜像是"未修改的对照物"，用它可以在自己镜像还没编好、
# 或者自制镜像起不来时，判断问题出在**环境**还是**镜像**。

set -euo pipefail
EMU_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ABI 取默认（arm64）；成品镜像只有 arm64-v8a 这一份
STOCK="$EMU_DIR/.run/stock-image/arm64-v8a"

if [ ! -f "$STOCK/system.img" ]; then
    printf '\033[1;31m[x]\033[0m 没有成品镜像：%s\n' "$STOCK"
    printf '    先跑： ./get-stock-image.sh\n' >&2
    exit 1
fi

# 幂等补齐构建模式需要的两个文件（见 README 实测坑 2、3）
if [ -f "$STOCK/build.prop" ] && [ ! -f "$STOCK/system/build.prop" ]; then
    mkdir -p "$STOCK/system" && cp "$STOCK/build.prop" "$STOCK/system/build.prop"
fi
if [ -f "$STOCK/ramdisk.img" ] && [ ! -f "$STOCK/initrd" ]; then
    cp "$STOCK/ramdisk.img" "$STOCK/initrd"
fi

# 交给统一的启动脚本：覆盖 PRODUCT_OUT 指向成品镜像目录即可
exec env PRODUCT_OUT="$STOCK" "$EMU_DIR/run-emulator.sh" "$@"
