#!/usr/bin/env bash
# 导出/导入：**导出可用，恢复未通过验证** —— 这个脚本验的就是这条现状。
#
# ⚠️ 2026-10-09 重写过一次期望。原来它断言"导入成功、而且 guest 里的数据还在"，
#    可产品**已经把这条路径主动禁掉了**：`import` 不加 `--unsafe` 会直接拒绝
#    （理由：恢复出来的实例 /data 是空的，见 docs/12 §7）。也就是说这个脚本
#    一直在验一个**产品刻意不做的事** —— 它当然失败，而且已经失败很久了，
#    只是因为没人跑到它（`run-all-tests.sh` 默认跳过这一档）才没被发现。
#
# 现在的判据：
#   · 归档大小 ≈ 实例实占（证明 tar --sparse 真的跳过了 48G 的空洞）
#   · `import` 不带 `--unsafe` → **必须拒绝**，并说清理由（这是安全闸，不能悄悄放行）
#   · `import --unsafe` → 放行，且恢复出来的工作目录结构正确（镜像重新链、config 用本地真源）
#   · **恢复出来的机器里没有原来的数据** —— 如实刻画这个已知缺口，
#     哪天它修好了，这一项会红，提醒把上面那条闸也一起改掉
#
# 全程用临时实例，绝不碰 5580。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
EMU=./scripts/emulator.sh
ADB=/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb
ARCHIVE=/root/AutoSnapshotAndroid/.tmp/export-test.tar
MARK="export-$$"
pass=0; fail=0

die() { echo "[x] $*" >&2; exit 1; }

