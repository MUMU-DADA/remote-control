#!/usr/bin/env bash
# =============================================================================
# lan-up.sh —— 一键把整套东西拉起来，并暴露到局域网
#
# 依次做：
#   1. 起 Android 模拟器（没在跑的话）并等开机
#   2. 把 adb forward 指到 127.0.0.1:18088
#   3. 推 remote-control 并在模拟器里起它（绑 127.0.0.1:8088 + 0666 socket）
#   4. 确保 lan-forward 服务在跑（把端口暴露到 0.0.0.0）
#
# 用法:
#   bash tools/lan-up.sh              # 全部拉起来
#   bash tools/lan-up.sh --status     # 只看状态
#   bash tools/lan-up.sh --stop       # 停掉 remote-control（模拟器留着）
#
# 拉到什么程度：
#   局域网里打开   http://<本机IP>:8088/            网页控制台
#   adb connect    <本机IP>:15555                   完整 adb（可用 scrcpy）
# =============================================================================
set -euo pipefail

# 输出助手。**必须定义在下面的常量校验之前** —— 校验失败时要靠 bad() 报错，
# 定义晚了会变成 "bad: command not found"，把真正的提示吞掉。
step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; }

PROJECT_DIR=/root/AutoSnapshotAndroid
STOCK="$PROJECT_DIR/dev/04-android-rom/artifacts/rom-remote_control_x64_arm64"
ADB=/usr/bin/adb
DEV=/data/local/tmp

# 服务真正读的配置。默认走产品形态那条路径（init 的 --config 也指这里）。
# 要跑免 SELinux 原型（supervisord 监视共享存储那份）就覆盖：
#   REMOTE_CONTROL_CONFIG=/sdcard/remote-control.conf bash tools/lan-up.sh
# —— 与 remote-control-supervisord.sh 的 CONF 用的是同一个变量名。
CONF=${REMOTE_CONTROL_CONFIG:-/data/misc/remote-control/remote-control.conf}

# 模拟器二进制：环境变量 > 自动探测 > 报错。
# （早先写死 /opt/android/emu31b/...，那个目录早没了。）
EMU=${EMU:-}
if [ -z "$EMU" ]; then
    for _c in /opt/android/emulator-new/emulator/emulator \
              /opt/android/emu31b/emulator/emulator; do
        [ -x "$_c" ] && { EMU="$_c"; break; }
    done
fi
[ -n "$EMU" ] || { bad "找不到模拟器二进制（用 EMU=<路径> 指定）"; exit 1; }

# 串口：跟着实际在跑的模拟器走，不写死端口。
# （早先写死 emulator-5554；实例换到 5580 后这脚本就整个跑不通了。）
#
# 探测不到就**先留空** —— 第 1 步负责起一台（走 scripts/emulator.sh），
# 起来之后再定串口。--status / --stop 不依赖串口存在，所以它们照常返回。
SERIAL=${SERIAL:-}
if [ -z "$SERIAL" ]; then
    SERIAL=$("$ADB" devices 2>/dev/null | awk '/^emulator-[0-9]+[ \t]+device/{print $1; exit}') || true
fi

# adb forward 绑 127.0.0.1:18088 → 模拟器:8088。
# 不用 8088 是因为转发器要绑 0.0.0.0:8088，两者在 Linux 上会冲突
# （通配地址与具体地址重叠，SO_REUSEADDR 也救不了）。
ADB_FWD_PORT=18088
LAN_PORT=8088
ADB_LAN_PORT=${ADB_LAN_PORT:-15555}
# ADB_TARGET_PORT（转发目标）要在第 1 步把串口定下来之后才算 —— 见那里的注释。

LAN_IP=$(ip -4 route get 1.1.1.1 2>/dev/null | grep -oP 'src \K[0-9.]+' | head -1)
[ -n "$LAN_IP" ] || LAN_IP=$(hostname -I | awk '{print $1}')

