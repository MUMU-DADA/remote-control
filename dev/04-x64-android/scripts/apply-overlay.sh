#!/usr/bin/env bash
# 把本项目的设备树与翻译层载荷同步进 AOSP 树（aosp/device/remote_control/）。
#
#   ./apply-overlay.sh            # 同步（幂等）
#   ./apply-overlay.sh --check    # 只校验载荷清单，不动 AOSP
#   ./apply-overlay.sh --revert   # 撤掉 AOSP 里的落点
#
# 为什么要同步而不是直接改 AOSP 树：
#   1. 本项目目录才是唯一真源，AOSP 树是可重来的构建依赖（85 GB，不纳入版本管理）；
#   2. 容器只挂了 aosp/，本项目目录不在容器里；
#   3. 全程**不改 AOSP 上游任何文件**——落点只有新增的 device/remote_control/ 一个目录，
#      lunch 能发现它是因为 Soong 会递归扫描 device/*/*/AndroidProducts.mk。
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

MODE=sync
case "${1:-}" in
    --check)         MODE=check ;;
    --revert)        MODE=revert ;;
    --patch-initrc)  MODE=patchinitrc ;;
    --unpatch-initrc) MODE=unpatchinitrc ;;
    "")              ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) die "未知参数：$1" ;;
esac

# ---------------------------------------------------------------------------
# 可选：init.rc 的 encryption=Require → Attempt
#
# 背景（见 docs/02-build-traps.md）：模拟器首启时，init 给 /data/misc 设 fscrypt 策略
# 会因目录非空而 ENOTEMPTY，而 AOSP 的兜底诊断要 exec /vendor/bin/toybox_vendor
# （SELinux 拒绝）→ 判定失败 → "Rebooting into recovery"，启动卡死。
# 官方 x86_64 成品镜像上没触发（实测能开机），所以这里**默认不打开**，
# 只在真的卡 recovery 时一键切换。改动可逆，备份为 init.rc.remote_control.orig。
# ---------------------------------------------------------------------------
INITRC="$AOSP_DIR/system/core/rootdir/init.rc"
if [ "$MODE" = patchinitrc ] || [ "$MODE" = unpatchinitrc ]; then
    [ -f "$INITRC" ] || die "找不到 $INITRC"
    if [ "$MODE" = unpatchinitrc ]; then
        [ -f "$INITRC.remote-control.orig" ] || die "没有备份文件，说明没打过补丁"
        cp -f "$INITRC.remote-control.orig" "$INITRC"
        log "已还原 $INITRC"
        exit 0
    fi
    if grep -q "encryption=Require" "$INITRC"; then
        [ -f "$INITRC.remote-control.orig" ] || cp -a "$INITRC" "$INITRC.remote-control.orig"
        sed -i 's|\(mkdir /data/misc .*\)encryption=Require|\1encryption=Attempt|' "$INITRC"
        grep -n "mkdir /data/misc" "$INITRC" | sed 's/^/    /'
        log "init.rc 已打补丁（备份：system/core/rootdir/init.rc.remote_control.orig）"
        warn "需要重新构建才会进镜像： ./build-rom.sh"
    else
        log "init.rc 已经是 Attempt（或已打过补丁），无需处理"
    fi
    exit 0
fi

# ---------------------------------------------------------------------------
if [ "$MODE" = revert ]; then
    [ -d "$DEVICE_DST" ] || { log "AOSP 里本来就没有 $DEVICE_DST"; exit 0; }
    rm -rf "$DEVICE_DST"
    log "已移除 $DEVICE_DST（AOSP 树回到未注入状态）"
    exit 0
fi

# ---------------------------------------------------------------------------
# 1) 校验载荷
[ -d "$PAYLOAD_DIR/system" ] || die "载荷不存在：$PAYLOAD_DIR/system
    先跑： ./fetch-payload.sh"

if [ -f "$PAYLOAD_DIR/MANIFEST.sha256" ]; then
    log "校验载荷完整性（MANIFEST.sha256）"
    ( cd "$PAYLOAD_DIR" && sha256sum -c --quiet MANIFEST.sha256 ) \
        || die "载荷校验失败：$PAYLOAD_DIR 与清单不一致，重跑 ./fetch-payload.sh"
