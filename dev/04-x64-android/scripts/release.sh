#!/usr/bin/env bash
# =============================================================================
# release 打包：把"无头运行环境 + 虚拟机镜像 + 模板"打成一个平台一个 zip。
#
#   ./scripts/release.sh                    # 打 linux + windows 两个平台的成品
#   ./scripts/release.sh --platform linux   # 只打 linux
#   ./scripts/release.sh --list             # 只打印计划（不下载、不打包）
#   ./scripts/release.sh --stage-only       # 只铺 staging，不压缩（调结构用）
#   ./scripts/release.sh --zip-only         # 复用已有 staging，只压缩
#   ./scripts/release.sh --zip-only --reuse-zip   # 连压缩也跳过：拿已有 zip 补跑冒烟
#   ./scripts/release.sh --smoke            # 打完再解压 linux 那份真启动验收
#
# 产物： release/autosnap-<版本>-<平台>-x86_64.zip
#
# 包里三样东西：
#   1. runtime/   完整**无头**运行环境：SDK 模拟器（含 qemu x86_64 后端）+ 自带 adb
#   2. images/    对应的虚拟机镜像：artifacts/rom-<product>/ 交付目录（硬链接，不复制实体）
#   3. templates/ 模板：config.ini（硬件唯一真源）、实例登记、工作目录布局说明
#   另有 bin/ 入口脚本（起/停/看状态/验收）与 START-HERE.md 首读文档。
#
# 设计要点（每条都是踩出来的，别随手改）：
#   · 两个平台用**同一个 build id** 的模拟器（渠道里 linux/windows 同版本发布），
#     这样"两个成品是同一份工程"才有据可依；build id 记进 RELEASE.json。
#   · 运行时不塞 SDK zip 原样：解出来、核对无头后端存在、记来源 URL + sha1。
#   · 镜像用**硬链接**进 staging：zip 只读，5.7 GB 不复制第二份。
#   · 镜像的 sha256 复用 ROM 自带的 SHA256SUMS（先 sha256sum -c 验过），
#     否则 6 GB 要被哈希两遍。
# =============================================================================
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

RELEASE_DIR="$X64_DIR/release"
PACKAGING_DIR="$X64_DIR/packaging"
# staging 与缓存可以指到别处（测试用沙箱时必需；也方便把大文件放别的盘）
CACHE_DIR="${RELEASE_CACHE_DIR:-$RUN_DIR/release-cache}"
STAGE_DIR="${RELEASE_STAGE_DIR:-$RUN_DIR/release-stage}"
ROM_DIR="$ARTIFACTS_DIR/rom-$PRODUCT_NAME"

PLATFORMS="both"
VERSION=""
CHANNEL="Stable"
ZIP_LEVEL=1
LIST_ONLY=0; STAGE_ONLY=0; ZIP_ONLY=0; NO_DOWNLOAD=0; SLIM=0; SMOKE=0; REUSE_ZIP=0
KEEP_STAGE=0; CLEAN_SMOKE=0; VERIFY_IMAGES=1
SMOKE_PORT="${SMOKE_PORT:-5588}"
SMOKE_ADB="${SMOKE_ADB:-/usr/bin/adb}"
declare -A EMU_ZIP_OVERRIDE=() PT_ZIP_OVERRIDE=()

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --platform)   PLATFORMS="${2:?}"; shift ;;
        --version)    VERSION="${2:?}"; shift ;;
        --channel)    CHANNEL="${2:?}"; shift ;;
        --images)     ROM_DIR="${2:?}"; shift ;;
        --out)        RELEASE_DIR="${2:?}"; shift ;;
        --zip-level)  ZIP_LEVEL="${2:?}"; shift ;;
        --linux-emulator-zip)   EMU_ZIP_OVERRIDE[linux]="${2:?}"; shift ;;
        --windows-emulator-zip) EMU_ZIP_OVERRIDE[windows]="${2:?}"; shift ;;
        --linux-platform-tools-zip)   PT_ZIP_OVERRIDE[linux]="${2:?}"; shift ;;
        --windows-platform-tools-zip) PT_ZIP_OVERRIDE[windows]="${2:?}"; shift ;;
        --smoke-port) SMOKE_PORT="${2:?}"; shift ;;
        --smoke-adb)  SMOKE_ADB="${2:?}"; shift ;;
        --list|--dry-run) LIST_ONLY=1 ;;
        --stage-only) STAGE_ONLY=1 ;;
        --zip-only)   ZIP_ONLY=1 ;;
        --reuse-zip)  REUSE_ZIP=1 ;;
        --no-download) NO_DOWNLOAD=1 ;;
        --slim)       SLIM=1 ;;
        --smoke)      SMOKE=1 ;;
        --keep-stage) KEEP_STAGE=1 ;;
        --clean-smoke) CLEAN_SMOKE=1 ;;
        --no-verify-images) VERIFY_IMAGES=0 ;;
        -h|--help) usage; exit 0 ;;
        *) die "未知参数：$1（./scripts/release.sh --help）" ;;
    esac
    shift
done

case "$PLATFORMS" in
    linux|windows|both) ;;
    *) die "--platform 只能是 linux / windows / both（现在：$PLATFORMS）" ;;
esac
case "$ZIP_LEVEL" in ''|*[!0-9]*) die "--zip-level 要是 0..9 的数字" ;; esac

