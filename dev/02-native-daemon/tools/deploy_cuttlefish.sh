#!/usr/bin/env bash
# 把 autod 部署到 Cuttlefish 并做冒烟测试。
#
# 用法:
#   tools/deploy_cuttlefish.sh [autod 二进制路径] [autodctl 二进制路径]
#
# 默认从 AOSP 输出目录取：
#   $ANDROID_PRODUCT_OUT/system/bin/autod
#   $ANDROID_PRODUCT_OUT/system/bin/autodctl
#
# 这是「阶段 1 原型」路径：以 root 身份前台跑，跳过 sepolicy。
# 验证 Binder 调用无误之后，再走 autod.rc + sepolicy 的正式路径。

set -euo pipefail

AUTOD_BIN="${1:-${ANDROID_PRODUCT_OUT:-}/system/bin/autod}"
AUTODCTL_BIN="${2:-${ANDROID_PRODUCT_OUT:-}/system/bin/autodctl}"

SOCKET=/data/local/tmp/autod.sock
SHOT=/data/local/tmp/autod-shot.png

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
log "检查前置条件"

command -v adb >/dev/null || die "找不到 adb"

for f in "$AUTOD_BIN" "$AUTODCTL_BIN"; do
    [ -f "$f" ] || die "找不到二进制: $f
    先编译:  source build/envsetup.sh && lunch <target> && m autod autodctl"
done

adb wait-for-device
log "设备: $(adb shell getprop ro.product.name | tr -d '\r') / \
$(adb shell getprop ro.build.version.release | tr -d '\r')"

# ---------------------------------------------------------------------------
log "获取 root 并 remount"

adb root >/dev/null 2>&1 || warn "adb root 失败，可能已经是 root 或镜像不允许"
adb wait-for-device
adb remount >/dev/null 2>&1 || warn "adb remount 失败（非 userdebug/eng 镜像？）"

# ---------------------------------------------------------------------------
log "推送二进制"

adb push "$AUTOD_BIN"    /system/bin/autod    >/dev/null
adb push "$AUTODCTL_BIN" /system/bin/autodctl >/dev/null
adb shell chmod 755 /system/bin/autod /system/bin/autodctl

# ---------------------------------------------------------------------------
log "启动 autod（前台模式，root 身份，跳过 sepolicy）"

adb shell "pkill -f 'autod --socket' 2>/dev/null || true"
adb shell "rm -f $SOCKET"

# nohup 到后台，日志进 logd，我们通过 logcat 看
adb shell "nohup /system/bin/autod --socket $SOCKET --foreground --verbose \
    >/data/local/tmp/autod.log 2>&1 &"

sleep 2

if ! adb shell "test -S $SOCKET"; then
    echo "--- autod 日志 ---" >&2
    adb shell "cat /data/local/tmp/autod.log" >&2 || true
    echo "--- logcat ---" >&2
    adb logcat -d -s autod:* >&2 || true
    die "socket $SOCKET 没有创建，autod 启动失败"
fi
log "socket 已就绪"

# ---------------------------------------------------------------------------
log "冒烟测试"

echo
echo "--- info ---"
adb shell "/system/bin/autodctl --socket $SOCKET info"

echo
echo "--- capture ---"
adb shell "/system/bin/autodctl --socket $SOCKET capture -o $SHOT"
adb pull "$SHOT" ./autod-shot.png >/dev/null 2>&1 \
    && log "截图已拉回 ./autod-shot.png" \
    || warn "拉取截图失败"

echo
echo "--- tap 中心点 ---"
# 取屏幕中心
read -r W H <<<"$(adb shell wm size | tr -d '\r' | awk -F'[: x]+' '{print $(NF-1), $NF}')"
log "屏幕尺寸 ${W}x${H}，点击中心 ($((W/2)), $((H/2)))"
adb shell "/system/bin/autodctl --socket $SOCKET tap $((W/2)) $((H/2))"

echo
echo "--- swipe ---"
adb shell "/system/bin/autodctl --socket $SOCKET swipe \
    $((W/2)) $((H*3/4)) $((W/2)) $((H/4)) --ms 300"

# ---------------------------------------------------------------------------
log "完成"
cat <<'EOF'

下一步:
  1. 检查 .autod-shot.png 是不是真实画面
     - 全黑 → 截图路径有问题，看 logcat -s autod:*
     - 正常 → 截图通道 OK
  2. 确认 tap/swipe 真的点中了界面（打开一个 App 试）
  3. 两者都 OK 后，进入阶段 2：补 autod.rc + sepolicy，改成开机自启

排查命令:
  adb logcat -s autod:*
  adb shell cat /data/local/tmp/autod.log
  adb shell dmesg | grep avc        # SELinux 拒绝
EOF
