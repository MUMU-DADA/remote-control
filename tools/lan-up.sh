#!/usr/bin/env bash
# =============================================================================
# lan-up.sh —— 一键把整套东西拉起来，并暴露到局域网
#
# 依次做：
#   1. 起 Android 模拟器（没在跑的话）并等开机
#   2. 把 adb forward 指到 127.0.0.1:18088
#   3. 推 autod 并在模拟器里起它（绑 127.0.0.1:8088 + 0666 socket）
#   4. 确保 lan-forward 服务在跑（把端口暴露到 0.0.0.0）
#
# 用法:
#   bash tools/lan-up.sh              # 全部拉起来
#   bash tools/lan-up.sh --status     # 只看状态
#   bash tools/lan-up.sh --stop       # 停掉 autod（模拟器留着）
#
# 拉到什么程度：
#   局域网里打开   http://<本机IP>:8088/            网页控制台
#   adb connect    <本机IP>:15555                   完整 adb（可用 scrcpy）
# =============================================================================
set -euo pipefail

PROJECT_DIR=/root/AutoSnapshotAndroid
EMU=/opt/android/emu31b/emulator/emulator
STOCK="$PROJECT_DIR/dev/04-emulator/linux-arm64/.run/stock-image/x86_64"
ADB=/usr/bin/adb
SERIAL=emulator-5554
DEV=/data/local/tmp

# adb forward 绑 127.0.0.1:18088 → 模拟器:8088。
# 不用 8088 是因为转发器要绑 0.0.0.0:8088，两者在 Linux 上会冲突
# （通配地址与具体地址重叠，SO_REUSEADDR 也救不了）。
ADB_FWD_PORT=18088
LAN_PORT=8088
ADB_LAN_PORT=15555

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; }

LAN_IP=$(ip -4 route get 1.1.1.1 2>/dev/null | grep -oP 'src \K[0-9.]+' | head -1)
[ -n "$LAN_IP" ] || LAN_IP=$(hostname -I | awk '{print $1}')

# -----------------------------------------------------------------------------
if [ "${1:-}" = "--status" ]; then
    step "状态"
    if pgrep -f "qemu-system-x86_64.*$STOCK" >/dev/null 2>&1; then
        ok "模拟器：运行中"
    else
        bad "模拟器：未运行"
    fi
    "$ADB" -s "$SERIAL" shell true >/dev/null 2>&1 && ok "adb：已连接" || bad "adb：未连接"
    "$ADB" -s "$SERIAL" shell "pidof autod" >/dev/null 2>&1 \
        && ok "autod：运行中" || bad "autod：未运行"
    systemctl is-active --quiet autod-lan-forward \
        && ok "lan-forward：运行中" || bad "lan-forward：未运行"
    echo
    echo "  局域网地址："
    echo "    网页控制台  http://$LAN_IP:$LAN_PORT/"
    echo "    HTTP API    http://$LAN_IP:$LAN_PORT/api/v1/describe"
    echo "    adb         $ADB connect $LAN_IP:$ADB_LAN_PORT"
    exit 0
fi

if [ "${1:-}" = "--stop" ]; then
    step "停掉 autod"
    "$ADB" -s "$SERIAL" shell "pkill -f 'autod --socket' 2>/dev/null; rm -f $DEV/autod.sock" || true
    ok "已停"
    exit 0
fi

# -----------------------------------------------------------------------------
step "1/4  模拟器"
if pgrep -f "qemu-system-x86_64.*$STOCK" >/dev/null 2>&1; then
    ok "已在运行"
else
    [ -d "$STOCK" ] || { bad "找不到系统镜像：$STOCK"; exit 1; }
    echo "  启动中（KVM，约 30 秒）…"
    ANDROID_PRODUCT_OUT="$STOCK" ANDROID_BUILD_TOP="$PROJECT_DIR/aosp" \
        setsid nohup "$EMU" -sysdir "$STOCK" \
            -datadir /tmp/emu-lan -port 5554 \
            -no-window -gpu swiftshader_indirect -no-snapshot -no-boot-anim \
            -no-audio -accel on -memory 4096 -cores 4 \
            > /var/log/emu-lan.log 2>&1 < /dev/null &
    ok "已启动进程"
