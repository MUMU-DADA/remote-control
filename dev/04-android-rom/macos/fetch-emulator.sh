#!/usr/bin/env bash
# fetch-emulator.sh —— 拿到 macOS 版 emulator + adb（platform-tools）。
#
#   ./fetch-emulator.sh                    # 默认：稳定渠道，自动按宿主架构选包
#   ./fetch-emulator.sh --arch aarch64     # 强制选 Apple Silicon 版
#   ./fetch-emulator.sh --arch x64         # 强制选 Intel Mac 版
#   ./fetch-emulator.sh --dest DIR         # 默认 ./sdk
#   ./fetch-emulator.sh --channel Beta     # 换渠道
#   ./fetch-emulator.sh --emulator-zip F   # 用本地/指定 URL 的包，跳过仓库清单
#   ./fetch-emulator.sh --dry-run          # 只打印将下载什么
#   ./fetch-emulator.sh --force            # 已有也重下
#
# ── 与 windows/fetch-emulator.ps1 的对应关系 ────────────────────────────────
# 算法一致（本机 SDK → 指定 zip → 镜像/官方仓库），只换了平台取值。
#
# ⚠️ 四个 mac 特有的坑，都在下面处理了：
#   1. **host-os 是 `macosx`，而 host-arch 要自己判断**：同一份 host-os 下有
#      darwin_aarch64 与 darwin_x64 两个包。下错架构的包，模拟器根本起不来
#      （后端只有本机架构的 qemu-system-*）。见 §7.0.2。
#   2. **macOS 自带 /bin/bash 是 3.2**（2007 年 GPLv2 版）：没有 `declare -A`、
#      没有 `${var^^}`、没有 `mapfile`。本脚本只用 3.2 有的东西。
#   3. **`unzip` 不保留可执行位**：解出来必须显式 chmod，否则 emulator/adb
#      是 0644，一执行就 "Permission denied"。Windows 的 Expand-Archive 没这问题。
#   4. **没有 sha1sum**：用 `shasum -a 1`（Perl 自带）或 `openssl sha1` 兜底。
#
# 清单结构说明（与 Windows 脚本踩过的同一个坑）：每个包有**按 host-os 分的多个
# archive**，直接取第一个会下到 Linux 版；渠道也不是用名字写的，是
# `<channelRef ref="channel-0"/>` + `<channel id="channel-0">Stable</channel>`。
#
set -uo pipefail

DEST="./sdk"
CHANNEL="Stable"
ARCH=""
REPO_BASE="https://mirrors.cloud.tencent.com/AndroidSDK"
OFFICIAL_BASE="https://dl.google.com/android/repository"
EMU_ZIP=""
DRY_RUN=0
FORCE=0
SKIP_PT=0
PROXY="${HTTPS_PROXY:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --dest)          DEST="$2"; shift 2 ;;
        --channel)       CHANNEL="$2"; shift 2 ;;
        --arch)          ARCH="$2"; shift 2 ;;
        --repo-base)     REPO_BASE="$2"; shift 2 ;;
        --emulator-zip)  EMU_ZIP="$2"; shift 2 ;;
        --proxy)         PROXY="$2"; shift 2 ;;
        --skip-platform-tools) SKIP_PT=1; shift ;;
        --dry-run)       DRY_RUN=1; shift ;;
        --force)         FORCE=1; shift ;;
        -h|--help)       sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m  ! %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 前置：确认在 macOS 上，并定出 host-os / host-arch
# ---------------------------------------------------------------------------
OS="$(uname -s)"
[ "$OS" = "Darwin" ] || die "这个脚本只用于 macOS（当前 uname -s = $OS）。
    Linux 用 scripts/run-linux.sh 那条线，Windows 用 windows/fetch-emulator.ps1。"

if [ -z "$ARCH" ]; then
    case "$(uname -m)" in
        arm64|aarch64) ARCH="aarch64" ;;
        x86_64)        ARCH="x64" ;;
        *) die "认不出宿主架构：$(uname -m)" ;;
    esac
fi
HOST_OS="macosx"

command -v python3 >/dev/null 2>&1 || die "需要 python3 来解析仓库清单（macOS 自带：xcode-select --install）"
command -v unzip   >/dev/null 2>&1 || die "需要 unzip"
command -v curl    >/dev/null 2>&1 || die "需要 curl"

sha1_of() {   # sha1_of <文件>
    if command -v shasum >/dev/null 2>&1; then shasum -a 1 "$1" | awk '{print $1}'
    else openssl sha1 "$1" | awk '{print $NF}'; fi
}

mkdir -p "$DEST"
log "宿主：macOS / $(uname -m) → 选包 host-os=$HOST_OS host-arch=$ARCH"

