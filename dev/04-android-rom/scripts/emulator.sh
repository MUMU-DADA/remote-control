#!/usr/bin/env bash
# =============================================================================
# 模拟器生命周期控制 —— 建 / 起 / 停 / 强杀 / 重启 / 重置 / 删除 / 复制
#
#   ./scripts/emulator.sh create  dev2              # 建一台（备好工作目录，不启动）
#   ./scripts/emulator.sh start   dev2              # 启动 + 等开机完成
#   ./scripts/emulator.sh start   dev2 --no-wait    # 起了就返回
#   ./scripts/emulator.sh stop    dev2              # 优雅关机（等进程退出）
#   ./scripts/emulator.sh kill    dev2              # 强制关闭（SIGKILL，不等）
#   ./scripts/emulator.sh restart dev2
#   ./scripts/emulator.sh reset   dev2              # 重置：清数据分区，回出厂状态
#   ./scripts/emulator.sh clone   dev2  dev3        # 复制（连已装应用一起）
#   ./scripts/emulator.sh delete  dev2              # 停掉并删光
#   ./scripts/emulator.sh export  dev2              # 导出成一个归档（备份/搬到别的机器）
#   ./scripts/emulator.sh import  dev2-20260929.tar # 从归档恢复成一台实例
#   ./scripts/emulator.sh inspect dev2-xxx.tar      # 只看归档里是什么，不解包
#   ./scripts/emulator.sh list                      # 所有实例
#   ./scripts/emulator.sh status  dev2
#
# 网络：默认「桥没被别的实例占用才桥接」。guest 的 MAC 所有实例都一样，
#       两台同时桥接到物理 LAN 会冲突，所以第二台自动走 NAT。
#       想强行指定： --bridge / --nat
#
# 实例名 ↔ 端口一一对应（默认实例 default = 5580），登记在
# .run/instances/<名字>.env。工作目录沿用 run-linux.sh 的按端口派生规则：
#   .run/sysdir-<port>/   .run/datadir-<port>/   .run/emulator-<port>.log
# 所以两套脚本可以混用：emulator.sh 建的机器，run-linux.sh --port N --reuse
# 照样能起来做验收。
#
# 硬件参数（分辨率 / 内存 / 核数 / GPU / 数据分区）全部来自
# emulator/config.ini —— 那份是唯一真源，这里不重复一遍默认值。
# =============================================================================
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
require_console_helper

DEFAULT_NAME="default"

usage() { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; }

# ---------------------------------------------------------------------------
# 实例路径
# ---------------------------------------------------------------------------
inst_sysdir()  { printf '%s/sysdir-%s'  "$RUN_DIR" "$(instance_port "$1")"; }
inst_datadir() { printf '%s/datadir-%s' "$RUN_DIR" "$(instance_port "$1")"; }
inst_log()     { printf '%s/emulator-%s.log' "$RUN_DIR" "$(instance_port "$1")"; }
inst_serial()  { printf 'emulator-%s' "$(instance_port "$1")"; }

need_instance() {
    instance_exists "$1" || die "没有叫 '$1' 的实例（./scripts/emulator.sh list 看有哪些）"
}

