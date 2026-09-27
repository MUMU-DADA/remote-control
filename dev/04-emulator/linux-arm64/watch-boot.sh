#!/usr/bin/env bash
# 等 arm64 模拟器开机完成 → 自动抓截图（开机证据）。
# 后台跑，日志：.run/boot-watch.log
set -uo pipefail

ADB=/usr/bin/adb
SERIAL="${1:-emulator-5554}"
RUN=/root/AutoSnapshotAndroid/dev/04-emulator/linux-arm64/.run
SHOTS="$RUN/shots"
mkdir -p "$SHOTS"
OUT="$SHOTS/boot-proof-$SERIAL.png"

echo "[$(date +%T)] 开始监视 $SERIAL（最多 2 小时）"
for i in $(seq 1 240); do
    state="$("$ADB" -s "$SERIAL" get-state 2>/dev/null | tr -d '\r')"
    bc="$("$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')"
    echo "[$(date +%T)] #$i state=${state:-无} boot_completed=${bc:-空}"
    if [ "$bc" = "1" ]; then
        echo "[$(date +%T)] 开机完成 ✓"
        "$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
        sleep 3
        printf '  Android %s / %s / %s\n' \
            "$("$ADB" -s "$SERIAL" shell getprop ro.build.version.release | tr -d '\r')" \
            "$("$ADB" -s "$SERIAL" shell getprop ro.product.cpu.abi | tr -d '\r')" \
            "$("$ADB" -s "$SERIAL" shell getprop ro.build.type | tr -d '\r')"
        "$ADB" -s "$SERIAL" exec-out screencap -p > "$OUT"
        sz=$(stat -c%s "$OUT" 2>/dev/null || echo 0)
        echo "[$(date +%T)] 截图 $sz 字节 → $OUT"
        # 再来一张开机后 30 秒的（等界面稳定）
        sleep 30
        "$ADB" -s "$SERIAL" exec-out screencap -p > "${OUT%.png}-30s.png"
        echo "[$(date +%T)] 第二张 $(stat -c%s "${OUT%.png}-30s.png" 2>/dev/null) 字节"
        echo "[$(date +%T)] /dev/uinput: $("$ADB" -s "$SERIAL" shell 'test -e /dev/uinput && echo 有 || echo 无' | tr -d '\r')"
        echo DONE > "$RUN/boot-watch.status"
        exit 0
    fi
    if ! pgrep -f "qemu-system-aarch64" >/dev/null 2>&1; then
        echo "[$(date +%T)] 模拟器进程已退出，停止监视"
        exit 1
    fi
    sleep 30
done
echo "[$(date +%T)] 超时（2 小时）"
