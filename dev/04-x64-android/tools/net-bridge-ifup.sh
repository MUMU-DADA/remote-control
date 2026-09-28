#!/bin/sh
# =============================================================================
# net-bridge-ifup.sh —— 模拟器（QEMU）拉起 TAP 网卡时回调的脚本
#
# 由 scripts/run-linux.sh 通过 `-net-tap-script-up` 指到这里，QEMU 会把
# TAP 接口名作为 $1 传进来。这里只做一件事：把它挂进 tools/net-bridge.sh
# 建好的那座桥。
#
# 桥必须**先**建好（tools/net-bridge.sh up）—— run-linux.sh 只在
# /sys/class/net/<桥>/bridge 存在时才加 -net-tap 参数，所以正常顺序不会缺桥。
# =============================================================================
BR="${NET_BRIDGE_IF:-br0}"

if [ ! -d "/sys/class/net/$BR/bridge" ]; then
    echo "net-bridge-ifup: 桥 $BR 不存在 —— 先跑 tools/net-bridge.sh up" >&2
    exit 1
fi

ip link set "$1" master "$BR" || exit 1
ip link set "$1" up
exit 0
