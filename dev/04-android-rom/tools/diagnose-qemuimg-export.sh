#!/usr/bin/env bash
# 换一种导出方式：用 qemu-img 把「raw backing + qcow2 覆盖层」**压平成一张
# 自包含的 qcow2**，而不是把两个文件原样塞进 tar。
#
# 为什么换：
#   现在的问题不是归档不忠实 —— 实测归档里 qcow2 的 md5 与源**逐字节相同**，
#   解出来也相同。但恢复出来的实例开机后 /data 是空的，而原实例重启却好好的。
#   剩下唯一可疑的是 **raw backing 那个 64 GiB 稀疏文件**（实占约 550MB）：
#     · 它的逻辑内容 md5 一致，但 tar 解开后的**实占块数**和源差了 8 块
#       （1126712 vs 1126704）—— 当初据此怀疑空洞布局还原得不完全一样
#     · 我的合成稀疏测试（3G 文件 8MB 数据）是通过的，但真实文件
#       空洞多得多，GNU sparse 格式在极端情况下是有坑的
#
#   ⚠️ 2026-10-09 复核：**这条假设没能复现。** 拿真实的 userdata-qemu.img
#      （64 GiB 表观 / 550MB 实占 / 1126904 块）做 tar -cS / -xS 往返，
#      解出来**还是 1126904 块，差 0**。所以"tar 丢掉空洞布局"不是根因。
#      同一轮还确认了：覆盖层在 sysdir 里、会被打包（arch_exclude 不排它），
#      而且它的 backing 引用是**相对路径**，换目录也接得上。
#      真因仍未找到 —— 见 docs/12 §7。
#
# ✅ 2026-10-09 跑通了（结果见下，也写进了 docs/12 §7）：
#       tB /data/local/tmp = [qi-731324]、/sdcard = [qi-731324]   ← 数据在
#       对照 tA 自己重启 = [qi-731324]                            ← 对照组也对
#       退出码=0
#    **这条路线是可行的** —— 压平成一张自包含 qcow2、再转回 raw 当 backing，
#    guest 数据完好。而 tar 那条路线恢复出来 /data 是空的。
#
#    ⚠️ 顺带纠正：头部原来把"实占块数差 8 块"当作保真度损失的证据，**方向错了**。
#       实测（a）tar 往返真实文件：差 **0** 块；
#            （b）压平路线里 1,126,904 → 351,528 块，**而数据完好**。
#       块数会变是因为 convert **跳过全零簇** + `-c` 压缩 —— 它是压缩策略的
#       产物，和"数据在不在"无关。
#
# qemu-img 路线的好处：
#   · 只存**已分配的簇**，天然紧凑，根本没有稀疏文件这回事
#   · -c 压缩再省一截
#   · 恢复时 convert -O raw 会重新写出正确的稀疏 raw 文件
#   · Linux / Windows 两侧模拟器包里都自带 qemu-img，跨平台一致
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
EMU=./scripts/emulator.sh
# ── 前提：这个探针要**再起两个模拟器**并转换 64 GiB 镜像 ──────────────
#    2026-10-09 复核时这台机器只剩 3.8G 可用、可用内存 8G，**跑不动** ——
#    而脚本原来一声不响就往下走。前提立不住就当场说清楚，别让人以为"跑过了"。
#    磁盘要按**仓库所在的分区**算 —— 2026-10-09 我先量错了对象：
#    `/root` 只剩 3.8G，而仓库挂在另一块盘上（458G/可用 113G）。
#    用 `.`（脚本自己所在的分区）才是对的。
NEED_DISK_MB=12000        # 一次 convert(-c) + 转回 raw 的余量
#    内存**量过**再定：一个 --memory 6144 的实例实测 RSS 5.2GB（`default` 那份）。
#    探针同时只多这一个（起 tA→写→停→转换→起 tB），所以 6500MB 够，留了余量。
#    （前两版是拍的：先按"两个同时跑"估 14000，又凭感觉改 9000 —— 都用不住。）
NEED_MEM_MB=6500
_free_mb=$(df -Pm . | awk 'NR==2{print $4}')
_avail_mb=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
if [ "${_free_mb:-0}" -lt "$NEED_DISK_MB" ]; then
    echo "[x] 磁盘不够：需要约 ${NEED_DISK_MB}MB，当前可用 ${_free_mb}MB"
    echo "    这个探针会用 qemu-img 转换 64GiB 的 userdata 镜像，装不下就别硬跑。"
    exit 2
