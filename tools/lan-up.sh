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
STOCK="$PROJECT_DIR/dev/04-x64-android/artifacts/rom-autosnap_x64_arm64"
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
    PORT=$("$ADB" -s "$SERIAL" shell "grep '^port=' /sdcard/autod.conf | cut -d= -f2" 2>/dev/null | tr -d '\r\n')
    BIND=$("$ADB" -s "$SERIAL" shell "grep '^bind=' /sdcard/autod.conf | cut -d= -f2" 2>/dev/null | tr -d '\r\n')
    AUTH=$("$ADB" -s "$SERIAL" shell "grep '^auth=' /sdcard/autod.conf | cut -d= -f2" 2>/dev/null | tr -d '\r\n')
    RUN=$("$ADB" -s "$SERIAL" shell "grep '^running=' /sdcard/autod.status | cut -d= -f2" 2>/dev/null | tr -d '\r\n')
    echo "    服务状态    running=$RUN  bind=$BIND  port=$PORT  auth=$AUTH"
    HOST=$LAN_IP
    [ "$BIND" = "127.0.0.1" ] && HOST=127.0.0.1
    echo "    网页控制台  http://$HOST:$PORT/"
    echo "    HTTP API    http://$HOST:$PORT/api/v1/describe"
    echo "    adb         $ADB connect $LAN_IP:$ADB_LAN_PORT"
    if [ "$AUTH" = "1" ]; then
        T=$("$ADB" -s "$SERIAL" shell "grep '^token=' /sdcard/autod.conf | cut -d= -f2" 2>/dev/null | tr -d '\r\n')
        echo "    访问令牌    $T"
    fi
    exit 0
fi

if [ "${1:-}" = "--stop" ]; then
    step "停掉 autod"
    "$ADB" -s "$SERIAL" shell "pkill -f 'autod --socket' 2>/dev/null; rm -f $DEV/autod.sock" || true
    ok "已停"
    exit 0
fi

# -----------------------------------------------------------------------------
step "1/5  模拟器"
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
step "2/5  adb 连接"
"$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
sleep 1
# 不再给 8088 做 adb forward：autod 自己绑对外地址，这样
# 上位机改端口能真正生效（转发器不会跟着改，两边会失联）。
ok "HTTP 由 autod 直接监听，无需转发"

# -----------------------------------------------------------------------------
step "3/5  部署文件"
# -----------------------------------------------------------------------------
# 优先用 AOSP 构建（SurfaceFlinger 直连后端），它比 NDK 版快 3 倍：
#   实测 NDK/screencap 后端 120ms/帧 → 9.6 fps
#        AOSP/SF 后端       23ms/帧 → 29.9 fps
# NDK 版作为退路 —— 它不依赖平台私有库，任何 root 设备都能跑。
AOSP_BIN="$PROJECT_DIR/aosp/out/target/product/emulator64_x86_64/system/bin/autod"
AOSP_CTL="$PROJECT_DIR/aosp/out/target/product/emulator64_x86_64/system/bin/autodctl"
NDK_BIN="$PROJECT_DIR/dev/02-native-daemon/out/ndk/x86_64/autod"
NDK_CTL="$PROJECT_DIR/dev/02-native-daemon/out/ndk/x86_64/autodctl"

if [ -f "$AOSP_BIN" ]; then
    BIN="$AOSP_BIN"; CTL="$AOSP_CTL"
    ok "用 AOSP 构建（SurfaceFlinger 后端）"
else
    BIN="$NDK_BIN"; CTL="$NDK_CTL"
    warn "没有 AOSP 产物，退回 NDK 构建（screencap 后端，慢约 3 倍）"
fi
[ -f "$BIN" ] || { bad "没编出 autod"; exit 1; }

SUP="$PROJECT_DIR/tools/autod-supervisord.sh"

# 停掉旧的（手工起的和 supervisor 起的都要停，否则会抢同一个端口）
"$ADB" -s "$SERIAL" shell "pkill -f supervisord 2>/dev/null; \
    pkill -f 'autod --socket' 2>/dev/null; rm -f $DEV/autod.sock" || true
sleep 1

"$ADB" -s "$SERIAL" push "$BIN" "$DEV/autod" >/dev/null
"$ADB" -s "$SERIAL" push "$CTL" "$DEV/autodctl" >/dev/null
"$ADB" -s "$SERIAL" push "$SUP" "$DEV/autod-supervisord.sh" >/dev/null
"$ADB" -s "$SERIAL" shell "chmod 755 $DEV/autod $DEV/autodctl $DEV/autod-supervisord.sh"

# 剪贴板辅助工具
if [ ! -f "$PROJECT_DIR/dev/02-native-daemon/tools/cliptool/build/cliptool.jar" ]; then
    bash "$PROJECT_DIR/dev/02-native-daemon/tools/cliptool/build.sh" >/dev/null 2>&1 || true
fi
[ -f "$PROJECT_DIR/dev/02-native-daemon/tools/cliptool/build/cliptool.jar" ] && \
    "$ADB" -s "$SERIAL" push \
        "$PROJECT_DIR/dev/02-native-daemon/tools/cliptool/build/cliptool.jar" \
        "$DEV/" >/dev/null && "$ADB" -s "$SERIAL" shell "chmod 644 $DEV/cliptool.jar"
ok "文件已推送"

# -----------------------------------------------------------------------------
step "4/5  写入配置并启动 supervisor"
# -----------------------------------------------------------------------------
# 只在配置文件不存在时写默认值 —— 不覆盖用户（或上位应用）已经设好的。
if ! "$ADB" -s "$SERIAL" shell "test -f /sdcard/autod.conf" 2>/dev/null; then
    "$ADB" -s "$SERIAL" shell "printf 'enabled=1\nbind=0.0.0.0\nport=8088\nauth=0\ntoken=\n' > /sdcard/autod.conf"
    ok "已写入默认配置（对外监听 8088，无鉴权）"
else
    ok "沿用已有 /sdcard/autod.conf"
fi

# supervisor 管理真正的启停。**必须 setsid** —— adb shell 一退出，
# 普通后台进程会被一起带走（实测踩过：以为起来了，其实早没了）。
"$ADB" -s "$SERIAL" shell \
    "setsid nohup $DEV/autod-supervisord.sh > $DEV/sup.log 2>&1 < /dev/null &"
sleep 4

# -----------------------------------------------------------------------------
step "5/5  局域网转发"
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
echo "     先给 autod 加 --http-token（见 docs/api/04-config.md 的鉴权一节）。"
