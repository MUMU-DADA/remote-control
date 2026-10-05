#!/usr/bin/env bash
# Unified lifecycle entry point for a packaged emulator.
set -eo pipefail

BIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$BIN_DIR/lib.sh"

usage() {
    cat <<'EOF'
用法：./bin/emulator.sh <命令> [实例名] [选项]

命令：
  create [名称]       创建实例，不启动
  start [名称]        启动；已登记实例默认保留数据
  stop [名称]         正常关机；--force 允许强制停止
  kill [名称]         强制停止
  restart [名称]      正常关机后保留数据重启
  status [名称]       查看实例；list 查看全部实例
  verify [名称]       检查正在运行的实例
  reset [名称]        清空实例数据与快照，保留实例和 ROM
  clone <源> <新名称> 复制实例数据与应用，不复制快照
  delete [名称]       停止并删除实例及其数据
  inspect <归档>      只读查看实例归档清单与内容
  help                显示本页

常用选项：--name NAME、--port PORT、--timeout SEC、--yes、--force
启动选项：--gpu MODE、--memory MB、--cores N、--accel MODE、--gui、--no-wait
示例：
  ./bin/emulator.sh start --port 5582 --no-wait
  ./bin/emulator.sh create test --port 5584
  ./bin/emulator.sh clone test copy --port 5586
  ./bin/emulator.sh delete copy --yes
EOF
}

die_usage() { die "$1（运行 ./bin/emulator.sh help 查看用法）"; }
valid_name() {
    case "$1" in
        ''|.*|*[!A-Za-z0-9._-]*) die "实例名只能以字母或数字开头，并由字母、数字、点、下划线或连字符组成：$1" ;;
    esac
}
validate_port() {
    local value="$1" number
    case "$value" in ''|*[!0-9]*) die "端口必须是偶数数字：$value" ;; esac
    [ "${#value}" -le 5 ] || die "端口超出范围：$value"
    number=$((10#$value))
    [ "$number" -ge 5554 ] && [ "$number" -le 65534 ] && [ $((number % 2)) -eq 0 ] \
        || die "模拟器 console 端口必须是 5554..65534 的偶数：$value"
    printf '%s' "$number"
}
port_owner() {
    local owners
    owners="$(instance_names_for_port "$1")"
    case "$owners" in
        *$'\n'*) die "端口 $1 被多个实例登记，先修复 .run/instances 里的重复登记" ;;
    esac
    printf '%s' "$owners"
}
path_exists() { [ -e "$1" ] || [ -L "$1" ]; }
assert_port_paths_clear() {
    local port="$1" name="$2" path
    for path in "$(sysdir_for_port "$port")" "$(datadir_for_port "$port")" "$(logfile_for_port "$port")"; do
        if path_exists "$path"; then
            die "端口 $port 的路径已存在但不属于新实例 '$name'：$path；拒绝覆盖"
        fi
    done
}
assert_new_instance_clear() {
    local name="$1"
    instance_exists "$name" && die "实例 '$name' 已经存在"
    if path_exists "$(service_token_file "$name")"; then
        die "实例 '$name' 有遗留令牌文件；先检查后再处理：$(service_token_file "$name")"
    fi
}
ensure_free_port() {
    local port="$1"
    if port_taken "$port"; then die "端口 $port 已登记给其他实例"; fi
    if port_listening "$port"; then die "端口 $port 已被模拟器进程占用"; fi
}
stop_instance() {
    local name="$1" port="$2" timeout="$3"
    [ -n "$(emu_pid_for_port "$port")" ] || return 0
    if ! bash "$BIN_DIR/stop.sh" --name "$name" --port "$port" --timeout "$timeout"; then
        warn "实例 '$name' 未能正常关机，改用强制停止"
        bash "$BIN_DIR/stop.sh" --name "$name" --port "$port" --timeout "$timeout" --force
    fi
    [ -z "$(emu_pid_for_port "$port")" ] || die "实例 '$name' 仍在运行，保留其数据目录"
}
copy_instance_service_metadata() {
    local source="$1" destination="$2" key value from_token to_token
    for key in SERVICE_PORT SERVICE_ENABLED SERVICE_AUTH SERVICE_BIND SERVICE_ADB; do
        value="$(instance_service_value "$source" "$key")"
        [ -z "$value" ] || printf '%s=%s\n' "$key" "$value" >> "$(instance_file "$destination")"
    done
    from_token="$(service_token_file "$source")"
    to_token="$(service_token_file "$destination")"
    if [ -s "$from_token" ]; then
        [ ! -L "$from_token" ] || die "源实例令牌是符号链接，拒绝复制：$from_token"
        (umask 077; cp "$from_token" "$to_token")
        chmod 600 "$to_token"
    fi
}

