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

die() { echo "[x] $*" >&2; exit 1; }

# ⚠️ 这个脚本的结尾是**只输出一个结论**的：
#      数据在磁盘上持久 / 数据没持久住（复现了未解之谜）
#    而下面这些前提原来**一个都没校验**：标记有没有真的写进去、第二次启动有没有起来。
#    任何一环出问题（adb 没就绪、串口不对、start 失败），$A/$B 就会是错误文本，
#    于是直接掉进 else 分支，把"环境没准备好"报告成
#    **"数据没持久住"** —— 那正是这个脚本存在的原因，也是最吓人的那个结论。
#    （2026-10-09：和 verify-clone-independent.sh 是同一个毛病，一起修。）
wait_boot() {   # $1 = 串口
    local i
    for i in $(seq 1 60); do
        [ "$($ADB -s "$1" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && return 0
        sleep 3
    done
    return 1
}

echo "== 1. 建并启动临时实例 t2"
$EMU delete t2 -y >/dev/null 2>&1
$EMU create t2 >/dev/null 2>&1
PORT=$($EMU list | awk '$1=="t2"{print $2}')
[ -n "$PORT" ] || die "查不到 t2 的端口（$EMU list 看看）"
echo "   端口 $PORT"
timeout 300 $EMU start t2 >/dev/null 2>&1 || die "起不来 t2"
wait_boot "emulator-$PORT" || die "t2 等不到 boot_completed=1"

echo "== 2. 往 guest 的 /data 里写点东西"
$ADB -s "emulator-$PORT" shell "echo hello-persist > /data/local/tmp/$MARK" || die "写 /data 标记失败"
$ADB -s "emulator-$PORT" shell "mkdir -p /sdcard/persisttest && echo hi > /sdcard/persisttest/$MARK" || die "写 /sdcard 标记失败"
# **必须读回来确认**：写没写进去和"重启后还在不在"是两个完全不同的问题，
# 不确认的话，写入失败会被后面的 else 分支报告成"数据没持久住"。
W1=$($ADB -s "emulator-$PORT" shell "cat /data/local/tmp/$MARK 2>/dev/null" | tr -d '\r')
W2=$($ADB -s "emulator-$PORT" shell "cat /sdcard/persisttest/$MARK 2>/dev/null" | tr -d '\r')
[ "$W1" = "hello-persist" ] || die "标记没写进 /data（读回是 [$W1]）—— 前提不成立，不下结论"
[ "$W2" = "hi" ] || die "标记没写进 /sdcard（读回是 [$W2]）—— 前提不成立，不下结论"
echo "   ✓ 两个标记都读回来了（/data=[$W1] /sdcard=[$W2]）"
USED_BEFORE=$($ADB -s "emulator-$PORT" shell "df /data | tail -1" | tr -d '\r' | awk '{print $3}')
echo "   标记: /data/local/tmp/$MARK 与 /sdcard/persisttest/$MARK"
echo "   /data 已用块: $USED_BEFORE"
echo "   qcow2: $(stat -c %s .run/sysdir-$PORT/userdata-qemu.img.qcow2 2>/dev/null) 字节"

echo "== 3. 优雅关机（就是 emulator.sh stop，等同用户"关机"）"
$EMU stop t2 2>&1 | tail -1
sleep 2
# ⚠️ **必须确认它真的停了。** 停机失败的话，下面那次"再启动"其实没重启
#    （实例已经在跑，start 会直接返回），标记当然还在 —— 于是脚本会得出
#    **"数据在磁盘上持久"** 这个**反向**的假结论。比报错坏得多。
$EMU list 2>/dev/null | awk '$1=="t2"' | grep -q "已停止" \
    || die "t2 没停下来 —— 没停机就没有'重启'可验，不下结论"
echo "   停机后磁盘上还在吗："
echo "     userdata-qemu.img        $(stat -c %s .run/sysdir-$PORT/userdata-qemu.img 2>/dev/null) 字节"
echo "     userdata-qemu.img.qcow2  $(stat -c %s .run/sysdir-$PORT/userdata-qemu.img.qcow2 2>/dev/null) 字节"

echo "== 4. 再启动（模拟"下次开机"）"
timeout 300 $EMU start t2 >/dev/null 2>&1 || die "第二次启动失败 —— 前提不成立，不下结论"
wait_boot "emulator-$PORT" || die "第二次开机等不到 boot_completed=1 —— 前提不成立，不下结论"
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
elif printf '%s' "$A$B" | grep -qi "No such file"; then
    echo "结论：**数据没持久住** —— 重启后 guest 是干净的（复现了历史记录里那条未解之谜）"
else
    # 不是"文件不在"，而是"读不出来"。这说明前提又崩了（adb / 串口 / guest 状态），
    # **不能**把它算成"数据没持久住" —— 那会把结论正好搞反。
    echo "[x] 读不回标记，但也**不是** No such file —— 无法判断，不下结论" >&2
    echo "    /data 读到:  [$A]" >&2
    echo "    /sdcard 读到:[$B]" >&2
    exit 1
fi

echo "== 5. 收尾"
$EMU delete t2 -y >/dev/null 2>&1
echo "   已删除临时实例 t2"
