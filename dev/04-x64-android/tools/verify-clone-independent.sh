#!/usr/bin/env bash
# 验证 clone 出来的实例与原实例**数据互不影响**。
#
# 为什么必须验：hardware-qemu.ini 里 disk.*.path 是**绝对路径**，
# 复制实例时如果把它一起抄过去，两台机器会指向同一份 userdata ——
# 那不是"复制"，是"两台机器共用一块硬盘"，写一个文件两边都变。
# emulator.sh 的 clone 会删掉 hardware-qemu.ini 让模拟器按新路径重建，
# 这个脚本就是去证明它真的重建对了。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ADB=/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb
MARK="clone-marker-$$"

echo "== 1. 等 dev3 开机"
for i in $(seq 1 40); do
    [ "$($ADB -s emulator-5584 shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break
    sleep 3
done
$ADB -s emulator-5584 shell "echo $MARK > /data/local/tmp/$MARK"
echo "   dev3 写入 /data/local/tmp/$MARK"
$ADB -s emulator-5584 shell "ls /data/local/tmp/$MARK" | tr -d '\r'

echo "== 2. 停 dev3，起 dev2"
./scripts/emulator.sh stop dev3 >/dev/null 2>&1 || ./scripts/emulator.sh kill dev3 >/dev/null 2>&1
./scripts/emulator.sh start dev2 >/dev/null 2>&1

echo "== 3. dev2 里**不该**有那个文件"
out=$($ADB -s emulator-5582 shell "ls /data/local/tmp/$MARK 2>&1" | tr -d '\r')
echo "   ls 结果: $out"
if printf '%s' "$out" | grep -q "No such file"; then
    echo "   ✓ 两台机器数据独立（clone 没有共用 userdata）"
    rc=0
else
    echo "   ✗ 两台机器共用了同一份 /data —— clone 的路径没重建对"
    rc=1
fi

echo "== 4. 两边的 qcow2 确实是两个文件"
ls -i .run/sysdir-5582/userdata-qemu.img.qcow2 .run/sysdir-5584/userdata-qemu.img.qcow2 2>/dev/null

echo "== 5. 收尾：停掉"
./scripts/emulator.sh stop dev2 >/dev/null 2>&1 || ./scripts/emulator.sh kill dev2 >/dev/null 2>&1
exit $rc
