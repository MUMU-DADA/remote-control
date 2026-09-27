#!/usr/bin/env bash
# =============================================================================
# build-cuttlefish.sh —— 编 Cuttlefish 镜像（用于真实验证 autod）
#
# 为什么需要它：autod 的 ARM64 二进制已经编出来了，但**从没在真实 Android 上跑过**。
# 没有实体设备的情况下，Cuttlefish 是唯一的验证途径 —— 它是 AOSP 官方的
# 虚拟设备方案，跑完整 Android，能装 autod 并真实执行。
#
# 为什么是 x86_64 而不是 arm64：
#   Cuttlefish 跑在 KVM 上，跨架构没有 KVM 加速。x86_64 主机只能用
#   aosp_cf_x86_64_phone。好在 autod 的代码是架构无关的，
#   x86_64 上验证过的逻辑，ARM64 上一样。
#
# 规模：完整系统镜像，首次约 40000-80000 个 ninja 动作，1-3 小时
#
# 用法:
#   setsid nohup bash tools/build-cuttlefish.sh > /var/log/cf-build.log 2>&1 &
#   tail -f /var/log/cf-build.log
# =============================================================================
set -euo pipefail

# ⚠️ 本文件里 `docker exec ... bash -lc "..."` 用的是双引号字符串，
#    所以字符串内部**不能出现反引号** —— 宿主 shell 会把它当命令替换执行。
#    （第一版注释里写了 `m`，结果宿主上真的去执行了 m 命令并报错。）

CONTAINER=${CONTAINER:-autod-builder}
TARGET=${TARGET:-aosp_cf_x86_64_phone-userdebug}
JOBS=${JOBS:-12}

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; }

# -----------------------------------------------------------------------------
step "前置检查"
# -----------------------------------------------------------------------------
docker inspect -f '{{.State.Running}}' "$CONTAINER" 2>/dev/null | grep -q true \
    || { bad "容器 $CONTAINER 未运行"; exit 1; }

# 防重入（两个编译抢 out/.lock 的报错完全看不出真因）
if bash "$(dirname "$0")/check-no-build.sh"; then
    bad "容器里已有编译在跑"
    echo "  等它结束：while docker exec $CONTAINER pgrep -f soong_ui >/dev/null; do sleep 10; done"
    exit 1
fi
ok "无其他编译"

docker exec "$CONTAINER" test -d /aosp/device/google/cuttlefish \
    || { bad "缺 device/google/cuttlefish"; exit 1; }
ok "Cuttlefish 设备树存在"

DF=$(df -BG /root/AutoSnapshotAndroid | tail -1 | awk '{print $4}' | tr -d 'G')
[ "$DF" -gt 120 ] || { bad "磁盘余量不足（${DF}G，完整镜像需要约 100G）"; exit 1; }
ok "磁盘余量 ${DF}G"

# -----------------------------------------------------------------------------
step "编译 $TARGET"
# -----------------------------------------------------------------------------
LOG=/tmp/cf-build-$(date +%Y%m%d-%H%M%S).log

set +e
docker exec "$CONTAINER" bash -lc "
    set -e
    cd /aosp
    source build/envsetup.sh >/dev/null 2>&1
    lunch $TARGET >/dev/null 2>&1
    echo \"TARGET_PRODUCT=\$TARGET_PRODUCT\"
    echo \"TARGET_BUILD_VARIANT=\$TARGET_BUILD_VARIANT\"
    echo \"ANDROID_PRODUCT_OUT=\$ANDROID_PRODUCT_OUT\"
    # 先编镜像，再编 autod/autodctl。
    #
    # ⚠️ 光跑 m 是不够的 —— 它只编 PRODUCT_PACKAGES 里的模块，
    #    而 autod 不在其中，所以 system/bin/ 下不会出现 autod。
    #    verify-cuttlefish.sh 要 push 这个二进制，缺了就无从验证。
    m -j$JOBS droid autod autodctl
" 2>&1 | tee "$LOG"
RC=${PIPESTATUS[0]}
set -e

if [ "$RC" -ne 0 ]; then
    printf '\n\033[1;31m编译失败（退出码 %d）\033[0m\n' "$RC"
    echo "日志: $LOG"
    echo
    echo "─── 错误摘要 ───"
    grep -nE "error:|FAILED:" "$LOG" | head -20 || echo "（未匹配到标准错误格式）"
    exit "$RC"
fi

# -----------------------------------------------------------------------------
step "产物"
# -----------------------------------------------------------------------------
docker exec "$CONTAINER" bash -lc '
    cd /aosp
    source build/envsetup.sh >/dev/null 2>&1
    lunch '"$TARGET"' >/dev/null 2>&1
    echo "  OUT=$ANDROID_PRODUCT_OUT"
    ls -la "$ANDROID_PRODUCT_OUT"/*.img 2>/dev/null | awk "{printf \"    %-28s %10s\n\", \$NF, \$5}" | head -12
    echo
    echo "  启动工具:"
    ls -la out/host/linux-x86/bin/launch_cvd out/host/linux-x86/bin/assemble_cvd 2>/dev/null | awk "{printf \"    %s\n\", \$NF}"
'

echo
echo "日志: $LOG"
echo
cat <<'EOF'
下一步（启动 Cuttlefish 并部署 autod）:
  bash tools/verify-cuttlefish.sh

或者手工：
  docker exec -it autod-builder bash
  cd /aosp && source build/envsetup.sh && lunch aosp_cf_x86_64_phone-userdebug
  launch_cvd --daemon
  adb wait-for-device
  adb push $ANDROID_PRODUCT_OUT/system/bin/autod    /data/local/tmp/
  adb push $ANDROID_PRODUCT_OUT/system/bin/autodctl /data/local/tmp/
  adb root && adb shell /data/local/tmp/autod --selftest
EOF
