#!/usr/bin/env bash
# 编译 AutoSnap x86_64 + ARM64 桥 ROM（在 autod-builder 容器里跑 AOSP 构建）。
#
#   ./build-rom.sh              # 全量构建（droid），后台跑，日志 aosp/out/autosnap-build.log
#   ./build-rom.sh --wait       # 前台等它结束（几十分钟~几小时）
#   ./build-rom.sh --modules autod   # 只编模块（分钟级）
#   ./build-rom.sh --status     # 看进度
#   ./build-rom.sh --stop       # 停掉
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

LOG_FILE="$AOSP_DIR/out/autosnap-build.log"
PID_FILE="$RUN_DIR/build.pid"
mkdir -p "$RUN_DIR"

in_container() {   # 在容器里跑一段 bash
    docker exec "$BUILDER_CONTAINER" bash -lc "$1"
}

build_status() {
    # 用宿主 pgrep：容器进程在宿主 PID 命名空间可见（实测），
    # 这样即使宿主 /tmp 被写满导致 `docker exec` 失败，状态判断照样能用。
    if pgrep -f "/aosp/out/soong_ui" >/dev/null 2>&1; then echo "running"; else echo "idle"; fi
}

case "${1:-}" in
    --status)
        echo "容器：$BUILDER_CONTAINER（$(build_status)）"
        if [ -f "$LOG_FILE" ]; then
            echo "--- 日志尾部（${LOG_FILE#"$PROJECT_ROOT"/}）---"
            tail -15 "$LOG_FILE"
        else
            log "还没有构建日志"
        fi
        exit 0 ;;
    --stop)
        pkill -f "soong_ui" 2>/dev/null && log "已发停止信号" || warn "没在跑"
        exit 0 ;;
    --modules)
        shift
        [ $# -gt 0 ] || die "--modules 需要目标，例如： --modules autod"
        log "编模块：$*"
        in_container "cd $BUILDER_AOSP_PATH && source build/envsetup.sh >/dev/null && lunch $LUNCH_TARGET >/dev/null && m -j$JOBS $*"
        exit 0 ;;
    --wait) WAIT=1 ;;
    "")     WAIT=0 ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) die "未知参数：$1" ;;
esac

# ---------------------------------------------------------------------------
# 前置检查
docker ps --format '{{.Names}}' | grep -qx "$BUILDER_CONTAINER" \
    || die "容器 $BUILDER_CONTAINER 不在跑（docker start $BUILDER_CONTAINER）"
[ -f "$DEVICE_DST/AndroidProducts.mk" ] \
    || die "AOSP 里没有注入设备树。先跑： ./apply-overlay.sh"
[ -f "$DEVICE_DST/autosnap_x64_arm64/bridge/bridge-copy.mk" ] \
    || die "载荷拷贝规则缺失，重跑： ./apply-overlay.sh"

if [ "$(build_status)" = running ]; then
    warn "已经有一个构建在跑（--status 看进度）"
    exit 0
fi

# QEMU_DISABLE_AVB=true 是 AOSP 给模拟器镜像准备的正规开关
# （build/make/target/board/BoardConfigEmuCommon.mk 里读它 → BOARD_AVB_ENABLE := false，
#  源码里从来不赋值，就是设计成命令行传入的）。
# 不加它的后果实测如下：first-stage init 能挂到 dm-0，但 AVB 校验失败并致命重启——
#     [libfs_avb]Invalid hash size / Failed to verify vbmeta digest /
#     vbmeta digest error isn't allowed → Failed to mount required partitions early
#     → InitFatalReboot: signal 6
# 加了之后用 fstab.ranchu.noavb + dummy vbmeta，镜像也更快出。
CMD="cd $BUILDER_AOSP_PATH && source build/envsetup.sh >/dev/null && lunch $LUNCH_TARGET && m -j$JOBS QEMU_DISABLE_AVB=${QEMU_DISABLE_AVB:-true}"

# AOSP 树里可能同时有别人在编辑的模块（例如 frameworks/native/cmds/autod 正处于改动中，
# 其 libwebp_vendored 变体暂时对不上）。ALLOW_MISSING_DEPENDENCIES=true 让 Soong
# **跳过**这类模块而不是让整棵树编不过——本项目不需要 autod，跳过它没有任何影响。
# 想严格模式： ALLOW_MISSING_DEPS=0 ./build-rom.sh
ALLOW_MISSING_DEPS="${ALLOW_MISSING_DEPS:-1}"
if [ "$ALLOW_MISSING_DEPS" = 1 ]; then
    CMD="export ALLOW_MISSING_DEPENDENCIES=true && $CMD"
    warn "已开启 ALLOW_MISSING_DEPENDENCIES（跳过树里有依赖问题的模块，见日志 'missing dependencies'）"
fi

log "开始构建： lunch $LUNCH_TARGET && m -j$JOBS"
log "日志： ${LOG_FILE#"$PROJECT_ROOT"/}"

if [ "${WAIT:-0}" = 1 ]; then
    in_container "$CMD" 2>&1 | tee "$LOG_FILE"
else
    docker exec -d "$BUILDER_CONTAINER" bash -lc \
        "$CMD > $BUILDER_AOSP_PATH/out/autosnap-build.log 2>&1; echo \"EXIT=\$?\" >> $BUILDER_AOSP_PATH/out/autosnap-build.log"
    log "已在后台开跑。进度： ./build-rom.sh --status"
fi
