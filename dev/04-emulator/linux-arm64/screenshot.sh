#!/usr/bin/env bash
# 从正在运行的模拟器抓一张截图（留证 / 对照用）。
#
#   ./screenshot.sh                    # 存到 .run/shots/shot-<时间>.png，并打印路径与大小
#   ./screenshot.sh -o /tmp/a.png
#   ./screenshot.sh --analyze          # 顺手判定是不是全黑/纯色（复用 smoke 脚本的思路）
#
# 走 adb exec-out screencap（Android 自带），不依赖 autod，所以模拟器一起来就能用。

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

OUT=""
ANALYZE=0
while [ $# -gt 0 ]; do
    case "$1" in
        -o|--out) OUT="${2:?--out 需要参数}"; shift ;;
        --analyze) ANALYZE=1 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) die "未知参数：$1（-h 看用法）" ;;
    esac
    shift
done

SHOTS="$RUN_DIR/shots"
mkdir -p "$SHOTS"
[ -n "$OUT" ] || OUT="$SHOTS/shot-$(date +%Y%m%d-%H%M%S).png"

SERIAL="emulator-$EMULATOR_PORT"
ADB_BIN="$(resolve_adb)"

"$ADB_BIN" -s "$SERIAL" get-state >/dev/null 2>&1 || \
    die "$SERIAL 不在线。先启动： ./run-emulator.sh（或 ./run-stock-image.sh）"

log "抓图 $SERIAL → ${OUT#"$PROJECT_ROOT"/}"
"$ADB_BIN" -s "$SERIAL" exec-out screencap -p > "$OUT"
sz=$(stat -c%s "$OUT" 2>/dev/null || echo 0)
if [ "$sz" -lt 1000 ]; then
    die "截图只有 $sz 字节 —— 多半是设备没启动完或 screencap 失败"
fi
log "已保存 $(du -h "$OUT" | cut -f1)"

if [ "$ANALYZE" = 1 ]; then
    printf '  设备信息：%s / Android %s / %s / %s\n' \
        "$("$ADB_BIN" -s "$SERIAL" shell getprop ro.product.model | tr -d '\r')" \
        "$("$ADB_BIN" -s "$SERIAL" shell getprop ro.build.version.release | tr -d '\r')" \
        "$("$ADB_BIN" -s "$SERIAL" shell getprop ro.product.cpu.abi | tr -d '\r')" \
        "$("$ADB_BIN" -s "$SERIAL" shell getprop ro.build.type | tr -d '\r')"
fi

printf '\n文件： %s\n' "$OUT"