platform_tag()    { case "$1" in linux) printf 'linux-x86_64' ;; windows) printf 'windows-x86_64' ;; esac; }
platform_label()  { case "$1" in linux) printf 'Linux x86_64（KVM）' ;; windows) printf 'Windows x86_64（WHPX）' ;; esac; }
platform_hostos() { case "$1" in linux) printf 'linux' ;; windows) printf 'windows' ;; esac; }
platform_backend() { case "$1" in
    linux)   printf 'qemu/linux-x86_64/qemu-system-x86_64-headless' ;;
    windows) printf 'qemu/windows-x86_64/qemu-system-x86_64.exe' ;; esac; }
platform_list() { if [ "$PLATFORMS" = both ]; then printf 'linux windows'; else printf '%s' "$PLATFORMS"; fi; }

require_tools() {
    local t missing=""
    for t in zip unzip python3 curl sha1sum sha256sum flock; do
        command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
    done
    [ -z "$missing" ] || die "缺工具：$missing"
}

# ---------------------------------------------------------------------------
# 版本号：默认 <日期>-<git 短 sha>；"这个包是哪个提交打的"一眼看得出来
# ---------------------------------------------------------------------------
resolve_version() {
    [ -n "$VERSION" ] && return 0
    local sha; sha="$(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || true)"
    [ -n "$sha" ] || sha="nogit"
    # 只看**已跟踪文件**的改动：未跟踪的临时目录（比如 .dsh-tools/）不该让版本号变脏
    if [ -n "$(git -C "$PROJECT_ROOT" status --porcelain --untracked-files=no 2>/dev/null | head -1 || true)" ]; then
        sha="$sha-dirty"
    fi
    VERSION="$(date +%Y%m%d)-$sha"
}

# ---------------------------------------------------------------------------
# ROM 交付目录自检
#   ⚠️ -qemu 家族必须有：system/vendor/product/system_ext-qemu.img 是**带 GPT 分区表**
#      的包装版，ramdisk-qemu.img 是"系统 ramdisk + vendor ramdisk"的合并版
#      （first-stage 的 fstab.ranchu 在里面）。只给裸 *.img 会卡死并无限重启。
# ---------------------------------------------------------------------------
ROM_REQUIRED="system-qemu.img vendor-qemu.img product-qemu.img ramdisk-qemu.img \
kernel-ranchu encryptionkey.img userdata.img advancedFeatures.ini config.ini"

check_rom() {
    [ -d "$ROM_DIR" ] || die "ROM 交付目录不存在：$ROM_DIR
    先打 ROM： ./scripts/package-rom.sh"
    local missing="" f
    for f in $ROM_REQUIRED; do [ -s "$ROM_DIR/$f" ] || missing="$missing $f"; done
    [ -s "$ROM_DIR/system/build.prop" ] || missing="$missing system/build.prop"
    [ -z "$missing" ] || die "ROM 交付目录不全，缺：$missing
    重新打： ./scripts/package-rom.sh"
}

rom_fingerprint() {
    grep -m1 '^ro\.system\.build\.fingerprint' "$ROM_DIR/MANIFEST.txt" 2>/dev/null | sed 's/.*= *//' || true
}

# ---------------------------------------------------------------------------
# SDK 清单：挑出"该平台 + 该渠道"的那个 archive
#
# ⚠️ 两个坑（windows/fetch-emulator.ps1 里踩过，这里同样要处理）：
#   1. 每个包有**按 host-os 分的多个 archive**（linux/windows/macosx），
#      直接取第一个会下到错的平台；
#   2. 渠道不是用名字写的：`<channelRef ref="channel-0"/>` + `<channel id="channel-0">Stable</channel>`。
# ---------------------------------------------------------------------------
MANIFEST_XML="$CACHE_DIR/repository2-3.xml"

fetch_manifest() {
    mkdir -p "$CACHE_DIR"
    if [ -s "$MANIFEST_XML" ]; then
        log "用缓存的 SDK 清单：${MANIFEST_XML#"$PROJECT_ROOT"/}"
        return 0
    fi
    [ "$NO_DOWNLOAD" = 1 ] && die "没有 SDK 清单缓存，且指定了 --no-download
    要么去掉 --no-download，要么用 --*-emulator-zip / --*-platform-tools-zip 指定本地包"
    log "取 SDK 清单：$SDK_MIRROR/repository2-3.xml"
    curl -fsSL --retry 3 -o "$MANIFEST_XML.tmp" "$SDK_MIRROR/repository2-3.xml" \
        || die "清单下载失败（--no-download 可只用缓存）"
    mv -f "$MANIFEST_XML.tmp" "$MANIFEST_XML"
}

