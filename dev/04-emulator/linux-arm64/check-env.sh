#!/usr/bin/env bash
# 前置检查：跑模拟器之前，先确认缺什么。
#
#   ./check-env.sh
#
# 只读检查，不改任何东西；每项 FAIL 都会给出能直接执行的修复命令。

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

FAIL=0
ok()   { printf '  \033[1;32m✓\033[0m %s\n' "$*"; }
bad()  { printf '  \033[1;31m✗\033[0m %s\n' "$*"; FAIL=1; }
note() { printf '  \033[1;33m·\033[0m %s\n' "$*"; }

hdr() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

# ---------------------------------------------------------------------------
hdr "宿主系统库（模拟器的动态依赖）"

if ldconfig -p 2>/dev/null | grep -q 'libpulse\.so\.0'; then
    ok "libpulse.so.0（libpulse0）"
else
    bad "缺 libpulse.so.0 —— 模拟器连 --version 都跑不起来"
    note "修复： apt-get install -y libpulse0"
fi

if ldconfig -p 2>/dev/null | grep -q 'libGL\.so\.1'; then
    ok "libGL.so.1（libgl1）"
else
    bad "缺 libGL.so.1 —— 非 headless 后端与 emulator -version 会失败"
    note "修复： apt-get install -y libgl1"
fi

# ---------------------------------------------------------------------------
hdr "AOSP 自带的模拟器（含 arm64 QEMU 后端）"

if [ -x "$EMULATOR_BIN" ]; then
    ok "emulator: ${EMULATOR_BIN#"$PROJECT_ROOT"/}"
else
    bad "找不到 $EMULATOR_BIN"
    note "修复：容器内 cd $BUILDER_AOSP_PATH && repo sync -c -j4   # 同步 prebuilts/android-emulator"
fi

ARM64_QEMU="$EMULATOR_PREBUILTS/qemu/linux-x86_64/qemu-system-aarch64-headless"
if [ -x "$ARM64_QEMU" ]; then
    ok "arm64 QEMU 后端：qemu-system-aarch64-headless（x86_64 宿主跑 arm64 guest 的关键）"
else
    bad "找不到 arm64 QEMU 后端：$ARM64_QEMU"
fi

if [ -x "$EMULATOR_CHECK" ]; then
    accel="$("$EMULATOR_CHECK" accel 2>&1 | head -3 | tr '\n' ' ')"
    note "emulator-check accel: $accel"
    note "  ↑ 只对 x86_64 guest 有意义；arm64 guest 走 TCG，用不到 KVM"
fi

# ---------------------------------------------------------------------------
hdr "构建容器与镜像产物"

if docker inspect "$BUILDER_CONTAINER" >/dev/null 2>&1; then
    if [ "$(docker inspect -f '{{.State.Running}}' "$BUILDER_CONTAINER")" = "true" ]; then
        ok "容器 $BUILDER_CONTAINER 运行中"
    else
        bad "容器 $BUILDER_CONTAINER 存在但没运行"
        note "修复： docker start $BUILDER_CONTAINER"
    fi
else
    bad "没有容器 $BUILDER_CONTAINER（见 docs/07-environment.md 第 4 节）"
fi

note "lunch 目标：$LUNCH_TARGET  →  产物目录：out/target/product/$PRODUCT_DEVICE"

if [ -d "$PRODUCT_OUT" ]; then
    ok "产物目录存在"
    missing=0
    while read -r img; do
        if image_ok "$PRODUCT_OUT/$img"; then
            ok "$(printf '%-18s %s' "$img" "$(du -h "$PRODUCT_OUT/$img" 2>/dev/null | cut -f1)")"
        else
            bad "缺 $img"; missing=1
        fi
    done < <(required_images)
    while read -r img; do
        image_ok "$PRODUCT_OUT/$img" && ok "$(printf '%-18s %s' "$img" "$(du -h "$PRODUCT_OUT/$img" 2>/dev/null | cut -f1)")"
    done < <(optional_images)
    [ "$missing" = 1 ] && note "修复： ./build-images.sh"
else
    bad "还没有 $PRODUCT_OUT —— 没编过模拟器目标"
    note "修复： ./build-images.sh        （首次全量约 100GB、数小时）"
    note "说明： aosp_arm64-userdebug 是 GSI，产物在 generic_arm64/，起不了模拟器"
fi

# ---------------------------------------------------------------------------
hdr "adb（宿主没装，用 AOSP 编出来的）"

if [ -x "$ADB" ]; then
    ok "$ADB"
    "$ADB" version 2>/dev/null | head -1 | sed 's/^/    /'
elif command -v adb >/dev/null 2>&1; then
    note "用系统 adb：$(command -v adb)"
else
    bad "没有 adb"
    note "修复：容器内 m adb      （或 apt-get install -y adb）"
fi

# ---------------------------------------------------------------------------
hdr "磁盘与内存"

avail="$(df -BG --output=avail "$PROJECT_ROOT" 2>/dev/null | tail -1 | tr -dc '0-9')"
if [ -n "$avail" ] && [ "$avail" -ge 60 ]; then
    ok "${avail}GB 可用（$PROJECT_ROOT）"
else
    bad "可用空间只有 ${avail:-未知}GB；全量编译约需 100GB"
fi

mem="$(free -g 2>/dev/null | sed -n '2p' | awk '{print $2}')"
if [ -n "$mem" ] && [ "$mem" -ge 16 ]; then
    ok "${mem}GiB 内存"
else
    warn "内存只有 ${mem:-未知}GiB，TCG 模拟器 + 编译会紧张"
fi

if [ -e /dev/kvm ]; then
    note "/dev/kvm 存在，但对 arm64 guest 无效（只能同架构加速）"
fi

# ---------------------------------------------------------------------------
printf '\n'
if [ "$FAIL" = 0 ]; then
    printf '\033[1;32m全部就绪。\033[0m 下一步： ./run-emulator.sh\n'
else
    printf '\033[1;31m有项目未通过，按上面的「修复」执行。\033[0m\n'
    exit 1
fi