inspect_archive() {
    local archive="$1" manifest
    [ -s "$archive" ] || die "归档不存在或为空：$archive"
    command -v tar >/dev/null 2>&1 || die "inspect 需要系统 tar"
    if ! manifest="$(tar -xOf "$archive" ./INSTANCE-MANIFEST.json 2>/dev/null)"; then
        manifest="$(tar -xOf "$archive" INSTANCE-MANIFEST.json 2>/dev/null || true)"
    fi
    [ -n "$manifest" ] || die "归档中没有 INSTANCE-MANIFEST.json：$archive"
    log "实例归档：$archive"
    printf '%s\n' "$manifest" | sed 's/^/    /'
    log "归档内容（前 12 项）"
    tar -tf "$archive" | sed -n '1,12{s/^/    /;p;}'
}

command_name="${1:-help}"
[ "$#" -eq 0 ] || shift
case "$command_name" in
    create|start|stop|kill|restart|status|list|ls|verify|reset|delete|clone|inspect) ;;
    help|-h|--help) usage; exit 0 ;;
    *) die_usage "未知命令：$command_name" ;;
esac

NAME_OPT=""; NAME_SEEN=0; PORT_OPT=""; PORT_SEEN=0; TIMEOUT=""
GPU=""; MEMORY=""; CORES=""; ACCEL=""; PRODUCT=""
FORCE=0; YES=0; REUSE=0; WIPE=0; GUI=0; NO_WAIT=0; TEST_INSTANCE=0
BRIDGE=0; NAT=0; VERIFY_HOST=0; VERIFY_DEVICE=0; HELP=0
POS_COUNT=0; POS1=""; POS2=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --name|-n)
            [ "$#" -ge 2 ] || die "$1 后需要实例名"
            [ "$NAME_SEEN" = 0 ] || die "实例名只能指定一次"
            NAME_OPT="$2"; NAME_SEEN=1; shift 2; continue ;;
        --name=*)
            [ "$NAME_SEEN" = 0 ] || die "实例名只能指定一次"
            NAME_OPT="${1#*=}"; NAME_SEEN=1; [ -n "$NAME_OPT" ] || die "--name 后需要实例名"; shift; continue ;;
        --port)
            [ "$#" -ge 2 ] || die "--port 后需要端口"
            [ "$PORT_SEEN" = 0 ] || die "端口只能指定一次"
            PORT_OPT="$2"; PORT_SEEN=1; shift 2; continue ;;
        --port=*)
            [ "$PORT_SEEN" = 0 ] || die "端口只能指定一次"
            PORT_OPT="${1#*=}"; PORT_SEEN=1; [ -n "$PORT_OPT" ] || die "--port 后需要端口"; shift; continue ;;
        --timeout)
            [ "$#" -ge 2 ] || die "--timeout 后需要秒数"
            [ -z "$TIMEOUT" ] || die "--timeout 只能指定一次"
            TIMEOUT="$2"; shift 2; continue ;;
        --timeout=*)
            [ -z "$TIMEOUT" ] || die "--timeout 只能指定一次"
            TIMEOUT="${1#*=}"; [ -n "$TIMEOUT" ] || die "--timeout 后需要秒数"; shift; continue ;;
        --gpu|--memory|--cores|--accel|--product)
            [ "$#" -ge 2 ] || die "$1 后需要参数值"
            case "$1" in
                --gpu) [ -z "$GPU" ] || die "--gpu 只能指定一次"; GPU="$2" ;;
                --memory) [ -z "$MEMORY" ] || die "--memory 只能指定一次"; MEMORY="$2" ;;
                --cores) [ -z "$CORES" ] || die "--cores 只能指定一次"; CORES="$2" ;;
                --accel) [ -z "$ACCEL" ] || die "--accel 只能指定一次"; ACCEL="$2" ;;
                --product) [ -z "$PRODUCT" ] || die "--product 只能指定一次"; PRODUCT="$2" ;;
            esac
            shift 2; continue ;;
        --gpu=*|--memory=*|--cores=*|--accel=*|--product=*)
            option="${1%%=*}"; value="${1#*=}"
            [ -n "$value" ] || die "$option 后需要参数值"
            case "$option" in
                --gpu) [ -z "$GPU" ] || die "--gpu 只能指定一次"; GPU="$value" ;;
                --memory) [ -z "$MEMORY" ] || die "--memory 只能指定一次"; MEMORY="$value" ;;
                --cores) [ -z "$CORES" ] || die "--cores 只能指定一次"; CORES="$value" ;;
                --accel) [ -z "$ACCEL" ] || die "--accel 只能指定一次"; ACCEL="$value" ;;
                --product) [ -z "$PRODUCT" ] || die "--product 只能指定一次"; PRODUCT="$value" ;;
            esac
            shift; continue ;;
        --force|-f) FORCE=1 ;;
        --yes|-y) YES=1 ;;
        --reuse) REUSE=1 ;;
        --wipe-data) WIPE=1 ;;
        --gui) GUI=1 ;;
        --no-wait) NO_WAIT=1 ;;
        --test-instance) TEST_INSTANCE=1 ;;
        --bridge) BRIDGE=1 ;;
        --nat) NAT=1 ;;
        --host) VERIFY_HOST=1 ;;
        --device) VERIFY_DEVICE=1 ;;
        -h|--help) HELP=1 ;;
        -*) die_usage "未知选项：$1" ;;
        *)
            POS_COUNT=$((POS_COUNT + 1))
            case "$POS_COUNT" in 1) POS1="$1" ;; 2) POS2="$1" ;; *) die_usage "参数过多" ;; esac ;;
    esac
    shift
