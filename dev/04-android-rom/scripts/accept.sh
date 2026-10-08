#!/usr/bin/env bash
# 构建完成后的一条龙：自检 → 依赖检查 → 打包 → 启动 → 验收。
#
#   ./accept.sh                 # 全套
#   ./accept.sh --skip-boot     # 只做自检/依赖/打包，不启动模拟器
#   ./accept.sh --port 5580
#
# 每一步失败都会立刻停（`set -e`），并给出下一步该看哪个文档。
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

SKIP_BOOT=0
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-boot) SKIP_BOOT=1 ;;
        --port)      EMULATOR_PORT="${2:?}"; shift ;;
        -h|--help)   sed -n '2,10p' "$0"; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
    shift
done
export EMULATOR_PORT

# 0) 构建真的结束了吗
if pgrep -f "/aosp/out/soong_ui" >/dev/null 2>&1; then
    die "构建还在跑（./scripts/build-rom.sh --status）；等它结束再验收"
fi
LOG="$AOSP_DIR/out/remote-control-build.log"
if [ -s "$LOG" ]; then
    if grep -q '^EXIT=0' "$LOG"; then
        log "构建结束且 EXIT=0 ✓"
    else
        warn "构建日志里没有 EXIT=0（最后一行：$(tail -1 "$LOG" | cut -c1-100)）"
        die "先解决构建问题；排错见 docs/02-build-traps.md"
    fi
fi

# 1) ROM 自检
log "① ROM 自检"
"$X64_DIR/tools/verify-rom.sh" || die "ROM 自检未通过 —— 看上面哪一项不合格"

# 2) 翻译层动态依赖
log "② 翻译层依赖自检"
set +e
"$X64_DIR/tools/check-bridge-symbols.sh"
sym_rc=$?
set -e
if [ "$sym_rc" != 0 ]; then
    warn "依赖自检有告警（未解析符号）——不一定是真问题，但启动后要重点看 logcat"
    warn "  明细： .run/symcheck-missing.txt"
fi

# 3) 打包
log "③ 打包可交付 ROM 目录"
"$X64_DIR/scripts/package-rom.sh"

# 4) 启动 / 5) 验收
if [ "$SKIP_BOOT" = 1 ]; then
    log "已跳过启动验收（--skip-boot）。手动跑： ./scripts/run-linux.sh"
    exit 0
fi
log "④ Linux/KVM 启动（端口 $EMULATOR_PORT）"
"$X64_DIR/scripts/run-linux.sh" --port "$EMULATOR_PORT"

# ⑤ 真正的验收。
#
# ⚠️ 这一步以前**没有** —— `run-linux.sh --port` 只负责把模拟器拉起来、
#    等服务就绪就返回，`--verify` 那一段（镜像身份与 ABI、翻译层接线、
#    aarch64 机器码真的在跑、arm64 APK 能装能跑）是**显式**才跑的。
#    于是上面那一步标题写着"启动 + 验收"，实际一次检查都没做，而它是
#    交付流程的最后一道闸：跑了半小时构建、看到"一条龙"走完，其实
#    第 ④ 步只证明了"HTTP 起来了"。
log "⑤ 验收：镜像身份 / 翻译层 / aarch64 / arm64 APK"
"$X64_DIR/scripts/run-linux.sh" --verify --port "$EMULATOR_PORT"