# sdk_archive <包路径> <host-os> → "url<TAB>size<TAB>sha1<TAB>版本<TAB>渠道"
sdk_archive() {
    python3 - "$MANIFEST_XML" "$1" "$2" "$CHANNEL" <<'PY'
import sys, xml.etree.ElementTree as ET
xml, want, hostos, channel = sys.argv[1:5]
root = ET.parse(xml).getroot()
chans = {c.get("id"): (c.text or "").strip() for c in root.iter("channel")}
best = None
for pkg in root.iter("remotePackage"):
    if pkg.get("path") != want:
        continue
    ref = pkg.find("channelRef")
    cid = ref.get("ref") if ref is not None else ""
    cname = chans.get(cid, cid)
    rev = pkg.find("revision")
    ver = ".".join((x.text or "") for x in rev) if rev is not None else ""
    for a in pkg.iter("archive"):
        ho = a.find("host-os")
        ho = ho.text if ho is not None else "any"
        if ho not in (hostos, "any"):
            continue
        url = a.find("complete/url"); size = a.find("complete/size"); sha1 = a.find("complete/checksum")
        rec = (url.text if url is not None else "",
               size.text if size is not None else "0",
               sha1.text if sha1 is not None else "",
               ver, cname)
        if cname.lower() == channel.lower():     # 命中渠道直接用
            print("\t".join(rec)); sys.exit(0)
        if best is None:                          # 否则留第一个能用的当退路
            best = rec
if best:
    print("\t".join(best)); sys.exit(0)
sys.exit(1)
PY
}

# 下载 + 校验 sha1；已有且校验通过就不重下
fetch_zip() {   # fetch_zip <url> <sha1> <目标文件> [大小字节]
    local url="$1" want="$2" out="$3" size="${4:-0}"
    if [ -s "$out" ]; then
        if [ "$(sha1sum "$out" | cut -d' ' -f1)" = "$want" ]; then
            log "缓存命中（sha1 一致）：$(basename "$out")（$(du -h "$out" | cut -f1)）"
            return 0
        fi
        warn "$(basename "$out") 已存在但 sha1 不符，重下"
    fi
    [ "$NO_DOWNLOAD" = 1 ] && die "缺 $(basename "$out") 且指定了 --no-download"
    mkdir -p "$(dirname "$out")"
    log "下载 $(basename "$out")$([ "$size" != 0 ] && printf '（%s MiB）' "$((size / 1048576))")"
    # 交互终端给进度条，重定向到日志时给静默：否则日志会被进度条刷爆
    local prog="--progress-bar"; [ -t 2 ] || prog="-sS"
    curl -fL --retry 3 "$prog" -o "$out.part" "$url" || die "下载失败：$url"
    mv -f "$out.part" "$out"
    local got; got="$(sha1sum "$out" | cut -d' ' -f1)"
    [ "$got" = "$want" ] || die "$(basename "$out") sha1 不匹配：期望 $want，实得 $got"
    log "sha1 校验通过 ✓ $(basename "$out")"
}

