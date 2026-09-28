#!/usr/bin/env bash
# 把构建产物打成一个可直接交付/拷贝的 ROM 目录（Linux 与 Windows 共用同一份）。
#
#   ./package-rom.sh              # → artifacts/rom-remote_control_x64_arm64/
#   ./package-rom.sh --list       # 只看会打哪些文件
#
# 产物里同时写两份清单：
#   SHA256SUMS   —— 逐文件校验（Windows 侧 fetch-images.ps1 可对照）
#   MANIFEST.txt —— 这份 ROM 的关键属性（abilist / 翻译层 / 指纹），用于确认"两边跑的是同一份"
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

DEST="$ARTIFACTS_DIR/rom-$PRODUCT_NAME"

# 必需（少了起不来）
# ⚠️ 交付目录里最容易被忽略、但少一个就开不了机的，是 **-qemu 家族**：
#   1. ramdisk-qemu.img = (cat ramdisk.img vendor_ramdisk.img) 的合并版
#      （build/make/core/Makefile:5787），first-stage 的 fstab.ranchu 在 vendor ramdisk 那一半里。
#      只给 ramdisk.img → "failed to find device default fstab" → InitFatalReboot 无限重启。
#   2. system/vendor/product/system_ext 的 -qemu 变体是**带 GPT 分区表**的包装版
#      （file 可见 "DOS/MBR boot sector ... ID=0xee"），模拟器靠它把分区暴露成
#      /dev/block/by-name/<name>。只给裸 ext4 的 *.img → first-stage 卡在
#      "partition(s) not found in /sys, waiting for their uevent" 然后超时重启。
REQUIRED="system-qemu.img vendor-qemu.img product-qemu.img ramdisk-qemu.img \
kernel-ranchu encryptionkey.img userdata.img advancedFeatures.ini config.ini"
# 可选：裸 ext4 的分区镜像（模拟器优先用 -qemu 变体；这些留着方便挂载检视）、
#       以及构建模式/诊断用文件
OPTIONAL="system.img vendor.img product.img system_ext-qemu.img ramdisk.img \
build.prop dtb.img vbmeta.img"

LIST_ONLY=0
[ "${1:-}" = "--list" ] && LIST_ONLY=1

[ -d "$PRODUCT_OUT" ] || die "产物目录不存在：$PRODUCT_OUT（先 ./build-rom.sh）"

if [ "$LIST_ONLY" = 1 ]; then
    log "会打包到 $DEST ："
    for f in $REQUIRED $OPTIONAL; do
        [ -s "$PRODUCT_OUT/$f" ] && printf '    %-22s %s\n' "$f" "$(du -h "$PRODUCT_OUT/$f" | cut -f1)"
    done
    printf '    %-22s %s\n' "system/build.prop" "$(du -h "$PRODUCT_OUT/system/build.prop" 2>/dev/null | cut -f1)"
    exit 0
fi

missing=""
for f in $REQUIRED; do [ -s "$PRODUCT_OUT/$f" ] || missing="$missing $f"; done
if [ -n "$missing" ]; then
    die "产物不全，缺：$missing
    构建可能还没跑完： ./scripts/build-rom.sh --status"
fi

# 加锁：两个实例并发跑会互相踩（实测：一个在算 SHA256SUMS、另一个在写 MANIFEST.txt，
# 清单里就会出现 MANIFEST.txt 自己的哈希，校验必然失败）。被 kill 的旧实例也可能还在跑。
LOCK="$RUN_DIR/pack.lock"
mkdir -p "$RUN_DIR"
exec 9>"$LOCK"
flock -n 9 || die "另一个打包实例正在跑（锁：$LOCK）"

log "清空并重建 $DEST"
rm -rf "$DEST"; mkdir -p "$DEST/system"

for f in $REQUIRED $OPTIONAL; do
    [ -s "$PRODUCT_OUT/$f" ] && cp -f "$PRODUCT_OUT/$f" "$DEST/$f"
done

# 屏幕尺寸/密度：交付目录里那份 config.ini 用本项目 emulator/config.ini 覆盖。
# ROM 构建产出的是 goldfish 的 config.ini.xl（1440x2960 @560dpi）；统一成 720x1280 @320dpi。
# ⚠️ 必须在算 SHA256SUMS/MANIFEST 之前写，否则清单和实物对不上。
if [ -s "$EMULATOR_CONFIG" ]; then
    cp -f "$EMULATOR_CONFIG" "$DEST/config.ini"
    log "显示配置： $(grep -E '^(skin\.name|hw\.lcd\.density)=' "$EMULATOR_CONFIG" | paste -sd' ' -)"
