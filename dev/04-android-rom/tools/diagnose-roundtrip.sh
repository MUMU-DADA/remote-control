#!/usr/bin/env bash
# 最小对照实验：把"任何重启都丢"和"导出/导入丢"分开。
#
#   A. t8 写入标记 → 重启 **同一个 t8** → 标记在吗？   （对照：重启本身丢不丢）
#   B. 再导出 t8 → 导入为 t9 → 重启 t9 → 标记在吗？   （实验：导出/导入丢不丢）
#
# 标记写两个地方：/data/local/tmp/（/data 分区）和 /sdcard/（也是同一个分区，
# 但走 MediaProvider，能看出是不是只有某条路径有问题）。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
EMU=./scripts/emulator.sh
ADB=/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb
AR=/root/AutoSnapshotAndroid/.tmp/roundtrip.tar
M="rt-$$"

$EMU delete t8 -y >/dev/null 2>&1; $EMU delete t9 -y >/dev/null 2>&1
rm -f "$AR"

$EMU create t8 >/dev/null 2>&1
P8=$($EMU list | awk '$1=="t8"{print $2}')
echo "t8 端口 $P8"

boot()  { timeout 200 $EMU start "$1" >/dev/null 2>&1; }
halt()  { $EMU stop "$1" >/dev/null 2>&1 || $EMU kill "$1" >/dev/null 2>&1; }
read2() { printf 'tmp=[%s] sdcard=[%s]' \
    "$($ADB -s "emulator-$1" shell "cat /data/local/tmp/$M 2>&1" | tr -d '\r')" \
    "$($ADB -s "emulator-$1" shell "cat /sdcard/$M 2>&1" | tr -d '\r')"; }

echo "== 第一次启动 t8，写标记"
boot t8
$ADB -s "emulator-$P8" shell "echo $M > /data/local/tmp/$M; echo $M > /sdcard/$M; sync"
echo "   刚写完: $(read2 $P8)"

echo "== A. 重启**同一个实例**（对照）"
halt t8
boot t8
echo "   A 重启后: $(read2 $P8)"

echo "== B. 导出 → 导入成 t9 → 启动"
halt t8
$EMU export t8 -o "$AR" >/dev/null 2>&1
echo "   归档: $(du -h "$AR" | cut -f1)"
$EMU import "$AR" -n t9 >/dev/null 2>&1
P9=$($EMU list | awk '$1=="t9"{print $2}')
echo "   t9 端口 $P9"
boot t9
echo "   B 导入后: $(read2 $P9)"

echo
echo "== 文件级对照（都为停机态才可比）=="
halt t9; sleep 2
for f in userdata-qemu.img userdata-qemu.img.qcow2 encryptionkey.img.qcow2 cache.img.qcow2; do
    a=$(md5sum ".run/sysdir-$P8/$f" 2>/dev/null | cut -c1-16)
    b=$(md5sum ".run/sysdir-$P9/$f" 2>/dev/null | cut -c1-16)
    [ "$a" = "$b" ] && s="一致" || s="**不同**"
    printf '  %-28s %s  %s / %s\n' "$f" "$s" "$a" "$b"
done

echo
echo "== 结论 =="
A=$(read2 $P8); B=$(read2 $P9)
echo "  A（原实例重启）: $A"
echo "  B（导入后启动）: $B"

echo
echo "实例 t8(端口 $P8) / t9(端口 $P9) 保留"
