#!/usr/bin/env bash
# 看这台机器（或全部实例）现在是什么状态，以及这个包里的 ROM 是哪一份。
#
#   ./bin/status.sh                # 列所有实例
#   ./bin/status.sh --port 5580    # 看一个实例的细节
#
# macOS 版比 linux 版多一段「宿主」：架构 / Hypervisor.framework / GPU / 后端目录 ——
# 因为在 Mac 上这四样**决定了哪份 ROM 可用**（Apple Silicon 只能跑 arm64 ROM）。
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

NAME="$DEFAULT_NAME"; PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --name)    NAME="${2:?}"; shift ;;
        --port)    PORT="${2:?}"; shift ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
    shift
done

hr() { printf '\033[1;36m── %s\033[0m\n' "$1"; }

adb_state() {   # adb_state <端口>
    [ -x "$ADB" ] || { printf '无 adb'; return 0; }
    "$ADB" -s "$(serial_for_port "$1")" get-state 2>/dev/null | tr -d '\r' | sed -n '1p'
}
boot_state() {  # boot_state <端口>
    local v
    v="$("$ADB" -s "$(serial_for_port "$1")" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' | sed -n '1p')"
    if [ "$v" = 1 ]; then printf '已开机'; else printf '未就绪'; fi
}

# ---------------------------------------------------------------------------
hr "宿主"
printf '  %-16s %s\n' "macOS" "$(sw_vers -productVersion 2>/dev/null || echo '?')（$(sw_vers -buildVersion 2>/dev/null || echo '?')）"
printf '  %-16s %s\n' "架构" "$HOST_MACH（后端目录 $QEMU_DIR，可跑 guest：$GUEST_ABI_HINT）"
if hv_usable; then
    printf '  %-16s %s\n' "虚拟化" "Hypervisor.framework 可用（kern.hv_support=1）→ -accel on"
else
    printf '  %-16s \033[1;33m%s\033[0m\n' "虚拟化" "不可用（kern.hv_support≠1）→ 只能 TCG，开机分钟级"
fi
printf '  %-16s %s\n' "GPU" "$(gpu_reason)"
printf '  %-16s %s\n' "GPU 自适应结果" "$(resolve_gpu_mode "$(config_get hw.gpu.mode auto)")"

# ---------------------------------------------------------------------------
if [ -n "$PORT" ] || instance_exists "$NAME"; then
    [ -n "$PORT" ] || PORT="$(instance_port "$NAME")"
    [ -n "$PORT" ] || die "实例 '$NAME' 没登记过端口"
    SYSDIR="$(sysdir_for_port "$PORT")"
    PID="$(emu_pid_for_port "$PORT")"
    hr "实例 $NAME（端口 $PORT，序列号 $(serial_for_port "$PORT")）"
    if [ -n "$PID" ]; then
        printf '  %-16s %s\n' "进程" "在跑（PID $PID，已运行 $(ps -o etime= -p "$PID" 2>/dev/null | tr -d ' ' || echo '?'))"
        printf '  %-16s %s\n' "adb / 开机" "$(adb_state "$PORT") / $(boot_state "$PORT")"
        printf '  %-16s %s\n' "工作目录占用" "$(du -sh "$SYSDIR" 2>/dev/null | cut -f1 || echo '?')"
    else
        printf '  %-16s %s\n' "进程" "没在跑"
    fi
    printf '  %-16s %s\n' "工作目录" "${SYSDIR#"$ROOT"/}"
    printf '  %-16s %s\n' "日志" "${LOGF:-$(logfile_for_port "$PORT")}"
    if [ -s "$SYSDIR/hardware-qemu.ini" ]; then
        printf '  %-16s %s\n' "上次生效的硬件" "$(grep -E '^(hw\.ramSize|hw\.cpu\.ncore|hw\.lcd\.width|hw\.lcd\.height)\s*=' "$SYSDIR/hardware-qemu.ini" 2>/dev/null | paste -sd' ' - | tr -s ' ')"
    fi
    if [ -d "$SYSDIR/snapshots" ]; then
        printf '  %-16s %s\n' "快照" "$(ls "$SYSDIR/snapshots" 2>/dev/null | paste -sd' ' -)"
    fi
    # 设备侧的 guest 架构：一眼看出这份 ROM 是不是本机架构该用的那份
    if [ "$(boot_state "$PORT")" = "已开机" ]; then
        printf '  %-16s %s\n' "guest ABI" "$(adb_prop "$PORT" ro.product.cpu.abi)"
        printf '  %-16s %s\n' "guest 内核" "$("$ADB" -s "$(serial_for_port "$PORT")" shell uname -m 2>/dev/null | tr -d '\r' | sed -n '1p')"
    fi
    echo