fi
# initrd：模拟器 -initrd 指向它。AOSP 产物不一定生成这个文件；
# 缺了就用 ramdisk.img 生成（QEMU 拿到不存在的 initrd 会「主循环立刻结束且不报错」，很难查）。
if [ ! -s "$DEST/initrd" ]; then
    if [ -s "$DEST/ramdisk-qemu.img" ]; then
        cp -f "$DEST/ramdisk-qemu.img" "$DEST/initrd"
        log "已由 ramdisk-qemu.img（合并版）生成 initrd"
    elif [ -s "$DEST/ramdisk.img" ]; then
        cp -f "$DEST/ramdisk.img" "$DEST/initrd"
        log "已由 ramdisk.img 生成 initrd"
    fi
fi

# 模拟器靠它判断 guest 架构；缺了会退回宿主架构，必须带上
cp -f "$PRODUCT_OUT/system/build.prop" "$DEST/system/build.prop"

# AOSP 产物没有 source.properties；SDK 版模拟器（Windows 侧用的那个）更认它，
# 补一份最小可用的，AOSP 自带模拟器也不受影响。
cat > "$DEST/source.properties" <<EOF
Pkg.Desc=remote-control x86_64 with ARM64 bridge
Pkg.Revision=1
AndroidVersion.ApiLevel=31
SystemImage.Abi=x86_64
SystemImage.TagId=remote_control
SystemImage.TagDisplay=remote-control x86_64 + ARM64 bridge
EOF
for f in system_ext/build.prop vendor/build.prop product/build.prop; do
    [ -s "$PRODUCT_OUT/$f" ] && { mkdir -p "$DEST/$(dirname $f)"; cp -f "$PRODUCT_OUT/$f" "$DEST/$f"; }
done

log "生成校验清单"
( cd "$DEST" && find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS )

# 属性查找：跨分区 + 容错。
# ⚠️ 两个坑都在这里：
#   1. common.sh 开了 set -e，`v=$(grep ...)` 未命中时 grep 返回 1，会把脚本**静默带走**
#      （verify-rom.sh 里踩过同一个坑）；
#   2. AOSP 12 里 plain 的 ro.build.fingerprint / ro.product.device / ro.product.cpu.abilist
#      都是**运行时 init 从分区前缀属性派生**的，build.prop 里只有 ro.<分区>.xxx 形态。
prop_of() {
    local name="$1" f v
    local order=("$DEST/system/build.prop" "$DEST/system_ext/build.prop" \
                 "$DEST/vendor/build.prop" "$DEST/product/build.prop" "$DEST/odm/etc/build.prop")
    # ro.dalvik.vm.native.bridge 在 system 里固定是 0（runtime_libart.mk 的强赋值），
    # 真正生效的是 vendor 那份（build.prop 后加载覆盖先加载）——清单里要报生效值。
    if [ "$name" = "ro.dalvik.vm.native.bridge" ]; then
        order=("$DEST/vendor/build.prop" "$DEST/system/build.prop")
    fi
    for f in "${order[@]}"; do
        [ -f "$f" ] || continue
        v=$(grep -m1 "^${name}=" "$f" 2>/dev/null | cut -d= -f2- || true)
        [ -n "$v" ] && { printf '%s' "$v"; return 0; }
    done
    return 0
}

{
    echo "# remote-control x86_64 + ARM64 桥 ROM"
    echo "# 生成时间：$(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "# lunch 目标：$LUNCH_TARGET"
    echo
    echo "## 关键属性"
    for p in ro.build.version.sdk ro.product.system.device ro.product.cpu.abi \
             ro.system.product.cpu.abilist ro.system.product.cpu.abilist64 \
             ro.vendor.product.cpu.abilist64 ro.dalvik.vm.native.bridge; do
        v=$(prop_of "$p"); [ -n "$v" ] && printf '%-32s = %s\n' "$p" "$v"
    done
    echo
    echo "## 交付物指纹（Linux 与 Windows 两侧对照这个 + system.img 的 sha256）"
    printf '%-32s = %s\n' "ro.system.build.fingerprint" "$(prop_of ro.system.build.fingerprint)"
    printf '%-32s = %s\n' "system.img sha256" "$(cd "$DEST" && sha256sum system.img 2>/dev/null | cut -d' ' -f1 || true)"
    echo
    echo "## 翻译层（随 ROM 一起编进 /system）"
    bridge="$DEVICE_DST/remote_control_x64_arm64/bridge/system"
    echo "翻译器库：  $(ls "$bridge/lib64"/libndk_translation*.so 2>/dev/null | wc -l) 个"
    echo "arm64 系统库：$(ls "$bridge/lib64/arm64" 2>/dev/null | wc -l) 个"
    echo "载荷清单：  payload/MANIFEST.sha256"
    echo
    echo "## 文件"
    ( cd "$DEST" && ls -la | awk 'NR>1{printf "%-24s %12s\n", $9, $5}' )
} > "$DEST/MANIFEST.txt"

log "打包完成：${DEST#"$PROJECT_ROOT"/}（$(du -sh "$DEST" | cut -f1)）"
cat "$DEST/MANIFEST.txt" | sed -n '1,18p'
log "Windows 侧： windows/fetch-images.ps1 -RemoteDir $PRODUCT_OUT"