# ⚠️ 这个脚本第 [5] 节会断言"恢复后的机器里数据还在"。
#    可第 [1] 节**写标记是没有校验的** —— 写失败（adb 没就绪 / 串口不对）时，
#    第 [5] 节读回来是空的，于是报成"✗ ★ 恢复后的机器里数据还在"：
#    **把前提缺失说成"数据没搬过来"**，正好把这套验证的结论搞反。
#    （和 verify-data-persistence.sh / verify-clone-independent.sh 同一个毛病。）
wait_boot() {
    local i
    for i in $(seq 1 60); do
        [ "$($ADB -s "$1" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && return 0
        sleep 3
    done
    return 1
}
chk() { local d="$1"; shift; if "$@" >/dev/null 2>&1; then printf '  \033[1;32m✓\033[0m %s\n' "$d"; pass=$((pass+1)); else printf '  \033[1;31m✗\033[0m %s\n' "$d"; fail=$((fail+1)); fi; }
chk_out() { local d="$1" w="$2"; shift 2; local o; o="$("$@" 2>&1)"; if printf '%s' "$o" | grep -q -- "$w"; then printf '  \033[1;32m✓\033[0m %s\n' "$d"; pass=$((pass+1)); else printf '  \033[1;31m✗\033[0m %s\n    期望: %s\n    实际: %s\n' "$d" "$w" "$(printf '%s' "$o" | head -4 | tr '\n' '|')"; fail=$((fail+1)); fi; }

rm -f "$ARCHIVE"
$EMU delete t4 -y >/dev/null 2>&1
$EMU delete t5 -y >/dev/null 2>&1

echo "── [1] 造一台有数据的实例"
$EMU create t4 >/dev/null 2>&1
PORT=$($EMU list | awk '$1=="t4"{print $2}')
[ -n "$PORT" ] || die "查不到 t4 的端口 —— 前提不成立"
echo "   端口 $PORT"
timeout 300 $EMU start t4 >/dev/null 2>&1 || die "起不来 t4 —— 前提不成立"
wait_boot "emulator-$PORT" || die "t4 等不到 boot_completed=1 —— 前提不成立"
$ADB -s "emulator-$PORT" shell "echo $MARK > /data/local/tmp/$MARK; sync" || die "写标记失败"
WROTE=$($ADB -s "emulator-$PORT" shell "cat /data/local/tmp/$MARK 2>/dev/null" | tr -d '\r')
[ "$WROTE" = "$MARK" ] || die "标记没写进去（读回 [$WROTE]）—— 前提不成立，不下结论"
echo "   标记已写入并读回确认: [$WROTE]"
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
# 实测：inspect 把 manifest 的 **JSON 正文**打出来，不是文件清单；
# 原来期望 "INSTANCE-MANIFEST.json" 永远匹配不上（过期期望）。
chk_out "inspect 打印 manifest 正文" '"format": 1' $EMU inspect "$ARCHIVE"
chk_out "inspect 打印归档内容清单" "内容（前 12 项）" $EMU inspect "$ARCHIVE"
chk "停机了（导出前自动停的）" bash -c "! $EMU list | awk '\$1==\"t4\"' | grep -q 运行中"

echo
echo "── [4] 删掉原实例，从归档恢复出一台新的"
$EMU delete t4 -y >/dev/null 2>&1
chk "原实例已删" bash -c "! test -d .run/sysdir-$PORT"

# 安全闸：不带 --unsafe 必须拒绝，而且理由要说清楚
chk_out "import 不带 --unsafe 会**拒绝执行**" "已拒绝执行" $EMU import "$ARCHIVE" -n t5
chk_out "拒绝时说明原因（未通过验证）" "未通过验证" $EMU import "$ARCHIVE" -n t5
chk "拒绝之后**没有**留下半个实例" bash -c "! $EMU list | awk '\$1==\"t5\"' | grep -q t5"

chk_out "import --unsafe 才放行" "已导入" $EMU import "$ARCHIVE" -n t5 --unsafe
NEWPORT=$($EMU list | awk '$1=="t5"{print $2}')
echo "   t5 端口 ${NEWPORT:-（空）}"
# ⚠️ **空值必须先挡住。** 2026-10-09 实测：导入被拒绝时 t5 根本没被创建，
#    NEWPORT 取到空串，后面拼出来的串口是 "emulator-" —— 而 `adb -s emulator-`
#    会**一直等一个不存在的设备**，把脚本挂死（日志停在 [5] 不动，看起来像"卡在开机"）。
#    空串绝不能进 adb 参数。
[ -n "$NEWPORT" ] || die "t5 没被创建（NEWPORT 为空）—— 导入没成功，后面的检查无从谈起"
chk "恢复出来的工作目录在" test -d ".run/sysdir-$NEWPORT"
chk "userdata-qemu.img 落地了" test -s ".run/sysdir-$NEWPORT/userdata-qemu.img"
chk "qcow2 落地了" test -s ".run/sysdir-$NEWPORT/userdata-qemu.img.qcow2"
chk "镜像重新链上了（不是打包进去的）" test -L ".run/sysdir-$NEWPORT/system-qemu.img"
chk "**hardware-qemu.ini 没带过来**（绝对路径会指错机器）" test ! -e ".run/sysdir-$NEWPORT/hardware-qemu.ini"
chk "config.ini 用本地真源覆盖了" cmp -s ".run/sysdir-$NEWPORT/config.ini" emulator/config.ini
chk "归档里的 manifest 没散落在工作目录" test ! -e ".run/sysdir-$NEWPORT/INSTANCE-MANIFEST.json"

echo
echo "── [5] 开起来，看数据是不是真的搬过来了"
timeout 300 $EMU start t5 >/dev/null 2>&1 || die "恢复出来的 t5 起不来 —— 前提不成立，不下结论"
wait_boot "emulator-$NEWPORT" || die "t5 等不到 boot_completed=1 —— 前提不成立，不下结论"
GOT=$($ADB -s "emulator-$NEWPORT" shell "cat /data/local/tmp/$MARK 2>&1" | tr -d '\r')
echo "   标记读回: [$GOT]"
chk "恢复后能开机" test "$($ADB -s "emulator-$NEWPORT" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1"
# 已知缺口：归档搬得走磁盘结构，搬不走 guest 里的数据。
# **这里断言"数据不在"是刻意的** —— 它是这个闸存在的理由。
# 哪天数据能过来了，这一项会红，提醒同时把上面那条 --unsafe 闸去掉。
chk "★ 已知缺口：恢复出来的机器**没有**原来的数据" \
    bash -c "printf '%s' \"$GOT\" | grep -q 'No such file'"

echo
echo "── [6] 边界"
# 要带 --unsafe：不带的话先被安全闸拦下，就测不到"重名"这一条了。
chk_out "重名会拒绝" "已经存在" $EMU import "$ARCHIVE" -n t5 --unsafe
chk_out "不存在的归档会报错" "没有这个归档" $EMU inspect /tmp/nope.tar
# ⚠️ 原来这里打的是 `tar -cf /tmp/fake.tar -C /tmp t_heredoc.sh` —— 依赖
#    **别人留在 /tmp 的文件**。实测那个文件根本不存在：tar 报错退出码 2，
#    但仍然造出了一个残缺的 tar，于是这一项是**靠运气通过**的 ——
#    换一台机器（或 tar 拒绝建归档时）就会挂，而失败信息完全指不到真因。
#    改成自己造素材，测试上下文自己负责。
FAKE_SRC=/tmp/autosnap-fake-$$; FAKE_TAR=/tmp/autosnap-fake-$$.tar
printf 'not-an-autosnap-archive\n' > "$FAKE_SRC"
tar -cf "$FAKE_TAR" -C /tmp "$(basename "$FAKE_SRC")" \
    || die "造不出负向测试用的 tar（这一步失败说明环境有问题，不是被测对象的问题）"
chk_out "非本工具做的 tar 会被认出来" "不是本工具导出" $EMU inspect "$FAKE_TAR"

echo
echo "── [7] 收尾"
$EMU delete t5 -y >/dev/null 2>&1
rm -f "$ARCHIVE" /tmp/fake.tar "$FAKE_SRC" "$FAKE_TAR"
echo "   已清理"
printf '  ───────────────────────────────\n  通过 %d 项，失败 %d 项\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