fi
if [ "${_avail_mb:-0}" -lt "$NEED_MEM_MB" ]; then
    echo "[x] 内存可能不够：建议可用 ≥ ${NEED_MEM_MB}MB，当前 ${_avail_mb}MB"
    echo "    它会同时起 tA 与 tB 两个模拟器。"
    exit 2
fi

QI=/opt/android/emulator-new/emulator/qemu-img
[ -x "$QI" ] || QI=$(find /opt/android -name qemu-img -type f 2>/dev/null | head -1)
ADB=/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb
M="qi-$$"

echo "qemu-img: $QI"
$EMU delete tA -y >/dev/null 2>&1; $EMU delete tB -y >/dev/null 2>&1

$EMU create tA >/dev/null 2>&1
PA=$($EMU list | awk '$1=="tA"{print $2}')
echo "== tA 端口 $PA，启动并写标记"
timeout 200 $EMU start tA >/dev/null 2>&1
$ADB -s "emulator-$PA" shell "echo $M > /data/local/tmp/$M; echo $M > /sdcard/$M; sync"
echo "   写入: [$($ADB -s "emulator-$PA" shell "cat /data/local/tmp/$M" | tr -d '\r')]"
$EMU stop tA >/dev/null 2>&1

SA=".run/sysdir-$PA"
echo "== 压平成一张自包含 qcow2"
"$QI" convert -O qcow2 -c "$SA/userdata-qemu.img.qcow2" /root/AutoSnapshotAndroid/.tmp/flat.qcow2 2>&1 | head -2
echo "   扁平镜像: $(du -h /root/AutoSnapshotAndroid/.tmp/flat.qcow2 | cut -f1)"
echo "   原两个文件合计: $(du -shc "$SA/userdata-qemu.img" "$SA/userdata-qemu.img.qcow2" 2>/dev/null | tail -1 | cut -f1)"

echo "== 恢复到一个全新实例 tB：把扁平镜像转回 raw 当 backing"
$EMU create tB >/dev/null 2>&1
PB=$($EMU list | awk '$1=="tB"{print $2}')
SB=".run/sysdir-$PB"
timeout 120 "$QI" convert -O raw /root/AutoSnapshotAndroid/.tmp/flat.qcow2 "$SB/userdata-qemu.img" 2>&1 | head -2
echo "   恢复出的 backing: 表观 $(stat -c %s "$SB/userdata-qemu.img")  实占 $(stat -c %b "$SB/userdata-qemu.img") 块"
echo "   原 backing      : 表观 $(stat -c %s "$SA/userdata-qemu.img")  实占 $(stat -c %b "$SA/userdata-qemu.img") 块"
# 收尾：把 tA 的那份 qcow2 覆盖层删掉，让它就是从 raw 冷启动
rm -f "$SB/userdata-qemu.img.qcow2" "$SB/hardware-qemu.ini"

echo "== 启动 tB，看数据在不在"
timeout 200 $EMU start tB >/dev/null 2>&1
echo "   tB boot=$(timeout 12 $ADB -s emulator-$PB shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')"
echo "   tB /data/local/tmp: [$($ADB -s "emulator-$PB" shell "cat /data/local/tmp/$M 2>&1" | tr -d '\r')]"
echo "   tB /sdcard:         [$($ADB -s "emulator-$PB" shell "cat /sdcard/$M 2>&1" | tr -d '\r')]"
echo "   tB /data 已用: $($ADB -s "emulator-$PB" shell 'df /data | tail -1' | tr -d '\r')"

echo
echo "== 对照：tA 自己重启一次"
$EMU stop tA >/dev/null 2>&1
timeout 200 $EMU start tA >/dev/null 2>&1
echo "   tA 重启后标记: [$($ADB -s "emulator-$PA" shell "cat /data/local/tmp/$M 2>&1" | tr -d '\r')]"

echo
echo "实例 tA(端口 $PA) / tB(端口 $PB) 保留"
