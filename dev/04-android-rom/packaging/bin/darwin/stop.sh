#!/usr/bin/env bash
# 停掉这台虚拟机（**macOS 版**）。
#
#   ./bin/stop.sh                 # 停 default（端口 5580）
#   ./bin/stop.sh --port 5584
#   ./bin/stop.sh --force         # 60 秒还没退就 SIGKILL
#
# ⚠️ 为什么不能只 kill 进程：`adb emu kill` 是**硬断电**，不是关机。
#    实测（上游项目 tools/verify-kill-is-hard-poweroff.sh，同一台实例三组对照）：
#      写入后 sync 再关 → 数据在
#      写入后不 sync 直接关 → **数据没了**
#      写入后等 15 秒再关 → **还是没了**
#    所以这里一定先 `adb shell sync`。这条别"优化"掉。
#
# macOS 差异：进程枚举走 ps（见 lib.sh），其余逻辑与 linux 版逐条一致。
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
require_adb

PID="$(emu_pid_for_port "$PORT")"
if [ -z "$PID" ]; then
    warn "端口 $PORT 上没有模拟器在跑（实例 '$NAME'）"
    exit 0
fi

log "让 guest 把脏页落盘（adb shell sync）"
"$ADB" -s "$SERIAL" shell sync >/dev/null 2>&1 || warn "sync 没成功（设备可能已经挂了）"
sleep 1

log "请 guest 自己关机（adb emu kill）"
"$ADB" -s "$SERIAL" emu kill >/dev/null 2>&1 || warn "emu kill 没送到（进程可能已经退出）"

# ⚠️ 等的是**进程真的退出**，不是"命令返回了"：emu kill 只是递个关机请求，
#    guest 还要走完流程（卸载 /data、收 qcow2）。这中间就重启会撞上
#    multiinstance.lock，第二台报 another emulator instance is currently running。
for _ in $(seq 1 $((TIMEOUT / 2))); do
    [ -z "$(emu_pid_for_port "$PORT")" ] && { ok "已停止：$SERIAL"; exit 0; }
    sleep 2
done

warn "等了 ${TIMEOUT} 秒还没退出（PID $PID）—— 数据已经 sync 过，可以强杀"
if [ "$FORCE" = 1 ]; then
    kill -9 "$PID" 2>/dev/null || true
    sleep 2
    [ -z "$(emu_pid_for_port "$PORT")" ] && { ok "已强杀：$SERIAL"; exit 0; }
    die "强杀后进程还在：$PID"
fi
die "没停掉。重试： ./bin/stop.sh --port $PORT --force"
