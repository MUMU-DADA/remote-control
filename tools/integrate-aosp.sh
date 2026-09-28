#!/usr/bin/env bash
# =============================================================================
# integrate-aosp.sh —— 把 autod 的源码接进 AOSP 树
#
# 目标布局：
#   aosp/frameworks/native/cmds/autod/
#   ├── daemon/     （含 Android.bp，模块 autod / autod_binder / autod_stub）
#   ├── client/     （含 Android.bp，模块 autodctl）
#   ├── init/       （autod.rc，被 daemon/Android.bp 的 init_rc 引用）
#   └── sepolicy/   （后期待接入，见文末）
#
# 为什么 daemon/ 和 client/ 必须同级：
#   client/autodctl.cpp 里是 #include "../daemon/protocol.h"
#
# 用法:
#   bash tools/integrate-aosp.sh          # 接入 + 预检
#   bash tools/integrate-aosp.sh --check  # 只预检，不拷贝
# =============================================================================
set -euo pipefail

PROJECT_DIR=/root/AutoSnapshotAndroid
SRC="$PROJECT_DIR/dev/02-native-daemon"
AOSP="$PROJECT_DIR/aosp"
DST="$AOSP/frameworks/native/cmds/autod"

CHECK_ONLY=false
FORCE=false
for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=true ;;
        --force) FORCE=true ;;   # 预检失败也照拷（同步未完成时铺源码用）
    esac
done

ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m  ! %s\033[0m\n' "$*"; }
step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }

FAILED=0

# -----------------------------------------------------------------------------
step "预检 1/4  源码树是否就绪"
# -----------------------------------------------------------------------------
for d in \
    frameworks/native/libs/gui \
    frameworks/native/libs/ui \
    frameworks/native/libs/binder \
    frameworks/native/libs/input \
    frameworks/native/include/input
do
    if [ -d "$AOSP/$d" ]; then ok "$d"
    else bad "$d 缺失（repo sync 未完成？）"; FAILED=1; fi
done

for d in system/core system/logging system/libbase; do
    if [ -d "$AOSP/$d" ]; then ok "$d"
    else warn "$d 缺失 —— liblog/libutils/libcutils 在这里，现在编不过"; FAILED=1; fi
done

# -----------------------------------------------------------------------------
step "预检 2/4  校验 Android.bp 引用的 Soong 模块确实存在"
# -----------------------------------------------------------------------------
# 这些名字在 Android 12 里核实过；换版本时这里会第一时间报出来，
# 不用等 m autod 跑到一半才失败。
check_module() {
    local mod="$1"
    if grep -rq "name: \"$mod\"" "$AOSP/frameworks/native" "$AOSP/system" 2>/dev/null; then
        ok "模块 $mod"
    else
        bad "模块 $mod 在树里找不到"
        FAILED=1
    fi
}
for m in libgui libgui_headers libui libbinder libinput; do
    check_module "$m"
done
for m in libbase libcutils liblog libutils; do
    if grep -rq "name: \"$m\"" "$AOSP/system" 2>/dev/null; then
        ok "模块 $m"
    else
        warn "模块 $m 尚未确认（system/ 未同步完）"
    fi
done

# -----------------------------------------------------------------------------
step "预检 3/4  校验 Android 12 的截图 API 与实现一致"
# -----------------------------------------------------------------------------
GUI="$AOSP/frameworks/native/libs/gui/include/gui"

if grep -q "class ScreenshotClient" "$GUI/SurfaceComposerClient.h" 2>/dev/null; then
    ok "ScreenshotClient 存在"
else
    bad "ScreenshotClient 不存在 —— capture_surfaceflinger.cpp 需要改"; FAILED=1
fi

if grep -q "struct DisplayCaptureArgs" "$GUI/LayerState.h" 2>/dev/null; then
    ok "DisplayCaptureArgs 在 gui/LayerState.h"
else
    bad "DisplayCaptureArgs 位置变了"; FAILED=1
fi

if grep -q "status_t result" "$GUI/ScreenCaptureResults.h" 2>/dev/null; then
    ok "ScreenCaptureResults.result 是 status_t（Android 12 形态）"
else
    bad "ScreenCaptureResults 形态变了 —— 检查 result/fenceResult"; FAILED=1
fi

if [ -f "$GUI/SyncScreenCaptureListener.h" ]; then
    ok "SyncScreenCaptureListener 存在"
else
    bad "SyncScreenCaptureListener 不存在"; FAILED=1