fi

# ---------------------------------------------------------------------------
if [ -z "$PORT" ]; then
    hr "全部实例"
    any=0
    for n in $(instance_names); do
        any=1
        p="$(instance_port "$n")"
        pid="$(emu_pid_for_port "$p")"
        printf '  %-12s 端口 %-6s %s\n' "$n" "$p" \
            "$([ -n "$pid" ] && printf '在跑 (PID %s, %s)' "$pid" "$(boot_state "$p")" || printf '没在跑')"
    done
    [ "$any" = 1 ] || printf '  （还没有实例；./bin/start-headless.sh 建一台）\n'
    echo
fi

# ---------------------------------------------------------------------------
hr "这个包里的 ROM"
ABI="$(sed -n 's/^ro\.product\.cpu\.abi=//p' "$IMAGES/system/build.prop" 2>/dev/null | tail -1)"
ABILIST64="$(sed -n 's/^ro\.system\.product\.cpu\.abilist64=//p' "$IMAGES/system/build.prop" 2>/dev/null | tail -1)"
if [ -n "$ABI" ]; then
    if [ "$ABI" = "$GUEST_ABI_HINT" ] || { [ "$HOST_MACH" = "arm64" ] && [ "$ABI" = "arm64-v8a" ]; }; then
        printf '  %-16s %s\n' "guest ABI" "$ABI（与本机匹配 ✓）"
    else
        printf '  %-16s \033[1;31m%s\033[0m\n' "guest ABI" "$ABI（与本机 $HOST_MACH **不匹配** —— 这份 ROM 跑不了）"
    fi
    printf '  %-16s %s\n' "abilist64" "$ABILIST64"
fi
if [ -s "$IMAGES/MANIFEST.txt" ]; then
    grep -E '^(ro\.system\.build\.fingerprint|system\.img sha256)\s*=' "$IMAGES/MANIFEST.txt" 2>/dev/null | sed 's/^/  /' || true
else
    printf '  （images/MANIFEST.txt 不在，无法显示指纹）\n'
fi
if [ -s "$ROOT/RELEASE.json" ]; then
    python3 - "$ROOT/RELEASE.json" <<'PY' 2>/dev/null || true
import json,sys
d=json.load(open(sys.argv[1]))
r=d.get("runtime",{})
print("  发布版本        %s / %s" % (d.get("version"), d.get("platform")))
print("  运行时          %s（%s build %s）" % (r.get("package"), r.get("version"), r.get("buildId")))
print("  无头后端        %s" % r.get("backend"))
PY
fi

# ---------------------------------------------------------------------------
hr "本机模板（templates/config.ini）"
printf '  %-24s %s\n' "hw.lcd.width/height" "$(config_get hw.lcd.width ?)/$(config_get hw.lcd.height ?)"
printf '  %-24s %s\n' "hw.lcd.density" "$(config_get hw.lcd.density ?)"
printf '  %-24s %s\n' "hw.ramSize / ncore" "$(config_get hw.ramSize ?) MB / $(config_get hw.cpu.ncore ?)"
printf '  %-24s %s\n' "disk.dataPartition" "$(config_get disk.dataPartition.size ?)"
printf '  %-24s %s\n' "hw.gpu.mode" "$(config_get hw.gpu.mode auto)（自适应 → $(resolve_gpu_mode "$(config_get hw.gpu.mode auto)")）"
