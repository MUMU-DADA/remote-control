#!/usr/bin/env bash
# =============================================================================
# net-bridge.sh —— 把模拟器放到物理局域网上（"桥接模式"的宿主侧）
#
# 模拟器原生支持 TAP 桥接（`emulator -help | grep net-tap`）：
#     -net-tap <interface>          用这个 TAP 网卡承载 guest 的 eth0
#     -net-tap-script-up <script>   TAP 拉起时执行的脚本
# 本脚本负责宿主这半边：建一座桥、把物理上行网卡桥进去，让模拟器的 tap 直接
# 落在局域网的二层域里 —— guest 于是能从**真实 DHCP** 拿到 192.168.x.x，
# 而不是模拟器内置的 10.0.2.x（用户态 NAT / SLIRP）。
#
#   ./net-bridge.sh up         建桥并切换（默认带自动回滚）
#   ./net-bridge.sh confirm    确认没问题，撤掉自动回滚
#   ./net-bridge.sh down       还原成切换前的样子
#   ./net-bridge.sh status     看状态
#
# ⚠️ up 会把上行网卡（默认 ens33）的 IP 和默认路由搬到桥上，中间断链约 0.1 秒。
#    如果上行网卡正是你 SSH 进来的那块，你会短暂掉线 —— 所以 up 之后
#    **CONFIRM_WINDOW 秒内没有 confirm 就自动 down 还原**（走 systemd 瞬时定时器，
#    不依赖当前会话；会话断了照样会回滚）。
#
# 环境变量：
#   UPLINK=ens33        物理上行网卡（默认取默认路由所在网卡）
#   BRIDGE=br0          桥接口名（与 scripts/common.sh 的 NET_BRIDGE_IF 一致）
#   CONFIRM_WINDOW=90   自动回滚窗口（秒）
#
# 建完桥后，scripts/run-linux.sh 会自动带上 -net-tap 参数（它检查桥是否存在）。
# =============================================================================
set -uo pipefail

SCRIPT="$(readlink -f "${BASH_SOURCE[0]}")"
STATE=/run/autosnap-net-bridge.state
REVERT_UNIT=autosnap-bridge-revert

die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }
log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }

[ "$(id -u)" = 0 ] || die "要 root（建桥、搬 IP 都是特权操作）"
command -v ip >/dev/null || die "找不到 ip 命令（装 iproute2）"

BRIDGE="${BRIDGE:-${NET_BRIDGE_IF:-br0}}"
CONFIRM_WINDOW="${CONFIRM_WINDOW:-90}"
UPLINK="${UPLINK:-$(ip -4 route show default 2>/dev/null |
    awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')}"

bridge_exists() { [ -d "/sys/class/net/$BRIDGE/bridge" ]; }

# -----------------------------------------------------------------------------
do_up() {
    [ -n "$UPLINK" ] || die "探测不到上行网卡，显式指定：UPLINK=ens33 $0 up"
    [ -e "/sys/class/net/$UPLINK" ] || die "网卡不存在：$UPLINK"
    [ -e "$STATE" ] && die "已经处于桥接状态（$STATE）——先 $0 down"
    bridge_exists && die "$BRIDGE 已经存在（不是本脚本建的？）——先手动清掉"

    local cidr gw mac
    cidr="$(ip -4 -o addr show dev "$UPLINK" | awk '{print $4; exit}')"
    gw="$(ip -4 route show default dev "$UPLINK" | awk '{print $3; exit}')"
    mac="$(cat "/sys/class/net/$UPLINK/address")"
    [ -n "$cidr" ] || die "$UPLINK 上没有 IPv4 地址（先把它拉起来再桥）"

    umask 077
    cat > "$STATE" <<EOF
UPLINK=$UPLINK
BRIDGE=$BRIDGE
CIDR=$cidr
GW=$gw
MAC=$mac
EOF

    # 先武装自动回滚，再动网络 —— 顺序反了就没有安全网。
    systemctl stop "$REVERT_UNIT.timer" 2>/dev/null || true
    systemctl reset-failed "$REVERT_UNIT.service" 2>/dev/null || true
    if systemd-run --quiet --collect --unit="$REVERT_UNIT" \
                   --on-active="${CONFIRM_WINDOW}s" \
                   --description="AutoSnap 桥接自动回滚" \
                   "$SCRIPT" down >/dev/null 2>&1; then
        log "自动回滚已武装：${CONFIRM_WINDOW}s 后自动 down（除非 confirm）"
    else
        warn "自动回滚没武装成功（systemd-run 失败）—— 断链就只能靠带外恢复了，谨慎"
    fi

    log "建桥 $BRIDGE：上行 $UPLINK（$cidr via $gw，MAC $mac）"
    ip link add name "$BRIDGE" type bridge || die "建桥失败（br_netfilter/bridge 模块？）"
    # 继承上行的 MAC：上游交换机/网关看到的 MAC 不变，ARP 表不用重学
    ip link set "$BRIDGE" address "$mac"
    ip link set "$BRIDGE" type bridge forward_delay 0 2>/dev/null || true
    ip link set "$BRIDGE" up

    ip addr flush dev "$UPLINK"                 # ← 从这一刻起流量走桥
    ip link set "$UPLINK" master "$BRIDGE"
    ip link set "$UPLINK" up
    ip addr add "$cidr" dev "$BRIDGE"
    ip route replace default via "$gw" dev "$BRIDGE"

    echo
    log "已切到桥接：$BRIDGE = $cidr，端口 $(ls /sys/class/net/$BRIDGE/brif 2>/dev/null | tr '\n' ' ')"
    log "确认没问题就执行： $SCRIPT confirm      （否则 ${CONFIRM_WINDOW}s 后自动还原）"
}