fi

# -----------------------------------------------------------------------------
step "预检 4/4  校验 AIDL 情况（约束 1 是否仍成立）"
# -----------------------------------------------------------------------------
if grep -rqiE "backend|vintfstability" \
     "$AOSP/frameworks/base/core/java/android/hardware/input/IInputManager.aidl" 2>/dev/null
then
    warn "IInputManager.aidl 现在有 backend 标注了！"
    warn "→ 可能可以启用 inject_binder.cpp，见 docs/01-selection.md 的约束部分"
else
    ok "IInputManager 仍是 Java-only → uinput 后端仍必需"
fi

# -----------------------------------------------------------------------------
if [ "$FAILED" -ne 0 ]; then
    if $FORCE && ! $CHECK_ONLY; then
        printf '\n\033[1;33m预检未通过，但 --force 指定继续拷贝源码\033[0m\n'
        printf '  （现在还不能编译，等 repo sync 把 system/ 拉完）\n'
    else
        printf '\n\033[1;31m预检未通过，先解决上面的问题\033[0m\n'
        printf '  想先铺源码: bash tools/integrate-aosp.sh --force\n'
        exit 1
    fi
else
    printf '\n\033[1;32m预检通过\033[0m\n'
fi

if $CHECK_ONLY; then
    exit 0
fi

# -----------------------------------------------------------------------------
step "拷贝源码到 $DST"
# -----------------------------------------------------------------------------
mkdir -p "$DST"

for sub in daemon client sepolicy; do
    if [ ! -d "$SRC/$sub" ]; then
        warn "源目录不存在，跳过: $SRC/$sub"
        continue
    fi
    mkdir -p "$DST/$sub"
    # 只拷源文件，排除主机侧的构建产物
    rsync -a --delete \
        --exclude='*.o' --exclude='*.d' --exclude='__pycache__' \
        --exclude='test_*' --exclude='autod-host' --exclude='Makefile' \
        "$SRC/$sub/" "$DST/$sub/"
    ok "$sub/"
done

# tests/ 单独处理：保留源码，去掉 Makefile 和产物
mkdir -p "$DST/tests"
rsync -a --exclude='test_inject_uinput' --exclude='test_integration' \
    "$SRC/tests/" "$DST/tests/" 2>/dev/null || true
ok "tests/"

# -----------------------------------------------------------------------------
step "校验布局"
# -----------------------------------------------------------------------------
[ -f "$DST/daemon/Android.bp" ]        && ok "daemon/Android.bp"        || { bad "缺 daemon/Android.bp"; exit 1; }
[ -f "$DST/client/Android.bp" ]        && ok "client/Android.bp"        || { bad "缺 client/Android.bp"; exit 1; }
[ -f "$DST/client/autodctl.cpp" ]      && ok "client/autodctl.cpp"      || { bad "缺 client/autodctl.cpp"; exit 1; }
[ -f "$DST/daemon/autod.rc" ]          && ok "daemon/autod.rc"          || { bad "缺 daemon/autod.rc"; exit 1; }
[ -f "$DST/daemon/capture_surfaceflinger.cpp" ] && ok "capture 后端"    || { bad "缺 capture 后端"; exit 1; }

# client 依赖 ../daemon/protocol.h
if [ -f "$DST/daemon/protocol.h" ] && [ -f "$DST/client/autodctl.cpp" ]; then
    rel=$(dirname "$DST/client")/daemon/protocol.h
    [ -f "$rel" ] && ok "client/ 能通过 ../daemon/ 找到 protocol.h"
fi

echo
echo "树内路径: frameworks/native/cmds/autod/"
find "$DST" -maxdepth 2 -type d | sed "s|$DST|  .|" | sort

# -----------------------------------------------------------------------------
step "下一步"
# -----------------------------------------------------------------------------
cat <<'EOF'
  bash tools/build-autod.sh          # 编 autod + autodctl
  bash tools/build-autod.sh --stub   # 额外编 autod_stub（桩截图，CI 用）

SELinux 策略还没接入。`sepolicy/` 只是模板，且 Android 12 的
system/sepolicy 尚未同步完。等它到位后再：
  1. 把 autod.te 放进 system/sepolicy/private/
  2. file_contexts 追加到 system/sepolicy/private/file_contexts
  3. 用 permissive autod; 先跑通，再逐条收 allow
详见 dev/02-native-daemon/README.md 阶段 2。
EOF
