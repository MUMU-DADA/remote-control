#!/usr/bin/env bash
# Local emulator console and HTTP management; no ADB commands.
# Loaded by lib.sh on Linux and macOS (including the system Bash 3.2).

console_read_reply() {
    local line remaining deadline=$((SECONDS + 3))
    CONSOLE_REPLY=""
    while [ "$SECONDS" -lt "$deadline" ]; do
        remaining=$((deadline - SECONDS))
        IFS= read -r -t "$remaining" line <&9 || return 1
        line="${line%$'\r'}"
        CONSOLE_REPLY="${CONSOLE_REPLY:+$CONSOLE_REPLY
}$line"
        case "$line" in OK) return 0 ;; KO:*) return 1 ;; esac
        [ "${#CONSOLE_REPLY}" -lt 65536 ] || return 1
    done
    return 1
}

console_command() ( # console_command <console-port> <command>
    local port="$1" command="$2" token_file token auth=0
    # The subshell closes fd 9 on every failure path.
    exec 9<>"/dev/tcp/127.0.0.1/$port" 2>/dev/null || return 1
    console_read_reply || return 1
    case "$CONSOLE_REPLY" in *"Authentication required"*) auth=1 ;; esac
    if [ "$auth" = 1 ]; then
        token_file="${AUTOSNAP_CONSOLE_TOKEN_FILE:-${HOME:-}/.emulator_console_auth_token}"
        [ -r "$token_file" ] || return 1
        token="$(tr -d '\r\n' < "$token_file")"
        [ -n "$token" ] || return 1
        printf 'auth %s\n' "$token" >&9 || return 1
        console_read_reply || return 1
    fi
    printf '%s\n' "$command" >&9 || return 1
    if console_read_reply; then
        printf '%s\n' "$CONSOLE_REPLY"
        return 0
    fi
    # kill closes the socket after acknowledging the request on some versions.
    [ "$command" = kill ] && case "$CONSOLE_REPLY" in *"killing emulator"*) return 0 ;; esac
    return 1
)

console_ready() { console_command "$1" "avd status" >/dev/null 2>&1; }
console_redir_add() { console_command "$1" "redir add tcp:$2:$3" >/dev/null; }
console_redir_del() { console_command "$1" "redir del tcp:$2" >/dev/null 2>&1 || true; }
console_kill() { console_command "$1" kill >/dev/null 2>&1; }

service_token_file() { printf '%s/%s.token' "$INSTANCES_DIR" "$1"; }

service_load_token() {
    local file
    file="$(service_token_file "$1")"
    SERVICE_TOKEN=""
    [ -s "$file" ] && SERVICE_TOKEN="$(tr -d '\r\n' < "$file")"
    return 0
}

service_prepare_token() { # <name> <test-instance> [reuse]
    local file configured auth
    SERVICE_TOKEN=""
    auth="$(config_get service.auth 1)"
    if [ "${3:-0}" = 1 ]; then
        local existing_auth
        existing_auth="$(instance_service_value "$1" SERVICE_AUTH)"
        [ -z "$existing_auth" ] || auth="$existing_auth"
    fi
    file="$(service_token_file "$1")"
    [ "${2:-0}" = 1 ] && return 0
    [ "$auth" = 1 ] || return 0
    if [ "${3:-0}" = 1 ] && [ -s "$file" ]; then
        service_load_token "$1"
        return 0
    fi
    if [ "${3:-0}" = 1 ] && [ -n "$(instance_service_value "$1" SERVICE_AUTH)" ]; then
        die "复用实例缺少服务令牌文件：$file；恢复该文件后重试"
    fi
    configured="$(config_get service.token '')"
    mkdir -p "$INSTANCES_DIR"
    if [ -n "$configured" ]; then
        SERVICE_TOKEN="$configured"
    else
        service_load_token "$1"
        if [ -z "$SERVICE_TOKEN" ]; then
            SERVICE_TOKEN="$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')"
            [ "${#SERVICE_TOKEN}" = 64 ] || die "无法生成服务访问令牌"
        fi
    fi
    (umask 077; printf '%s\n' "$SERVICE_TOKEN" > "$file")
    chmod 600 "$file"
}

