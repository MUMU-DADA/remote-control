#!/usr/bin/env bash
# 查"导出的归档恢复后数据不在"的根因：先只比文件，不开机。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
EMU=./scripts/emulator.sh
ADB=/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb
A=/root/AutoSnapshotAndroid/.tmp/dbg-export.tar
MARK="dbg-$$"

$EMU delete t6 -y >/dev/null 2>&1; $EMU delete t7 -y >/dev/null 2>&1

$EMU create t6 >/dev/null 2>&1
P6=$($EMU list | awk '$1=="t6"{print $2}')
timeout 180 $EMU start t6 >/dev/null 2>&1
$ADB -s "emulator-$P6" shell "echo $MARK > /data/local/tmp/$MARK; sync"
echo "标记写入 t6（端口 $P6）"

S6=".run/sysdir-$P6"
echo "== 原实例文件 =="
for f in userdata-qemu.img userdata-qemu.img.qcow2; do
    printf '  %-26s %14s  md5(前1M)=%s\n' "$f" "$(stat -c %s $S6/$f)" "$(head -c 1048576 $S6/$f | md5sum | cut -c1-12)"
done
echo "  qcow2 里的 backing: $(/opt/android/emu31b/emulator/qemu-img info $S6/userdata-qemu.img.qcow2 2>/dev/null | sed -n 's/^backing file: //p')"

rm -f "$A"
$EMU export t6 -o "$A" >/dev/null 2>&1
echo "导出完成: $(du -h "$A" | cut -f1)"
echo "== 归档里这两个文件的**表观大小**（tar -tv 第三列）=="
tar -tvf "$A" 2>/dev/null | grep -E "userdata-qemu.img(\.qcow2)?$" | awk '{printf "  %-46s %s\n", $NF, $3}'

$EMU import "$A" -n t7 >/dev/null 2>&1
P7=$($EMU list | awk '$1=="t7"{print $2}')
S7=".run/sysdir-$P7"
echo "== 恢复后文件（端口 $P7）=="
for f in userdata-qemu.img userdata-qemu.img.qcow2; do
    printf '  %-26s %14s  md5(前1M)=%s\n' "$f" "$(stat -c %s $S7/$f 2>/dev/null)" "$(head -c 1048576 $S7/$f 2>/dev/null | md5sum | cut -c1-12)"
done
echo "== 逐个比 =="
for f in userdata-qemu.img userdata-qemu.img.qcow2; do
    a=$(stat -c %s $S6/$f 2>/dev/null); b=$(stat -c %s $S7/$f 2>/dev/null)
    [ "$a" = "$b" ] && echo "  ✓ $f 大小一致 ($a)" || echo "  ✗ $f 大小不一致: 原 $a → 恢复 $b"
done
echo "== 内容 hash（大文件，慢，只做 qcow2 的前 200MB）=="
echo "  原:   $(head -c 209715200 $S6/userdata-qemu.img.qcow2 | md5sum | cut -c1-16)"
echo "  恢复: $(head -c 209715200 $S7/userdata-qemu.img.qcow2 | md5sum | cut -c1-16)"

echo "== 恢复出来的 qcow2 还能正常打开吗 =="
/opt/android/emu31b/emulator/qemu-img info --backing-chain "$S7/userdata-qemu.img.qcow2" 2>&1 | head -6 | sed 's/^/  /'

echo "== 恢复后开机，标记在不在 =="
timeout 180 $EMU start t7 >/dev/null 2>&1
echo "  标记: [$($ADB -s "emulator-$P7" shell "cat /data/local/tmp/$MARK 2>&1" | tr -d '\r')]"
echo "  /data 已用: $($ADB -s "emulator-$P7" shell 'df /data | tail -1' | tr -d '\r')"

echo "实例 t6(端口 $P6) / t7(端口 $P7) 保留，归档 $A 保留，供继续排查"
