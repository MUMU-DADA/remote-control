#!/usr/bin/env bash
# 验证：**停机再启动之后，guest 里的数据还在不在。**
#
# 为什么要专门验：项目早期记录过一条"模拟器 /data 不持久（根因未查明）"——
# 观察到 qcow2 覆盖层在磁盘上确实在长，但 guest 重启后却是全新的。
# 那条结论一直没被推翻也没被确认。用户正在 5580 上往 guest 里下 20+GB
# 的游戏数据，所以"关机后数据还在不在"必须拿实测回答，不能靠推测。
#
# 全程在临时实例上做，绝不碰 5580。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
EMU=./scripts/emulator.sh
ADB=/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb
MARK="persist-$$"

echo "== 1. 建并启动临时实例 t2"
$EMU delete t2 -y >/dev/null 2>&1
$EMU create t2 >/dev/null 2>&1
PORT=$($EMU list | awk '$1=="t2"{print $2}')
echo "   端口 $PORT"
timeout 180 $EMU start t2 >/dev/null 2>&1 || { echo "   起不来，放弃"; exit 1; }

echo "== 2. 往 guest 的 /data 里写点东西"
$ADB -s "emulator-$PORT" shell "echo hello-persist > /data/local/tmp/$MARK"
$ADB -s "emulator-$PORT" shell "mkdir -p /sdcard/persisttest && echo hi > /sdcard/persisttest/$MARK"
USED_BEFORE=$($ADB -s "emulator-$PORT" shell "df /data | tail -1" | tr -d '\r' | awk '{print $3}')
echo "   标记: /data/local/tmp/$MARK 与 /sdcard/persisttest/$MARK"
echo "   /data 已用块: $USED_BEFORE"
echo "   qcow2: $(stat -c %s .run/sysdir-$PORT/userdata-qemu.img.qcow2 2>/dev/null) 字节"

echo "== 3. 优雅关机（就是 emulator.sh stop，等同用户"关机"）"
$EMU stop t2 2>&1 | tail -1
sleep 2
echo "   停机后磁盘上还在吗："
echo "     userdata-qemu.img        $(stat -c %s .run/sysdir-$PORT/userdata-qemu.img 2>/dev/null) 字节"
echo "     userdata-qemu.img.qcow2  $(stat -c %s .run/sysdir-$PORT/userdata-qemu.img.qcow2 2>/dev/null) 字节"

echo "== 4. 再启动（模拟"下次开机"）"
timeout 180 $EMU start t2 >/dev/null 2>&1
echo "   标记文件还在吗："
A=$($ADB -s "emulator-$PORT" shell "cat /data/local/tmp/$MARK 2>&1" | tr -d '\r')
B=$($ADB -s "emulator-$PORT" shell "cat /sdcard/persisttest/$MARK 2>&1" | tr -d '\r')
echo "     /data/local/tmp/$MARK  → $A"
echo "     /sdcard/persisttest/$MARK → $B"
USED_AFTER=$($ADB -s "emulator-$PORT" shell "df /data | tail -1" | tr -d '\r' | awk '{print $3}')
echo "   /data 已用块: $USED_BEFORE → $USED_AFTER"

echo
if [ "$A" = "hello-persist" ] && [ "$B" = "hi" ]; then
    echo "结论：**数据在磁盘上持久**，停机再启动后 guest 依然看得到"
else
    echo "结论：**数据没持久住** —— 重启后 guest 是干净的（复现了历史记录里那条未解之谜）"
fi

echo "== 5. 收尾"
$EMU delete t2 -y >/dev/null 2>&1
echo "   已删除临时实例 t2"