done

if [ "$HELP" = 1 ]; then
    case "$command_name" in
        start) exec bash "$BIN_DIR/start-headless.sh" --help ;;
        stop|kill) exec bash "$BIN_DIR/stop.sh" --help ;;
        delete) usage; exit 0 ;;
        status|list|ls) exec bash "$BIN_DIR/status.sh" --help ;;
        verify) exec bash "$BIN_DIR/verify.sh" --help ;;
        reset) exec bash "$BIN_DIR/reset.sh" --help ;;
        create|clone|inspect) usage; exit 0 ;;
        restart) exec bash "$BIN_DIR/start-headless.sh" --help ;;
    esac
fi

case "$command_name" in ls) command_name=list ;; esac
if [ -n "$TIMEOUT" ]; then
    case "$TIMEOUT" in ''|*[!0-9]*) die "--timeout 必须是正整数：$TIMEOUT" ;; esac
    [ "${#TIMEOUT}" -le 6 ] && [ "$TIMEOUT" -gt 0 ] || die "--timeout 必须是正整数：$TIMEOUT"
fi
if [ "$PORT_SEEN" = 1 ]; then PORT_OPT="$(validate_port "$PORT_OPT")"; fi

if [ "$command_name" = inspect ]; then
    [ "$POS_COUNT" -eq 1 ] || die_usage "inspect 需要一个归档路径"
    [ "$NAME_SEEN$PORT_SEEN$FORCE$YES$REUSE$WIPE$GUI$NO_WAIT$TEST_INSTANCE$BRIDGE$NAT$VERIFY_HOST$VERIFY_DEVICE" = 0000000000000 ] \
        && [ -z "$TIMEOUT$GPU$MEMORY$CORES$ACCEL$PRODUCT" ] \
        || die_usage "inspect 只接受归档路径"
    inspect_archive "$POS1"
    exit 0