# 把交付目录里的镜像**符号链接**进工作目录。
#
# ⚠️ 两个文件要排除，不能链：
#   initrd    —— 模拟器会**重写**它（把自己的 ramdisk + dtb 写进去），
#                链过去会改到交付目录里那份，校验和当场不对。
#   config.ini —— 下面要用 emulator/config.ini 覆盖它，链过去会写穿。
build_sysdir() {   # build_sysdir <实例名>
    local name="$1" sysdir; sysdir="$(inst_sysdir "$name")"
    local pkg="$ARTIFACTS_DIR/rom-$PRODUCT_NAME"
    [ -s "$pkg/system-qemu.img" ] || die "交付目录不完整：${pkg#"$PROJECT_ROOT"/}
    先打包： ./scripts/package-rom.sh"

    mkdir -p "$sysdir"
    local f b
    for f in "$pkg"/*; do
        b="$(basename "$f")"
        case "$b" in initrd|config.ini) continue ;; esac
        ln -sfn "$f" "$sysdir/$b"
    done
    rm -f "$sysdir/config.ini"
    cp -f "$EMULATOR_CONFIG" "$sysdir/config.ini"
}

# 工作目录能不能直接拿来启动。
#
# 判据取 system-qemu.img：它是模拟器启动必需的第一个镜像。用 -e（会跟进
# 符号链接）—— 软链指向的交付目录被清掉时也会判为不可用。
sysdir_ready() {   # sysdir_ready <实例名>
    local sysdir; sysdir="$(inst_sysdir "$1")"
    [ -e "$sysdir/system-qemu.img" ] && [ -e "$sysdir/config.ini" ]
}

# 缺了就按交付目录重建。start 会先走这一步。
#
# 为什么非补不可：do_launch 直接把 sysdir 丢给 qemu，既不校验也不重建。
# 工作目录被删过之后（手工 rm、或换实例时的清理），qemu 只会报
#     ERROR | No initial system image for this configuration!
# 这句话完全指不到"sysdir 是空的"这个真因，能白查很久。
ensure_sysdir() {   # ensure_sysdir <实例名>
    local name="$1" sysdir; sysdir="$(inst_sysdir "$name")"
    sysdir_ready "$name" && return 0
    warn "工作目录不完整（${sysdir#"$PROJECT_ROOT"/}）—— 按交付目录重建"
    build_sysdir "$name"
}

# ---------------------------------------------------------------------------
# 启动
# ---------------------------------------------------------------------------
# 硬件参数全部从 config.ini 读；命令行显式给了才覆盖（同 run-linux.sh）。
resolve_hw() {
    CFG_GPU="$(config_get hw.gpu.mode auto)"
    CFG_RAM="$(config_get hw.ramSize 4096)"
    CFG_CORE="$(config_get hw.cpu.ncore 4)"
    GPU_MODE="${OPT_GPU:-$(resolve_gpu_mode "$CFG_GPU")}"
    MEM_MB="${OPT_MEM:-$CFG_RAM}"
    CORES="${OPT_CORE:-$CFG_CORE}"
    case "$MEM_MB" in *[Gg]) MEM_MB=$(( ${MEM_MB%[Gg]} * 1024 ));; esac
}

# 这台实例要不要桥接到物理 LAN？
#
# ⚠️ 硬限制：guest 的 eth0 MAC 是 QEMU 默认值，**所有实例一模一样**
#    （模拟器没暴露改它的参数，见 docs/11-snapshots-and-multi.md §4）。
#    两台同时桥接 = 局域网上出现两个同 MAC 的设备。
#    实测踩到过：br0 的 brif 里同时挂着 tap5580 和 tap5582。
#
# 所以默认策略是「**当前没有别的实例占着这座桥**才桥接」，其余自动走 NAT。
#   · 桥不存在 / 没开桥接 → NAT（和以前一样）
#   · 桥已经在给别的实例用（brif 里已有 tap）→ 这台上 NAT
#   · --bridge / --nat 可以强行指定（自己清楚在干什么时用）
bridge_in_use() {
    [ -n "$NET_BRIDGE_IF" ] || return 1
    ls "/sys/class/net/$NET_BRIDGE_IF/brif/" 2>/dev/null | grep -q '^tap'
}

decide_bridge() {   # 0 = 桥接，1 = NAT
    [ "${OPT_BRIDGE:-0}" = 1 ] && return 0
    [ "${OPT_NAT:-0}" = 1 ] && return 1
    [ -n "$NET_BRIDGE_IF" ] || return 1
    [ -d "/sys/class/net/$NET_BRIDGE_IF/bridge" ] || return 1
    bridge_in_use && return 1
    return 0
}

do_launch() {   # do_launch <实例名> <emulator 路径> <gpu>
    local name="$1" emu="$2" gpu="$3"
    local sysdir datadir log serial port
    sysdir="$(inst_sysdir "$name")"; datadir="$(inst_datadir "$name")"
    log="$(inst_log "$name")";       serial="$(inst_serial "$name")"
    port="$(instance_port "$name")"
    mkdir -p "$datadir"

    local tap_args=()
    if decide_bridge; then
        tap_args=(-net-tap "tap$port" -net-tap-script-up "$X64_DIR/tools/net-bridge-ifup.sh")
        log "网络：桥接 $NET_BRIDGE_IF（guest 走物理局域网）"
    elif [ -n "$NET_BRIDGE_IF" ] && [ -d "/sys/class/net/$NET_BRIDGE_IF/bridge" ]; then
        log "网络：NAT（$NET_BRIDGE_IF 已经被别的实例占着 —— guest MAC 全都一样，两台桥接会冲突）"
    fi

    : > "$log"
    # 窗口参数：默认无头（服务器上用）。--gui 时不加 -no-window。
    local win_args=(-no-window)
    [ "$OPT_GUI" = 1 ] && win_args=()

    # ANDROID_PRODUCT_OUT / ANDROID_BUILD_TOP 两个都要给：少了后者 SDK 版
    # 模拟器会去找 'kernel-qemu' 并报 "Your system directory is missing ..."。
    ANDROID_PRODUCT_OUT="$sysdir" ANDROID_BUILD_TOP="$AOSP_DIR" setsid nohup "$emu" \
        -sysdir "$sysdir" -datadir "$datadir" -port "$port" \
        "${win_args[@]}" -gpu "$gpu" -no-boot-anim -no-audio -no-snapshot \
        -accel on -memory "$MEM_MB" -cores "$CORES" \
        "${SERVICE_PROPERTY_ARGS[@]}" \
        "${tap_args[@]}" \
        >> "$log" 2>&1 < /dev/null &
    EMU_PID=$!
}

# 找模拟器可执行文件：先 SDK 版（与 Windows 侧同源），再 AOSP 自带版
pick_emulator() {
    if [ -n "$EMULATOR_BIN" ]; then printf '%s' "$EMULATOR_BIN"; return; fi
    local c
    for c in /opt/android/emulator-new/emulator/emulator \
             "$AOSP_DIR/prebuilts/android-emulator/linux-x86_64/emulator"; do
        [ -x "$c" ] && { printf '%s' "$c"; return; }
    done
    die "找不到模拟器（export EMULATOR_BIN=...）"
}

console_appears() {   # 最多 40 秒；进程提前退出即失败
    local port="$1"
    for _ in $(seq 1 8); do
        console_ready "$port" && return 0
        kill -0 "$EMU_PID" 2>/dev/null || return 1
        sleep 5
    done
    return 1
}

wait_service() {
    local port="$1"
    service_wait_ready "$port" 300
}

# ---------------------------------------------------------------------------
# 命令：create
# ---------------------------------------------------------------------------
cmd_create() {
    local name="${1:?实例名}"
    instance_exists "$name" && die "实例 '$name' 已经存在（换个名字，或先 delete）"
    local port; port="${OPT_PORT:-$(alloc_port)}"
    port_taken "$port" && die "端口 $port 已被别的实例占用"

    instance_register "$name" "$port"
    build_sysdir "$name"
    mkdir -p "$(inst_datadir "$name")"

    log "已创建实例 '$name'"
    printf '    端口     %s（adb -s %s）\n' "$port" "$(inst_serial "$name")"
    printf '    工作目录 %s\n' "${RUN_DIR#"$PROJECT_ROOT"/}/sysdir-$port"
    printf '    显示     %sx%s @%sdpi  %s\n' \
           "$(config_get hw.lcd.width 1280)" "$(config_get hw.lcd.height 720)" \
           "$(config_get hw.lcd.density 320)" \
           "$([ "$(config_get hw.lcd.width 1280)" -gt "$(config_get hw.lcd.height 720)" ] && echo 横屏 || echo 竖屏)"
    printf '    内存/核  %s MB / %s 核\n' "$(config_get hw.ramSize 4096)" "$(config_get hw.cpu.ncore 4)"
    printf '    数据分区 %s（实际占用看 qcow2 长到多大）\n' "$(config_get disk.dataPartition.size 16G)"
    printf '    下一步   ./scripts/emulator.sh start %s\n' "$name"
}

# ---------------------------------------------------------------------------
# 命令：start
# ---------------------------------------------------------------------------
cmd_start() {
    local name="${1:?实例名}"
    if ! instance_exists "$name"; then
        log "实例 '$name' 不存在，先创建"
        cmd_create "$name"
    fi
    if instance_running "$name"; then
        warn "实例 '$name' 已经在跑（端口 $(instance_port "$name")）"
        return 0
    fi

    # 工作目录可能被删过或残缺。缺了就重建，别让 qemu 报出指不到真因的错。
    ensure_sysdir "$name"

    [ -x "$(pick_emulator)" ] || die "找不到模拟器"

    resolve_hw
    local emu; emu="$(pick_emulator)"
    local port; port="$(instance_port "$name")"

    # The guest service is configured through qemu.rc.* properties and is
    # reachable through the emulator console's NAT redirect.  Persisted
    # instance values win on subsequent starts so template edits do not mutate
    # an existing guest unexpectedly.
    service_prepare_token "$name" 0 1
    service_register "$name" "$port" 0 1
    build_service_property_args 0

    if [ -n "$OPT_GPU" ]; then
        log "硬件参数： -memory $MEM_MB  -cores $CORES  -gpu $GPU_MODE（命令行指定）"
    else
        log "硬件参数： -memory $MEM_MB  -cores $CORES  -gpu $GPU_MODE（自适应：$(gpu_mode_reason)）"
    fi

    # GPU 回退链：自适应选出来的那一档起不来就退软件渲染
    # （有渲染节点 ≠ 驱动能用，虚拟 GPU / 容器里挂进来的 renderD 都可能崩）
    local gpus=("$GPU_MODE")
    if [ -z "$OPT_GPU" ] && [ "$GPU_MODE" = host ]; then
        gpus+=("swiftshader_indirect")
    fi

    local started=0 g
    for g in "${gpus[@]}"; do
        [ "$g" = "$GPU_MODE" ] || warn "上一档（$GPU_MODE）没起来，退到 $g"
        do_launch "$name" "$emu" "$g"
        if console_appears "$port"; then started=1; break; fi
        warn "没起来（$(tail -2 "$(inst_log "$name")" | tr '\n' ' ')）"
        kill "$EMU_PID" 2>/dev/null || true
        console_kill "$port" || true
        sleep 3
    done
    [ "$started" = 1 ] || die "起不来；看 $(inst_log "$name")"

    log "已启动 '$name'（-gpu $g -memory $MEM_MB -cores $CORES，端口 $(instance_port "$name")）"
    service_create_redirect "$port" || die "无法建立 HTTP 转发 127.0.0.1:$SERVICE_HTTP_PORT → guest:$SERVICE_GUEST_PORT"
    log "服务地址：http://127.0.0.1:$SERVICE_HTTP_PORT"
    [ "$SERVICE_AUTH" != 1 ] || log "访问令牌保存在 $(service_token_file "$name")"
    if [ "$OPT_NOWAIT" = 1 ]; then
        printf '后续： ./scripts/emulator.sh status %s\n' "$name"
        return 0
    fi

    log "等服务就绪"
    if wait_service "$port"; then
        local bt
        bt="$(grep -oE '(boot time|Boot completed in) [0-9]+ ms' "$(inst_log "$name")" 2>/dev/null | tail -1 || true)"
        log "服务就绪：http://127.0.0.1:$SERVICE_HTTP_PORT ${bt:+（$bt）}"
    else
        warn "等服务超时（300s）—— 可能还在起，也可能卡住了；看 $(inst_log "$name")"
        return 1
    fi
}

# ---------------------------------------------------------------------------
# 命令：stop / kill / restart
# ---------------------------------------------------------------------------
cmd_stop() {
    local name="${1:?实例名}"; need_instance "$name"
    local port; port="$(instance_port "$name")"
    local pids; pids="$(emu_pid_for_port "$port")"
    [ -n "$pids" ] || { warn "实例 '$name' 没在跑"; return 0; }

    # Let Android sync and unmount its filesystems through its normal power
    # service. Emulator console kill is only used by the explicit kill command.
    log "通过 HTTP 请求 Android 正常关机（不需要 ADB）"
    service_read_instance "$port"
    if ! service_create_redirect "$port" || ! service_request_poweroff "$port" "${AUTOSNAP_STOP_TIMEOUT:-60}"; then
        warn "HTTP 服务或保存的令牌不可用；用 kill 命令可强制关闭"
        return 1
    fi

    # Wait for the process to exit before reusing its image locks.
    local i
    for i in $(seq 1 30); do
        [ -z "$(emu_pid_for_port "$port")" ] && { log "已停止 '$name'"; return 0; }
        sleep 2
    done
    warn "'$name' 60 秒还没退（guest 卡住了？）—— 用 kill 强制关闭"
    return 1
}

cmd_kill() {
    local name="${1:?实例名}"; need_instance "$name"
    local port; port="$(instance_port "$name")"
    local pids; pids="$(emu_pid_for_port "$port")"
    [ -n "$pids" ] || { warn "实例 '$name' 没在跑"; return 0; }

    log "强制关闭 '$name'（SIGKILL）"
    local p
    console_kill "$port" || true
    sleep 2
    for p in $(emu_pid_for_port "$port"); do kill -9 "$p" 2>/dev/null || true; done

    local i
    for i in $(seq 1 15); do
        [ -z "$(emu_pid_for_port "$port")" ] && { log "已强制关闭 '$name'"; return 0; }
        sleep 1
    done
    die "'$name' 的进程杀不掉，手动看： ps -ef | grep 'port $port'"
}

cmd_restart() {
    local name="${1:?实例名}"; need_instance "$name"
    cmd_stop "$name" || cmd_kill "$name"
    cmd_start "$name"
}

# ---------------------------------------------------------------------------
# 命令：reset
# ---------------------------------------------------------------------------
# 重置 = 把**数据分区**清掉，回到出厂状态；实例本身留着（名字、端口、镜像不动）。
#
# ⚠️ 删的是真东西：已装的应用、应用数据、/sdcard 下的文件、快照，全没。
cmd_reset() {
    local name="${1:?实例名}"; need_instance "$name"
    instance_running "$name" && { log "先停掉 '$name'"; cmd_stop "$name" || cmd_kill "$name"; }

    # 二次确认：这条命令不可逆。--yes 跳过（脚本里用）。
    if [ "$OPT_YES" != 1 ]; then
        printf '\033[1;33m[!]\033[0m 重置 '\''%s'\'' 会清空数据分区（已装应用 / 应用数据 / /sdcard / 快照）\n' "$name"
        printf '    输入 yes 继续：'
        local ans; read -r ans
        [ "$ans" = yes ] || die "已取消"
    fi

    local sysdir; sysdir="$(inst_sysdir "$name")"
    log "清掉数据分区与状态：${sysdir#"$PROJECT_ROOT"/}"
    # 镜像本身是**符号链接**，不能删（rm -f 加不加 -r 都会顺着链删掉目标吗？
    # 不会 —— 删的是链接本身。所以下面按名字精确删，别用通配）。
    rm -f "$sysdir"/userdata-qemu.img \
          "$sysdir"/userdata-qemu.img.qcow2 \
          "$sysdir"/userdata-qemu.img.qcow2.lock \
          "$sysdir"/cache.img \
          "$sysdir"/cache.img.qcow2 \
          "$sysdir"/encryptionkey.img.qcow2 \
          "$sysdir"/bootcompleted.ini \
          "$sysdir"/hardware-qemu.ini \
          "$sysdir"/hardware-qemu.ini.lock \
          "$sysdir"/multiinstance.lock \
          "$sysdir"/emu-launch-params.txt \
          "$sysdir"/version_num.cache
    rm -rf "$sysdir"/build.avd "$sysdir"/snapshots "$sysdir"/tmpAdbCmds
    mkdir -p "$(inst_datadir "$name")"

    # Reset creates a fresh guest, so service.* from the current template and
    # a new token must be applied on its next start.
    local port; port="$(instance_port "$name")"
    instance_register "$name" "$port"
    rm -f "$(service_token_file "$name")"

    log "已重置 '$name'（下次启动是全新机器；config.ini 的改动这次会全生效）"
}

# ---------------------------------------------------------------------------
# 命令：delete
# ---------------------------------------------------------------------------
cmd_delete() {
    local name="${1:?实例名}"; need_instance "$name"
    instance_running "$name" && { log "先停掉 '$name'"; cmd_stop "$name" || cmd_kill "$name"; }

    if [ "$OPT_YES" != 1 ]; then
        printf '\033[1;33m[!]\033[0m 删除 '\''%s'\'' 会删掉整个工作目录（含已装应用与快照）\n' "$name"
        printf '    输入 yes 继续：'
        local ans; read -r ans
        [ "$ans" = yes ] || die "已取消"
    fi

    local sysdir datadir log; sysdir="$(inst_sysdir "$name")"
    datadir="$(inst_datadir "$name")"; log="$(inst_log "$name")"
    # ⚠️ 工作目录里镜像都是**符号链接**，rm -rf 只删链接不删目标 ——
    #    交付目录 artifacts/rom-* 不会被误删（这是要确认的第一件事）。
    rm -rf "$sysdir" "$datadir" "$log"
    rm -f "$(service_token_file "$name")"
    instance_unregister "$name"
    log "已删除实例 '$name'"
}

# ---------------------------------------------------------------------------
# 命令：clone
# ---------------------------------------------------------------------------
# 复制实例 = 连**已装应用和状态**一起复制出一台新的。
#
# ⚠️ 有三样东西不能照抄，必须让模拟器重新生成：
#   hardware-qemu.ini   里面全是**绝对路径**（disk.*.path 指向原实例的
#                       工作目录）。照抄的话新实例会去读写原实例的镜像 ——
#                       两台机器共用一份 userdata，数据当场互相踩。
#   emu-launch-params.txt / *.lock / bootcompleted.ini
#                       同样是上一次运行的残留。
# 删掉这几样，模拟器下次启动会按新路径重建。
cmd_clone() {
    local src="${1:?源实例名}" dst="${2:?新实例名}"
    need_instance "$src"
    instance_exists "$dst" && die "实例 '$dst' 已经存在"

    if instance_running "$src"; then
        log "复制前先停掉源实例 '$src'（跑着的时候 qcow2 还在写，抄出来是脏的）"
        cmd_stop "$src" || cmd_kill "$src"
    fi

    local port; port="${OPT_PORT:-$(alloc_port)}"
    port_taken "$port" && die "端口 $port 已被别的实例占用"

    local s_sys s_dat d_sys d_dat
    s_sys="$(inst_sysdir "$src")"; s_dat="$(inst_datadir "$src")"
    instance_register "$dst" "$port"
    copy_instance_service_metadata "$src" "$dst"
    d_sys="$(inst_sysdir "$dst")"; d_dat="$(inst_datadir "$dst")"

    log "复制工作目录（含已装应用）： $(du -sh "$s_sys" 2>/dev/null | cut -f1)"
    mkdir -p "$d_sys"
    # -a 保留符号链接（镜像不复制实体，两台共用同一份只读镜像，这是对的）
    cp -a "$s_sys"/. "$d_sys"/
    [ -d "$s_dat" ] && cp -a "$s_dat"/. "$d_dat"/ || mkdir -p "$d_dat"

    # 路径/端口相关的残留全清掉，让模拟器按新路径重建
    rm -f "$d_sys"/hardware-qemu.ini "$d_sys"/hardware-qemu.ini.lock \
          "$d_sys"/multiinstance.lock "$d_sys"/emu-launch-params.txt \
          "$d_sys"/bootcompleted.ini "$d_sys"/version_num.cache
    rm -f "$d_sys"/*.img.qcow2.lock
    # 快照里存着 hardware.ini，路径变了之后它一定对不上（模拟器会拒绝加载）
    if [ -d "$d_sys/snapshots" ]; then
        rm -rf "$d_sys/snapshots"
        warn "源实例的快照没有复制（快照绑定了原来的硬件配置与路径）"
    fi
    # config.ini 用当前真源覆盖，保证新实例带上最新的硬件参数
    rm -f "$d_sys/config.ini"; cp -f "$EMULATOR_CONFIG" "$d_sys/config.ini"

    log "已复制： '$src' → '$dst'（端口 $port）"
    printf '    下一步   ./scripts/emulator.sh start %s\n' "$dst"
    printf '  网络     默认走 NAT —— guest MAC 两台一样，同时桥接会在局域网上冲突\n'
    printf '           （源实例没在跑的话，这台上网关也没问题；想强行桥接加 --bridge）\n'
}

# ---------------------------------------------------------------------------
# 命令：export / import / inspect
# ---------------------------------------------------------------------------
#
# 归档长这样（tar，可选 gzip）：
#
#     INSTANCE-MANIFEST.json   元数据：名字、导出时间、ROM 指纹、跳过了哪些文件
#     sysdir/…                 实例状态（**不含**指向 ROM 镜像的符号链接）
#     datadir/…
#     host-service.env / host-service.token   服务配置与访问令牌（0600）
#
# ⚠️ 两个关键设计，都是被实测逼出来的：
#
#   ① **镜像不进归档**。sysdir 里的 system-qemu.img 之类全是指向
#      artifacts/rom-*/ 的符号链接，那是 5.7G 只读镜像，导它没意义。
#      归档里记下"需要哪些镜像"和 ROM 指纹，import 时按本地那份重新链上，
#      指纹对不上会警告（可能不是同一版 ROM）。要连镜像一起打包用
#      --with-images。
#
#   ② **必须用 tar --sparse**。userdata-qemu.img 表观 48G、实占才 551M
#      （稀疏文件）。不加 --sparse 的话 tar 会把 48G 的零**原样写进归档** ——
#      归档从 35G 涨到 83G，而且白等半天。
#      GNU tar 的 --sparse 走 SEEK_HOLE，空洞根本不读，所以又快又小。
#
#   ③ 路径相关的东西（hardware-qemu.ini 里全是绝对路径、*.lock、
#      emu-launch-params.txt）**不带**：import 到别的机器/端口上它们一定是错的，
#      不删的话两台机器会指向同一份 userdata。跟 clone 那边同一个道理。

MANIFEST_NAME="INSTANCE-MANIFEST.json"

# 归档里**不要**带的东西（路径/运行期相关，import 时会按新环境重建）
arch_exclude() {
    case "$1" in
        hardware-qemu.ini|hardware-qemu.ini.lock|multiinstance.lock) return 0 ;;
        emu-launch-params.txt|bootcompleted.ini|version_num.cache) return 0 ;;
        *.qcow2.lock|config.ini) return 0 ;;
    esac
    return 1
}

