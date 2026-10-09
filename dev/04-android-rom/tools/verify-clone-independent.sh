#!/usr/bin/env bash
# 验证 clone 出来的实例与原实例**数据互不影响**。
#
# 为什么必须验：hardware-qemu.ini 里 disk.*.path 是**绝对路径**，
# 复制实例时如果把它一起抄过去，两台机器会指向同一份 userdata ——
# 那不是"复制"，是"两台机器共用一块硬盘"，写一个文件两边都变。
# emulator.sh 的 clone 会删掉 hardware-qemu.ini 让模拟器按新路径重建，
# 这个脚本就是去证明它真的重建对了。
#
# ⚠️ 2026-10-09 修了两个会让结论**不可信**的问题：
#
#   1. **前提缺失被误报成缺陷。** 原来它不建任何实例，直接
#      `start dev2` / `stop dev3`，而那对实例得之前手工建好；
#      前提不在时 `>/dev/null 2>&1` 把错误吞掉，脚本最后照样打印
#      "✗ 两台机器共用了同一份 /data" —— 把"没建实例"诊断成最严重的
#      数据共用缺陷。现在它**自己建**：create dev2 → clone dev3。
#
#   2. **端口写死 5582/5584。** 本机这两个端口上经常有别的东西
#      （实测 5582 上就有一个 .tmp/b5 的测试实例）。那样脚本会去问
#      **错的对象**，甚至可能靠问错对象得出一个漂亮的 "✓ 数据独立"。
#      现在端口从 `emulator.sh status` 的串口行里取，两边不同才继续。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ADB=/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb
MARK="clone-marker-$$"
EMU=./scripts/emulator.sh

die() { echo "[x] $*" >&2; exit 1; }

has_instance() { $EMU status "$1" >/dev/null 2>&1; }

# 从 status 输出里取串口：产品会打印 "串口  adb -s emulator-5582" 这样的行
port_of() {
    $EMU status "$1" 2>/dev/null |
        sed -n 's/.*emulator-\([0-9][0-9]*\).*/\1/p' | head -1
}

echo "== 0. 准备这一对实例（没有就自己建）"
if ! has_instance dev2; then
    echo "   dev2 不存在，创建它"
    $EMU create dev2 >/dev/null 2>&1 || die "create dev2 失败"
fi
if ! has_instance dev3; then
    echo "   dev3 不存在，从 dev2 克隆一个"
    $EMU clone dev2 dev3 >/dev/null 2>&1 || die "clone dev2 dev3 失败"
fi

P2="$(port_of dev2)"; P3="$(port_of dev3)"
[ -n "$P2" ] || die "查不到 dev2 的端口（$EMU status dev2 看看）"
[ -n "$P3" ] || die "查不到 dev3 的端口"
[ "$P2" != "$P3" ] || die "dev2 和 dev3 的端口都是 $P2 —— 它们不是两个实例"
S2="emulator-$P2"; S3="emulator-$P3"
echo "   dev2 → $S2 ，dev3 → $S3"

echo "== 1. 起 dev3 并等它开机"
$EMU start dev3 || die "起不来 dev3"
for i in $(seq 1 60); do
    [ "$($ADB -s "$S3" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break
    sleep 3
done
[ "$($ADB -s "$S3" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] \
    || die "dev3 等不到 boot_completed=1"
$ADB -s "$S3" shell "echo $MARK > /data/local/tmp/$MARK" || die "往 dev3 写标记失败"
echo "   dev3 写入 /data/local/tmp/$MARK"
$ADB -s "$S3" shell "ls /data/local/tmp/$MARK" | tr -d '\r'

echo "== 2. 停 dev3，起 dev2"
$EMU stop dev3 || $EMU kill dev3 || die "停不掉 dev3"
$EMU start dev2 || die "起不来 dev2"
for i in $(seq 1 60); do
    [ "$($ADB -s "$S2" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break
    sleep 3
done
[ "$($ADB -s "$S2" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] \
    || die "dev2 等不到 boot_completed=1"

echo "== 3. dev2 里**不该**有那个文件"
out=$($ADB -s "$S2" shell "ls /data/local/tmp/$MARK 2>&1" | tr -d '\r')
echo "   ls 结果: $out"
if printf '%s' "$out" | grep -q "No such file"; then
    echo "   ✓ 两台机器数据独立（clone 没有共用 userdata）"
    rc=0
else
    echo "   ✗ 两台机器共用了同一份 /data —— clone 的路径没重建对"
    rc=1
fi

echo "== 4. 两边的 qcow2 确实是两个文件"
ls -i ".run/sysdir-$P2/userdata-qemu.img.qcow2" ".run/sysdir-$P3/userdata-qemu.img.qcow2" 2>/dev/null

echo "== 5. 收尾：停掉"
$EMU stop dev2 >/dev/null 2>&1 || $EMU kill dev2 >/dev/null 2>&1
exit $rc