# -----------------------------------------------------------------------------
if [ "${1:-}" = "--status" ]; then
    step "状态"
    # 判据用串口（adb）而不是 pgrep：早先是
    #   pgrep -f "qemu-system-x86_64.*$STOCK"
    # 而实际 cmdline 里是 `-sysdir .run/sysdir-5580`，不含 $STOCK ——
    # 永远匹配不上，于是模拟器明明在跑却报"未运行"。
    if [ -n "$SERIAL" ]; then
        ok "模拟器：运行中（$SERIAL）"
    else
        bad "模拟器：未运行"
    fi
    "$ADB" -s "$SERIAL" shell true >/dev/null 2>&1 && ok "adb：已连接" || bad "adb：未连接"
    "$ADB" -s "$SERIAL" shell "pidof remote-control" >/dev/null 2>&1 \
        && ok "remote-control：运行中" || bad "remote-control：未运行"
    systemctl is-active --quiet remote-control-lan-forward \
        && ok "lan-forward：运行中" || bad "lan-forward：未运行"
    echo
    echo "  局域网地址："
    PORT=$("$ADB" -s "$SERIAL" shell "grep '^port=' $CONF | cut -d= -f2" 2>/dev/null | tr -d '\r\n')
    BIND=$("$ADB" -s "$SERIAL" shell "grep '^bind=' $CONF | cut -d= -f2" 2>/dev/null | tr -d '\r\n')
    AUTH=$("$ADB" -s "$SERIAL" shell "grep '^auth=' $CONF | cut -d= -f2" 2>/dev/null | tr -d '\r\n')
    # 配置文件可能**不存在**（出厂默认形态：服务就按内置默认跑）。
    # 不兜底的话下面会拼出 "http://IP:/" 这种半截 URL，看着像坏了。
    PORT=${PORT:-8088}
    BIND=${BIND:-127.0.0.1}
    AUTH=${AUTH:-0}
    RUN=$("$ADB" -s "$SERIAL" shell "grep '^running=' /sdcard/remote-control.status | cut -d= -f2" 2>/dev/null | tr -d '\r\n')
    echo "    服务状态    running=$RUN  bind=$BIND  port=$PORT  auth=$AUTH"
    HOST=$LAN_IP
    [ "$BIND" = "127.0.0.1" ] && HOST=127.0.0.1
    echo "    网页控制台  http://$HOST:$PORT/"
    echo "    HTTP API    http://$HOST:$PORT/api/v1/describe"
    echo "    adb         $ADB connect $LAN_IP:$ADB_LAN_PORT"
    if [ "$AUTH" = "1" ]; then
        T=$("$ADB" -s "$SERIAL" shell "grep '^token=' $CONF | cut -d= -f2" 2>/dev/null | tr -d '\r\n')
        echo "    访问令牌    $T"
    fi
    exit 0
fi

if [ "${1:-}" = "--stop" ]; then
    step "停掉 remote-control"
    "$ADB" -s "$SERIAL" shell "pkill -f 'remote-control --socket' 2>/dev/null; rm -f $DEV/remote-control.sock" || true
    ok "已停"
    exit 0
fi

# -----------------------------------------------------------------------------
step "1/5  模拟器"
# ⚠️ **不要在这里自己拼 emulator 命令行。**
#
#    这段早先是 `"$EMU" -sysdir "$STOCK" -datadir /tmp/emu-lan -port 5554 …`，
#    三个后果，全是真的踩过的：
#      ① `-sysdir` 指向**交付目录**。模拟器启动时会**重写 initrd**（把自己的
#         ramdisk+dtb 写进去），并往同一目录灌 `userdata-qemu.img`（表观 32 GB，
#         而且是**跑过的用户数据**）、`*.qcow2`、`build.avd`、各种 lock。
#         交付物当场被污染：SHA256SUMS 对不上，而且这是数据泄漏不只是体积问题。
#         package-rom.sh 的白名单闸门就是为这类污染加的 —— 闸门在打包时才发现，
#         而污染在这里就发生了，所以源头必须堵。
#      ② `-port 5554` 写死，和 scripts/emulator.sh 的默认 5580 不一致。
#      ③ `pgrep -f "qemu-system-x86_64.*$STOCK"` 永远匹配不上 —— 实际 cmdline
#         里是 `-sysdir .run/sysdir-5580`，不含 $STOCK。于是每次都会再起一台。
#
#    正确做法：交给 scripts/emulator.sh。它把镜像**软链**进 .run/sysdir-<port>/，
#    并刻意排除 initrd / config.ini（那两个会被写穿），交付目录保持只读。
if [ -n "$SERIAL" ]; then
    ok "已在运行：$SERIAL"
else
    [ -x "$EMU" ] || { bad "找不到模拟器二进制（用 EMU=<路径> 指定）"; exit 1; }
    echo "  启动中（走 scripts/emulator.sh，约 30 秒）…"
    "$PROJECT_DIR/dev/04-android-rom/scripts/emulator.sh" start default --no-wait \
        > /var/log/emu-lan.log 2>&1 || { bad "启动失败，看 /var/log/emu-lan.log"; exit 1; }
    SERIAL=$("$ADB" devices 2>/dev/null | awk '/^emulator-[0-9]+[ \t]+(device|offline)/{print $1; exit}') || true
    [ -n "$SERIAL" ] || { bad "起来后仍找不到串口，看 /var/log/emu-lan.log"; exit 1; }
    ok "已启动：$SERIAL"
fi

