#!/usr/bin/env bash
# ROM 自检：构建完成后、启动之前，把"该在的东西在不在、值对不对"一次性核对掉。
#
#   ./verify-rom.sh
#
# 检查五大类：
#   1. 镜像产物齐全（system/vendor/product/ramdisk/initrd/kernel-ranchu…）
#   2. 翻译层落位（21 个翻译器 + 61 个 arm64 侧车 + rc + ld.config + binfmt 规则）
#   3. **arm64 侧车字节一致性**：构建自己编的 arm64 库 vs 载荷里的同名文件
#      （实测应完全相同——两者都是 AOSP 的确定性产物，见 docs/01-design.md §3.1）
#   4. 三处 build.prop 的属性（abilist64 / isa / exec / native.bridge）
#   5. 首启风险提示（init.rc 是否已打 encryption 补丁）
#
set -uo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../scripts" && pwd)/common.sh"

SYS="$PRODUCT_OUT/system"
BRIDGE="$DEVICE_DST/remote_control_x64_arm64/bridge/system"
fails=0; warns=0

ok()   { printf '  \033[1;32m[✓]\033[0m %s\n' "$*"; }
bad()  { printf '  \033[1;31m[✗]\033[0m %s\n' "$*"; fails=$((fails+1)); }
warn() { printf '  \033[1;33m[!]\033[0m %s\n' "$*"; warns=$((warns+1)); }
hr()   { printf '\033[1;36m── %s\033[0m\n' "$1"; }

hr "1. 镜像产物"
for f in system.img vendor.img product.img ramdisk.img initrd kernel-ranchu encryptionkey.img userdata.img; do
    if [ -s "$PRODUCT_OUT/$f" ]; then ok "$f  $(du -h "$PRODUCT_OUT/$f" | cut -f1)"
    else
        case "$f" in system.img|vendor.img|ramdisk.img|kernel-ranchu) bad "$f 缺失（必需）" ;;
        *) warn "$f 缺失（可选）" ;; esac
    fi
done
[ -s "$PRODUCT_OUT/system/build.prop" ] && ok "system/build.prop（模拟器识别 guest 架构要用）" \
    || bad "system/build.prop 缺失 —— 模拟器会退回宿主架构"

hr "2. 翻译层落位"
n_tr=$(ls "$SYS/lib64"/libndk_translation*.so 2>/dev/null | wc -l)
[ "$n_tr" -eq 21 ] && ok "翻译器 + proxy：21 个" || bad "翻译器 + proxy：$n_tr 个（期望 21）"
for f in \
    bin/ndk_translation_program_runner_binfmt_misc_arm64 \
    bin/arm64/linker64 bin/arm64/app_process64 \
    etc/init/ndk_translation.rc etc/ld.config.arm.txt etc/ld.config.arm64.txt \
    etc/binfmt_misc/arm_exe etc/binfmt_misc/arm_dyn \
    etc/binfmt_misc/arm64_exe etc/binfmt_misc/arm64_dyn ; do
    [ -e "$SYS/$f" ] && ok "$f" || bad "$f 缺失"
done
[ -x "$SYS/bin/ndk_translation_program_runner_binfmt_misc_arm64" ] \
    && ok "binfmt 执行器有可执行位（0755）" || bad "binfmt 执行器没有可执行位 —— binfmt 注册会失败"
n_side=$(ls "$SYS/lib64/arm64" 2>/dev/null | wc -l)
[ "$n_side" -ge 59 ] && ok "arm64 侧车库：$n_side 个" || warn "arm64 侧车库：$n_side 个（期望 ≥59）"

hr "3. arm64 侧车字节一致性（构建产出 vs 载荷）"
same=0; diff=0; only_build=0
if [ -d "$SYS/lib64/arm64" ]; then
    for f in "$SYS/lib64/arm64"/*; do
        b=$(basename "$f")
        if [ -e "$BRIDGE/lib64/arm64/$b" ]; then
            if cmp -s "$f" "$BRIDGE/lib64/arm64/$b"; then same=$((same+1)); else diff=$((diff+1)); printf '      \033[1;33m差异\033[0m %s\n' "$b"; fi
        else
            only_build=$((only_build+1))
        fi
    done
fi
if [ "$diff" -eq 0 ] && [ "$same" -gt 0 ]; then
    ok "$same 个文件与载荷逐字节相同（另有 $only_build 个只由构建产出）"
    ok "→ 说明这些 arm64 库是 AOSP 自己编的，与 x86_64 框架同源（docs/01-design.md §3.1）"
else
    [ "$diff" -gt 0 ] && warn "$diff 个文件与载荷不同（可能框架版本有差异，需关注翻译层兼容性）"
    [ "$same" -eq 0 ] && warn "没有可比对的侧车文件"
fi

hr "4. 三处 build.prop 属性"
chk_prop() {  # 文件 属性 期望
    # 末尾的 `|| true` 是必须的：common.sh 开了 set -e，
    # 文件不存在时 grep 返回 2 会把整个脚本带走（踩过，退出码 2 且无任何输出）
    local got; got=$(grep -m1 "^$2=" "$1" 2>/dev/null | cut -d= -f2- || true)
    if [ "$got" = "$3" ]; then ok "$2 = $got"
    else bad "$2 = '${got:-<无>}'（期望 $3）"; fi
}
chk_prop "$SYS/build.prop"            ro.system.product.cpu.abilist64  "x86_64,arm64-v8a"
chk_prop "$SYS/build.prop"            ro.dalvik.vm.isa.arm64           "x86_64"
chk_prop "$SYS/build.prop"            ro.dalvik.vm.isa.arm             "x86"
chk_prop "$SYS/build.prop"            ro.enable.native.bridge.exec     "1"
chk_prop "$PRODUCT_OUT/vendor/build.prop"  ro.vendor.product.cpu.abilist64 "x86_64,arm64-v8a"
chk_prop "$PRODUCT_OUT/vendor/build.prop"  ro.dalvik.vm.native.bridge      "libndk_translation.so"
if [ -f "$PRODUCT_OUT/odm/etc/build.prop" ]; then
    chk_prop "$PRODUCT_OUT/odm/etc/build.prop" ro.odm.product.cpu.abilist64 "x86_64,arm64-v8a"
else
    ok "本产品无 odm 分区（不影响：init 派生 abilist 时 vendor 的优先级已足够）"
fi

hr "5. 首启风险提示"
if grep -q "mkdir /data/misc .*encryption=Require" "$AOSP_DIR/system/core/rootdir/init.rc" 2>/dev/null; then
    warn "init.rc 仍是 encryption=Require —— 若首启卡 'Rebooting into recovery'，"
    warn "  执行： ./scripts/apply-overlay.sh --patch-initrc && ./scripts/build-rom.sh"
else
    ok "init.rc 已放宽（encryption=Attempt）或已是补丁后状态"
fi

echo
if [ "$fails" = 0 ]; then
    log "ROM 自检通过 ✓（警告 $warns 条）  下一步： ./scripts/run-linux.sh"
else
    die "ROM 自检失败：$fails 项不合格（警告 $warns 条）"
fi
