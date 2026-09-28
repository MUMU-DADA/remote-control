#!/usr/bin/env bash
# 从 Google 官方镜像里提取 ARM 翻译层（libndk_translation）到 payload/。
#
#   ./fetch-payload.sh                 # 下载 → 启动官方镜像 → adb 拉取 → 校验
#   ./fetch-payload.sh --force         # 强制重新下载镜像
#   ./fetch-payload.sh --from-tar F    # 跳过下载与启动，直接用一个已有的 tar
#
# 产物：payload/system/**（90 个文件）+ payload/MANIFEST.sha256
#
# 为什么必须从官方镜像取：libndk_translation 是 Google 专有二进制，AOSP 公开树里没有，
# 只能从随 SDK 分发的系统镜像里提取（社区同类工具：Droid-NDK-Extractor、
# waydroid_script 的 libndk 包、android_proprietary_native_bridge）。
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

FORCE=0
FROM_TAR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --force)    FORCE=1 ;;
        --from-tar) FROM_TAR="${2:?}"; shift ;;
        -h|--help)  sed -n '2,12p' "$0"; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
    shift
done

mkdir -p "$RUN_DIR"
IMG_DIR="$RUN_DIR/bridge-image"
PKG="system-images;android-$BRIDGE_API;$BRIDGE_TAG;$BRIDGE_ABI"
STAGE="$RUN_DIR/payload-stage"
EMU_BOOT_PORT=5590

# ---------------------------------------------------------------------------
extract_tar() {   # $1 = tar（内容形如 system/lib64/...）
    local tar="$1"
    rm -rf "$STAGE"; mkdir -p "$STAGE"
    tar -xf "$tar" -C "$STAGE"
    [ -s "$STAGE/system/lib64/libndk_translation.so" ] || die "tar 里没有 system/lib64/libndk_translation.so"
}

if [ -n "$FROM_TAR" ]; then
    [ -f "$FROM_TAR" ] || die "找不到 tar：$FROM_TAR"
    log "从一个已有 tar 提取：$FROM_TAR"
    extract_tar "$FROM_TAR"
else
    # 1) 解析 SDK 清单拿下载地址与 sha1
    mkdir -p "$IMG_DIR"
    XML="$IMG_DIR/sys-img2-3.xml"
    if [ ! -s "$XML" ] || [ "$FORCE" = 1 ]; then
        log "取 SDK 清单（$BRIDGE_TAG 渠道）"
        curl -fsSL --max-time 120 -o "$XML" "$SDK_MIRROR/sys-img/$BRIDGE_TAG/sys-img2-3.xml" \
            || curl -fsSL --max-time 120 -o "$XML" "$SDK_REPO/sys-img/$BRIDGE_TAG/sys-img2-3.xml" \
            || die "下载 SDK 清单失败"
    fi
    read -r ZIPNAME SHA1 SIZE <<<"$(python3 - "$XML" "$PKG" <<'PY'
import re, sys
xml, want = sys.argv[1], sys.argv[2]
s = open(xml, encoding='utf-8', errors='replace').read()
m = re.search(r'<remotePackage path="' + re.escape(want) + r'">(.*?)</remotePackage>', s, re.S)
if not m:
    sys.exit("清单里没有这个包：" + want)
a = re.search(r'<size>(\d+)</size>\s*<checksum type="[^"]+">([0-9a-f]+)</checksum>\s*<url>([^<]+)</url>', m.group(1))
if not a:
    sys.exit("解析 archive 失败：" + want)