# 转发目标 = 模拟器 **adb** 端口 = console 端口 + 1。
# 串口名里的数字就是 console 端口（emulator-5580 → console 5580, adb 5581）。
# 串口是其它形态（比如 SERIAL=<IP>:5555 直连）时推不出来，用环境变量兜底。
case "$SERIAL" in
    emulator-*) ADB_TARGET_PORT=$(( ${SERIAL#emulator-} + 1 )) ;;
    *)          ADB_TARGET_PORT=${ADB_TARGET_PORT:-5581}
                echo "  ⚠️ 串口 '$SERIAL' 不是 emulator-N 形态，转发目标按 $ADB_TARGET_PORT 处理" >&2 ;;
esac

step "等待开机"
for i in $(seq 1 90); do
    bc=$("$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r\n' || true)
    [ "$bc" = "1" ] && break
    sleep 2
done
[ "$bc" = "1" ] || { bad "模拟器没起来，看 /var/log/emu-lan.log 与 .run/emulator-*.log"; exit 1; }
ok "已开机"

# -----------------------------------------------------------------------------
step "2/5  adb 连接"
"$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
sleep 1
# 不再给 8088 做 adb forward：remote-control 自己绑对外地址，这样
# 上位机改端口能真正生效（转发器不会跟着改，两边会失联）。
ok "HTTP 由 remote-control 直接监听，无需转发"

# -----------------------------------------------------------------------------
step "3/5  部署文件"
# -----------------------------------------------------------------------------
# 优先用 AOSP 构建（SurfaceFlinger 直连后端），它比 NDK 版快 3 倍：
#   实测 NDK/screencap 后端 120ms/帧 → 9.6 fps
#        AOSP/SF 后端       23ms/帧 → 29.9 fps
# NDK 版作为退路 —— 它不依赖平台私有库，任何 root 设备都能跑。
AOSP_BIN="$PROJECT_DIR/aosp/out/target/product/emulator64_x86_64/system/bin/remote-control"
AOSP_CTL="$PROJECT_DIR/aosp/out/target/product/emulator64_x86_64/system/bin/rcctl"
NDK_BIN="$PROJECT_DIR/dev/02-native-daemon/out/ndk/x86_64/remote-control"
NDK_CTL="$PROJECT_DIR/dev/02-native-daemon/out/ndk/x86_64/rcctl"

if [ -f "$AOSP_BIN" ]; then
    BIN="$AOSP_BIN"; CTL="$AOSP_CTL"
    ok "用 AOSP 构建（SurfaceFlinger 后端）"
else
    BIN="$NDK_BIN"; CTL="$NDK_CTL"
    warn "没有 AOSP 产物，退回 NDK 构建（screencap 后端，慢约 3 倍）"
fi
[ -f "$BIN" ] || { bad "没编出 remote-control"; exit 1; }

SUP="$PROJECT_DIR/tools/remote-control-supervisord.sh"

# 停掉旧的（手工起的和 supervisor 起的都要停，否则会抢同一个端口）
"$ADB" -s "$SERIAL" shell "pkill -f supervisord 2>/dev/null; \
    pkill -f 'remote-control --socket' 2>/dev/null; rm -f $DEV/remote-control.sock" || true
sleep 1

"$ADB" -s "$SERIAL" push "$BIN" "$DEV/remote-control" >/dev/null
"$ADB" -s "$SERIAL" push "$CTL" "$DEV/rcctl" >/dev/null
"$ADB" -s "$SERIAL" push "$SUP" "$DEV/remote-control-supervisord.sh" >/dev/null
"$ADB" -s "$SERIAL" shell "chmod 755 $DEV/remote-control $DEV/rcctl $DEV/remote-control-supervisord.sh"

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
if ! "$ADB" -s "$SERIAL" shell "test -f $CONF" 2>/dev/null; then
    "$ADB" -s "$SERIAL" shell "printf 'enabled=1\nbind=0.0.0.0\nport=8088\nauth=0\ntoken=\n' > $CONF"
    ok "已写入默认配置（对外监听 8088，无鉴权）"
else
    ok "沿用已有 $CONF"
fi

# supervisor 管理真正的启停。**必须 setsid** —— adb shell 一退出，
# 普通后台进程会被一起带走（实测踩过：以为起来了，其实早没了）。
"$ADB" -s "$SERIAL" shell \
    "setsid nohup $DEV/remote-control-supervisord.sh > $DEV/sup.log 2>&1 < /dev/null &"
sleep 4

# -----------------------------------------------------------------------------
step "5/5  局域网转发"
# 转发目标必须是**本实例的 adb 端口**，用 drop-in 盖掉单元模板里的默认值。
# 模板默认 5581（= 默认实例 5580 的 adb 端口）；换了实例端口不覆盖的话，
# 转发会一直连一个不存在的端口 —— `adb connect <IP>:$ADB_LAN_PORT` 永远 offline。
DROPIN=/etc/systemd/system/remote-control-lan-forward.service.d
mkdir -p "$DROPIN"
printf '[Service]\nEnvironment=LAN_FORWARD_LISTEN=%s\nEnvironment=LAN_FORWARD_TARGET=%s\n' \
    "$ADB_LAN_PORT" "$ADB_TARGET_PORT" > "$DROPIN/target.conf"
systemctl daemon-reload
if systemctl is-active --quiet remote-control-lan-forward; then
    systemctl restart remote-control-lan-forward
    ok "lan-forward 已重启（$ADB_LAN_PORT → 127.0.0.1:$ADB_TARGET_PORT）"
else
    systemctl start remote-control-lan-forward
    ok "lan-forward 已启动（$ADB_LAN_PORT → 127.0.0.1:$ADB_TARGET_PORT）"
fi
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
echo "     先给 remote-control 加 --http-token（见 docs/api/04-config.md 的鉴权一节）。"