# -----------------------------------------------------------------------------
do_down() {
    if [ ! -e "$STATE" ]; then
        warn "没有桥接状态文件（$STATE）—— 本来就没桥接，什么都不用做"
        systemctl stop "$REVERT_UNIT.timer" 2>/dev/null || true
        return 0
    fi
    # shellcheck disable=SC1090
    . "$STATE"

    systemctl stop "$REVERT_UNIT.timer" 2>/dev/null || true
    systemctl reset-failed "$REVERT_UNIT.service" 2>/dev/null || true
    log "还原：$BRIDGE → $UPLINK"

    ip link set "$UPLINK" nomaster 2>/dev/null || true
    if bridge_exists; then
        ip addr flush dev "$BRIDGE" 2>/dev/null || true
        ip link set "$BRIDGE" down 2>/dev/null || true
        ip link del "$BRIDGE" 2>/dev/null || true
    fi
    ip link set "$UPLINK" up
    ip addr add "$CIDR" dev "$UPLINK" 2>/dev/null || true
    [ -n "${GW:-}" ] && ip route replace default via "$GW" dev "$UPLINK"
    rm -f "$STATE"
    log "已还原：$(ip -4 -br addr show dev "$UPLINK" 2>/dev/null | tr -s ' ')"
    log "默认路由：$(ip route show default 2>/dev/null | head -1)"
}

# -----------------------------------------------------------------------------
do_confirm() {
    [ -e "$STATE" ] || die "当前不是桥接状态，没什么可确认的"
    # shellcheck disable=SC1090
    . "$STATE"
    if systemctl stop "$REVERT_UNIT.timer" 2>/dev/null; then
        log "自动回滚定时器已撤掉"
    else
        warn "没找到自动回滚定时器（可能已经触发过了）"
    fi
    systemctl reset-failed "$REVERT_UNIT.service" 2>/dev/null || true
    grep -q '^CONFIRMED=' "$STATE" 2>/dev/null || echo "CONFIRMED=1" >> "$STATE"
    log "已确认桥接。桥 $BRIDGE：$(ip -4 -br addr show dev "$BRIDGE" 2>/dev/null | tr -s ' ')"
    log "端口：$(ls /sys/class/net/$BRIDGE/brif 2>/dev/null | tr '\n' ' ')"
}

# -----------------------------------------------------------------------------
do_status() {
    # 已经桥接时默认路由指向 br0，直接探测会把自己显示成"上行" —— 从状态文件取真值
    if [ -e "$STATE" ]; then
        # shellcheck disable=SC1090
        . "$STATE"
    fi
    echo "上行    ：$UPLINK"
    echo "桥      ：$BRIDGE"
    if bridge_exists; then
        echo "  状态  ：$(cat /sys/class/net/$BRIDGE/operstate)  地址 $(ip -4 -br addr show dev "$BRIDGE" | awk '{print $3}')"
        echo "  端口  ：$(ls /sys/class/net/$BRIDGE/brif 2>/dev/null | tr '\n' ' ')"
    else
        echo "  状态  ：不存在（未桥接，模拟器走内置用户态 NAT）"
    fi
    if [ -e "$STATE" ]; then
        echo "自动回滚：$(systemctl is-active "$REVERT_UNIT.timer" 2>/dev/null)（窗口 ${CONFIRM_WINDOW}s）"
        grep -q '^CONFIRMED=' "$STATE" && echo "已确认  ：是" || echo "已确认  ：否 ← 还没 confirm"
    fi
    echo "上行现状：$(ip -4 -br addr show dev "$UPLINK" 2>/dev/null | tr -s ' ')"
    echo "默认路由：$(ip route show default 2>/dev/null | head -1)"
    echo
    echo "模拟器会不会走桥：$(bridge_exists && echo '会（run-linux.sh 会自动加 -net-tap）' || echo '不会')"
}

case "${1:-}" in
    up)      do_up ;;
    down)    do_down ;;
    confirm) do_confirm ;;
    status|"") do_status ;;
    -h|--help) sed -n '2,30p' "$SCRIPT" ;;
    *) die "未知参数：$1（up | down | confirm | status）" ;;
esac