fi

step "等待开机"
for i in $(seq 1 60); do
    bc=$("$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r\n' || true)
    [ "$bc" = "1" ] && break
    sleep 2
done
[ "$bc" = "1" ] || { bad "模拟器没起来，看 /var/log/emu-lan.log"; exit 1; }
ok "已开机"

# -----------------------------------------------------------------------------
step "2/4  adb forward"
"$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
sleep 1
"$ADB" -s "$SERIAL" forward --remove "tcp:$LAN_PORT" >/dev/null 2>&1 || true
"$ADB" -s "$SERIAL" forward "tcp:$ADB_FWD_PORT" tcp:8088 >/dev/null
ok "127.0.0.1:$ADB_FWD_PORT → 模拟器:8088"

# -----------------------------------------------------------------------------
step "3/4  autod"
BIN="$PROJECT_DIR/dev/02-native-daemon/out/ndk/x86_64/autod"
CTL="$PROJECT_DIR/dev/02-native-daemon/out/ndk/x86_64/autodctl"
[ -f "$BIN" ] || { bad "没编出 autod（bash tools/build-ndk.sh ABI=x86_64）"; exit 1; }

"$ADB" -s "$SERIAL" shell "pkill -f 'autod --socket' 2>/dev/null; rm -f $DEV/autod.sock" || true
"$ADB" -s "$SERIAL" push "$BIN" "$DEV/autod" >/dev/null
"$ADB" -s "$SERIAL" push "$CTL" "$DEV/autodctl" >/dev/null
"$ADB" -s "$SERIAL" shell "chmod 755 $DEV/autod $DEV/autodctl"
# cliptool.jar：剪贴板功能要用（daemon 是 root，剪贴板必须以 shell 身份访问）
if [ ! -f "$PROJECT_DIR/dev/02-native-daemon/tools/cliptool/build/cliptool.jar" ]; then
    bash "$PROJECT_DIR/dev/02-native-daemon/tools/cliptool/build.sh" >/dev/null 2>&1 || true
fi
[ -f "$PROJECT_DIR/dev/02-native-daemon/tools/cliptool/build/cliptool.jar" ] && \
    "$ADB" -s "$SERIAL" push \
        "$PROJECT_DIR/dev/02-native-daemon/tools/cliptool/build/cliptool.jar" \
        "$DEV/" >/dev/null && "$ADB" -s "$SERIAL" shell "chmod 644 $DEV/cliptool.jar"

"$ADB" -s "$SERIAL" shell \
    "nohup $DEV/autod --socket $DEV/autod.sock --socket-mode 0666 \
        --http-bind 127.0.0.1 --http-port 8088 --foreground \
        > /dev/null 2>&1 &"
sleep 3
"$ADB" -s "$SERIAL" shell "test -S $DEV/autod.sock" \
    && ok "autod 已就绪" || { bad "autod 没起来"; exit 1; }

# -----------------------------------------------------------------------------
step "4/4  局域网转发"
systemctl is-active --quiet autod-lan-forward \
    && ok "lan-forward 已在运行" \
    || { systemctl start autod-lan-forward && ok "已启动"; }
sleep 1

# -----------------------------------------------------------------------------
step "完成"
echo
echo "  局域网里可以直接用："
echo "    网页控制台   http://$LAN_IP:$LAN_PORT/"
echo "    HTTP API     http://$LAN_IP:$LAN_PORT/api/v1/describe"
echo "    实时画面流   http://$LAN_IP:$LAN_PORT/api/v1/stream"
echo "    adb 连接     $ADB connect $LAN_IP:$ADB_LAN_PORT"
echo
echo "  自检： bash tools/lan-up.sh --status"
echo
echo "  ⚠️ 这些端口没有鉴权 —— 同一局域网内任何设备都能控制模拟器。"
echo "     只在可信网络里用；要暴露到不可信网络的话，"
echo "     先给 autod 加 --http-token（见 docs/10-http-api.md 的安全模型）。"
