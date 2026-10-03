#!/usr/bin/env bash
# 导出/导入端到端：**导出的归档能不能真的把一台机器原样搬回来。**
#
# 判据不是"命令没报错"，而是：
#   · 归档大小 ≈ 实例实占（证明 tar --sparse 真的跳过了 48G 的空洞）
#   · 导入后能开机
#   · 开机后 guest 里那个标记文件**还在**（数据真的搬过来了）
#   · 再删掉原实例，确认恢复出来的是一台独立机器
#
# 全程用临时实例，绝不碰 5580。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
EMU=./scripts/emulator.sh
ADB=/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb
ARCHIVE=/root/AutoSnapshotAndroid/.tmp/export-test.tar
MARK="export-$$"
pass=0; fail=0
chk() { local d="$1"; shift; if "$@" >/dev/null 2>&1; then printf '  \033[1;32m✓\033[0m %s\n' "$d"; pass=$((pass+1)); else printf '  \033[1;31m✗\033[0m %s\n' "$d"; fail=$((fail+1)); fi; }
chk_out() { local d="$1" w="$2"; shift 2; local o; o="$("$@" 2>&1)"; if printf '%s' "$o" | grep -q -- "$w"; then printf '  \033[1;32m✓\033[0m %s\n' "$d"; pass=$((pass+1)); else printf '  \033[1;31m✗\033[0m %s\n    期望: %s\n    实际: %s\n' "$d" "$w" "$(printf '%s' "$o" | head -4 | tr '\n' '|')"; fail=$((fail+1)); fi; }

rm -f "$ARCHIVE"
$EMU delete t4 -y >/dev/null 2>&1
$EMU delete t5 -y >/dev/null 2>&1

echo "── [1] 造一台有数据的实例"
$EMU create t4 >/dev/null 2>&1
PORT=$($EMU list | awk '$1=="t4"{print $2}')
echo "   端口 $PORT"
timeout 180 $EMU start t4 >/dev/null 2>&1
$ADB -s "emulator-$PORT" shell "echo $MARK > /data/local/tmp/$MARK; sync"
echo "   标记已写入: [$($ADB -s "emulator-$PORT" shell "cat /data/local/tmp/$MARK" | tr -d '\r')]"
USED=$(du -sm ".run/sysdir-$PORT" | cut -f1)
echo "   工作目录实占: ${USED}MB"

echo
echo "── [2] 导出（会自动先停机 + sync）"
chk_out "export 报告已导出" "已导出" $EMU export t4 -o "$ARCHIVE"
chk_out "export 说明镜像未打包" "未打包" $EMU export t4 -o "$ARCHIVE" --force
chk "归档文件存在" test -s "$ARCHIVE"
ASZ=$(du -sm "$ARCHIVE" | cut -f1)
echo "   归档大小: ${ASZ}MB（实例实占 ${USED}MB）"
# 关键：稀疏处理生效的话，归档不该比实占大太多；
# 不生效的话会多出 48G（userdata-qemu.img 的表观大小）
chk "归档没有把 48G 空洞写进去（< 实例实占 × 2）" \
    test "$ASZ" -lt $((USED * 2 + 100))

echo
echo "── [3] inspect 不解包就能看内容"
chk_out "inspect 报出实例名" '"instance": "t4"' $EMU inspect "$ARCHIVE"
chk_out "inspect 报出 ROM 指纹" "romFingerprint" $EMU inspect "$ARCHIVE"
chk_out "inspect 列出内容" "INSTANCE-MANIFEST.json" $EMU inspect "$ARCHIVE"
chk "停机了（导出前自动停的）" bash -c "! $EMU list | awk '\$1==\"t4\"' | grep -q 运行中"

echo
echo "── [4] 删掉原实例，从归档恢复出一台新的"
$EMU delete t4 -y >/dev/null 2>&1
chk "原实例已删" bash -c "! test -d .run/sysdir-$PORT"
chk_out "import 报告已导入" "已导入" $EMU import "$ARCHIVE" -n t5
NEWPORT=$($EMU list | awk '$1=="t5"{print $2}')
echo "   t5 端口 $NEWPORT"
chk "恢复出来的工作目录在" test -d ".run/sysdir-$NEWPORT"
chk "userdata-qemu.img 落地了" test -s ".run/sysdir-$NEWPORT/userdata-qemu.img"
chk "qcow2 落地了" test -s ".run/sysdir-$NEWPORT/userdata-qemu.img.qcow2"
chk "镜像重新链上了（不是打包进去的）" test -L ".run/sysdir-$NEWPORT/system-qemu.img"
chk "**hardware-qemu.ini 没带过来**（绝对路径会指错机器）" test ! -e ".run/sysdir-$NEWPORT/hardware-qemu.ini"
chk "config.ini 用本地真源覆盖了" cmp -s ".run/sysdir-$NEWPORT/config.ini" emulator/config.ini
chk "归档里的 manifest 没散落在工作目录" test ! -e ".run/sysdir-$NEWPORT/INSTANCE-MANIFEST.json"

echo
echo "── [5] 开起来，看数据是不是真的搬过来了"
timeout 180 $EMU start t5 >/dev/null 2>&1
GOT=$($ADB -s "emulator-$NEWPORT" shell "cat /data/local/tmp/$MARK 2>&1" | tr -d '\r')
echo "   标记读回: [$GOT]"
chk "★ 恢复后的机器里数据还在" test "$GOT" = "$MARK"
chk "恢复后能开机" test "$($ADB -s "emulator-$NEWPORT" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1"

echo
echo "── [6] 边界"
chk_out "重名会拒绝" "已经存在" $EMU import "$ARCHIVE" -n t5
chk_out "不存在的归档会报错" "没有这个归档" $EMU inspect /tmp/nope.tar
chk_out "非本工具做的 tar 会被认出来" "不是本工具导出" bash -c "tar -cf /tmp/fake.tar -C /tmp t_heredoc.sh; $EMU inspect /tmp/fake.tar"

echo
echo "── [7] 收尾"
$EMU delete t5 -y >/dev/null 2>&1
rm -f "$ARCHIVE" /tmp/fake.tar
echo "   已清理"
printf '  ───────────────────────────────\n  通过 %d 项，失败 %d 项\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
