#!/usr/bin/env bash
# 把本项目的设备树（两个产品）同步进 AOSP 树（aosp/device/remote_control/）。
#
#   ./apply-overlay.sh                  # 默认 PRODUCT=x64_arm64：同步设备树 + 翻译层载荷
#   PRODUCT=arm64 ./apply-overlay.sh    # 原生 arm64 产品：只同步设备树，**跳过载荷**
#   ./apply-overlay.sh --check          # 只校验，不动 AOSP
#   ./apply-overlay.sh --revert         # 撤掉 AOSP 里的落点
#
# 为什么要同步而不是直接改 AOSP 树：
#   1. 本项目目录才是唯一真源，AOSP 树是可重来的构建依赖（85 GB，不纳入版本管理）；
#   2. 容器只挂了 aosp/，本项目目录不在容器里；
#   3. 设备树落在 device/remote_control/；另有可逆的 qemu-props 日志补丁，
#      防止首启传入的 token 被上游写入 logcat。
#
# ⚠️ 两个产品**一起同步**，不是只同步当前 PRODUCT：
#    AndroidProducts.mk 里两个产品并列，AOSP 树里缺任何一个，对应产品的
#    lunch 目标就会消失。只同步当前产品会让并行开发互相踩。
#    差别只在"要不要搬翻译层载荷"——那是 x64_arm64 独有的。
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
    token_patch="$X64_DIR/patches/qemu-props-redact-token.patch"
    if patch -d "$AOSP_DIR" -p1 -R --dry-run -f < "$token_patch" >/dev/null 2>&1; then
        patch -d "$AOSP_DIR" -p1 -R -f < "$token_patch"
    fi
    [ -d "$DEVICE_DST" ] || { log "AOSP 里本来就没有 $DEVICE_DST"; exit 0; }
    rm -rf "$DEVICE_DST"
    log "已移除 $DEVICE_DST（AOSP 树回到未注入状态）"
    exit 0
fi

# ---------------------------------------------------------------------------
# 1) 校验载荷 —— **只有带翻译层的产品需要**
#
# arm64 产品是原生 arm64 guest，翻译层在它上面没有立足点：
#   · 那一整套 libndk_translation*.so + 59 个 arm64 系统库是"让 x86_64 系统跑 arm64 应用"用的
#   · 原生 arm64 系统自己就是 arm64，应用本来就跑得动
# 所以这一段整段跳过。见 docs/13-macos-port.md §2.2。
#
# ⚠️ 跳过的同时要在 --check 模式下**明确说出来**，否则"检查通过"会被误读成
#    "载荷没问题" —— 而实际上根本没查。
if [ "$HAS_BRIDGE" = 1 ]; then
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
else
    log "PRODUCT=$PRODUCT（原生 arm64）：跳过翻译层载荷 ✓ 不适用"
    log "  翻译层载荷（$PAYLOAD_DIR）是为 x86_64 guest 准备的，本产品不需要"
fi

# 顺带核对：这个产品的设备树目录在源里存在
[ -d "$DEVICE_SRC/$PRODUCT_NAME" ] || die "设备树目录不存在：$DEVICE_SRC/$PRODUCT_NAME
    PRODUCT=$PRODUCT → 期望 device/$PRODUCT_NAME/"

[ "$MODE" = check ] && exit 0

token_patch="$X64_DIR/patches/qemu-props-redact-token.patch"
if patch -d "$AOSP_DIR" -p1 --dry-run -f < "$token_patch" >/dev/null 2>&1; then
    patch -d "$AOSP_DIR" -p1 -f < "$token_patch"
elif ! patch -d "$AOSP_DIR" -p1 -R --dry-run -f < "$token_patch" >/dev/null 2>&1; then
    die "qemu-props 日志补丁与当前源码不匹配：$token_patch"
fi

# ---------------------------------------------------------------------------
# 2) 同步设备树（保留权限位：bin/ 下的可执行文件必须是 0755）
#
# ⚠️ 这一步会 `cp -a` **整个** device/ 目录，所以两个产品的设备树会一起同步过去
#    （外加共享的 sepolicy）。这是有意的：AndroidProducts.mk 里两个产品并列，
#    AOSP 树里也必须两个都在，否则另一个产品的 lunch 目标会消失。
#    —— 只同步"当前产品"会让并行开发的两个产品互相踩。
log "同步设备树 → ${DEVICE_DST#"$PROJECT_ROOT"/}（PRODUCT=$PRODUCT，含全部产品）"
mkdir -p "$DEVICE_DST"
cp -a "$DEVICE_SRC"/. "$DEVICE_DST"/

