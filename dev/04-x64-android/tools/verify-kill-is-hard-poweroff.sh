#!/usr/bin/env bash
# 决定性实验：`adb emu kill` 到底是不是"硬断电"？
#
# 两次结果不一致：
#   t2（失败）写入标记后**没 sync** 就关机 → 重启后标记没了
#   t3（成功）写入标记后 **sync** 了   → 重启后标记还在
# 一个样本各一次不够，这里在**同一台实例**上做对照：
#   A: 写完立刻 sync 再关  → 应该活
#   B: 写完**不 sync**直接关 → 如果死了，就证明 emu kill 是硬断电
# 两次之间都重启一遍，互不干扰。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
EMU=./scripts/emulator.sh
ADB=/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb
PORT=$($EMU list | awk '$1=="t3"{print $2}')
[ -n "$PORT" ] || { echo "t3 不在，先跑 diagnose-persistence.sh"; exit 1; }
S="emulator-$PORT"

boot()   { timeout 180 $EMU start t3 >/dev/null 2>&1; }
stopit() { $EMU stop t3 >/dev/null 2>&1; }
read1()  { $ADB -s "$S" shell "cat /data/local/tmp/$1 2>&1" | tr -d '\r'; }

echo "== A: 写入后 **sync**，再关机 =="
boot
$ADB -s "$S" shell "echo A-ok > /data/local/tmp/markA; sync"
echo "   写好了: [$(read1 markA)]"
stopit
boot
echo "   重启后 markA = [$(read1 markA)]"

echo
echo "== B: 写入后 **不 sync**，立刻关机 =="
$ADB -s "$S" shell "echo B-ok > /data/local/tmp/markB"
echo "   写好了: [$(read1 markB)]"
stopit
boot
echo "   重启后 markB = [$(read1 markB)]"

echo
echo "== C: 不 sync 但**等 15 秒**再关机（让 ext4 自己提交）=="
$ADB -s "$S" shell "echo C-ok > /data/local/tmp/markC"
echo "   写好了: [$(read1 markC)]，等 15 秒"
sleep 15
stopit
boot
echo "   重启后 markC = [$(read1 markC)]"

echo
echo "=========== 结论 ==========="
echo "  A（sync 后再关）      : [$(read1 markA)]"
echo "  B（不 sync 立刻关）   : [$(read1 markB)]"
echo "  C（不 sync 等 15 秒） : [$(read1 markC)]"
echo
echo "  A 活说明数据能持久；B 死说明 emu kill 是硬断电；"
echo "  C 活说明只是最后几秒没落盘，不是整块盘丢了。"
