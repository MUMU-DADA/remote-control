#!/usr/bin/env bash
# 验证：**已有的 userdata 镜像 + 改小的 disk.dataPartition.size** 会怎样？
#
# 为什么必须验：5580（用户正在用的那台）就是这个处境 —— 镜像比配置大，
# 而新默认可能更小。项目注释里说"会按旧尺寸沿用，改了不生效"，
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

echo "== 1. 建一台全新实例（使用当前默认数据分区）"
$EMU delete t1 -y >/dev/null 2>&1
$EMU create t1 2>&1 | grep -E "端口|数据分区"
PORT=$($EMU list | awk '$1=="t1"{print $2}')
echo "   端口 $PORT"

CONFIG_FILE=".run/sysdir-$PORT/config.ini"
CURRENT_SIZE=$(sed -n 's/^[[:space:]]*disk\.dataPartition\.size[[:space:]]*=[[:space:]]*\([^[:space:]]*\)[[:space:]]*$/\1/p' "$CONFIG_FILE" | tail -1)
case "$CURRENT_SIZE" in
    [0-9]*[Gg])
        CURRENT_VALUE=${CURRENT_SIZE%?}
        [ "$CURRENT_VALUE" -gt 1 ] 2>/dev/null || { echo "[x] 数据分区必须大于 1G：$CURRENT_SIZE" >&2; exit 1; }
        SMALLER_SIZE="$((CURRENT_VALUE / 2))G"
        ;;
    [0-9]*[Mm])
        CURRENT_VALUE=${CURRENT_SIZE%?}
        [ "$CURRENT_VALUE" -gt 1 ] 2>/dev/null || { echo "[x] 数据分区必须大于 1M：$CURRENT_SIZE" >&2; exit 1; }
        SMALLER_SIZE="$((CURRENT_VALUE / 2))M"
        ;;
    [0-9]*)
        [ "$CURRENT_SIZE" -gt 1 ] 2>/dev/null || { echo "[x] 数据分区必须大于 1：$CURRENT_SIZE" >&2; exit 1; }
        SMALLER_SIZE="$((CURRENT_SIZE / 2))"
        ;;
    *)
        echo "[x] 无法解析数据分区大小：$CURRENT_SIZE" >&2
        exit 1
        ;;
esac
echo "   当前数据分区：$CURRENT_SIZE；临时缩小验证值：$SMALLER_SIZE"

echo "== 2. 首次启动，看镜像建成多大"
timeout 180 $EMU start t1 >/dev/null 2>&1
echo "   userdata-qemu.img : $(img_size $PORT) 字节  （配置值 $CURRENT_SIZE）"
echo "   hardware-qemu.ini : $(part_size $PORT)"
echo "   guest /data       : $(guest_data $PORT)"
FIRST=$(img_size $PORT)

echo "== 3. 停掉，把该实例的 config.ini 改成 $SMALLER_SIZE"
$EMU stop t1 >/dev/null 2>&1
sed -i "s/^disk.dataPartition.size=.*/disk.dataPartition.size=$SMALLER_SIZE/" "$CONFIG_FILE"
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
    echo "      → 5580 那台重启不会动数据，但也不会缩到 $SMALLER_SIZE"
else
    echo "结论：镜像**变了**！$FIRST → $SECOND"
    echo "      → 改分区大小会真的动镜像，_data 有被重新格式化的风险"
fi

echo "== 5. 收尾"
$EMU delete t1 -y >/dev/null 2>&1
echo "   已删除临时实例 t1"