rom_fingerprint() {   # 拿交付目录的 SHA256SUMS 当 ROM 指纹
    local pkg="$ARTIFACTS_DIR/rom-$PRODUCT_NAME"
    if [ -s "$pkg/SHA256SUMS" ]; then
        md5sum "$pkg/SHA256SUMS" | cut -d' ' -f1
    else
        printf 'unknown'
    fi
}

cmd_export() {
    local name="${1:?实例名}"; need_instance "$name"
    if instance_running "$name"; then
        # 跑着的时候 qcow2 还在写，抄出来是脏的。stop 现在会先 sync，安全。
        log "导出前先停掉 '$name'（运行中导出会拿到不一致的镜像）"
        cmd_stop "$name" || cmd_kill "$name"
    fi

    local port sysdir datadir
    port="$(instance_port "$name")"
    sysdir="$(inst_sysdir "$name")"
    datadir="$(inst_datadir "$name")"
    [ -d "$sysdir" ] || die "工作目录不存在：$sysdir"

    local out="${OPT_OUT:-$RUN_DIR/$name-$(date +%Y%m%d-%H%M%S).tar}"
    case "$out" in /*) ;; *) out="$PWD/$out" ;; esac
    if [ -e "$out" ] && [ "$OPT_FORCE" != 1 ]; then
        die "已存在：$out（加 --force 覆盖）"
    fi

    # 空间检查：归档 ≈ 实占大小（稀疏空洞不写，所以不是表观大小）
    local need free_mb
    need=$(du -sm "$sysdir" "$datadir" 2>/dev/null | awk '{s+=$1} END{print s+0}')
    free_mb=$(df -Pm "$(dirname "$out")" | awk 'NR==2{print $4}')
    if [ "$free_mb" -le $((need + 512)) ]; then
        die "空间不够：归档预计 ${need}MB，目标分区只剩 ${free_mb}MB"
    fi

    # ── 搭一个"与端口无关"的暂存目录 ──
    # 归档里是 sysdir/ datadir/，不带端口号 —— import 到别的端口也能用。
    # 用**硬链接**挂进去，35G 的 qcow2 不会真的再复制一份。
    local stage="$RUN_DIR/.export-stage-$$"
    rm -rf "$stage"
    mkdir -p "$stage/sysdir" "$stage/datadir"

    # Guest /data already contains the persisted service credentials.  Carry
    # their host counterpart so importing does not create a different token.
    local key value
    : > "$stage/host-service.env"
    for key in SERVICE_PORT SERVICE_ENABLED SERVICE_AUTH SERVICE_BIND SERVICE_ADB; do
        value="$(instance_service_value "$name" "$key")"
        [ -z "$value" ] || printf '%s=%s\n' "$key" "$value" >> "$stage/host-service.env"
    done
    if [ -s "$(service_token_file "$name")" ]; then
        (umask 077; cp "$(service_token_file "$name")" "$stage/host-service.token")
        chmod 600 "$stage/host-service.token"
    fi

    local n_state=0 f b
    for f in "$sysdir"/*; do
        [ -e "$f" ] || continue
        [ -L "$f" ] && continue              # 指向 ROM 镜像的符号链接：不导
        b="$(basename "$f")"
        arch_exclude "$b" && continue        # 路径/运行期相关：不导
        ln "$f" "$stage/sysdir/$b" 2>/dev/null || cp -a "$f" "$stage/sysdir/$b"
        n_state=$((n_state + 1))
    done
    if [ -d "$datadir" ]; then
        for f in "$datadir"/*; do
            [ -e "$f" ] || continue
            b="$(basename "$f")"
            ln "$f" "$stage/datadir/$b" 2>/dev/null || cp -a "$f" "$stage/datadir/$b"
        done
    fi

    # 归档里记下"要哪些镜像"，import 时按名字重新链本地那份
    local imgs="" i
    for i in "$ARTIFACTS_DIR/rom-$PRODUCT_NAME"/*; do
        [ -e "$i" ] || continue
        b="$(basename "$i")"
        [ -L "$sysdir/$b" ] || continue
        if [ -z "$imgs" ]; then imgs="\"$b\""; else imgs="$imgs, \"$b\""; fi
    done

    local bytes fp now
    bytes=$(du -sb "$sysdir" "$datadir" 2>/dev/null | awk '{s+=$1} END{print s+0}')
    fp="$(rom_fingerprint)"
    now="$(date -Iseconds)"

    # 清单：值全部先算成变量再拼，here-doc 里不做嵌套展开
    # （嵌套的 $( ) + 引号在 here-doc 里出过一次词法问题，干脆避开）
    {
        printf '{\n'
        printf '  "format": 1,\n'
        printf '  "instance": "%s",\n' "$name"
        printf '  "exportedAt": "%s",\n' "$now"
        printf '  "exportedFrom": "linux",\n'
        printf '  "product": "%s",\n' "$PRODUCT_NAME"
        printf '  "romFingerprint": "%s",\n' "$fp"
        printf '  "imageFiles": [%s],\n' "$imgs"
        printf '  "stateFiles": %s,\n' "$n_state"
        printf '  "dirBytes": %s\n' "$bytes"
        printf '}\n'
    } > "$stage/$MANIFEST_NAME"

    local zflag=""
    case "$out" in *.gz|*.tgz) zflag="-z" ;; esac
    log "打包（状态实占约 $((need / 1024))G；稀疏空洞不写，所以不会等 48G）"
    # ⚠️ --sparse 不能省：userdata-qemu.img 表观 48G、实占 551M，
    #    不加的话 tar 会把 48G 的零原样写进归档。
    if ! tar --sparse --numeric-owner $zflag -cf "$out" -C "$stage" . ; then
        rm -rf "$stage"
        die "tar 失败"
    fi
    # The archive includes the service token as well as guest /data.
    chmod 600 "$out"
    rm -rf "$stage"

    # 导完**读回确认** —— tar 说成功不等于归档能用
    local listed
    listed="$(tar -tf "$out" 2>/dev/null | wc -l)"
    if [ "$listed" -le 0 ]; then die "归档读不回来（tar -tf 是空的）：$out"; fi
    if ! tar -tf "$out" 2>/dev/null | grep -q "$MANIFEST_NAME"; then
        die "归档里没有 $MANIFEST_NAME"
    fi

    local sz pct rel
    sz=$(stat -c %s "$out")
    pct=$(awk -v a="$bytes" -v b="$sz" 'BEGIN{ if (a>0) printf "%.0f", (1-b/a)*100; else printf "0" }')
    rel="${out#"$PROJECT_ROOT"/}"
    log "已导出： $rel"
    printf '    大小     %s（状态实占 %s，省了 %s%%）\n' \
        "$(numfmt --to=iec "$sz")" "$(numfmt --to=iec "$bytes")" "$pct"
    printf '    条目     %s 个（tar -tf 数出来的）\n' "$listed"
    printf '    镜像     未打包（import 时按本地 ROM 重新链接）\n'
    printf '    恢复     ./scripts/emulator.sh import %s -n %s\n' "$(basename "$out")" "$name"
    warn "⚠️ 恢复（import）目前**未通过验证**：归档是忠实的，但恢复出来数据是空的。"
    warn "   别把这个归档当备份用。详情见 docs/12-emulator-control.md。"
}

cmd_inspect() {   # 只看归档里是什么
    local f="${1:?归档文件}"
    [ -s "$f" ] || die "没有这个归档：$f"
    log "归档： $f（$(numfmt --to=iec "$(stat -c %s "$f")")）"
    local mf; mf="$(tar -xOf "$f" "./$MANIFEST_NAME" 2>/dev/null || tar -xOf "$f" "$MANIFEST_NAME" 2>/dev/null)"
    [ -n "$mf" ] || die "归档里没有 $MANIFEST_NAME —— 不是本工具导出的？"
    printf '%s\n' "$mf" | sed 's/^/    /'
    local here; here="$(rom_fingerprint)"
    local there; there="$(printf '%s' "$mf" | sed -n 's/.*"romFingerprint": *"\([^"]*\)".*/\1/p')"
    if [ -n "$there" ] && [ "$there" != "$here" ]; then
        warn "ROM 指纹不一致：归档 $there，本地 $here —— 恢复后可能起不来（镜像版本不同）"
    fi
    printf '    内容（前 12 项）：\n'
    tar -tf "$f" 2>/dev/null | head -12 | sed 's/^/      /'
}