instance_service_value() { # <name> <key>
    local file
    file="$(instance_file "$1")"
    [ -s "$file" ] || return 0
    sed -n "s/^$2=//p" "$file" | tail -1
}
instance_http_port() { instance_service_value "$1" HTTP_PORT; }

service_register() { # <name> <console-port> <test-instance> [reuse]
    local old_host port auth enabled bind adb_enabled
    old_host="$(instance_http_port "$1")"
    SERVICE_GUEST_PORT="$(config_get service.port 8088)"
    SERVICE_ENABLED="$(config_get service.enabled 1)"
    SERVICE_BIND="$(config_get service.bind 0.0.0.0)"
    SERVICE_ADB="$(config_get service.adb_enabled 1)"
    SERVICE_AUTH="$(config_get service.auth 1)"
    [ "$3" = 1 ] && SERVICE_AUTH=0
    if [ "${4:-0}" = 1 ]; then
        # A reused guest keeps its own persisted config and token.
        port="$(instance_service_value "$1" SERVICE_PORT)"
        [ -z "$port" ] || SERVICE_GUEST_PORT="$port"
        auth="$(instance_service_value "$1" SERVICE_AUTH)"
        [ -z "$auth" ] || SERVICE_AUTH="$auth"
        enabled="$(instance_service_value "$1" SERVICE_ENABLED)"
        [ -z "$enabled" ] || SERVICE_ENABLED="$enabled"
        bind="$(instance_service_value "$1" SERVICE_BIND)"
        [ -z "$bind" ] || SERVICE_BIND="$bind"
        adb_enabled="$(instance_service_value "$1" SERVICE_ADB)"
        [ -z "$adb_enabled" ] || SERVICE_ADB="$adb_enabled"
    fi
    SERVICE_HTTP_PORT="${AUTOSNAP_HTTP_PORT:-${old_host:-$((18088 + ($2 - PORT_BASE) / 2))}}"
    case "$SERVICE_HTTP_PORT" in ''|*[!0-9]*) die "HTTP 端口必须是 1-65535 的整数" ;; esac
    [ "$SERVICE_HTTP_PORT" -ge 1 ] && [ "$SERVICE_HTTP_PORT" -le 65535 ] || die "HTTP 端口必须是 1-65535 的整数"
    instance_register "$1" "$2"
    cat >> "$(instance_file "$1")" <<EOF
HTTP_PORT=$SERVICE_HTTP_PORT
SERVICE_PORT=$SERVICE_GUEST_PORT
SERVICE_ENABLED=$SERVICE_ENABLED
SERVICE_AUTH=$SERVICE_AUTH
SERVICE_BIND=$SERVICE_BIND
SERVICE_ADB=$SERVICE_ADB
EOF
}

service_read_instance() { # <console-port>
    local name="" bind adb_enabled
    for name in $(instance_names_for_port "$1"); do break; done
    SERVICE_HTTP_PORT=""
    SERVICE_GUEST_PORT="$(config_get service.port 8088)"
    SERVICE_TOKEN=""
    SERVICE_ENABLED="$(config_get service.enabled 1)"
    SERVICE_BIND="$(config_get service.bind 0.0.0.0)"
    SERVICE_ADB="$(config_get service.adb_enabled 1)"
    if [ -n "$name" ]; then
        SERVICE_HTTP_PORT="$(instance_http_port "$name")"
        local guest enabled
        guest="$(instance_service_value "$name" SERVICE_PORT)"
        enabled="$(instance_service_value "$name" SERVICE_ENABLED)"
        [ -z "$guest" ] || SERVICE_GUEST_PORT="$guest"
        [ -z "$enabled" ] || SERVICE_ENABLED="$enabled"
        bind="$(instance_service_value "$name" SERVICE_BIND)"
        [ -z "$bind" ] || SERVICE_BIND="$bind"
        adb_enabled="$(instance_service_value "$name" SERVICE_ADB)"
        [ -z "$adb_enabled" ] || SERVICE_ADB="$adb_enabled"
        service_load_token "$name"
    fi
    [ -n "$SERVICE_HTTP_PORT" ] || SERVICE_HTTP_PORT="${AUTOSNAP_HTTP_PORT:-$((18088 + ($1 - PORT_BASE) / 2))}"
}

