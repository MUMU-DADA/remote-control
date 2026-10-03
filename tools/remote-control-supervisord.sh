#!/system/bin/sh
# =============================================================================
# remote-control-supervisord —— 按 /sdcard/remote-control.conf 管理 remote-control 的启停
#
# 为什么需要它：
#
#   上位应用是普通 Android 应用，**没有 root**，起不了 root 守护进程。
#   但它能读写共享存储。所以让它写配置文件，这个常驻脚本监视文件、
#   负责真正的 proc 管理。应用侧因此只依赖"文件能写"这一件事，
#   不需要任何特权。
#
# 它做的事：
#   1. 每 INTERVAL 秒读一次配置
#   2. **进程始终保活**，bind/port/auth/token 变了才重启
#   3. 把实际状态写进 /sdcard/remote-control.status，供上位应用显示
#
# ⚠️ enabled=0 **不再停进程**。
#
#   它现在的含义是"不对外提供服务能力"，由守护进程自己实现（软开关：
#   HTTP 一律回 503，只放行"重新开启"这一条）。进程停掉的话就没人能
#   把它开回来了 —— 网页打不开、接口不通，只能跑到机器跟前。
#   supervisor 的职责因此收敛成两个字：保活。
#
# 用法（需要 root；真实设备上建议做成 init 服务，见 remote-control.rc）：
#   nohup /data/local/tmp/remote-control-supervisord.sh >/dev/null 2>&1 &
# =============================================================================

CONF=${REMOTE_CONTROL_CONFIG:-/sdcard/remote-control.conf}
STATUS=/sdcard/remote-control.status
BIN=${REMOTE_CONTROL_BIN:-/data/local/tmp/remote-control}
SOCK=${REMOTE_CONTROL_SOCK:-/data/local/tmp/remote-control.sock}
LOG=/data/local/tmp/remote-control-run.log
INTERVAL=2

# 当前生效的值，用来判断"要不要重启"
cur_bind=""
cur_port=""
cur_pid=""
cur_fp=""      # 影响服务行为的配置指纹（bind/port/auth/token）

log() { echo "[supervisord] $*"; }

read_conf() {
    # 输出 "enabled bind port auth token"
    #
    # ⚠️ auth/token 也要读出来参与"要不要重启"的判断。
    #    早先只看 bind/port，结果在上位应用里打开鉴权开关之后
    #    配置文件变了、服务却没重启，令牌也就没生成 ——
    #    用户看到的是"开关拨过去了但什么都没发生"。
    E=1; B=127.0.0.1; P=8088; A=0; T=""
    [ -f "$CONF" ] || { echo "1 127.0.0.1 8088 0 "; return; }
    while IFS='=' read -r k v; do
        # 去掉注释与空白
        k=$(echo "$k" | tr -d ' \t\r')
        v=$(echo "$v" | sed 's/#.*//' | tr -d ' \t\r')
        case "$k" in
            enabled) [ "$v" = "0" ] && E=0 || E=1 ;;
            bind)    [ -n "$v" ] && B="$v" ;;
            port)    case "$v" in ''|*[!0-9]*) ;; *) P="$v" ;; esac ;;
            auth)    [ "$v" = "1" ] && A=1 || A=0 ;;
            token)   T="$v" ;;
        esac
    done < "$CONF"
    echo "$E $B $P $A $T"
}

write_status() {
    # 上位应用读这个文件显示状态。比去探端口可靠 ——
    # 端口可能被防火墙/转发规则挡住，而"进程在不在"是确定的。
    {
        echo "running=$1"
        echo "pid=${2:-0}"
        echo "bind=$3"
        echo "port=$4"
        echo "since=$(date +%s 2>/dev/null || echo 0)"
    } > "$STATUS.tmp" 2>/dev/null && mv "$STATUS.tmp" "$STATUS" 2>/dev/null
}

stop_remote-control() {
    if [ -n "$cur_pid" ] && kill -0 "$cur_pid" 2>/dev/null; then
        log "停止 remote-control (pid $cur_pid)"
        kill "$cur_pid" 2>/dev/null
        # 给它 3 秒优雅退出（它会关掉 uinput 设备、清理 socket 文件）
        i=0
        while [ $i -lt 15 ] && kill -0 "$cur_pid" 2>/dev/null; do
            sleep 0.2; i=$((i+1))
        done
        kill -0 "$cur_pid" 2>/dev/null && kill -9 "$cur_pid" 2>/dev/null
    fi
    # 兜底：pid 文件丢了但进程还在（比如换了 supervisor）
    pkill -f "$BIN --socket" 2>/dev/null
    rm -f "$SOCK"
    cur_pid=""
}

start_remote-control() {
    stop_remote-control
    log "启动 remote-control (bind=$1 port=$2)"
    # **不传 --http-bind/--http-port** —— 让守护进程自己读配置文件。
    # 传了的话 CLI 优先级更高，配置文件里改端口就不会生效了，
    # 而那正是上位机要控制的东西。
    #
    # ⚠️ 但 **--config 必须显式传**。
    #    守护进程没给 --config 时会落到 ConfigFile::DefaultPath()，而那个默认
    #    指向产品形态的 /data/misc/remote-control/remote-control.conf ——
    #    与本脚本监视的 $CONF 不是同一个文件，配置改了也不会生效。
    #    --config 只指定**路径**、不提供**值**，所以上面那条"不传 --http-*"
    #    的设计不受影响：bind/port 依旧由文件决定。
    nohup "$BIN" --socket "$SOCK" --socket-mode 0666 --foreground \
        --config "$CONF" \
        > "$LOG" 2>&1 &
    cur_pid=$!
    cur_bind=$1
    cur_port=$2
    sleep 1
    if kill -0 "$cur_pid" 2>/dev/null; then
        log "已启动 pid $cur_pid"
    else
        log "启动失败，见 $LOG"
        cur_pid=""
    fi
}

log "启动，监视 $CONF（每 ${INTERVAL}s）"

while true; do
    set -- $(read_conf)
    E=$1; B=$2; P=$3; A=$4; T=$5

    # 指纹只包含**影响服务行为**的字段。把注释和格式也算进去的话，
    # 改一行注释就会重启服务，反而让人不敢编辑配置。
    # 指纹**不含 enabled**：拨对外开关不该重启服务，
    # 那会打断所有正在看的画面流和触控连接。
    fp="$B|$P|$A|$T"

    alive=0
    [ -n "$cur_pid" ] && kill -0 "$cur_pid" 2>/dev/null && alive=1

    # enabled 只影响写入 status 的显示（告诉上位应用"对外开着还是关着"），
    # 不影响进程本身 —— 进程必须一直活着，软开关才有入口。
    #
    # 注意 fp 里**不含 enabled**：拨开关不该重启服务，
    # 那会打断所有正在看的画面流和触控连接。
    if [ "$alive" = "0" ] || [ "$fp" != "$cur_fp" ]; then
        start_remote-control "$B" "$P"
        cur_fp="$fp"
    fi
    if [ -n "$cur_pid" ]; then
        write_status 1 "$cur_pid" "$cur_bind" "$cur_port"
        # 把软开关状态也写进去（上位应用要显示它）
        {
            echo "serving=$E"
        } >> "$STATUS" 2>/dev/null
    else
        write_status 0 0 "$B" "$P"
    fi

    sleep "$INTERVAL"
done