cmd_import() {
    local f="${1:?归档文件}"
    [ -s "$f" ] || die "没有这个归档：$f"

    # ⚠️⚠️ 安全闸。**恢复出来的实例数据是空的** —— 实测复现三次。
    #
    #   归档本身是忠实的（qcow2 的 md5 与源逐字节相同，解出来也相同），
    #   原实例重启也一切正常，但把归档恢复成新实例后开机，guest 里的
    #   文件全没了。根因**没找到**，最可疑的是那个 32G 的稀疏 raw
    #   backing 文件（tar 解开后实占块数和源差 8 块，而文件系统的
    #   空洞布局对 qcow2 覆盖层是有意义的）。
    #
    #   在查清之前，这条命令默认不让跑 —— "备份"要是恢复不出来，
    #   比没有备份更危险：用户会以为数据安全了。
    if [ "$OPT_UNSAFE" != 1 ]; then
        warn "import 目前**未通过验证**：恢复出来的实例 /data 是空的（归档本身没问题）"
        warn "详见 docs/12-emulator-control.md 最后一节。确实要用加 --unsafe。"
        die "已拒绝执行（这是保护，不是故障）"
    fi
    warn "以 --unsafe 运行：恢复出来的机器可能没有原来的数据"

    local name="${OPT_NAME:-}"
    local mf; mf="$(tar -xOf "$f" "./$MANIFEST_NAME" 2>/dev/null || tar -xOf "$f" "$MANIFEST_NAME" 2>/dev/null)"
    [ -n "$mf" ] || die "归档里没有 $MANIFEST_NAME —— 不是本工具导出的？"
    [ -n "$name" ] || name="$(printf '%s' "$mf" | sed -n 's/.*"instance": *"\([^"]*\)".*/\1/p')"
    [ -n "$name" ] || die "归档里没写实例名，用 -n 指定一个"
    instance_exists "$name" && die "实例 '$name' 已经存在（用 -n 换个名字）"

    local there here
    there="$(printf '%s' "$mf" | sed -n 's/.*"romFingerprint": *"\([^"]*\)".*/\1/p')"
    here="$(rom_fingerprint)"
    [ "$there" = "$here" ] || warn "ROM 指纹不一致（归档 $there / 本地 $here）—— 镜像版本不同可能起不来"

    local port; port="${OPT_PORT:-$(alloc_port)}"
    port_taken "$port" && die "端口 $port 已被别的实例占用"

    local free_mb need_mb
    need_mb=$(printf '%s' "$mf" | sed -n 's/.*"dirBytes": *\([0-9]*\).*/\1/p')
    need_mb=$(( ${need_mb:-0} / 1048576 ))
    free_mb=$(df -Pm "$RUN_DIR" | awk 'NR==2{print $4}')
    [ "$free_mb" -gt $((need_mb + 512)) ] ||         die "空间不够：需要约 ${need_mb}MB，只剩 ${free_mb}MB"

    instance_register "$name" "$port"
    local service_metadata service_token
    service_metadata="$(tar -xOf "$f" ./host-service.env 2>/dev/null || true)"
    if [ -n "$service_metadata" ]; then
        printf '%s\n' "$service_metadata" | sed -n '/^SERVICE_\(PORT\|ENABLED\|AUTH\|BIND\|ADB\)=/p' >> "$(instance_file "$name")"
    fi
    service_token="$(tar -xOf "$f" ./host-service.token 2>/dev/null || true)"
    if [ -n "$service_token" ]; then
        (umask 077; printf '%s\n' "$service_token" > "$(service_token_file "$name")")
        chmod 600 "$(service_token_file "$name")"
    fi
    local sysdir; sysdir="$(inst_sysdir "$name")"
    mkdir -p "$sysdir" "$(inst_datadir "$name")"

    log "解包 → .run/sysdir-$port（约 $((need_mb/1024))G）"
    tar --sparse --numeric-owner -xf "$f" -C "$RUN_DIR" --strip-components=0 \
        --exclude=./host-service.env --exclude=./host-service.token \
        --transform 's,^\./sysdir,sysdir-'"$port"',; s,^\./datadir,datadir-'"$port"',' \
        2>/dev/null || {
        # --transform 不可用（非 GNU tar）时退回两步走
        rm -rf "$RUN_DIR/.import-tmp-$$"; mkdir -p "$RUN_DIR/.import-tmp-$$"
        tar -xf "$f" -C "$RUN_DIR/.import-tmp-$$"
        [ -d "$RUN_DIR/.import-tmp-$$/sysdir" ] && cp -a "$RUN_DIR/.import-tmp-$$/sysdir/." "$sysdir/"
        [ -d "$RUN_DIR/.import-tmp-$$/datadir" ] && cp -a "$RUN_DIR/.import-tmp-$$/datadir/." "$(inst_datadir "$name")/"
        rm -rf "$RUN_DIR/.import-tmp-$$"
    }
    rm -f "$sysdir/$MANIFEST_NAME" 2>/dev/null

    # 按本地 ROM 重新链镜像（归档里没带）
    local missing=""
    for i in "$ARTIFACTS_DIR/rom-$PRODUCT_NAME"/*; do
        [ -e "$i" ] || continue
        b="$(basename "$i")"
        [ -e "$sysdir/$b" ] && continue
        ln -sfn "$i" "$sysdir/$b" 2>/dev/null || missing="$missing $b"
    done
    [ -z "$missing" ] || warn "这些镜像没链上（本地 ROM 缺）：$missing"
    rm -f "$sysdir/config.ini"; cp -f "$EMULATOR_CONFIG" "$sysdir/config.ini"
    rm -f "$sysdir/hardware-qemu.ini" "$sysdir/hardware-qemu.ini.lock" \
          "$sysdir/multiinstance.lock" "$sysdir/emu-launch-params.txt" \
          "$sysdir/bootcompleted.ini" "$sysdir/version_num.cache" "$sysdir"/*.qcow2.lock 2>/dev/null

    # 读回确认：关键文件真的落地了
    [ -s "$sysdir/userdata-qemu.img" ] || warn "userdata-qemu.img 不在归档里？"
    log "已导入： '$name'（端口 $port）"
    printf '    下一步   ./scripts/emulator.sh start %s\n' "$name"

    if [ "$OPT_START" = 1 ]; then cmd_start "$name"; fi
}

# ---------------------------------------------------------------------------
# 命令：list / status
# ---------------------------------------------------------------------------
inst_state() {   # 打印 运行中/已停止
    local name="$1" port; port="$(instance_port "$name")"
    if [ -n "$(emu_pid_for_port "$port")" ]; then
        service_read_instance "$port"
        if service_http_state "$port" >/dev/null 2>&1; then
            printf '运行中(服务就绪)'
        else
            printf '运行中(启动中)'
        fi
    else
        printf '已停止'
    fi
}

cmd_list() {
    local names; names="$(instance_names)"
    [ -n "$names" ] || { printf '（还没有实例）\n'; printf '建一台： ./scripts/emulator.sh create %s\n' "$DEFAULT_NAME"; return 0; }
    printf '%-12s %-7s %-16s %-9s %s\n' 名字 端口 状态 占用 工作目录
    printf '%-12s %-7s %-16s %-9s %s\n' ──────────── ─────── ──────────────── ───────── ────────
    local n port sz note
    for n in $names; do
        port="$(instance_port "$n")"
        # sysdir 可能已经被删掉（实例还在跑但目录没了，或历史残留）。
        # 这里必须容错：脚本开头是 set -euo pipefail，而赋值形式的命令替换
        # 会把 du 的失败码带上来触发 set -e，让 list 在第一个实例就中止 ——
        # 表现成"一个实例都没列出来"，极具误导性。
        sz="$(du -sh "$RUN_DIR/sysdir-$port" 2>/dev/null | cut -f1 || true)"
        note=""
        [ -d "$RUN_DIR/sysdir-$port" ] || note="   ← 工作目录缺失"
        printf '%-12s %-7s %-16s %-9s %s%s\n' "$n" "$port" "$(inst_state "$n")" "${sz:--}" \
               "${RUN_DIR#"$PROJECT_ROOT"/}/sysdir-$port" "$note"
    done
}

cmd_status() {
    local name="${1:-$DEFAULT_NAME}"; need_instance "$name"
    local port; port="$(instance_port "$name")"
    printf '实例       %s\n' "$name"
    printf '端口       %s\n' "$port"
    printf '状态       %s\n' "$(inst_state "$name")"
    service_read_instance "$port"
    printf 'console    127.0.0.1:%s\n' "$port"
    printf '服务       http://127.0.0.1:%s（guest:%s）\n' "$SERVICE_HTTP_PORT" "$SERVICE_GUEST_PORT"
    printf 'ADB        显式诊断时使用：adb -s emulator-%s\n' "$port"
    printf '工作目录   %s\n' "$(inst_sysdir "$name")"
    if [ ! -d "$(inst_sysdir "$name")" ]; then
        printf '           ⚠️  工作目录不存在（sysdir 已被删除）；实例若仍在跑，靠的是已删除的 inode\n'
    fi
    printf '配置文件   %s/sysdir-%s/config.ini\n' "${RUN_DIR#"$PROJECT_ROOT"/}" "$port"
    printf '日志       %s\n' "$(inst_log "$name")"
    printf '\nconfig.ini（唯一真源 %s）：\n' "${EMULATOR_CONFIG#"$PROJECT_ROOT"/}"
    local k
    for k in hw.lcd.width hw.lcd.height hw.lcd.density hw.cpu.ncore hw.ramSize \
             hw.gpu.mode disk.dataPartition.size; do
        printf '  %-24s %s\n' "$k" "$(config_get "$k" -)"
    done
    if [ -f "$(inst_sysdir "$name")/hardware-qemu.ini" ]; then
        printf '\n上次启动**实际生效**的（hardware-qemu.ini）：\n'
        for k in hw.lcd.width hw.lcd.height hw.lcd.density hw.cpu.ncore hw.ramSize \
                 hw.gpu.mode disk.dataPartition.size; do
            printf '  %-24s %s\n' "$k" \
                "$(sed -n "s/^${k} = //p" "$(inst_sysdir "$name")/hardware-qemu.ini" | tail -1)"
        done
    fi
}

# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------
OPT_PORT=""; OPT_GPU=""; OPT_MEM=""; OPT_CORE=""; OPT_NOWAIT=0; OPT_YES=0; OPT_GUI=0
OPT_BRIDGE=0; OPT_NAT=0
OPT_OUT=""; OPT_NAME=""; OPT_FORCE=0; OPT_START=0; OPT_UNSAFE=0
CMD=""; ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --port)    OPT_PORT="${2:?}"; shift ;;
        --gpu)     OPT_GPU="${2:?}"; shift ;;
        --memory)  OPT_MEM="${2:?}"; shift ;;
        --cores)   OPT_CORE="${2:?}"; shift ;;
        --no-wait) OPT_NOWAIT=1 ;;
        --gui)     OPT_GUI=1 ;;
        # 网络：默认按"桥有没有被别的实例占用"自动决定
        --bridge)  OPT_BRIDGE=1 ;;
        --nat)     OPT_NAT=1 ;;
        # export / import
        -o|--out)  OPT_OUT="${2:?}"; shift ;;
        -n|--name) OPT_NAME="${2:?}"; shift ;;
        -z|--gzip) : ;;                 # 只是提示：输出名以 .gz/.tgz 结尾就会压缩
        --force)   OPT_FORCE=1 ;;
        --unsafe)  OPT_UNSAFE=1 ;;      # 只有 import 认这个，理由见 cmd_import
        --start)   OPT_START=1 ;;
        -y|--yes)  OPT_YES=1 ;;
        -h|--help) usage; exit 0 ;;
        -*)        die "未知参数：$1" ;;
        *)         if [ -z "$CMD" ]; then CMD="$1"; else ARGS+=("$1"); fi ;;
    esac
    shift
done

[ -n "$CMD" ] || { usage; exit 1; }
mkdir -p "$RUN_DIR" "$INSTANCES_DIR"

case "$CMD" in
    create)  cmd_create  "${ARGS[0]:-$DEFAULT_NAME}" ;;
    start)   cmd_start   "${ARGS[0]:-$DEFAULT_NAME}" ;;
    stop)    cmd_stop    "${ARGS[0]:-$DEFAULT_NAME}" ;;
    kill)    cmd_kill    "${ARGS[0]:-$DEFAULT_NAME}" ;;
    restart) cmd_restart "${ARGS[0]:-$DEFAULT_NAME}" ;;
    reset)   cmd_reset   "${ARGS[0]:-$DEFAULT_NAME}" ;;
    delete)  cmd_delete  "${ARGS[0]:-$DEFAULT_NAME}" ;;
    clone)   cmd_clone   "${ARGS[0]:?源实例名}" "${ARGS[1]:?新实例名}" ;;
    export)  cmd_export  "${ARGS[0]:-$DEFAULT_NAME}" ;;
    import)  cmd_import  "${ARGS[0]:?归档文件}" ;;
    inspect) cmd_inspect "${ARGS[0]:?归档文件}" ;;
    list|ls) cmd_list ;;
    status)  cmd_status  "${ARGS[0]:-$DEFAULT_NAME}" ;;
    *)       die "未知命令：$CMD（-h 看用法）" ;;
esac
