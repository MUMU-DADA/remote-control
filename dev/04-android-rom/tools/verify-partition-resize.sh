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

die() { echo "[x] $*" >&2; exit 1; }

# ⚠️ 这个脚本的结论只有两种，其中一种会**确认项目注释里的说法**：
#      "镜像没被改小，仍按旧尺寸沿用（项目注释的说法成立）"
#    而 FIRST/SECOND 来自 img_size，取不到时它返回的是字符串"不存在" ——
#    于是"两次都取不到"会让 FIRST == SECOND，正好落进"结论：没被改小"那一支：
#    **前提崩了，却确认了本该检验的假设。** 这是最坏的一种假绿。
#    （2026-10-09：和 verify-clone-independent.sh / verify-data-persistence.sh 同源，一起修。）
wait_boot() {
    local i
    for i in $(seq 1 60); do
        [ "$($ADB -s "$1" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && return 0
        sleep 3
    done
    return 1
}
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
# 原来这里没有判空：create 失败时 PORT 为空，要等到 CONFIG_FILE 取不到内容
# 才在下游炸 —— 中间隔了十几行，报错完全指不到真因。
[ -n "$PORT" ] || die "查不到 t1 的端口（create 没成功？）—— 前提不成立，不下结论"
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
timeout 300 $EMU start t1 >/dev/null 2>&1 || die "首次启动失败 —— 前提不成立，不下结论"
wait_boot "emulator-$PORT" || die "首次开机等不到 boot_completed=1 —— 前提不成立，不下结论"
echo "   userdata-qemu.img : $(img_size $PORT) 字节  （配置值 $CURRENT_SIZE）"
echo "   hardware-qemu.ini : $(part_size $PORT)"
echo "   guest /data       : $(guest_data $PORT)"
FIRST=$(img_size $PORT)
# 取不到就不能比 —— "不存在" == "不存在" 会伪装成"没被改小"
case "$FIRST" in ""|*不存在*) die "首次启动后读不到 userdata-qemu.img 大小（FIRST=[$FIRST]）—— 不下结论" ;; esac

echo "== 3. 停掉，把该实例的 config.ini 改成 $SMALLER_SIZE"
$EMU stop t1 >/dev/null 2>&1 || die "停不掉 t1 —— 前提不成立，不下结论"
sed -i "s/^disk.dataPartition.size=.*/disk.dataPartition.size=$SMALLER_SIZE/" "$CONFIG_FILE"
grep '^disk.dataPartition.size' ".run/sysdir-$PORT/config.ini" | sed 's/^/   实例 config.ini: /'

echo "== 4. 再启动，看它怎么办"
timeout 300 $EMU start t1 >/dev/null 2>&1
rc=$?
SECOND=$(img_size $PORT)
# 这一次"起不来"本身就是结论的一部分（改小分区可能导致起不来），所以不 die，
# 但必须记下来，并在下面按"起没起来"分开说，不能混进"镜像变没变"。
if [ "$rc" = 0 ]; then
    wait_boot "emulator-$PORT" || echo "   [!] 起来了但没等到 boot_completed=1"
fi
echo "   退出码 $rc"
echo "   userdata-qemu.img : $SECOND 字节"
echo "   hardware-qemu.ini : $(part_size $PORT)"
echo "   guest /data       : $(guest_data $PORT)"
echo
case "$SECOND" in ""|*不存在*)
    echo "[x] 第二次启动后读不到 userdata-qemu.img（SECOND=[$SECOND]）" >&2
    echo "    注意：**不能**据此说'镜像没变' —— 那是把它误读成了结论。" >&2
    echo "    第二次启动退出码 $rc。" >&2
    exit 1
    ;;
esac
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
