#!/usr/bin/env bash
# 下载 Google 官方的 Android 12 arm64 成品镜像（SDK system-image）。
#
#   ./get-stock-image.sh                     # 默认 android-31 / default / arm64-v8a（606MB）
#   ./get-stock-image.sh --api 31 --tag default --abi arm64-v8a
#
# 用途：**还没编出自己的镜像时，先确认模拟器环境本身是通的**，
#       也用来在自制镜像出问题时做对照（同一台机器、同一套启动参数）。
#
# 下完会补齐"构建模式"必需的两个文件（见 README 实测坑 2、3）：
#   system/build.prop   ← 不够它模拟器会退回宿主架构 x86_64
#   initrd              ← 模拟器给 QEMU 传的是这个，不是 ramdisk.img
#
# 产物：.run/stock-image/<abi>/

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

API=31
TAG=default
ABI=arm64-v8a
REPO_BASE="${REPO_BASE:-https://dl.google.com/android/repository}"
MIRROR="${MIRROR:-https://mirrors.cloud.tencent.com/AndroidSDK}"

while [ $# -gt 0 ]; do
    case "$1" in
        --api) API="${2:?}"; shift ;;
        --tag) TAG="${2:?}"; shift ;;
        --abi) ABI="${2:?}"; shift ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) die "未知参数：$1（-h 看用法）" ;;
    esac
    shift
done

# SDK 清单里，AOSP/default 镜像的目录名是 android（不是 default）
case "$TAG" in
    default) XMLDIR=android ;;
    *)       XMLDIR="$TAG" ;;
esac

DEST="$RUN_DIR/stock-image/$ABI"
PKG="system-images;android-$API;$TAG;$ABI"

# ---------------------------------------------------------------------------
log "解析 SDK system-image 清单（$TAG 渠道）"
XML="$RUN_DIR/stock-image/sys-img2-3.xml"
mkdir -p "$(dirname "$XML")"
curl -fsSL --max-time 120 -o "$XML" "$REPO_BASE/sys-img/$XMLDIR/sys-img2-3.xml" \
    || die "下载清单失败：$REPO_BASE/sys-img/$XMLDIR/sys-img2-3.xml"

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
print(a.group(3), a.group(2), a.group(1))
PY
)" || die "解析清单失败"

log "包：$PKG"
log "  $ZIPNAME  $((SIZE/1024/1024))MB  sha1=${SHA1:0:12}…"

# ---------------------------------------------------------------------------
ZIP="$RUN_DIR/stock-image/$ZIPNAME"
if [ -s "$ZIP" ] && [ "$(sha1sum "$ZIP" | cut -d' ' -f1)" = "$SHA1" ]; then
    log "已有且校验通过，跳过下载"
else
    # 优先国内镜像（同源同字节，已实测）
    URL="$MIRROR/sys-img/$XMLDIR/$ZIPNAME"
    if ! curl -fsSI --max-time 20 "$URL" >/dev/null 2>&1; then
        URL="$REPO_BASE/sys-img/$XMLDIR/$ZIPNAME"
        warn "镜像上没有，改用官方源"
    fi
    log "下载 $URL"
    curl -fSL --max-time 1800 -o "$ZIP" "$URL" || die "下载失败"
    actual="$(sha1sum "$ZIP" | cut -d' ' -f1)"
    [ "$actual" = "$SHA1" ] || die "SHA1 不匹配：实际 $actual / 清单 $SHA1"
    log "SHA1 校验通过 ✓"
fi

# ---------------------------------------------------------------------------
rm -rf "$DEST"; mkdir -p "$DEST"
log "解压到 .run/stock-image/$ABI/"
unzip -q -o "$ZIP" -d "$DEST"
# zip 里自带一层 <abi>/，把它提升上来
if [ -d "$DEST/$ABI" ]; then
    mv "$DEST/$ABI"/* "$DEST"/ 2>/dev/null || true
    rmdir "$DEST/$ABI" 2>/dev/null || true
fi

# 补齐构建模式必需的两样（幂等）
if [ -f "$DEST/build.prop" ] && [ ! -f "$DEST/system/build.prop" ]; then
    mkdir -p "$DEST/system"
    cp "$DEST/build.prop" "$DEST/system/build.prop"
    log "已补 system/build.prop（否则模拟器会退回宿主架构 x86_64）"
fi
if [ -f "$DEST/ramdisk.img" ] && [ ! -f "$DEST/initrd" ]; then
    cp "$DEST/ramdisk.img" "$DEST/initrd"
    log "已补 initrd（模拟器给 QEMU 传的是它，不是 ramdisk.img）"
fi

log "完成。内容："
ls -la "$DEST" | awk 'NR>3 {printf "  %10s  %s\n", $5, $9}'
printf '\n下一步： ./run-stock-image.sh\n'