fi

case "$command_name" in
    create|delete|start|stop|kill|restart|status|verify|reset)
        [ "$POS_COUNT" -le 1 ] || die_usage "$command_name 只接受一个实例名"
        if [ "$NAME_SEEN" = 1 ] && [ "$POS_COUNT" -gt 0 ]; then die "不能同时使用位置实例名和 --name"; fi
        NAME="$DEFAULT_NAME"
        [ "$NAME_SEEN" = 0 ] || NAME="$NAME_OPT"
        [ "$POS_COUNT" -eq 0 ] || NAME="$POS1"
        valid_name "$NAME" ;;
    clone)
        [ "$NAME_SEEN" = 0 ] || die "clone 使用位置参数：clone <源实例> <新实例>"
        [ "$POS_COUNT" -eq 2 ] || die_usage "clone 需要源实例名和新实例名"
        SRC="$POS1"; NAME="$POS2"
        valid_name "$SRC"; valid_name "$NAME" ;;
    list)
        [ "$POS_COUNT" -eq 0 ] || die_usage "list 不接受实例名" ;;
esac
if [ "$command_name" = list ]; then
    [ "$NAME_SEEN$PORT_SEEN$TIMEOUT" = 00 ] || die_usage "list 不接受实例名、端口或超时选项"
fi
case "$command_name" in start|stop|kill|restart|reset|delete|clone) ;; *)
    [ -z "$TIMEOUT" ] || die_usage "--timeout 不适用于 $command_name" ;;
esac

case "$command_name" in start|restart) ;; *)
    [ -z "$GPU$MEMORY$CORES$ACCEL" ] || die_usage "--gpu、--memory、--cores 和 --accel 只用于 start/restart" ;;
esac
case "$command_name" in start|restart) ;; *)
    [ "$GUI$NO_WAIT$TEST_INSTANCE$BRIDGE$NAT$REUSE$WIPE" = 0000000 ] || die_usage "此选项只用于 start/restart" ;;
esac
case "$command_name" in stop|kill|restart|reset|delete) ;; *)
    [ "$FORCE" = 0 ] || die_usage "--force 只用于 stop/kill/restart/reset/delete" ;;
esac
case "$command_name" in reset|delete) ;; *)
    [ "$YES" = 0 ] || die_usage "--yes 只用于 reset/delete" ;;
esac
case "$command_name" in verify) ;; *)
    [ "$VERIFY_HOST$VERIFY_DEVICE$PRODUCT" = 00 ] || die_usage "--host、--device 和 --product 只用于 verify" ;;
esac
if { [ "$VERIFY_HOST$VERIFY_DEVICE" != 00 ] || [ -n "$PRODUCT" ]; } && [ "$(uname -s)" != Darwin ]; then
    die "--host、--device 和 --product 只适用于 macOS 的 verify.sh"
fi

# A port identifies its registered instance. Resolve it before constructing helper
# calls so `start --port 5582` reuses the instance actually registered on 5582.
case "$command_name" in start|stop|kill|restart|status|verify|reset|delete)
    if [ "$PORT_SEEN" = 1 ] && [ "$NAME_SEEN" = 0 ] && [ "$POS_COUNT" -eq 0 ]; then
        owner="$(port_owner "$PORT_OPT")"
        [ -z "$owner" ] || NAME="$owner"
    fi ;;
esac
case "$command_name" in start|stop|kill|restart|status|verify|reset|delete)
    if [ "$PORT_SEEN" = 1 ] && { [ "$NAME_SEEN" = 1 ] || [ "$POS_COUNT" -gt 0 ]; }; then
        owner="$(port_owner "$PORT_OPT")"
        [ -z "$owner" ] || [ "$owner" = "$NAME" ] || die "端口 $PORT_OPT 属于实例 '$owner'，不是 '$NAME'"
        registered="$(instance_port "$NAME")"
        [ -z "$registered" ] || [ "$registered" = "$PORT_OPT" ] || die "实例 '$NAME' 登记的端口是 $registered，不是 $PORT_OPT"
    fi ;;
