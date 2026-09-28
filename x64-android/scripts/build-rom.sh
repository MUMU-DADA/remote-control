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
    if docker exec "$BUILDER_CONTAINER" pgrep -f "soong_ui.bash|ninja" >/dev/null 2>&1; then
        echo "running"
    else
        echo "idle"
    fi
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
        docker exec "$BUILDER_CONTAINER" pkill -f "soong_ui.bash" 2>/dev/null && log "已发停止信号" || warn "没在跑"
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

CMD="cd $BUILDER_AOSP_PATH && source build/envsetup.sh >/dev/null && lunch $LUNCH_TARGET && m -j$JOBS"
log "开始构建： lunch $LUNCH_TARGET && m -j$JOBS"
log "日志： ${LOG_FILE#"$PROJECT_ROOT"/}"

if [ "${WAIT:-0}" = 1 ]; then
    in_container "$CMD" 2>&1 | tee "$LOG_FILE"
else
    docker exec -d "$BUILDER_CONTAINER" bash -lc \
        "$CMD > $BUILDER_AOSP_PATH/out/autosnap-build.log 2>&1; echo \"EXIT=\$?\" >> $BUILDER_AOSP_PATH/out/autosnap-build.log"
    log "已在后台开跑。进度： ./build-rom.sh --status"
fi
