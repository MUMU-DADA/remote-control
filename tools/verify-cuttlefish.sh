#!/usr/bin/env bash
# =============================================================================
# verify-cuttlefish.sh —— 在 Cuttlefish 上真实验证 remote-control
#
# 前置：先跑 tools/build-cuttlefish.sh 编出镜像
#
# 做什么：
#   1. 确认容器有 /dev/kvm（没有就重建容器 —— 这是 Cuttlefish 的硬前提）
#   2. 启动 Cuttlefish 并等它开机
#   3. 推 remote-control / rcctl 到设备
#   4. 跑 remote-control --selftest（逐项检查环境）
#   5. 端到端冒烟：截图 + 点击 + 滑动
#
# 这是**唯一能在没有实体设备时真正验证 remote-control 的途径** ——
# 前面的主机测试都只覆盖了平台无关的逻辑，SurfaceFlinger 抓屏、
# 真实 /dev/uinput、SELinux 都只有在真实 Android 上才能验证。
#
# 用法:
#   bash tools/verify-cuttlefish.sh
#   bash tools/verify-cuttlefish.sh --no-restart   # 容器已有 KVM，不重建
# =============================================================================
set -euo pipefail

CONTAINER=${CONTAINER:-remote-control-builder}
TARGET=${TARGET:-aosp_cf_x86_64_phone-userdebug}
PROJECT_DIR=/root/AutoSnapshotAndroid
RESTART_CONTAINER=true
[ "${1:-}" = "--no-restart" ] && RESTART_CONTAINER=false

SOCKET=/data/local/tmp/remote-control.sock
DEV=/data/local/tmp

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; }
warn() { printf '\033[1;33m  ! %s\033[0m\n' "$*"; }

