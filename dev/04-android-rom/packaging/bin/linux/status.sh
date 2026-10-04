#!/usr/bin/env bash
# 看这台机器（或全部实例）现在是什么状态，以及这个包里的 ROM 是哪一份。
#
#   ./bin/status.sh                # 列所有实例
#   ./bin/status.sh --port 5580    # 看一个实例的细节
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

NAME="$DEFAULT_NAME"; PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --name)    NAME="${2:?}"; shift ;;
        --port)    PORT="${2:?}"; shift ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
    shift
done

hr() { printf '\033[1;36m── %s\033[0m\n' "$1"; }

boot_state() {
    service_http_state "$1" 2>/dev/null || true
}

console_state() {
    if console_ready "$1"; then printf '已连接'; else printf '不可用'; fi
}

if [ -n "$PORT" ] || instance_exists "$NAME"; then
    [ -n "$PORT" ] || PORT="$(instance_port "$NAME")"
    [ -n "$PORT" ] || die "实例 '$NAME' 没登记过端口"
    SYSDIR="$(sysdir_for_port "$PORT")"
    PID="$(emu_pid_for_port "$PORT")"
    hr "实例 $NAME（端口 $PORT，序列号 $(serial_for_port "$PORT")）"
    if [ -n "$PID" ]; then
        printf '  %-16s %s\n' "进程" "在跑（PID $PID，已运行 $(ps -o etime= -p "$PID" 2>/dev/null | tr -d ' ' || echo '?'))"
        printf '  %-16s %s\n' "console / 服务" "$(console_state "$PORT") / $(boot_state "$PORT")"
        printf '  %-16s %s\n' "工作目录占用" "$(du -sh "$SYSDIR" 2>/dev/null | cut -f1 || echo '?')"
    else
        printf '  %-16s %s\n' "进程" "没在跑"
    fi
    show_userdata_storage "$SYSDIR" "$(datadir_for_port "$PORT")"
    service_read_instance "$PORT"
    printf '  %-16s %s\n' "服务地址" "http://127.0.0.1:$SERVICE_HTTP_PORT → guest:$SERVICE_GUEST_PORT"
    printf '  %-16s %s\n' "工作目录" "${SYSDIR#"$ROOT"/}"
    printf '  %-16s %s\n' "日志" "${LOGF:-$(logfile_for_port "$PORT")}"
    if [ -s "$SYSDIR/hardware-qemu.ini" ]; then
        printf '  %-16s %s\n' "上次生效的硬件" "$(grep -E '^(hw\.ramSize|hw\.cpu\.ncore|hw\.lcd\.width|hw\.lcd\.height)\s*=' "$SYSDIR/hardware-qemu.ini" 2>/dev/null | paste -sd' ' - | tr -s ' ')"
    fi
    if [ -d "$SYSDIR/snapshots" ]; then
        printf '  %-16s %s\n' "快照" "$(ls "$SYSDIR/snapshots" 2>/dev/null | paste -sd' ' -)"
    fi
    echo
fi

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

hr "这个包里的 ROM"
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
print(f"  发布版本        {d.get('version')} / {d.get('platform')}")
print(f"  运行时          {r.get('package')}（{r.get('version')} build {r.get('buildId')}）")
print(f"  无头后端        {r.get('backend')}")
PY
fi

hr "本机模板（templates/config.ini）"
printf '  %-24s %s\n' "hw.lcd.width/height" "$(config_get hw.lcd.width ?)/$(config_get hw.lcd.height ?)"
printf '  %-24s %s\n' "hw.lcd.density" "$(config_get hw.lcd.density ?)"
printf '  %-24s %s\n' "hw.ramSize / ncore" "$(config_get hw.ramSize ?) MB / $(config_get hw.cpu.ncore ?)"
printf '  %-24s %s\n' "disk.dataPartition" "$(config_get disk.dataPartition.size ?)"
printf '  %-24s %s\n' "hw.gpu.mode" "$(config_get hw.gpu.mode auto)（自适应 → $(resolve_gpu_mode "$(config_get hw.gpu.mode auto)")）"