# ---------------------------------------------------------------------------
# 1) 本机已有 Android SDK？（与 Windows 脚本同策略）
# ---------------------------------------------------------------------------
if [ "$FORCE" -eq 0 ]; then
    for cand in "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" "$HOME/Library/Android/sdk"; do
        [ -n "$cand" ] || continue
        if [ -x "$cand/emulator/emulator" ]; then
            # 架构要对得上：Intel 的 SDK 里没有 arm64 后端，反之亦然
            if ls "$cand/emulator/qemu/" 2>/dev/null | grep -q "$ARCH"; then
                log "命中已有 SDK：$cand（qemu 后端含 $ARCH）"
                log "如需强制自备一份，加 --force"
                exit 0
            fi
            warn "$cand 里有 emulator，但 qemu 后端里没有 $ARCH —— 忽略它，继续自备一份"
        fi
    done
fi

EMU_DIR="$DEST/emulator"
EMU_BIN="$EMU_DIR/emulator"
if [ -x "$EMU_BIN" ] && [ "$FORCE" -eq 0 ]; then
    if ls "$EMU_DIR/qemu/" 2>/dev/null | grep -q "$ARCH"; then
        ok "已有 emulator：$EMU_BIN（后端 $ARCH）"
        exit 0
    fi
    warn "已有 emulator，但架构不对（需要 $ARCH），重下"
fi

# ---------------------------------------------------------------------------
# 2) 仓库清单
# ---------------------------------------------------------------------------
XML="$DEST/repository2-3.xml"
fetch() {   # fetch <相对路径或完整URL> <输出文件>
    local url="$1" out="$2"
    local args=(-fL --retry 3 --http1.1 -o "$out")
    [ -n "$PROXY" ] && args=(-x "$PROXY" "${args[@]}")
    # ⚠️ 腾讯镜像在 HTTP/2 下会在几 MB 处 INTERNAL_ERROR（实测），所以显式 --http1.1；
    #    再加停滞超时，避免卡死时无限等
    curl "${args[@]}" --speed-time 30 --speed-limit 10240 "$url" || return 1
}

if [ ! -s "$XML" ]; then
    log "下载仓库清单"
    fetch "$REPO_BASE/repository2-3.xml" "$XML" || fetch "$OFFICIAL_BASE/repository2-3.xml" "$XML" \
        || die "仓库清单下载失败"
fi
ok "清单：$XML（$(stat -f%z "$XML" 2>/dev/null || stat -c %s "$XML") 字节）"

# ---------------------------------------------------------------------------
# 3) 选包：按渠道 → host-os → host-arch
# ---------------------------------------------------------------------------
resolve() {   # resolve <包路径>  →  输出 "url<TAB>sha1<TAB>size<TAB>版本"
    python3 - "$XML" "$1" "$HOST_OS" "$ARCH" "$CHANNEL" <<'PY'
import sys, xml.etree.ElementTree as ET
xml, want_path, host_os, host_arch, chan_name = sys.argv[1:6]
t = ET.parse(xml)
root = t.getroot()

# channelRef.ref -> 渠道名
chan_ids = {}
for c in root.iter("channel"):
    cid = c.get("id")
    txt = (c.text or "").strip()
    if cid and txt:
        chan_ids[cid] = txt

def emit(pkg):
    for a in pkg.iter("archive"):
        ho = a.findtext("host-os") or "any"
        ha = (a.findtext("host-arch") or "").lower()
        if ho not in (host_os, "any"):
            continue
        # host-arch 只在清单给了的时候比；给了就必须匹配
        if ha and ha not in (host_arch, ""):
            continue
        url = a.findtext("complete/url")
        if not url:
            continue
        rev = pkg.find("revision")
        ver = "%s.%s.%s" % (rev.findtext("major"), rev.findtext("minor"), rev.findtext("micro")) if rev is not None else "?"
        print("%s\t%s\t%s\t%s" % (url, a.findtext("complete/checksum") or "", a.findtext("complete/size") or "0", ver))
        return True
    return False

pkgs = [p for p in root.iter("remotePackage") if p.get("path") == want_path]
if not pkgs:
    sys.exit("清单里没有 %s 包" % want_path)

# 先按渠道名找
want_ids = [cid for cid, name in chan_ids.items() if name.lower() == chan_name.lower()]
for pkg in pkgs:
    ch = pkg.find("channelRef")
    if ch is not None and ch.get("ref") in want_ids and emit(pkg):
        sys.exit(0)
# 退回第一个能给出该平台的包
for pkg in pkgs:
    if emit(pkg):
        sys.exit(0)
sys.exit("清单里没有 %s 的 %s/%s archive" % (want_path, host_os, host_arch))
PY
}

if [ -n "$EMU_ZIP" ]; then
    EMU_REC=""
    log "使用指定的模拟器包：$EMU_ZIP"
else
    EMU_REC="$(resolve emulator)" || die "$EMU_REC"
    IFS="$(printf '\t')" read -r EMU_URL EMU_SHA EMU_SIZE EMU_VER <<EOF
$EMU_REC
EOF
    log "选中 emulator 版本 $EMU_VER：$(basename "$EMU_URL")（$(( ${EMU_SIZE:-0} / 1048576 )) MiB）"
    log "  sha1 ${EMU_SHA:-（清单未提供）}"
fi

