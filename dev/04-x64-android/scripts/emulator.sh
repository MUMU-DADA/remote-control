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

wait_adb() {   # 最多 40 秒；进程提前退出即失败
    local serial="$1"
    for _ in $(seq 1 8); do
        "$ADB" -s "$serial" get-state >/dev/null 2>&1 && return 0
        kill -0 "$EMU_PID" 2>/dev/null || return 1
        sleep 5
    done
    return 1
}

wait_boot() {
    local serial="$1" i
    for i in $(seq 1 60); do
        [ "$("$ADB" -s "$serial" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && return 0
        sleep 3
    done
    return 1
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
    printf '    数据分区 %s（实际占用看 qcow2 长到多大）\n' "$(config_get disk.dataPartition.size 32G)"
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
    [ -x "$(pick_emulator)" ] || die "找不到模拟器"

    resolve_hw
    local emu; emu="$(pick_emulator)"
    local serial; serial="$(inst_serial "$name")"

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
        if wait_adb "$serial"; then started=1; break; fi
        warn "没起来（$(tail -2 "$(inst_log "$name")" | tr '\n' ' ')）"
        kill "$EMU_PID" 2>/dev/null || true
        "$ADB" -s "$serial" emu kill >/dev/null 2>&1 || true
        sleep 3
    done
    [ "$started" = 1 ] || die "起不来；看 $(inst_log "$name")"

    log "已启动 '$name'（-gpu $g -memory $MEM_MB -cores $CORES，端口 $(instance_port "$name")）"
    if [ "$OPT_NOWAIT" = 1 ]; then
        printf '后续： adb -s %s shell getprop sys.boot_completed\n' "$serial"
        return 0
    fi

    log "等开机完成"
    if wait_boot "$serial"; then
        "$ADB" -s "$serial" root >/dev/null 2>&1 || true
        "$ADB" -s "$serial" wait-for-device
        local bt
        bt="$(grep -oE '(boot time|Boot completed in) [0-9]+ ms' "$(inst_log "$name")" 2>/dev/null | tail -1 || true)"
        log "开机完成 ${bt:+（$bt）}"
        "$ADB" -s "$serial" shell getprop ro.build.version.sdk >/dev/null 2>&1 || true
    else
        warn "等开机超时（300s）—— 可能还在起，也可能卡住了；看 $(inst_log "$name")"
        return 1
    fi
}

# ---------------------------------------------------------------------------
# 命令：stop / kill / restart
# ---------------------------------------------------------------------------
cmd_stop() {
    local name="${1:?实例名}"; need_instance "$name"
    local port serial; port="$(instance_port "$name")"; serial="$(inst_serial "$name")"
    local pids; pids="$(emu_pid_for_port "$port")"
    [ -n "$pids" ] || { warn "实例 '$name' 没在跑"; return 0; }

    log "请 guest 自己关机（adb emu kill）"
    "$ADB" -s "$serial" emu kill >/dev/null 2>&1 || true

    # ⚠️ 等的是**进程真的退出**，不是"命令返回了"。emu kill 只是递个请求，
    #    guest 还要走完关机流程（卸载 /data、收 qcow2）；这时候就重启会
    #    撞上 multiinstance.lock —— 第二台会报 "another emulator instance
    #    is currently running"。
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
    # ⚠️ 先把 "请它关" 的路径也走一遍：正常路径能落盘 qcow2，直接 SIGKILL
    #    会让上一次的写入丢一点（qcow2 的元数据在进程里缓存着）。
    #    强杀是为了"现在就关掉"，不是为了更快 —— 差那 1~2 秒不值当。
    "$ADB" -s "$(inst_serial "$name")" emu kill >/dev/null 2>&1 || true
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
# 命令：list / status
# ---------------------------------------------------------------------------
inst_state() {   # 打印 运行中/已停止
    local name="$1" port; port="$(instance_port "$name")"
    if [ -n "$(emu_pid_for_port "$port")" ]; then
        local b; b="$("$ADB" -s "emulator-$port" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')"
        [ "$b" = 1 ] && printf '运行中(已开机)' || printf '运行中(启动中)'
    else
        printf '已停止'
    fi
}

cmd_list() {
    local names; names="$(instance_names)"
    [ -n "$names" ] || { printf '（还没有实例）\n'; printf '建一台： ./scripts/emulator.sh create %s\n' "$DEFAULT_NAME"; return 0; }
    printf '%-12s %-7s %-16s %-9s %s\n' 名字 端口 状态 占用 工作目录
    printf '%-12s %-7s %-16s %-9s %s\n' ──────────── ─────── ──────────────── ───────── ────────
    local n port sz du
    for n in $names; do
        port="$(instance_port "$n")"
        sz="$(du -sh "$RUN_DIR/sysdir-$port" 2>/dev/null | cut -f1)"
        printf '%-12s %-7s %-16s %-9s %s\n' "$n" "$port" "$(inst_state "$n")" "${sz:--}" \
               "${RUN_DIR#"$PROJECT_ROOT"/}/sysdir-$port"
    done
}

cmd_status() {
    local name="${1:-$DEFAULT_NAME}"; need_instance "$name"
    local port; port="$(instance_port "$name")"
    printf '实例       %s\n' "$name"
    printf '端口       %s\n' "$port"
    printf '状态       %s\n' "$(inst_state "$name")"
    printf '串口       adb -s emulator-%s\n' "$port"
    printf '工作目录   %s\n' "$(inst_sysdir "$name")"
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
    list|ls) cmd_list ;;
    status)  cmd_status  "${ARGS[0]:-$DEFAULT_NAME}" ;;
    *)       die "未知命令：$CMD（-h 看用法）" ;;
esac