# -----------------------------------------------------------------------------
step "1/6  检查镜像"
# -----------------------------------------------------------------------------
IMG_DIR=$(docker exec "$CONTAINER" bash -lc '
    cd /aosp && source build/envsetup.sh >/dev/null 2>&1 && lunch '"$TARGET"' >/dev/null 2>&1
    echo $ANDROID_PRODUCT_OUT' 2>/dev/null | tail -1)

if [ -z "$IMG_DIR" ]; then
    bad "拿不到 ANDROID_PRODUCT_OUT —— 先跑 tools/build-cuttlefish.sh"
    exit 1
fi
ok "产物目录 $IMG_DIR"

N_IMG=$(docker exec "$CONTAINER" bash -lc "ls $IMG_DIR/*.img 2>/dev/null | wc -l")
if [ "$N_IMG" -lt 3 ]; then
    bad "只有 $N_IMG 个 .img —— 镜像没编完，先跑 tools/build-cuttlefish.sh"
    exit 1
fi
ok "$N_IMG 个镜像文件"

# -----------------------------------------------------------------------------
step "2/6  确保容器有 /dev/kvm"
# -----------------------------------------------------------------------------
NEED_DEVS="/dev/kvm /dev/net/tun /dev/vhost-net"
MISSING=""
for d in $NEED_DEVS; do
    docker exec "$CONTAINER" test -e "$d" 2>/dev/null || MISSING="$MISSING $d"
done

if [ -z "$MISSING" ]; then
    ok "容器内设备齐全（$NEED_DEVS）"
else
    warn "容器内缺设备:$MISSING —— Cuttlefish 跑不起来"
    if ! $RESTART_CONTAINER; then
        bad "--no-restart 指定了但容器缺 KVM，无法继续"
        exit 1
    fi
    echo "  重建容器（加 --device /dev/kvm）…"
    docker rm -f "$CONTAINER" >/dev/null
    # --privileged 而不是只挂 /dev/kvm：
    # Cuttlefish 还要 /dev/net/tun + /dev/vhost-net + /dev/vhost-vsock
    # 以及 CAP_NET_ADMIN 来创建 TAP 网卡（见 crosvm_manager.cpp 的
    # OpenTapInterface / --vhost-net）。
    docker run -d \
        --name "$CONTAINER" \
        --hostname builder \
        --privileged \
        -v "$PROJECT_DIR/aosp":/aosp \
        -w /aosp \
        remote-control-aosp12-builder \
        sleep infinity >/dev/null
    sleep 3
    for d in $NEED_DEVS; do
        docker exec "$CONTAINER" test -e "$d" || { bad "重建后仍缺 $d"; exit 1; }
    done
    ok "容器已重建，设备齐全"
fi

# -----------------------------------------------------------------------------
step "3/6  启动 Cuttlefish"
# -----------------------------------------------------------------------------
# 先停掉可能残留的实例
docker exec "$CONTAINER" bash -lc '
    cd /aosp && source build/envsetup.sh >/dev/null 2>&1 && lunch '"$TARGET"' >/dev/null 2>&1
    stop_cvd >/dev/null 2>&1 || true' 2>/dev/null || true

echo "  启动中（首次约 1-2 分钟）…"
if ! docker exec "$CONTAINER" bash -lc '
    cd /aosp && source build/envsetup.sh >/dev/null 2>&1 && lunch '"$TARGET"' >/dev/null 2>&1
    launch_cvd --daemon 2>&1 | tail -20'; then
    bad "launch_cvd 失败"
    echo "  常见原因："
    echo "    • /dev/kvm 不可用 —— 见上一步"
    echo "  排查: docker exec $CONTAINER bash -lc 'cd /aosp && source build/envsetup.sh && lunch $TARGET && launch_cvd'"
    exit 1
fi
ok "launch_cvd 返回"

ADB="docker exec $CONTAINER bash -lc 'cd /aosp && source build/envsetup.sh >/dev/null 2>&1 && lunch $TARGET >/dev/null 2>&1 && adb'"

step "4/6  等待开机"
for i in $(seq 1 90); do
    if eval $ADB wait-for-device 2>/dev/null; then
        BOOTED=$(eval $ADB shell getprop sys.boot_completed 2>/dev/null | tr -d '\r\n')
        [ "$BOOTED" = "1" ] && break
    fi
    sleep 4
    [ $((i % 15)) -eq 0 ] && echo "  已等 $((i*4)) 秒…"
done
BOOTED=$(eval $ADB shell getprop sys.boot_completed 2>/dev/null | tr -d '\r\n')
if [ "$BOOTED" != "1" ]; then
    bad "设备未在预期时间内开机完成"
    exit 1
fi
ok "已开机"

eval $ADB root >/dev/null 2>&1 || true
sleep 2
eval $ADB wait-for-device

# -----------------------------------------------------------------------------
step "5/6  部署 remote-control"
# -----------------------------------------------------------------------------
eval $ADB push "$IMG_DIR/system/bin/remote-control"    $DEV/ >/dev/null
eval $ADB push "$IMG_DIR/system/bin/rcctl" $DEV/ >/dev/null
eval $ADB shell chmod 755 $DEV/remote-control $DEV/rcctl
ok "已推送 remote-control / rcctl"

echo
echo "════════════════ remote-control --selftest ════════════════"
eval $ADB shell $DEV/remote-control --selftest 2>&1 || true
echo "═══════════════════════════════════════════════════"

# -----------------------------------------------------------------------------
step "6/6  端到端冒烟"
# -----------------------------------------------------------------------------
eval $ADB shell "pkill -f 'remote-control --socket' 2>/dev/null || true"
eval $ADB shell "rm -f $SOCKET"

WxH=$(eval $ADB shell wm size 2>/dev/null | tr -d '\r' | sed 's/.*: //')
echo "  屏幕尺寸: $WxH"
W=${WxH%x*}
H=${WxH#*x}

eval $ADB shell "nohup $DEV/remote-control --socket $SOCKET --touch-range ${W}x${H} --foreground >/data/local/tmp/remote-control.log 2>&1 &"
sleep 2

if ! eval $ADB shell "test -S $SOCKET"; then
    bad "socket 没建起来，看服务日志："
    eval $ADB shell "cat /data/local/tmp/remote-control.log" 2>&1 | head -20 || true
    exit 1
fi
ok "服务已就绪"

echo
echo "── info ──"
eval $ADB shell "$DEV/rcctl --socket $SOCKET info" 2>&1 || true

echo
echo "── capture ──"
eval $ADB shell "$DEV/rcctl --socket $SOCKET capture -o $DEV/shot.png" 2>&1 || true
eval $ADB pull $DEV/shot.png ./cuttlefish-shot.png >/dev/null 2>&1 \
    && ok "截图已拉回 ./cuttlefish-shot.png（用图片查看器打开确认不是黑屏）" \
    || warn "截图拉取失败"

echo
echo "── tap 屏幕中心 ──"
eval $ADB shell "$DEV/rcctl --socket $SOCKET tap $((W/2)) $((H/2))" 2>&1 || true

echo
echo "── swipe ──"
eval $ADB shell "$DEV/rcctl --socket $SOCKET swipe $((W/2)) $((H*3/4)) $((W/2)) $((H/4)) --ms 300" 2>&1 || true

echo
echo "── 服务端日志 ──"
eval $ADB shell "cat /data/local/tmp/remote-control.log" 2>&1 | head -20 || true

cat <<EOF

═══════════════════════════════════════════════════
验证完成。检查清单：

  [ ] --selftest 是否全绿
  [ ] cuttlefish-shot.png 是真实画面（不是全黑）
  [ ] tap 是否真的点中了界面
  [ ] swipe 是否被应用响应

排查：
  docker exec $CONTAINER bash -lc 'cd /aosp && source build/envsetup.sh && lunch $TARGET && adb logcat -s remote-control:*'
  docker exec $CONTAINER bash -lc 'cd /aosp && source build/envsetup.sh && lunch $TARGET && adb shell dmesg | grep avc'

停止 Cuttlefish:
  docker exec $CONTAINER bash -lc 'cd /aosp && source build/envsetup.sh && lunch $TARGET && stop_cvd'
EOF