print(a.group(3), a.group(2), int(a.group(1)) // 1024 // 1024)
PY
)" || die "解析清单失败：$PKG"
    log "官方镜像：$PKG  →  $ZIPNAME（${SIZE} MiB，sha1 ${SHA1:0:12}…）"

    ZIP="$IMG_DIR/$ZIPNAME"
    if [ -s "$ZIP" ] && [ "$FORCE" != 1 ]; then
        log "已有镜像，跳过下载（--force 可重下）"
    else
        rm -f "$ZIP"
        log "下载中（腾讯镜像 → 失败自动回退 Google）"
        curl -fL --retry 2 -o "$ZIP" "$SDK_MIRROR/sys-img/$BRIDGE_TAG/$ZIPNAME" \
            || curl -fL --retry 2 -o "$ZIP" "$SDK_REPO/sys-img/$BRIDGE_TAG/$ZIPNAME" \
            || die "下载失败"
    fi
    got=$(sha1sum "$ZIP" | cut -d' ' -f1)
    [ "$got" = "$SHA1" ] || die "sha1 不匹配：期望 $SHA1，实得 $got"
    log "sha1 校验通过 ✓"

    # 2) 解包成"构建模式"sysdir（模拟器需要 system/build.prop 与 initrd 两个文件）
    SYSDIR="$IMG_DIR/sysdir"
    if [ ! -s "$SYSDIR/system.img" ]; then
        log "解包镜像"
        mkdir -p "$SYSDIR"
        ( cd "$SYSDIR" && unzip -q -o "$ZIP" && mv "$BRIDGE_ABI"/* . && rmdir "$BRIDGE_ABI" )
    fi
    mkdir -p "$SYSDIR/system" && cp -f "$SYSDIR/build.prop" "$SYSDIR/system/build.prop"
    [ -f "$SYSDIR/initrd" ] || cp -f "$SYSDIR/ramdisk.img" "$SYSDIR/initrd"

    # 3) 启动官方镜像（KVM），拉取载荷
    [ -x "$EMULATOR_BIN" ] || die "找不到模拟器，export EMULATOR_BIN=/path/to/emulator"
    ADB_BIN="$(command -v adb || echo "$ADB")"
    [ -x "$ADB_BIN" ] || die "找不到 adb"
    SERIAL="emulator-$EMU_BOOT_PORT"
    log "启动官方镜像（端口 $EMU_BOOT_PORT，仅用于取载荷）"
    ANDROID_PRODUCT_OUT="$SYSDIR" setsid nohup "$EMULATOR_BIN" \
        -sysdir "$SYSDIR" -datadir "$RUN_DIR/bridge-image/data" -port "$EMU_BOOT_PORT" \
        -no-window -gpu swiftshader_indirect -no-snapshot -no-boot-anim -no-audio \
        -accel on -memory 3072 -cores 4 > "$RUN_DIR/bridge-image/emulator.log" 2>&1 < /dev/null &
    trap '"$ADB_BIN" -s "$SERIAL" emu kill >/dev/null 2>&1 || true' EXIT

    log "等开机（最多 300 s）"
    "$ADB_BIN" -s "$SERIAL" wait-for-device
    for i in $(seq 1 30); do
        [ "$("$ADB_BIN" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break
        sleep 10
    done
    "$ADB_BIN" -s "$SERIAL" root >/dev/null 2>&1 || true
    "$ADB_BIN" -s "$SERIAL" wait-for-device

    abilist=$("$ADB_BIN" -s "$SERIAL" shell getprop ro.product.cpu.abilist | tr -d '\r')
    log "官方镜像 abilist = $abilist"
    case "$abilist" in *arm64-v8a*) ;; *) die "这份镜像不带 arm64 桥，别继续（abilist=$abilist）" ;; esac

    # 4) 打包 + 拉回
    log "打包载荷并拉回"
    "$ADB_BIN" -s "$SERIAL" shell 'cd / && tar -cf /data/local/tmp/ndkpayload.tar \
        system/lib64/libndk_translation.so system/lib64/libndk_translation_proxy_lib*.so \
        system/lib64/arm64 system/bin/arm64 \
        system/bin/ndk_translation_program_runner_binfmt_misc_arm64 \
        system/etc/binfmt_misc system/etc/init/ndk_translation.rc \
        system/etc/ld.config.arm.txt system/etc/ld.config.arm64.txt'
    mkdir -p "$RUN_DIR/tar"
    "$ADB_BIN" -s "$SERIAL" pull /data/local/tmp/ndkpayload.tar "$RUN_DIR/tar/ndkpayload.tar" >/dev/null
    extract_tar "$RUN_DIR/tar/ndkpayload.tar"
    "$ADB_BIN" -s "$SERIAL" emu kill >/dev/null 2>&1 || true
    trap - EXIT
fi

# ---------------------------------------------------------------------------
# 5) 落盘 + 校验 + 清单
log "写入 $PAYLOAD_DIR"
rm -rf "$PAYLOAD_DIR"; mkdir -p "$PAYLOAD_DIR"
cp -a "$STAGE"/. "$PAYLOAD_DIR"/
( cd "$PAYLOAD_DIR" && find . -type f ! -name MANIFEST.sha256 -print0 \
    | LC_ALL=C sort -z | xargs -0 sha256sum > MANIFEST.sha256 )

n_files=$(find "$PAYLOAD_DIR" -type f ! -name MANIFEST.sha256 | wc -l)
n_libs=$(ls "$PAYLOAD_DIR/system/lib64"/libndk_translation*.so | wc -l)
n_arm64=$(ls "$PAYLOAD_DIR/system/lib64/arm64" | wc -l)
log "载荷完成：$n_files 个文件（翻译器 $n_libs，arm64 系统库 $n_arm64），$(du -sh "$PAYLOAD_DIR" | cut -f1)"
[ "$n_arm64" -ge 59 ] || warn "arm64 系统库只有 $n_arm64 个（官方镜像里是 59 个），确认一下"
log "下一步： ./apply-overlay.sh"