service_create_redirect() { # <console-port>
    local existing candidate
    existing="$(console_command "$1" 'redir list')" || return 1
    if printf '%s\n' "$existing" | grep -Eq "tcp[[:space:]]*:[[:space:]]*$SERVICE_HTTP_PORT[[:space:]]*=>[[:space:]]*$SERVICE_GUEST_PORT([[:space:]]|$)"; then
        return 0
    fi
    console_redir_add "$1" "$SERVICE_HTTP_PORT" "$SERVICE_GUEST_PORT"
}

service_http_request() { # <host-port> <token> <path> [JSON body]
    local url="http://127.0.0.1:$1$3"
    if [ $# -gt 3 ]; then
        curl --noproxy '*' -fsS --connect-timeout 2 --max-time 8 -H "X-Remote-Control-Token: $2" \
            -H 'Content-Type: application/json' --data "$4" "$url"
    else
        curl --noproxy '*' -fsS --connect-timeout 2 --max-time 3 -H "X-Remote-Control-Token: $2" "$url"
    fi
}

service_http_state() { # <console-port>
    local response
    service_read_instance "$1"
    if response="$(service_http_request "$SERVICE_HTTP_PORT" "$SERVICE_TOKEN" /api/v1 2>/dev/null)"; then
        case "$response" in
            *'"service":"remote-control"'*|*'"service": "remote-control"'*) printf '已就绪'; return 0 ;;
        esac
    fi
    printf '未就绪'
    return 1
}

service_wait_ready() { # <console-port> <timeout-seconds>
    local started=$SECONDS
    while [ $((SECONDS - started)) -lt "$2" ]; do
        service_http_state "$1" >/dev/null && return 0
        [ -n "$(emu_pid_for_port "$1")" ] || return 1
        sleep 2
    done
    return 1
}

service_request_poweroff() { # <console-port> [timeout-seconds]
    local timeout="${2:-60}" started
    service_read_instance "$1"
    if service_http_request "$SERVICE_HTTP_PORT" "$SERVICE_TOKEN" /api/v1/power \
        '{"action":"shutdown"}' >/dev/null 2>&1; then
        return 0
    fi
    # Some emulator versions close the connection while Android powers off.
    # Confirm process exit for the caller's full shutdown timeout: Android can
    # take several seconds to sync and unmount after HTTP disconnects.
    started=$SECONDS
    while [ $((SECONDS - started)) -lt "$timeout" ]; do
        [ -z "$(emu_pid_for_port "$1")" ] && return 0
        sleep 1
    done
    [ -z "$(emu_pid_for_port "$1")" ]
}


show_userdata_storage() { # <sysdir> <datadir>
    local image sizes logical allocated label
    declare -F image_storage_bytes >/dev/null || return 0
    for image in "$IMAGES/userdata.img" "$1/userdata-qemu.img" "$2/userdata-qemu.img"; do
        [ -f "$image" ] && [ ! -L "$image" ] || continue
        sizes="$(image_storage_bytes "$image" 2>/dev/null)" || continue
        read -r logical allocated <<< "$sizes"
        label="${image##*/}"
        [ "$image" != "$IMAGES/userdata.img" ] || label="userdata 底图"
        printf '  %-16s %s MiB 容量 / %s MiB 实占\n' "$label" "$((logical / 1048576))" "$((allocated / 1048576))"
    done
}