# ---------------------------------------------------------------------------
# 运行时：解出模拟器 + platform-tools，核对无头后端，写 RUNTIME.txt 与 .runtime-<平台>.meta
# ---------------------------------------------------------------------------
prepare_runtime() {   # prepare_runtime <平台>
    local plat="$1" rt="$STAGE_DIR/runtime-$plat" hostos
    hostos="$(platform_hostos "$plat")"
    mkdir -p "$STAGE_DIR"; rm -rf "$rt"; mkdir -p "$rt"

    local emu_url emu_size emu_sha emu_ver emu_chan emu_zip rec
    if [ -n "${EMU_ZIP_OVERRIDE[$plat]:-}" ]; then
        emu_zip="${EMU_ZIP_OVERRIDE[$plat]}"
        [ -s "$emu_zip" ] || die "指定的模拟器 zip 不存在：$emu_zip"
        emu_url="(本地) $emu_zip"; emu_sha="$(sha1sum "$emu_zip" | cut -d' ' -f1)"; emu_ver="-"; emu_chan="(指定)"
    else
        rec="$(sdk_archive emulator "$hostos")" || die "SDK 清单里没有 $hostos 的 emulator 包"
        IFS=$'\t' read -r emu_url emu_size emu_sha emu_ver emu_chan <<<"$rec"
        # ⚠️ 清单里的 url 是**相对路径**（"emulator-linux_x64-16428233.zip"），
        #    必须拼上镜像基址；直接拿去下载会 "Could not resolve host"（踩过）。
        case "$emu_url" in http*|ftp*) ;; *) emu_url="$SDK_MIRROR/$emu_url" ;; esac
        emu_zip="$CACHE_DIR/$(basename "$emu_url")"
        fetch_zip "$emu_url" "$emu_sha" "$emu_zip" "$emu_size"
    fi

    log "[$plat] 解包模拟器 → runtime/（$emu_chan $(basename "$emu_zip")）"
    unzip -q -o "$emu_zip" -d "$rt"

    local backend="$rt/emulator/$(platform_backend "$plat")"
    [ -s "$backend" ] || die "运行时里没有 x86_64 无头后端：$backend
    这个包带不动本 ROM（guest 是 x86_64，别的架构后端不行）"

    # platform-tools（adb）：让"解压即用"不依赖宿主装 Android SDK
    local pt_url pt_sha pt_ver pt_zip pt_size prec
    if [ -n "${PT_ZIP_OVERRIDE[$plat]:-}" ]; then
        pt_zip="${PT_ZIP_OVERRIDE[$plat]}"; pt_url="(本地) $pt_zip"
        pt_sha="$(sha1sum "$pt_zip" | cut -d' ' -f1)"; pt_ver="-"
    else
        prec="$(sdk_archive platform-tools "$hostos")" || die "SDK 清单里没有 $hostos 的 platform-tools 包"
        IFS=$'\t' read -r pt_url pt_size pt_sha pt_ver _pt_chan <<<"$prec"
        case "$pt_url" in http*|ftp*) ;; *) pt_url="$SDK_MIRROR/$pt_url" ;; esac
        pt_zip="$CACHE_DIR/$(basename "$pt_url")"
        fetch_zip "$pt_url" "$pt_sha" "$pt_zip" "$pt_size"
    fi
    unzip -q -o "$pt_zip" -d "$rt"
    local adbname="adb"; [ "$plat" = windows ] && adbname="adb.exe"
    [ -s "$rt/platform-tools/$adbname" ] || die "platform-tools 里没有 $adbname"

    # 只用得上本平台的 x86_64 后端：别的架构后端（若包里有）删掉省体积
    local qdir
    for qdir in "$rt"/emulator/qemu/*/; do
        [ -d "$qdir" ] || continue
        case "$(basename "$qdir")" in
            "$hostos-x86_64") ;;
            *) log "[$plat]   移除非本平台后端：emulator/qemu/$(basename "$qdir")"; rm -rf "$qdir" ;;
        esac
    done

    if [ "$SLIM" = 1 ]; then
        log "[$plat] 精简运行时（--slim）：删开发用文件（include / cmake / pkgconfig / 宏预览）"
        rm -rf "$rt/emulator/include" "$rt/emulator/lib/cmake" "$rt/emulator/lib/pkgconfig" \
               "$rt/emulator/resources/macroPreviews"
    fi

    # 版本/来源记录：包一解压就知道"这份运行时是哪来的"
    local buildid pkgrev
    pkgrev="$(sed -n 's/^Pkg\.Revision=//p' "$rt/emulator/source.properties" 2>/dev/null | head -1 || true)"
    buildid="$(sed -n 's/^Pkg\.BuildId=//p' "$rt/emulator/source.properties" 2>/dev/null | head -1 || true)"
    cat > "$rt/RUNTIME.txt" <<EOF
# 无头运行环境（$(platform_label "$plat")）
#
# 这个目录是 SDK 模拟器包 + platform-tools 解出来的，内容没有改动
# （只删掉了其它宿主平台的 qemu 后端；--slim 时另删开发用文件）。
包名        $(basename "$emu_zip")
渠道        $emu_chan
版本        ${pkgrev:-$emu_ver}
BuildId     ${buildid:-未知}
来源        $emu_url
sha1        $emu_sha
无头后端    emulator/$(platform_backend "$plat")
adb         platform-tools/$adbname（$(basename "$pt_zip")，sha1 $pt_sha）
EOF

    printf '%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n' \
        "$emu_zip" "$emu_url" "$emu_sha" "${pkgrev:-$emu_ver}" "${buildid:-}" "$(basename "$emu_zip")" \
        "$pt_zip" "$pt_url" "$pt_sha" "${pt_ver:-}" > "$STAGE_DIR/.runtime-$plat.meta"
    log "[$plat] 运行时就绪：$(basename "$emu_zip") → 版本 ${pkgrev:-?} build ${buildid:-?}"
}

# ---------------------------------------------------------------------------
# staging：铺出包里的目录树（结果路径放到 STAGED_ROOT）
# ---------------------------------------------------------------------------
STAGED_ROOT=""
stage_release() {   # stage_release <平台>
    local plat="$1" tag root
    tag="$(platform_tag "$plat")"
    root="$STAGE_DIR/autosnap-$VERSION-$tag"
    rm -rf "$root"; mkdir -p "$root"

    log "[$plat] 镜像 → images/（硬链接，不复制实体）"
    cp -al "$ROM_DIR" "$root/images" 2>/dev/null || { warn "硬链接不可用（跨文件系统？），退回复制"; cp -a "$ROM_DIR" "$root/images"; }

    log "[$plat] 运行时 → runtime/"
    cp -a "$STAGE_DIR/runtime-$plat" "$root/runtime"

    log "[$plat] 入口脚本 + 模板"
    mkdir -p "$root/bin" "$root/templates" "$root/tools"
    cp -a "$PACKAGING_DIR/bin/$plat/." "$root/bin/"
    chmod +x "$root"/bin/* 2>/dev/null || true
    cp -a "$PACKAGING_DIR/templates/." "$root/templates/"
    cp -f "$EMULATOR_CONFIG" "$root/templates/config.ini"     # 硬件唯一真源
    cat > "$root/templates/instance.env" <<'EOF'
# 实例登记模板（名字 ↔ 端口）。启动脚本按这个格式写到 .run/instances/<名字>.env。
PORT=5580
EOF

    # 验收探针 +（Linux）桥接工具
    [ -s "$ARTIFACTS_DIR/arm64-probe.apk" ] && cp -f "$ARTIFACTS_DIR/arm64-probe.apk" "$root/tools/"
    [ -s "$RUN_DIR/arm64-probe" ] && cp -f "$RUN_DIR/arm64-probe" "$root/tools/"
    if [ "$plat" = linux ]; then
        cp -f "$X64_DIR/tools/net-bridge.sh" "$root/tools/" 2>/dev/null || true
        cp -f "$X64_DIR/tools/net-bridge-ifup.sh" "$root/tools/" 2>/dev/null || true
    fi
    chmod +x "$root"/tools/* 2>/dev/null || true
    rm -f "$root/tools/.gitkeep"

    STAGED_ROOT="$root"
}

# START-HERE.md 的占位符替换（多行块用 python3 做，不比 sed 打架）
render_start_here() {   # render_start_here <平台> <root>
    local plat="$1" root="$2" tag; tag="$(platform_tag "$plat")"
    python3 - "$PACKAGING_DIR/START-HERE.md" "$root/START-HERE.md" "$plat" \
             "autosnap-$VERSION-$tag" "$VERSION" "$(platform_label "$plat")" \
             "$(date '+%Y-%m-%d %H:%M:%S %z')" "$(rom_fingerprint)" \
             "$(du -sh --apparent-size "$ROM_DIR" | cut -f1)" <<'PY'
import os, sys
(tpl, out, plat, rootdir, ver, label, built, fp, imgsize) = sys.argv[1:10]

def quickstart():
    if plat == "linux":
        return "\n".join(["```bash", f"cd {rootdir}",
                          "./bin/start-headless.sh                 # 默认端口 5580，全新冷启动，等开机完成",
                          "```"])
    return "\n".join(["```powershell", f"cd {rootdir}",
                      ".\\bin\\start-headless.ps1                 # 默认端口 5580，全新冷启动，等开机完成",
                      "```"])

def multi():
    if plat == "linux":
        return "\n".join(["./bin/start-headless.sh --name vm2 --port 5584    # 第二台（走 NAT）",
                          "./bin/start-headless.sh --name vm2 --reuse        # 复用它的数据再起",
                          "./bin/status.sh                                   # 看所有实例"])
    return "\n".join([".\\bin\\start-headless.ps1 -Name vm2 -Port 5584    # 第二台（走 NAT）",
                      ".\\bin\\start-headless.ps1 -Name vm2 -Reuse        # 复用它的数据再起",
                      ".\\bin\\status.ps1                                # 看所有实例"])

# 运行时信息从包内 RUNTIME.txt 读，保证"文档写的 == 包里实际带的"
rt = {}
rtfile = os.path.join(os.path.dirname(out), "runtime", "RUNTIME.txt")
if os.path.exists(rtfile):
    for line in open(rtfile, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split(None, 1)
        if len(parts) == 2:
            rt[parts[0]] = parts[1].strip()

adb = "./runtime/platform-tools/adb" if plat == "linux" else ".\\runtime\\platform-tools\\adb.exe"
subs = {
    "@VER@": ver, "@PLATFORM_LABEL@": label, "@BUILT_AT@": built, "@ROM_FINGERPRINT@": fp,
    "@RUNTIME_PKG@": rt.get("包名", "?"), "@RUNTIME_VER@": rt.get("版本", "?"),
    "@BUILD_ID@": rt.get("BuildId", "?"), "@IMAGES_SIZE@": imgsize,
    "@QUICKSTART@": quickstart(), "@MULTI_EXAMPLE@": multi(),
    "@STOP_CMD@": "./bin/stop.sh" if plat == "linux" else ".\\bin\\stop.ps1",
    "@VERIFY_CMD@": "./bin/verify.sh" if plat == "linux" else ".\\bin\\verify.ps1",
    "@ADB_EXAMPLE@": f"{adb} -s emulator-5580 shell getprop ro.product.cpu.abilist",
    "@ROOT_DIR@": rootdir,
    "@ENTRY_NAMES@": "start-headless / stop / status / verify",
    "@TOOLS_EXTRA@": "（另有 net-bridge.sh / net-bridge-ifup.sh：guest 桥接到物理 LAN）" if plat == "linux" else "",
}
text = open(tpl, encoding="utf-8").read()
for k, v in subs.items():
    text = text.replace(k, v)
leftover = [k for k in subs if k in text]
if leftover:
    sys.stderr.write("START-HERE.md 还有没替换的占位符：%s\n" % ", ".join(leftover))
    sys.exit(3)
open(out, "w", encoding="utf-8").write(text)
PY
}

# RELEASE.json + SHA256SUMS
write_manifest() {   # write_manifest <平台> <root>
    local plat="$1" root="$2" tag; tag="$(platform_tag "$plat")"
    local emu_zip emu_url emu_sha emu_ver emu_build _emubase pt_zip pt_url pt_sha pt_ver
    IFS='|' read -r emu_zip emu_url emu_sha emu_ver emu_build _emubase pt_zip pt_url pt_sha pt_ver \
        < "$STAGE_DIR/.runtime-$plat.meta"

    if [ "$VERIFY_IMAGES" = 1 ]; then
        log "[$plat] 校验镜像（sha256sum -c images/SHA256SUMS，$(du -sh "$root/images" | cut -f1)）"
        ( cd "$root/images" && sha256sum -c SHA256SUMS --quiet ) \
            || die "镜像与 SHA256SUMS 不符（$ROM_DIR 可能被改过；--no-verify-images 可跳过）"
    fi

    log "[$plat] RELEASE.json"
    # ⚠️ 顺序要紧：先写 RELEASE.json，再算 SHA256SUMS ——
    #    否则 RELEASE.json 自己不在清单里（"包内每个文件都可校验"就不成立了）。
    local n
    n="$(python3 - "$root" "$VERSION" "$plat" "$tag" "$emu_zip" "$emu_url" "$emu_sha" "$emu_ver" "$emu_build" \
             "$pt_zip" "$pt_url" "$pt_sha" "$pt_ver" "$ROM_DIR" "$PROJECT_ROOT" <<'PY'
import json, os, subprocess, sys
(root, ver, plat, tag, emu_zip, emu_url, emu_sha, emu_ver, emu_build,
 pt_zip, pt_url, pt_sha, pt_ver, romdir, projroot) = sys.argv[1:16]

def from_manifest(prefix):
    man = os.path.join(root, "images", "MANIFEST.txt")
    if not os.path.exists(man):
        return ""
    for line in open(man, encoding="utf-8", errors="replace"):
        if line.startswith(prefix):
            return line.split("=", 1)[1].strip()
    return ""

def system_sha():
    sums = os.path.join(root, "images", "SHA256SUMS")
    if not os.path.exists(sums):
        return ""
    for line in open(sums, encoding="utf-8", errors="replace"):
        h, _, p = line.strip().partition("  ")
        if p.lstrip("./") == "system.img":
            return h
    return ""

def git_head():
    try:
        return subprocess.run(["git", "-C", projroot, "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
    except Exception:
        return ""

# 此刻包里还没有 RELEASE.json 与 SHA256SUMS；清单最终列的是"除自己之外"的每个文件
files = 0; total = 0
for dirpath, _dirs, names in os.walk(root):
    for nm in names:
        files += 1
        try:
            total += os.path.getsize(os.path.join(dirpath, nm))
        except OSError:
            pass
sums_lines = files + 1

start = "bin/start-headless.sh" if plat == "linux" else "bin\\start-headless.ps1"
doc = {
    "name": f"autosnap-{ver}-{tag}",
    "version": ver,
    "platform": tag,
    "builtAt": subprocess.run(["date", "-Iseconds"], capture_output=True, text=True).stdout.strip(),
    "builtBy": "dev/04-x64-android/scripts/release.sh",
    "gitHead": git_head(),
    "rom": {
        "product": "remote_control_x64_arm64",
        "lunch": "remote_control_x64_arm64-userdebug",
        "fingerprint": from_manifest("ro.system.build.fingerprint"),
        "systemImgSha256": system_sha(),
        "androidApi": 31,
        "abilist": "x86_64,arm64-v8a",
    },
    "runtime": {
        "package": os.path.basename(emu_zip),
        "channel": "Stable",
        "version": emu_ver,
        "buildId": emu_build,
        "url": emu_url,
        "sha1": emu_sha,
        "backend": "emulator/" + ("qemu/linux-x86_64/qemu-system-x86_64-headless" if plat == "linux"
                                  else "qemu/windows-x86_64/qemu-system-x86_64.exe"),
        "accuracy": "headless：只带本平台的 x86_64 qemu 后端",
    },
    "platformTools": {"package": os.path.basename(pt_zip), "version": pt_ver, "url": pt_url, "sha1": pt_sha},
    "entrypoints": {
        "start": start,
        "stop": "bin/stop.sh" if plat == "linux" else "bin\\stop.ps1",
        "status": "bin/status.sh" if plat == "linux" else "bin\\status.ps1",
        "verify": "bin/verify.sh" if plat == "linux" else "bin\\verify.ps1",
    },
    "templates": ["templates/config.ini", "templates/instance.env", "templates/README.md"],
    "files": {"count": files + 1, "bytes": total, "sha256sumsLines": sums_lines},
    "sourceRomDir": romdir,
}
with open(os.path.join(root, "RELEASE.json"), "w", encoding="utf-8") as fh:
    json.dump(doc, fh, ensure_ascii=False, indent=2)
    fh.write("\n")
print(files)
PY
)"
    n="$((n + 1))"   # 清单最终会包含 RELEASE.json（python 计数时它还没写出来）

    log "[$plat] 逐文件 sha256（整包清单，$n 行）"
    local sums="$root/SHA256SUMS"
    # 中间文件放在 staging 里、**不放包根**：放包根会被自己的 find 收进清单，
    # 于是清单里出现两个打完包就不存在的临时文件（校验必然失败，踩过）。
    local tmpimg="$STAGE_DIR/.sums-$plat.images" tmprest="$STAGE_DIR/.sums-$plat.rest"
    local tmpcov="$STAGE_DIR/.sums-$plat.covered" tmpall="$STAGE_DIR/.sums-$plat.all"
    # 镜像那份直接用 ROM 自带的 SHA256SUMS（同一批文件、上面刚验过），改写路径即可；
    # 其余文件现算。这样 6 GB 的镜像不会被哈希两遍。
    sed -n 's|^\([0-9a-f]\{64\}\)  \.\/|\1  images/|p' "$root/images/SHA256SUMS" > "$tmpimg"
    # ⚠️ ROM 那份清单**不覆盖它自己**，也不覆盖打包后追加的 MANIFEST.txt ——
    #    所以要把"清单没覆盖的镜像文件"补算一遍，否则整包清单里就少了这两个
    #    （实测踩过：images/MANIFEST.txt 漏了）。
    ( cd "$root" && awk '{print $2}' images/SHA256SUMS | sed 's|^\./|images/|' | LC_ALL=C sort ) > "$tmpcov"
    ( cd "$root" && find images -type f | LC_ALL=C sort ) > "$tmpall"
    # ⚠️ comm 也要用 C locale：两份输入是按 LC_ALL=C 排的，用别的 locale 比较会报
    #    "文件没有被正确排序" 并退出非 0（在 set -e 下直接把打包带走，踩过）
    ( cd "$root" && LC_ALL=C comm -13 "$tmpcov" "$tmpall" | tr '\n' '\0' | xargs -0 -r sha256sum ) >> "$tmpimg"
    ( cd "$root" && find . -type f ! -path './images/*' ! -name SHA256SUMS \
        -print0 | LC_ALL=C sort -z | xargs -0 sha256sum ) > "$tmprest"
    cat "$tmpimg" "$tmprest" | LC_ALL=C sort -k2 > "$sums"
    rm -f "$tmpimg" "$tmprest" "$tmpcov" "$tmpall"
    [ "$(wc -l < "$sums")" = "$n" ] || warn "清单行数（$(wc -l < "$sums")）与文件数（$n）不一致，可能有文件没被覆盖"
}

make_zip() {   # make_zip <平台> <root>  → 打印 zip 路径（最后一行）
    local plat="$1" root="$2" name zip
    name="$(basename "$root")"
    mkdir -p "$RELEASE_DIR"
    zip="$RELEASE_DIR/$name.zip"
    rm -f "$zip"
    log "[$plat] 压缩（zip -r -$ZIP_LEVEL）→ release/$name.zip"
    # 在 staging 目录里打包，包里第一层就是 autosnap-<版本>-<平台>-x86_64/
    ( cd "$STAGE_DIR" && zip -q -r -"$ZIP_LEVEL" "$zip" "$name" )
    check_zip "$plat" "$zip"
    printf '%s\n' "$zip"
}

check_zip() {   # check_zip <平台> <zip>：结构自检（三样东西都在），打印条目数
    local plat="$1" zip="$2" name entries must missing="" adbname listing
    name="$(basename "$zip" .zip)"
    # ⚠️ 清单**只取一次**再比对：
    #    `unzip -Z1 "$zip" | grep -qxF ...` 里 grep 一命中就退出，unzip 吃 SIGPIPE（141），
    #    配上 `set -o pipefail` 就变成"文件明明在包里，却报缺失"（实测踩过：
    #    结构自检把 system-qemu.img / 无头后端 / templates 全报成缺失）。
    listing="$STAGE_DIR/.listing-$plat.txt"
    unzip -Z1 "$zip" > "$listing"
    entries="$(wc -l < "$listing")"
    adbname="adb"; [ "$plat" = windows ] && adbname="adb.exe"
    for must in \
        "$name/bin/start-headless.$([ "$plat" = linux ] && echo sh || echo ps1)" \
        "$name/runtime/emulator/$(platform_backend "$plat")" \
        "$name/runtime/platform-tools/$adbname" \
        "$name/images/system-qemu.img" \
        "$name/images/kernel-ranchu" \
        "$name/images/system/build.prop" \
        "$name/templates/config.ini" \
        "$name/templates/instance.env" \
        "$name/RELEASE.json" \
        "$name/SHA256SUMS" \
        "$name/START-HERE.md"; do
        grep -qxF -- "$must" "$listing" || missing="$missing $must"
    done
    rm -f "$listing"
    [ -z "$missing" ] || die "包结构不对，缺：$missing"
    printf '    %-46s %8s  %s 个条目\n' "$(basename "$zip")" "$(du -h "$zip" | cut -f1)" "$entries" >&2
}

smoke_linux() {   # smoke_linux <zip>
    local zip="$1" dir="$RUN_DIR/release-smoke" root pkg listing
    # ⚠️ 别写 `unzip -Z1 "$zip" | head -1`：head 拿到一行就退出，unzip 吃 SIGPIPE，
    #    pipefail 下整条命令非 0，`set -e` 会**静默**把打包脚本带走（踩过：冒烟一步都没跑）。
    #    先落成文件再取第一行，就没有管道早退这回事。
    listing="$RUN_DIR/.smoke-listing.txt"
    unzip -Z1 "$zip" > "$listing"
    root="$(head -1 "$listing" | cut -d/ -f1)"
    rm -f "$listing"
    log "冒烟：解压并真启动（端口 $SMOKE_PORT，adb=$SMOKE_ADB）"
    rm -rf "$dir"; mkdir -p "$dir"
    unzip -q "$zip" -d "$dir" || die "解压失败"
    pkg="$dir/$root"
    [ -d "$pkg" ] || die "解压后没有 $root 目录"
    if [ -x "$SMOKE_ADB" ]; then
        AUTOSNAP_ADB="$SMOKE_ADB" "$pkg/bin/start-headless.sh" --port "$SMOKE_PORT" --timeout 420 \
            || die "冒烟失败：从 release 包里起不来（看 $pkg/.run/emulator-$SMOKE_PORT.log）"
        AUTOSNAP_ADB="$SMOKE_ADB" "$pkg/bin/verify.sh" --port "$SMOKE_PORT" || die "冒烟失败：验收没过"
        AUTOSNAP_ADB="$SMOKE_ADB" "$pkg/bin/stop.sh" --port "$SMOKE_PORT" \
            || warn "停机没干净，手工看：$pkg/bin/stop.sh --port $SMOKE_PORT --force"
    else
        warn "没有可用的 adb（$SMOKE_ADB），只做解压 + 结构检查，不启动"
    fi
    log "冒烟目录：${dir#"$PROJECT_ROOT"/}（含启动日志与验收输出）"
    [ "$CLEAN_SMOKE" = 1 ] && { rm -rf "$dir"; log "已清理冒烟目录"; }
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
require_tools
resolve_version
check_rom

log "发布版本：$VERSION   平台：$PLATFORMS   压缩级别：-$ZIP_LEVEL"
log "ROM：${ROM_DIR#"$PROJECT_ROOT"/}（$(du -sh "$ROM_DIR" | cut -f1)）"
log "指纹：$(rom_fingerprint)"

if [ "$LIST_ONLY" = 1 ]; then
    log "计划（--list，不做任何改动）"
    printf '  输出目录   %s\n' "${RELEASE_DIR#"$PROJECT_ROOT"/}"
    printf '  缓存目录   %s\n' "${CACHE_DIR#"$PROJECT_ROOT"/}"
    printf '  运行时渠道 %s（linux/windows 取同一版本发布，build id 记进 RELEASE.json）\n' "$CHANNEL"
    if [ -s "$MANIFEST_XML" ]; then
        for p in linux windows; do
            rec="$(sdk_archive emulator "$p" 2>/dev/null || true)"
            if [ -n "$rec" ]; then
                IFS=$'\t' read -r u s sh v c <<<"$rec"
                printf '  %-8s %s（%s，%s MiB，sha1 %s…）\n' "$p" "$(basename "$u")" "$v" \
                    "$((s / 1048576))" "$(printf '%s' "$sh" | cut -c1-10)"
            fi
        done
    else
        printf '  （还没有 SDK 清单缓存：正式跑一次会先下载 %s/repository2-3.xml）\n' "$SDK_MIRROR"
    fi
    printf '  打包内容   runtime/（无头模拟器 + adb） + images/（%s） + templates/ + bin/ + START-HERE.md\n' \
        "$(du -sh --apparent-size "$ROM_DIR" | cut -f1)"
    exit 0
fi

mkdir -p "$RUN_DIR" "$CACHE_DIR" "$STAGE_DIR" "$RELEASE_DIR"
exec 9>"$RUN_DIR/release.lock"
flock -n 9 || die "另一个 release 打包实例正在跑（锁：$RUN_DIR/release.lock）"

if [ "$ZIP_ONLY" = 0 ]; then
    # 只有真的需要"按清单找包"时才去下载清单（全程用本地 zip 覆盖时可以完全离线）
    need_sdk=0
    for plat in $(platform_list); do
        [ -n "${EMU_ZIP_OVERRIDE[$plat]:-}" ] || need_sdk=1
        [ -n "${PT_ZIP_OVERRIDE[$plat]:-}" ] || need_sdk=1
    done
    [ "$need_sdk" = 1 ] && fetch_manifest
    for plat in $(platform_list); do prepare_runtime "$plat"; done
else
    log "--zip-only：复用 $STAGE_DIR 里已有的运行时与 staging"
fi

ZIPS=()
for plat in $(platform_list); do
    if [ "$ZIP_ONLY" = 1 ]; then
        root="$STAGE_DIR/autosnap-$VERSION-$(platform_tag "$plat")"
        [ -d "$root" ] || die "--zip-only 但 staging 不在：$root"
    else
        stage_release "$plat"; root="$STAGED_ROOT"
        render_start_here "$plat" "$root"
        write_manifest "$plat" "$root"
    fi
    if [ "$STAGE_ONLY" = 1 ]; then
        log "[$plat] --stage-only：只铺到 ${root#"$PROJECT_ROOT"/}（$(du -sh "$root" | cut -f1)）"
        ZIPS+=("$root")
    elif [ "$REUSE_ZIP" = 1 ] && [ -s "$RELEASE_DIR/$(basename "$root").zip" ]; then
        log "[$plat] --reuse-zip：复用已有 $(basename "$root").zip（只做结构自检）"
        check_zip "$plat" "$RELEASE_DIR/$(basename "$root").zip"
        ZIPS+=("$RELEASE_DIR/$(basename "$root").zip")
    else
        ZIPS+=("$(make_zip "$plat" "$root" | tail -1)")
    fi
done

echo
log "产物（$(du -sh "$RELEASE_DIR" 2>/dev/null | cut -f1)）"
for z in "${ZIPS[@]}"; do printf '  %s\n' "$z"; done

if [ "$SMOKE" = 1 ]; then
    [ "$STAGE_ONLY" = 1 ] && die "--smoke 需要真打成 zip（去掉 --stage-only）"
    for z in "${ZIPS[@]}"; do
        case "$z" in *linux*) smoke_linux "$z" ;; esac
    done
fi

if [ "$KEEP_STAGE" = 0 ] && [ "$STAGE_ONLY" = 0 ]; then
    log "清理 staging（--keep-stage 可保留）"
    rm -rf "$STAGE_DIR"/autosnap-* "$STAGE_DIR"/runtime-*
fi

log "完成。入口见包内 START-HERE.md；平台=${PLATFORMS}"