if [ "$HAS_BRIDGE" = 1 ]; then
    # 3) 同步载荷
    BRIDGE_DST="$DEVICE_DST/$PRODUCT_NAME/bridge"
    log "同步载荷 → ${BRIDGE_DST#"$PROJECT_ROOT"/}"
    rm -rf "$BRIDGE_DST"
    mkdir -p "$BRIDGE_DST"
    cp -a "$PAYLOAD_DIR/system" "$BRIDGE_DST/system"

    # 4) 生成 bridge-copy.mk（显式 PRODUCT_COPY_FILES，不用 make 里遍历目录）
    COPY_MK="$BRIDGE_DST/bridge-copy.mk"
    {
        echo "# 由 dev/04-android-rom/scripts/apply-overlay.sh 自动生成，请勿手改。"
        echo "# 源：payload/system/<相对路径>  目的：\$(TARGET_COPY_OUT_SYSTEM)/<相对路径>"
        echo "PRODUCT_COPY_FILES += \\"
        ( cd "$BRIDGE_DST" && find system -type f | LC_ALL=C sort ) | while read -r rel; do
            echo "    device/remote_control/$PRODUCT_NAME/bridge/$rel:\$(TARGET_COPY_OUT_SYSTEM)/${rel#system/} \\"
        done
        echo ""
    } > "$COPY_MK"

    log "生成 bridge-copy.mk：$(grep -c 'TARGET_COPY_OUT_SYSTEM' "$COPY_MK") 条拷贝规则"
else
    # 原生 arm64：清掉可能残留的 bridge 目录（从旧产品切过来时会有）
    STALE="$DEVICE_DST/$PRODUCT_NAME/bridge"
    if [ -d "$STALE" ]; then
        rm -rf "$STALE"
        log "已清理残留的 bridge 目录（原生 arm64 不需要）：${STALE#"$PROJECT_ROOT"/}"
    fi
fi

# 5) 校验落点
#
# ⚠️ 两个产品**都要校验**，不只是当前产品：AndroidProducts.mk 里列了两个，
#    缺任何一个都会让对应产品的 lunch 目标消失（而且报错信息很难指向真因）。
for f in \
    "$DEVICE_DST/AndroidProducts.mk" \
    "$DEVICE_DST/remote_control_x64_arm64/BoardConfig.mk" \
    "$DEVICE_DST/remote_control_x64_arm64/device.mk" \
    "$DEVICE_DST/remote_control_x64_arm64/product/remote_control_x64_arm64.mk" \
    "$DEVICE_DST/remote_control_arm64/BoardConfig.mk" \
    "$DEVICE_DST/remote_control_arm64/device.mk" \
    "$DEVICE_DST/remote_control_arm64/product/remote_control_arm64.mk" \
    "$DEVICE_DST/remote_control_x64_arm64/sepolicy/remote_control.te" \
    "$DEVICE_DST/remote_control_x64_arm64/sepolicy/file_contexts" ; do
    [ -s "$f" ] || die "落点缺失：$f"
done

if [ "$HAS_BRIDGE" = 1 ]; then
    for f in \
        "$DEVICE_DST/$PRODUCT_NAME/bridge/system/bin/ndk_translation_program_runner_binfmt_misc_arm64" \
        "$DEVICE_DST/$PRODUCT_NAME/bridge/system/lib64/libndk_translation.so" \
        "$DEVICE_DST/$PRODUCT_NAME/bridge/system/etc/init/ndk_translation.rc" \
        "$DEVICE_DST/$PRODUCT_NAME/bridge/system/etc/ld.config.arm64.txt" ; do
        [ -s "$f" ] || die "落点缺失：$f"
    done
    [ -x "$DEVICE_DST/$PRODUCT_NAME/bridge/system/bin/ndk_translation_program_runner_binfmt_misc_arm64" ] \
        || die "binfmt 执行器没有可执行位（cp -a 应保留 0755，检查载荷）"
fi

# sepolicy 是两个产品共用的（BoardConfig 里都指向 remote_control_x64_arm64/sepolicy），
# 所以这里显式提示一句，避免以后有人"找不到 arm64 的 sepolicy"。
log "sepolicy：两个产品共用 $DEVICE_DST/remote_control_x64_arm64/sepolicy（命名是历史原因）"

log "注入完成 ✓  PRODUCT=$PRODUCT  （构建： PRODUCT=$PRODUCT ./build-rom.sh ；撤销： ./apply-overlay.sh --revert）"
