#!/usr/bin/env bash
# 换一种导出方式：用 qemu-img 把「raw backing + qcow2 覆盖层」**压平成一张
# 自包含的 qcow2**，而不是把两个文件原样塞进 tar。
#
# 为什么换：
#   现在的问题不是归档不忠实 —— 实测归档里 qcow2 的 md5 与源**逐字节相同**，
#   解出来也相同。但恢复出来的实例开机后 /data 是空的，而原实例重启却好好的。
#   剩下唯一可疑的是 **raw backing 那个 32G 稀疏文件**：
#     · 它的逻辑内容 md5 一致，但 tar 解开后的**实占块数**和源差了 8 块
#       （1126712 vs 1126704）—— 说明空洞布局还原得并不完全一样
#     · 我的合成稀疏测试（3G 文件 8MB 数据）是通过的，但真实文件
#       空洞多得多，GNU sparse 格式在极端情况下是有坑的
#
# qemu-img 路线的好处：
#   · 只存**已分配的簇**，天然紧凑，根本没有稀疏文件这回事
#   · -c 压缩再省一截
#   · 恢复时 convert -O raw 会重新写出正确的稀疏 raw 文件
#   · Linux / Windows 两侧模拟器包里都自带 qemu-img，跨平台一致
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
EMU=./scripts/emulator.sh
QI=/opt/android/emulator-new/emulator/qemu-img
[ -x "$QI" ] || QI=$(find /opt/android -name qemu-img -type f 2>/dev/null | head -1)
ADB=/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb
M="qi-$$"

echo "qemu-img: $QI"
$EMU delete tA -y >/dev/null 2>&1; $EMU delete tB -y >/dev/null 2>&1

$EMU create tA >/dev/null 2>&1
PA=$($EMU list | awk '$1=="tA"{print $2}')
echo "== tA 端口 $PA，启动并写标记"
timeout 200 $EMU start tA >/dev/null 2>&1
$ADB -s "emulator-$PA" shell "echo $M > /data/local/tmp/$M; echo $M > /sdcard/$M; sync"
echo "   写入: [$($ADB -s "emulator-$PA" shell "cat /data/local/tmp/$M" | tr -d '\r')]"
$EMU stop tA >/dev/null 2>&1

SA=".run/sysdir-$PA"
echo "== 压平成一张自包含 qcow2"
"$QI" convert -O qcow2 -c "$SA/userdata-qemu.img.qcow2" /root/AutoSnapshotAndroid/.tmp/flat.qcow2 2>&1 | head -2
echo "   扁平镜像: $(du -h /root/AutoSnapshotAndroid/.tmp/flat.qcow2 | cut -f1)"
echo "   原两个文件合计: $(du -shc "$SA/userdata-qemu.img" "$SA/userdata-qemu.img.qcow2" 2>/dev/null | tail -1 | cut -f1)"

echo "== 恢复到一个全新实例 tB：把扁平镜像转回 raw 当 backing"
$EMU create tB >/dev/null 2>&1
PB=$($EMU list | awk '$1=="tB"{print $2}')
SB=".run/sysdir-$PB"
timeout 120 "$QI" convert -O raw /root/AutoSnapshotAndroid/.tmp/flat.qcow2 "$SB/userdata-qemu.img" 2>&1 | head -2
echo "   恢复出的 backing: 表观 $(stat -c %s "$SB/userdata-qemu.img")  实占 $(stat -c %b "$SB/userdata-qemu.img") 块"
echo "   原 backing      : 表观 $(stat -c %s "$SA/userdata-qemu.img")  实占 $(stat -c %b "$SA/userdata-qemu.img") 块"
# 收尾：把 tA 的那份 qcow2 覆盖层删掉，让它就是从 raw 冷启动
rm -f "$SB/userdata-qemu.img.qcow2" "$SB/hardware-qemu.ini"

echo "== 启动 tB，看数据在不在"
timeout 200 $EMU start tB >/dev/null 2>&1
echo "   tB boot=$(timeout 12 $ADB -s emulator-$PB shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')"
echo "   tB /data/local/tmp: [$($ADB -s "emulator-$PB" shell "cat /data/local/tmp/$M 2>&1" | tr -d '\r')]"
echo "   tB /sdcard:         [$($ADB -s "emulator-$PB" shell "cat /sdcard/$M 2>&1" | tr -d '\r')]"
echo "   tB /data 已用: $($ADB -s "emulator-$PB" shell 'df /data | tail -1' | tr -d '\r')"

echo
echo "== 对照：tA 自己重启一次"
$EMU stop tA >/dev/null 2>&1
timeout 200 $EMU start tA >/dev/null 2>&1
echo "   tA 重启后标记: [$($ADB -s "emulator-$PA" shell "cat /data/local/tmp/$M 2>&1" | tr -d '\r')]"

echo
echo "实例 tA(端口 $PA) / tB(端口 $PB) 保留"