esac

case "$command_name" in stop|kill|restart|reset|delete)
    if [ "$PORT_SEEN" = 1 ]; then
        owner="$(port_owner "$PORT_OPT")"
        [ -n "$owner" ] && [ "$owner" = "$NAME" ] \
            || die "端口 $PORT_OPT 没有唯一登记给实例 '$NAME'；拒绝操作"
    fi ;;
esac
case "$command_name" in start|stop|kill|restart|reset|delete)
    if instance_exists "$NAME"; then
        registered="$(instance_port "$NAME")"
        [ -n "$registered" ] || die "实例 '$NAME' 没有有效端口登记"
        owner="$(port_owner "$registered")"
        [ "$owner" = "$NAME" ] || die "实例 '$NAME' 的端口 $registered 没有唯一登记；拒绝操作"
    fi ;;
esac

BASE_ARGS=(--name "$NAME")
[ "$PORT_SEEN" = 0 ] || BASE_ARGS+=(--port "$PORT_OPT")
[ -z "$TIMEOUT" ] || BASE_ARGS+=(--timeout "$TIMEOUT")

case "$command_name" in
    create)
        assert_new_instance_clear "$NAME"
        if [ "$PORT_SEEN" = 1 ]; then PORT="$PORT_OPT"; else PORT="$(alloc_port)"; fi
        validate_port "$PORT" >/dev/null
        ensure_free_port "$PORT"
        assert_port_paths_clear "$PORT" "$NAME"
        require_images
        mkdir -p "$RUN_DIR" "$INSTANCES_DIR"
        build_sysdir "$PORT" fresh
        mkdir -p "$(datadir_for_port "$PORT")"
        instance_register "$NAME" "$PORT"
        ok "已创建实例 '$NAME'（端口 $PORT；尚未启动）"
        printf '    启动：./bin/emulator.sh start %s\n' "$NAME"
        ;;
    start)
        [ -z "$GPU" ] || BASE_ARGS+=(--gpu "$GPU")
        [ -z "$MEMORY" ] || BASE_ARGS+=(--memory "$MEMORY")
        [ -z "$CORES" ] || BASE_ARGS+=(--cores "$CORES")
        [ -z "$ACCEL" ] || BASE_ARGS+=(--accel "$ACCEL")
        [ "$GUI" = 0 ] || BASE_ARGS+=(--gui)
        [ "$NO_WAIT" = 0 ] || BASE_ARGS+=(--no-wait)
        [ "$TEST_INSTANCE" = 0 ] || BASE_ARGS+=(--test-instance)
        [ "$BRIDGE" = 0 ] || BASE_ARGS+=(--bridge)
        [ "$NAT" = 0 ] || BASE_ARGS+=(--nat)
        if instance_exists "$NAME"; then
            [ "$WIPE" = 1 ] || BASE_ARGS+=(--reuse)
        elif [ "$REUSE" = 1 ]; then
            BASE_ARGS+=(--reuse)
        fi
        [ "$WIPE" = 0 ] || BASE_ARGS+=(--wipe-data)
        exec bash "$BIN_DIR/start-headless.sh" "${BASE_ARGS[@]}" ;;
    stop)
        [ "$FORCE" = 0 ] || BASE_ARGS+=(--force)
        exec bash "$BIN_DIR/stop.sh" "${BASE_ARGS[@]}" ;;
    kill)
        warn "强制停止可能丢失未保存的数据"
        BASE_ARGS+=(--force)
        exec bash "$BIN_DIR/stop.sh" "${BASE_ARGS[@]}" ;;
    restart)
        port="$(instance_port "$NAME")"
        [ -n "$port" ] || port="${PORT_OPT:-}"
        [ -n "$port" ] || die "实例 '$NAME' 没登记过端口"
        port="$(validate_port "$port")"
        bash "$BIN_DIR/stop.sh" --name "$NAME" --port "$port" --timeout "${TIMEOUT:-60}" --force
        [ -z "$(emu_pid_for_port "$port")" ] || die "实例 '$NAME' 仍在运行，停止后再重启"
        [ "$WIPE" = 1 ] || REUSE=1
        [ "$REUSE" = 0 ] || BASE_ARGS+=(--reuse)
        [ "$WIPE" = 0 ] || BASE_ARGS+=(--wipe-data)
        [ "$GUI" = 0 ] || BASE_ARGS+=(--gui)
        [ "$NO_WAIT" = 0 ] || BASE_ARGS+=(--no-wait)
        [ "$TEST_INSTANCE" = 0 ] || BASE_ARGS+=(--test-instance)
        [ "$BRIDGE" = 0 ] || BASE_ARGS+=(--bridge)
        [ "$NAT" = 0 ] || BASE_ARGS+=(--nat)
        [ -z "$GPU" ] || BASE_ARGS+=(--gpu "$GPU")
        [ -z "$MEMORY" ] || BASE_ARGS+=(--memory "$MEMORY")
        [ -z "$CORES" ] || BASE_ARGS+=(--cores "$CORES")
        [ -z "$ACCEL" ] || BASE_ARGS+=(--accel "$ACCEL")
        exec bash "$BIN_DIR/start-headless.sh" "${BASE_ARGS[@]}" ;;
    status)
        exec bash "$BIN_DIR/status.sh" "${BASE_ARGS[@]}" ;;
    list)
        exec bash "$BIN_DIR/status.sh" ;;
    verify)
        verify_args=()
        if [ "$PORT_SEEN" = 1 ]; then verify_args+=(--port "$PORT_OPT")
        elif [ "$NAME_SEEN" = 1 ] || [ "$POS_COUNT" -gt 0 ] || instance_exists "$NAME"; then
            port="$(instance_port "$NAME")"
            [ -z "$port" ] || verify_args+=(--port "$port")
        fi
        [ "$VERIFY_HOST" = 0 ] || verify_args+=(--host)
        [ "$VERIFY_DEVICE" = 0 ] || verify_args+=(--device)
        [ -z "$PRODUCT" ] || verify_args+=(--product "$PRODUCT")
        exec bash "$BIN_DIR/verify.sh" "${verify_args[@]}" ;;
    reset)
        reset_args=()
        [ "$NAME_SEEN" = 1 ] || [ "$POS_COUNT" -gt 0 ] || [ "$PORT_SEEN" = 1 ] || NAME=""
        [ -z "$NAME" ] || reset_args+=(--name "$NAME")
        [ "$PORT_SEEN" = 0 ] || reset_args+=(--port "$PORT_OPT")
        [ -z "$TIMEOUT" ] || reset_args+=(--timeout "$TIMEOUT")
        [ "$YES" = 0 ] || reset_args+=(--yes)
        [ "$FORCE" = 0 ] || reset_args+=(--force)
        exec bash "$BIN_DIR/reset.sh" "${reset_args[@]}" ;;
    delete)
        if [ "$PORT_SEEN" = 1 ]; then
            owner="$(port_owner "$PORT_OPT")"
            [ -n "$owner" ] || die "端口 $PORT_OPT 没有登记实例；拒绝删除未登记数据"
            [ "$owner" = "$NAME" ] || die "端口 $PORT_OPT 属于实例 '$owner'，不是 '$NAME'"
        fi
        instance_exists "$NAME" || die "实例 '$NAME' 没有登记过端口"
        PORT="$(instance_port "$NAME")"
        PORT="$(validate_port "$PORT")"
        [ "$PORT_SEEN" = 0 ] || [ "$PORT_OPT" = "$PORT" ] || die "实例 '$NAME' 的端口是 $PORT，不是 $PORT_OPT"
        [ "$(port_owner "$PORT")" = "$NAME" ] || die "实例 '$NAME' 的端口登记不唯一；拒绝删除"
        stop_instance "$NAME" "$PORT" "${TIMEOUT:-60}"
        if [ "$YES" = 0 ]; then
            printf '[!] 删除 %s 会移除已安装应用、用户数据和快照。\n    输入 yes 继续：' "$NAME"
            read -r answer
            [ "$answer" = yes ] || die "已取消"
        fi
        rm -rf "$(sysdir_for_port "$PORT")" "$(datadir_for_port "$PORT")"
        rm -f "$(logfile_for_port "$PORT")" "$(service_token_file "$NAME")"
        instance_unregister "$NAME"
        ok "已删除实例 '$NAME'（端口 $PORT）"
        ;;
    clone)
        instance_exists "$SRC" || die "没有叫 '$SRC' 的实例（先用 list 查看实例）"
        assert_new_instance_clear "$NAME"
        SOURCE_PORT="$(validate_port "$(instance_port "$SRC")")"
        if [ "$PORT_SEEN" = 1 ]; then PORT="$PORT_OPT"; else PORT="$(alloc_port)"; fi
        validate_port "$PORT" >/dev/null
        ensure_free_port "$PORT"
        assert_port_paths_clear "$PORT" "$NAME"
        SOURCE_SYSDIR="$(sysdir_for_port "$SOURCE_PORT")"
        SOURCE_DATADIR="$(datadir_for_port "$SOURCE_PORT")"
        [ ! -L "$SOURCE_SYSDIR" ] && [ -d "$SOURCE_SYSDIR" ] \
            || die "源实例工作目录缺失或是符号链接：$SOURCE_SYSDIR"
        [ -e "$SOURCE_SYSDIR/system-qemu.img" ] && [ -e "$SOURCE_SYSDIR/config.ini" ] \
            || die "源实例工作目录不完整：$SOURCE_SYSDIR"
        [ ! -L "$SOURCE_DATADIR" ] || die "源实例数据目录是符号链接，拒绝复制：$SOURCE_DATADIR"
        stop_instance "$SRC" "$SOURCE_PORT" "${TIMEOUT:-60}"
        mkdir -p "$RUN_DIR" "$INSTANCES_DIR"
        DEST_SYSDIR="$(sysdir_for_port "$PORT")"
        DEST_DATADIR="$(datadir_for_port "$PORT")"
        mkdir -p "$DEST_SYSDIR"
        cp -a "$SOURCE_SYSDIR"/. "$DEST_SYSDIR"/
        mkdir -p "$DEST_DATADIR"
        if [ -d "$SOURCE_DATADIR" ]; then cp -a "$SOURCE_DATADIR"/. "$DEST_DATADIR"/; fi
        for stale in hardware-qemu.ini hardware-qemu.ini.lock multiinstance.lock \
                     emu-launch-params.txt bootcompleted.ini version_num.cache read-snapshot.txt; do
            rm -f "$DEST_SYSDIR/$stale"
        done
        for stale in "$DEST_SYSDIR"/*.img.qcow2.lock; do
            if path_exists "$stale"; then rm -f "$stale"; fi
        done
        if path_exists "$DEST_SYSDIR/snapshots"; then
            rm -rf "$DEST_SYSDIR/snapshots"
            warn "源实例快照没有复制（快照保存了旧硬件路径）"
        fi
        [ -s "$CONFIG_FILE" ] || die "模板缺失：$CONFIG_FILE"
        rm -f "$DEST_SYSDIR/config.ini"
        cp -f "$CONFIG_FILE" "$DEST_SYSDIR/config.ini"
        instance_register "$NAME" "$PORT"
        copy_instance_service_metadata "$SRC" "$NAME"
        ok "已复制 '$SRC' → '$NAME'（端口 $PORT；保留 guest 数据与服务令牌）"
        printf '    启动：./bin/emulator.sh start %s\n' "$NAME"
        ;;
esac
