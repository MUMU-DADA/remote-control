#!/usr/bin/env bash
# 验证：**已有的 userdata 镜像 + 改小的 disk.dataPartition.size** 会怎样？
#
# 为什么必须验：5580（用户正在用的那台）就是这个处境 —— 镜像 48G，
# 而新默认写的是 32G。项目注释里说"会按旧尺寸沿用，改了不生效"，
# 但那句话是当初从 6G 改到 48G 时留下的，**没人验过反向（改小）**。
# 万一模拟器改成"重新格式化 /data"，用户那 20G 游戏就没了 ——
# 这种事不能靠推测。
#
# 全程在临时实例 t1 上做，绝不碰 5580。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
EMU=./scripts/emulator.sh
ADB=/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb

img_size() {   # 打印 userdata-qemu.img 的字节数
    stat -c %s ".run/sysdir-$1/userdata-qemu.img" 2>/dev/null || echo "不存在"
}
part_size() {  # hardware-qemu.ini 里报的
    sed -n 's/^disk.dataPartition.size = //p' ".run/sysdir-$1/hardware-qemu.ini" 2>/dev/null || echo "?"
}
guest_data() { # guest 看到的 /data 总容量
    $ADB -s "emulator-$1" shell "df -h /data | tail -1" 2>/dev/null | tr -d '\r'
}

echo "== 1. 建一台全新实例（新默认 = 32G）"
$EMU delete t1 -y >/dev/null 2>&1
$EMU create t1 2>&1 | grep -E "端口|数据分区"
PORT=$($EMU list | awk '$1=="t1"{print $2}')
echo "   端口 $PORT"

echo "== 2. 首次启动，看镜像建成多大"
timeout 180 $EMU start t1 >/dev/null 2>&1
echo "   userdata-qemu.img : $(img_size $PORT) 字节  （32G = 34359738368）"
echo "   hardware-qemu.ini : $(part_size $PORT)"
echo "   guest /data       : $(guest_data $PORT)"
FIRST=$(img_size $PORT)

echo "== 3. 停掉，把该实例的 config.ini 改成 16G"
$EMU stop t1 >/dev/null 2>&1
sed -i 's/^disk.dataPartition.size=.*/disk.dataPartition.size=16G/' ".run/sysdir-$PORT/config.ini"
grep '^disk.dataPartition.size' ".run/sysdir-$PORT/config.ini" | sed 's/^/   实例 config.ini: /'

echo "== 4. 再启动，看它怎么办"
timeout 180 $EMU start t1 >/dev/null 2>&1
rc=$?
SECOND=$(img_size $PORT)
echo "   退出码 $rc"
echo "   userdata-qemu.img : $SECOND 字节"
echo "   hardware-qemu.ini : $(part_size $PORT)"
echo "   guest /data       : $(guest_data $PORT)"
echo
if [ "$FIRST" = "$SECOND" ]; then
    echo "结论：镜像**没被改小**，仍按旧尺寸沿用（项目注释的说法成立）"
    echo "      → 5580 那台重启不会动数据，但也不会缩到 32G"
else
    echo "结论：镜像**变了**！$FIRST → $SECOND"
    echo "      → 改分区大小会真的动镜像，_data 有被重新格式化的风险"
fi

echo "== 5. 收尾"
$EMU delete t1 -y >/dev/null 2>&1
echo "   已删除临时实例 t1"
