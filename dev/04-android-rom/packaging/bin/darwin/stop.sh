#!/usr/bin/env bash
# 停掉这台虚拟机（**macOS 版**）。
#
#   ./bin/stop.sh                 # 停 default（端口 5580）
#   ./bin/stop.sh --port 5584
#   ./bin/stop.sh --force         # 60 秒还没退就 SIGKILL
#
# 常规停机通过 HTTP 请求 Android 正常关机；--force 才允许硬停止。
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

NAME="$DEFAULT_NAME"; PORT=""; FORCE=0; TIMEOUT=60
while [ $# -gt 0 ]; do
    case "$1" in
        --name)    NAME="${2:?}"; shift ;;
        --port)    PORT="${2:?}"; shift ;;
        --timeout) TIMEOUT="${2:?}"; shift ;;
        --force|-f) FORCE=1 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
    shift
done

if [ -z "$PORT" ]; then
    PORT="$(instance_port "$NAME")"
    [ -n "$PORT" ] || die "实例 '$NAME' 没登记过端口（./bin/status.sh 看有哪些）"
fi
SERIAL="$(serial_for_port "$PORT")"

PID="$(emu_pid_for_port "$PORT")"
if [ -z "$PID" ]; then
    warn "端口 $PORT 上没有模拟器在跑（实例 '$NAME'）"
    exit 0
fi

log "通过 HTTP 请求 Android 正常关机（不需要 ADB）"
service_read_instance "$PORT"
if ! service_create_redirect "$PORT" || ! service_request_poweroff "$PORT" "$TIMEOUT"; then
    if [ "$FORCE" != 1 ]; then
        die "无法请求正常关机：HTTP 服务或保存的令牌不可用。修复服务后重试，或使用 --force 硬停止"
    fi
    warn "HTTP 正常关机不可用，--force 将直接终止模拟器；未落盘数据可能丢失"
    console_kill "$PORT" || true
fi

# 等进程真的退出，Android 正常关机需要卸载文件系统并保存覆盖层。
# 在进程退出前重启会撞上
#    multiinstance.lock，第二台报 another emulator instance is currently running。
for _ in $(seq 1 $((TIMEOUT / 2))); do
    [ -z "$(emu_pid_for_port "$PORT")" ] && { ok "已停止：$SERIAL"; exit 0; }
    sleep 2
done

warn "等了 ${TIMEOUT} 秒还没退出（PID $PID）—— 尚未确认正常关机完成"
if [ "$FORCE" = 1 ]; then
    console_kill "$PORT" || true
    kill -9 "$PID" 2>/dev/null || true
    sleep 2
    [ -z "$(emu_pid_for_port "$PORT")" ] && { ok "已强杀：$SERIAL"; exit 0; }
    die "强杀后进程还在：$PID"
fi
die "没停掉。重试： ./bin/stop.sh --port $PORT --force"