fi

n_libs=$(ls "$PAYLOAD_DIR/system/lib64"/libndk_translation*.so 2>/dev/null | wc -l)
n_arm64=$(ls "$PAYLOAD_DIR/system/lib64/arm64" 2>/dev/null | wc -l)
[ "$n_libs" -ge 20 ] || die "翻译器数量不对（$n_libs，期望 ≥20）"
[ "$n_arm64" -ge 59 ] || die "arm64 系统库数量不对（$n_arm64，期望 59）"
log "载荷 OK：翻译器+proxy $n_libs 个，arm64 系统库 $n_arm64 个，总 $(du -sh "$PAYLOAD_DIR" | cut -f1)"

[ "$MODE" = check ] && exit 0

# ---------------------------------------------------------------------------
# 2) 同步设备树（保留权限位：bin/ 下的可执行文件必须是 0755）
log "同步设备树 → ${DEVICE_DST#"$PROJECT_ROOT"/}"
mkdir -p "$DEVICE_DST"
cp -a "$DEVICE_SRC"/. "$DEVICE_DST"/

# 3) 同步载荷
BRIDGE_DST="$DEVICE_DST/remote_control_x64_arm64/bridge"
log "同步载荷 → ${BRIDGE_DST#"$PROJECT_ROOT"/}"
rm -rf "$BRIDGE_DST"
mkdir -p "$BRIDGE_DST"
cp -a "$PAYLOAD_DIR/system" "$BRIDGE_DST/system"

# 4) 生成 bridge-copy.mk（显式 PRODUCT_COPY_FILES，不用 make 里遍历目录）
COPY_MK="$BRIDGE_DST/bridge-copy.mk"
{
    echo "# 由 dev/04-x64-android/scripts/apply-overlay.sh 自动生成，请勿手改。"
    echo "# 源：payload/system/<相对路径>  目的：\$(TARGET_COPY_OUT_SYSTEM)/<相对路径>"
    echo "PRODUCT_COPY_FILES += \\"
    ( cd "$BRIDGE_DST" && find system -type f | LC_ALL=C sort ) | while read -r rel; do
        echo "    device/remote_control/remote_control_x64_arm64/bridge/$rel:\$(TARGET_COPY_OUT_SYSTEM)/${rel#system/} \\"
    done
    echo ""
} > "$COPY_MK"

n_copy=$(grep -c 'PRODUCT_COPY_FILES\|:$(TARGET_COPY_OUT_SYSTEM)' "$COPY_MK" || true)
log "生成 bridge-copy.mk：$(grep -c 'TARGET_COPY_OUT_SYSTEM' "$COPY_MK") 条拷贝规则"

# 5) 校验落点
for f in \
    "$DEVICE_DST/AndroidProducts.mk" \
    "$DEVICE_DST/remote_control_x64_arm64/BoardConfig.mk" \
    "$DEVICE_DST/remote_control_x64_arm64/device.mk" \
    "$DEVICE_DST/remote_control_x64_arm64/product/remote_control_x64_arm64.mk" \
    "$DEVICE_DST/remote_control_x64_arm64/bridge/system/bin/ndk_translation_program_runner_binfmt_misc_arm64" \
    "$DEVICE_DST/remote_control_x64_arm64/bridge/system/lib64/libndk_translation.so" \
    "$DEVICE_DST/remote_control_x64_arm64/bridge/system/etc/init/ndk_translation.rc" \
    "$DEVICE_DST/remote_control_x64_arm64/bridge/system/etc/ld.config.arm64.txt" ; do
    [ -s "$f" ] || die "落点缺失：$f"
done
[ -x "$DEVICE_DST/remote_control_x64_arm64/bridge/system/bin/ndk_translation_program_runner_binfmt_misc_arm64" ] \
    || die "binfmt 执行器没有可执行位（cp -a 应保留 0755，检查载荷）"

log "注入完成 ✓  （构建： ./build-rom.sh ；撤销： ./apply-overlay.sh --revert）"