if [ "$DRY_RUN" -eq 1 ]; then
    log "DryRun：将下载到 $DEST/emulator"
    [ -n "${EMU_URL:-}" ] && log "  emulator: $EMU_URL"
    if [ "$SKIP_PT" -eq 0 ]; then
        PT_REC="$(resolve platform-tools 2>/dev/null)" || PT_REC=""
        [ -n "$PT_REC" ] && log "  platform-tools: $(printf '%s' "$PT_REC" | cut -f1)"
    fi
    exit 0
fi

# ---------------------------------------------------------------------------
# 4) 下载 + 校验
# ---------------------------------------------------------------------------
if [ -n "$EMU_ZIP" ]; then
    ZIP="$DEST/emulator.zip"
    case "$EMU_ZIP" in
        http://*|https://*) log "下载 $EMU_ZIP"; fetch "$EMU_ZIP" "$ZIP" || die "下载失败" ;;
        *) [ -s "$EMU_ZIP" ] || die "本地包不存在：$EMU_ZIP"; cp -f "$EMU_ZIP" "$ZIP" ;;
    esac
else
    ZIP="$DEST/$(basename "$EMU_URL")"
    if [ -s "$ZIP" ] && [ "$FORCE" -eq 0 ]; then
        ok "已有 zip，跳过下载：$(basename "$ZIP")"
    else
        log "下载 → $(basename "$ZIP")（约 $(( ${EMU_SIZE:-0} / 1048576 )) MiB，可能要几分钟）"
        fetch "$REPO_BASE/$EMU_URL" "$ZIP" || fetch "$OFFICIAL_BASE/$EMU_URL" "$ZIP" || die "下载失败：$EMU_URL"
    fi
    if [ -n "$EMU_SHA" ]; then
        got="$(sha1_of "$ZIP")"
        if [ "$got" = "$EMU_SHA" ]; then ok "sha1 校验通过（$got）"
        else die "sha1 不匹配：期望 $EMU_SHA，实得 $got
    删掉重下： rm -f '$ZIP'"; fi
    fi
fi

# ---------------------------------------------------------------------------
# 5) 解包 + 补可执行位
# ---------------------------------------------------------------------------
log "解包 → $DEST"
[ -d "$EMU_DIR" ] && rm -rf "$EMU_DIR"
unzip -q -o "$ZIP" -d "$DEST" || die "解包失败：$ZIP"
[ -f "$EMU_BIN" ] || die "解包后没有 emulator/emulator"

chmod +x "$EMU_BIN" 2>/dev/null || true
for b in "$EMU_DIR"/qemu/*/qemu-system-* "$EMU_DIR"/qsn "$EMU_DIR"/qemu-img \
         "$EMU_DIR"/mksdcard "$EMU_DIR"/emulator-check "$EMU_DIR"/crashpad_handler; do
    [ -e "$b" ] && chmod +x "$b" 2>/dev/null
done
ok "可执行位已补（unzip 在 macOS 上不保留）"

# 架构自检：包里必须真有本机架构的后端
if ls "$EMU_DIR/qemu/" 2>/dev/null | grep -q "$ARCH"; then
    ok "qemu 后端目录：$(ls "$EMU_DIR/qemu/" | tr '\n' ' ')"
else
    die "解出来的包里没有 $ARCH 后端：
    $(ls "$EMU_DIR/qemu/" 2>/dev/null | tr '\n' ' ')
    这说明选包选错了架构（见脚本头部坑 1）"
fi

# ---------------------------------------------------------------------------
# 6) platform-tools（adb）
# ---------------------------------------------------------------------------
if [ "$SKIP_PT" -eq 0 ] && [ ! -x "$DEST/platform-tools/adb" ]; then
    PT_REC="$(resolve platform-tools 2>/dev/null)" || PT_REC=""
    if [ -n "$PT_REC" ]; then
        PT_URL="$(printf '%s' "$PT_REC" | cut -f1)"
        PT_SHA="$(printf '%s' "$PT_REC" | cut -f2)"
        PT_ZIP="$DEST/$(basename "$PT_URL")"
        log "下载 platform-tools：$(basename "$PT_ZIP")"
        if [ ! -s "$PT_ZIP" ]; then
            fetch "$REPO_BASE/$PT_URL" "$PT_ZIP" || fetch "$OFFICIAL_BASE/$PT_URL" "$PT_ZIP" || die "platform-tools 下载失败"
        fi
        if [ -n "$PT_SHA" ]; then
            got="$(sha1_of "$PT_ZIP")"
            [ "$got" = "$PT_SHA" ] && ok "platform-tools sha1 校验通过" || die "platform-tools sha1 不匹配"
        fi
        unzip -q -o "$PT_ZIP" -d "$DEST" || die "platform-tools 解包失败"
        [ -x "$DEST/platform-tools/adb" ] && ok "adb：$DEST/platform-tools/adb" || warn "解出来没有 platform-tools/adb"
    else
        warn "清单里没有 platform-tools 的 $HOST_OS archive，跳过"
    fi
fi

# ---------------------------------------------------------------------------
log "完成：$EMU_BIN"
printf '    下一步： %s/emulator -version\n' "$EMU_DIR"
printf '              %s -accel-check      # 看 HVF 可用性（macOS 上应有 hvf 字样）\n' "$EMU_BIN"
