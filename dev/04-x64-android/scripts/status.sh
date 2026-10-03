#!/usr/bin/env bash
# 一眼看清本项目当前状态：注入 / 载荷 / 构建 / 产物 / 在跑的实例。
#
#   ./status.sh
#
set -uo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

hr() { printf '\033[1;36m── %s\033[0m\n' "$1"; }

hr "1. 载荷"
if [ -d "$PAYLOAD_DIR/system" ]; then
    n=$(find "$PAYLOAD_DIR" -type f ! -name MANIFEST.sha256 | wc -l)
    printf '   %s 个文件，%s\n' "$n" "$(du -sh "$PAYLOAD_DIR" | cut -f1)"
    printf '   翻译器 %s 个 / arm64 系统库 %s 个\n' \
        "$(ls "$PAYLOAD_DIR/system/lib64"/libndk_translation*.so 2>/dev/null | wc -l)" \
        "$(ls "$PAYLOAD_DIR/system/lib64/arm64" 2>/dev/null | wc -l)"
    if [ -f "$PAYLOAD_DIR/MANIFEST.sha256" ]; then
        ( cd "$PAYLOAD_DIR" && sha256sum -c --quiet MANIFEST.sha256 ) >/dev/null 2>&1 \
            && printf '   清单校验 ✓\n' || printf '   \033[1;31m清单校验失败 ✗\033[0m\n'
    fi
else
    printf '   \033[1;33m未提取\033[0m（先跑 ./fetch-payload.sh）\n'
fi

hr "2. AOSP 注入"
# 两个产品都看：AndroidProducts.mk 里并列，缺任何一个对应产品的 lunch 目标就没了
_inj=0
for _p in remote_control_x64_arm64 remote_control_arm64; do
    if [ -d "$DEVICE_DST/$_p" ]; then
        printf '   %-28s ✓\n' "$_p"
        _inj=$((_inj + 1))
    else
        printf '   %-28s \033[1;33m缺\033[0m\n' "$_p"
    fi
done
if [ "$_inj" -gt 0 ]; then
    if [ -f "$DEVICE_DST/remote_control_x64_arm64/bridge/bridge-copy.mk" ]; then
        printf '   翻译层载荷：拷贝规则 %s 条\n' \
            "$(grep -c 'TARGET_COPY_OUT_SYSTEM' "$DEVICE_DST/remote_control_x64_arm64/bridge/bridge-copy.mk")"
    else
        printf '   翻译层载荷：\033[1;33m未注入\033[0m（x64_arm64 产品需要）\n'
    fi
    printf '   当前 PRODUCT：%s（%s）\n' "$PRODUCT" \
        "$([ "$HAS_BRIDGE" = 1 ] && printf 'x86_64 guest + 翻译层' || printf '原生 arm64，无翻译层')"
else
    printf '   \033[1;33m未注入\033[0m（先跑 ./apply-overlay.sh）\n'
fi

hr "3. 构建"
LOG_FILE="$AOSP_DIR/out/remote-control-build.log"
if docker ps --format '{{.Names}}' | grep -qx "$BUILDER_CONTAINER"; then
    # 以**日志里的 EXIT 标记**为准（容器里可能有别人的构建在跑，单看进程会误判）
    if grep -q '^EXIT=0' "$LOG_FILE" 2>/dev/null; then
        printf '   \033[1;32m已完成 EXIT=0\033[0m  %s\n' "$(grep -oE '\[ *[0-9]+% [0-9]+/[0-9]+\]' "$LOG_FILE" 2>/dev/null | tail -1)"
    elif grep -q '^EXIT=' "$LOG_FILE" 2>/dev/null; then
        printf '   \033[1;31m结束但非 0\033[0m  %s\n' "$(grep '^EXIT=' "$LOG_FILE" | tail -1)"
    elif pgrep -f "/aosp/out/soong_ui" >/dev/null 2>&1; then
        printf '   \033[1;32m进行中\033[0m  %s\n' "$(grep -oE '\[ *[0-9]+% [0-9]+/[0-9]+\]' "$LOG_FILE" 2>/dev/null | tail -1)"
    else
        printf '   未在跑\n'
    fi
else
    printf '   \033[1;31m容器 %s 不在跑\033[0m\n' "$BUILDER_CONTAINER"
fi
if [ -s "$LOG_FILE" ]; then
    errs=$(grep -cE '(^| )error:' "$LOG_FILE" || true)
    printf '   错误数 %s ；日志 %s（%s）\n' "$errs" "${LOG_FILE#"$PROJECT_ROOT"/}" "$(stat -c%y "$LOG_FILE" | cut -d. -f1)"
    [ "$errs" != 0 ] && grep -m3 -E '(^| )error:' "$LOG_FILE" | sed 's/^/     /'
fi

hr "4. 产物"
for f in system.img vendor.img product.img ramdisk.img initrd kernel-ranchu; do
    if [ -s "$PRODUCT_OUT/$f" ]; then
        printf '   %-16s %s\n' "$f" "$(du -h "$PRODUCT_OUT/$f" | cut -f1)"
    else
        printf '   %-16s \033[1;33m—\033[0m\n' "$f"
    fi
done
[ -d "$ARTIFACTS_DIR/rom-$PRODUCT_NAME" ] && \
    printf '   打包目录： %s（%s）\n' "artifacts/rom-$PRODUCT_NAME" "$(du -sh "$ARTIFACTS_DIR/rom-$PRODUCT_NAME" | cut -f1)"
[ -s "$ARTIFACTS_DIR/arm64-probe.apk" ] && printf '   探针 APK： %s\n' "$(du -h "$ARTIFACTS_DIR/arm64-probe.apk" | cut -f1)"

hr "5. 设备"
ADB_BIN="$(command -v adb || echo "$ADB")"
if [ -x "$ADB_BIN" ]; then
    "$ADB_BIN" devices -l 2>/dev/null | tail -n +2 | sed 's/^/   /' || true
else
    printf '   没有 adb\n'
fi
