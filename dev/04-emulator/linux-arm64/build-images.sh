#!/usr/bin/env bash
# 编 arm64 模拟器镜像（在构建容器里跑）。
#
#   ./build-images.sh                     # 全量：lunch + m（首次约 100GB / 数小时）
#   ./build-images.sh --modules           # 只编 autod / autodctl（分钟级，改代码后常用）
#   ./build-images.sh --targets "systemimage"
#   ./build-images.sh --detach            # 后台跑，日志落在 .run/ 下
#   ./build-images.sh --yes               # 不确认，直接开跑
#
# 为什么要单独一个目标：aosp_arm64-userdebug 在 Android 12 是 GSI，
# 产物里没有 kernel-ranchu / ramdisk.img / vendor.img，模拟器起不来。
# 模拟器必须用 sdk_phone64_arm64-userdebug（见 ../README.md 硬约束 3）。

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

MODE=full
DETACH=0
ASSUME_YES=0
EXTRA_TARGETS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --modules) MODE=modules ;;
        --targets) MODE=custom; EXTRA_TARGETS="${2:?--targets 需要参数}"; shift ;;
        --detach)  DETACH=1 ;;
        --yes|-y)  ASSUME_YES=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) die "未知参数：$1（-h 看用法）" ;;
    esac
    shift
done

case "$MODE" in
    full)    TARGETS="" ;;
    modules) TARGETS="autod autodctl" ;;
    custom)  TARGETS="$EXTRA_TARGETS" ;;
esac

# ---------------------------------------------------------------------------
if ! docker inspect "$BUILDER_CONTAINER" >/dev/null 2>&1; then
    die "没有容器 $BUILDER_CONTAINER —— 见 docs/07-environment.md 第 4 节"
fi
if [ "$(docker inspect -f '{{.State.Running}}' "$BUILDER_CONTAINER")" != "true" ]; then
    die "容器 $BUILDER_CONTAINER 没在跑： docker start $BUILDER_CONTAINER"
fi

if [ "$MODE" = full ]; then
    avail="$(df -BG --output=avail "$PROJECT_ROOT" | tail -1 | tr -dc '0-9')"
    log "全量编译 $LUNCH_TARGET"
    log "  并行度   : -j$JOBS（内存 31GiB 的经验值）"
    log "  预计占用 : out/ 最多 ~100GB，当前可用 ${avail}GB"
    log "  预计耗时 : 首次数小时；TCG 模拟器镜像比 GSI 多编内核模块与 vendor"
    if [ "$ASSUME_YES" != 1 ]; then
        printf '\033[1;33m继续？[y/N] \033[0m'
        read -r ans
        case "$ans" in y|Y|yes) ;; *) die "已取消" ;; esac
    fi
fi

# ---------------------------------------------------------------------------
# 容器内：lunch 目标 + 编译
INNER="cd $BUILDER_AOSP_PATH && source build/envsetup.sh >/dev/null && lunch $LUNCH_TARGET && m -j$JOBS $TARGETS"

STAMP="$(date +%Y%m%d-%H%M%S)"
LOGFILE="$RUN_DIR/build-$STAMP.log"

log "容器内执行："
log "  $INNER"
log "日志：${LOGFILE#"$PROJECT_ROOT"/}"

if [ "$DETACH" = 1 ]; then
    setsid nohup docker exec "$BUILDER_CONTAINER" bash -lc "$INNER" \
        > "$LOGFILE" 2>&1 &
    log "已在后台启动（PID $!）。跟踪进度："
    log "  tail -f ${LOGFILE#"$PROJECT_ROOT"/}"
else
    # tee 保留完整日志，同时让编译输出实时可见
    set +e
    docker exec -it "$BUILDER_CONTAINER" bash -lc "$INNER" 2>&1 | tee "$LOGFILE"
    rc="${PIPESTATUS[0]}"
    set -e
    [ "$rc" = 0 ] || die "编译失败（exit $rc），完整日志：$LOGFILE"
fi

# ---------------------------------------------------------------------------
if [ "$DETACH" = 1 ]; then
    exit 0
fi

log "编译完成。产物：out/target/product/$PRODUCT_DEVICE/"
if [ "$MODE" = modules ]; then
    ls -la "$PRODUCT_OUT/system/bin/autod" "$PRODUCT_OUT/system/bin/autodctl" 2>/dev/null || \
        warn "没看到 autod/autodctl —— 检查 frameworks/native/cmds/autod/ 是否已放进 AOSP 树"
else
    while read -r img; do
        [ -e "$PRODUCT_OUT/$img" ] && printf '  %-18s %s\n' "$img" "$(du -h "$PRODUCT_OUT/$img" | cut -f1)"
    done < <(required_images)
fi
printf '\n下一步： ./check-env.sh && ./run-emulator.sh\n'
