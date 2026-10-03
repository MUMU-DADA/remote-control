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
# 主 ROM：默认是当前 PRODUCT 的产物（默认 PRODUCT=x64_arm64 → x86_64 桥 ROM）。
# 它服务于 linux / windows / darwin-x64 这三个目标。
ROM_DIR="$ARTIFACTS_DIR/rom-$PRODUCT_NAME"
# arm64 原生 ROM：服务于 darwin-aarch64。
# 没显式给就按约定找 —— 它的名字由 PRODUCT=arm64 决定（PRODUCT_NAME=remote_control_arm64）。
ROM_DIR_ARM64=""
ROM_DIR_ARM64_DEFAULT="$ARTIFACTS_DIR/rom-remote_control_arm64"
# --images 是否被显式给过（决定 darwin-aarch64 能不能把主 ROM 当自己那份用）
IMAGES_EXPLICIT=0

PLATFORMS="both"
# darwin 要出哪一档：aarch64（Apple Silicon）/ x64（Intel Mac）/ both（两档都出）。
# `--platform all` 会把 both 当成默认，因为"全出"就该包含 mac 的两端。
# ⚠️ 它与 ROM 的 guest 架构是两件事：
#   · aarch64 宿主只带 arm64 guest 后端 → 必须配 arm64 原生 ROM
#   · x64 宿主带 x86_64 guest 后端   → 配现有的 x86_64+翻译层 ROM
#   见 docs/13-macos-port.md §7.0.2（实测：darwin-aarch64 包里没有 x86_64 后端）。
DARWIN_ARCH="aarch64"
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
        --darwin-arch) DARWIN_ARCH="${2:?}"; shift ;;
        --images-arm64) ROM_DIR_ARM64="${2:?}"; shift ;;
        --version)    VERSION="${2:?}"; shift ;;
        --channel)    CHANNEL="${2:?}"; shift ;;
        --images)     ROM_DIR="${2:?}"; IMAGES_EXPLICIT=1; shift ;;   # 主 ROM（x86_64 那三个目标用）
        --out)        RELEASE_DIR="${2:?}"; shift ;;
        --zip-level)  ZIP_LEVEL="${2:?}"; shift ;;
        --linux-emulator-zip)   EMU_ZIP_OVERRIDE[linux]="${2:?}"; shift ;;
        --windows-emulator-zip) EMU_ZIP_OVERRIDE[windows]="${2:?}"; shift ;;
        # ⚠️ darwin 的覆盖要**同时设三个键**：目标名是 darwin-aarch64 / darwin-x64，
        #    而用户只会写 --darwin-emulator-zip（他不知道目标名这层）。
        #    只设 [darwin] 的话，加了 both 之后两个目标都查不到覆盖 → 静默去联网。
        --darwin-emulator-zip)  EMU_ZIP_OVERRIDE[darwin]="${2:?}"
                                EMU_ZIP_OVERRIDE[darwin-aarch64]="${2:?}"
                                EMU_ZIP_OVERRIDE[darwin-x86_64]="${2:?}"; shift ;;
        --linux-platform-tools-zip)   PT_ZIP_OVERRIDE[linux]="${2:?}"; shift ;;
        --windows-platform-tools-zip) PT_ZIP_OVERRIDE[windows]="${2:?}"; shift ;;
        --darwin-platform-tools-zip)  PT_ZIP_OVERRIDE[darwin]="${2:?}"
                                      PT_ZIP_OVERRIDE[darwin-aarch64]="${2:?}"
                                      PT_ZIP_OVERRIDE[darwin-x86_64]="${2:?}"; shift ;;
        --smoke-port) SMOKE_PORT="${2:?}"; shift ;;
        --smoke-adb)  SMOKE_ADB="${2:?}"; SMOKE_ADB_EXPLICIT="${2:?}"; shift ;;
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
    linux|windows|darwin|both|all|darwin-aarch64|darwin-x86_64) ;;
    *) die "--platform 只能是 linux / windows / darwin[-aarch64|-x86_64] / both / all（现在：$PLATFORMS）" ;;
esac
case "$DARWIN_ARCH" in
    aarch64|x64|both) ;;
    *) die "--darwin-arch 只能是 aarch64（Apple Silicon）/ x64（Intel Mac）/ both（两档都出）（现在：$DARWIN_ARCH）" ;;
esac
case "$ZIP_LEVEL" in ''|*[!0-9]*) die "--zip-level 要是 0..9 的数字" ;; esac

# ===========================================================================
# 「目标」= 平台 + 宿主架构
#
# 四个目标名： linux / windows / darwin-aarch64 / darwin-x64
#
# ⚠️ 为什么要有"目标"这一层，而不是继续用"平台"：
#    三种平台里只有 darwin 分两档，而这两档**必须能同时出现在一次调用里**
#    （用户要的就是"mac 的 x64 端和 arm64 端一起出"）。
#    原来架构存在全局变量 DARWIN_ARCH 里 —— 一次调用只能有一档。
#    现在架构**从目标名里取**，全局那个降级为"`--platform darwin` 时默认出哪一档"。
#
# 下面这些函数名保留 `platform_` 前缀（改动面小），但**参数是目标名**。
# ===========================================================================
target_platform() {   # target_platform <目标> → linux | windows | darwin
    case "$1" in darwin-*) printf 'darwin' ;; *) printf '%s' "$1" ;; esac
}
target_arch() {   # target_arch <目标> → aarch64 | x64 |（非 darwin 为空）
    case "$1" in darwin-aarch64) printf 'aarch64' ;; darwin-x86_64) printf 'x64' ;; *) printf '' ;; esac
}

# 后端目录名。
#
# ⚠️⚠️ darwin 这档有**三套命名**，别照着一处猜另一处（实测踩到）：
#      SDK 包名                emulator-darwin_x64-*.zip     ← 下划线 + x64
#      清单里的 host-arch 字段  x64                           ← 选包用（platform_hostarch）
#      **包内的后端目录**       darwin-x86_64                 ← 连字符 + x86_64（本函数）
#    aarch64 那档三者恰好都是 aarch64 的写法，所以"照着包名猜目录名"能蒙对；
#    x64 那档一猜就错，报错是"运行时里没有后端"（这行检查救了这一次）。
#
#    另外：darwin-x86_64 包里**有** qemu-system-aarch64-headless —— Intel Mac 上技术上
#    能跨架构跑 arm64 guest，但那是全系统模拟，正是 docs/09-why-not-full-arm64-sim.md
#    用 28 轮实验否决掉的路线（性能不可接受）。所以"Intel Mac 配 x86_64 ROM"不变，
#    别因为"包里有那个文件"就改这一条。
platform_backend_dir() {   # platform_backend_dir <目标>
    case "$(target_platform "$1")" in
        linux)   printf 'linux-x86_64' ;;
        windows) printf 'windows-x86_64' ;;
        darwin)  printf 'darwin-%s' "$([ "$(target_arch "$1")" = aarch64 ] && printf aarch64 || printf x86_64)" ;;
    esac
}
# 该目标的后端可执行文件名（darwin 按目标名里的架构取 aarch64 或 x86_64 后端）
platform_backend() {   # platform_backend <目标>
    case "$(target_platform "$1")" in
        linux)   printf 'qemu/linux-x86_64/qemu-system-x86_64-headless' ;;
        windows) printf 'qemu/windows-x86_64/qemu-system-x86_64.exe' ;;
        darwin)  case "$(target_arch "$1")" in
                     aarch64) printf 'qemu/darwin-aarch64/qemu-system-aarch64-headless' ;;
                     # ⚠️ darwin-**x86_64**（不是 darwin-x64）—— 见 platform_backend_dir 的说明
                     x64)     printf 'qemu/darwin-x86_64/qemu-system-x86_64-headless' ;;
                 esac ;;
    esac
}
# 该目标 guest 端要什么架构（**决定用哪份 ROM**，也用于错误信息）
platform_guest_arch() {   # platform_guest_arch <目标>
    case "$1" in
        darwin-aarch64) printf 'arm64-v8a' ;;
        *)              printf 'x86_64' ;;
    esac
}
# 包名后缀。linux/windows 补 -x86_64；darwin 那两档的名字本身就是后缀
# （darwin-aarch64 / darwin-x64），所以直接透传 —— 这也是目标名取成这样的原因。
# 包名后缀。三档都是 <os>-<arch> 的形式，且与模拟器包内的后端目录名一致 ——
# 这样"包里是哪个后端"和"zip 叫什么"一眼对得上，少一处要记的映射。
platform_tag()    { case "$1" in linux) printf 'linux-x86_64' ;; windows) printf 'windows-x86_64' ;; darwin-*) printf '%s' "$1" ;; esac; }
platform_label()  { case "$1" in
    linux)          printf 'Linux x86_64（KVM）' ;;
    windows)        printf 'Windows x86_64（WHPX）' ;;
    darwin-aarch64) printf 'macOS Apple Silicon（Hypervisor.framework）' ;;
    darwin-x86_64)  printf 'macOS Intel（Hypervisor.framework）' ;;
esac; }
platform_hostos() { case "$(target_platform "$1")" in linux) printf 'linux' ;; windows) printf 'windows' ;; darwin) printf 'macosx' ;; esac; }
# 该目标宿主的 CPU 架构（**选包用**，与 guest 架构无关）。
# SDK 清单里 linux/windows 只有 x64；macosx 下 aarch64 与 x64 都有。
platform_hostarch() {   # platform_hostarch <目标>
    case "$(target_platform "$1")" in
        darwin) printf '%s' "$(target_arch "$1")" ;;
        *)      printf 'x64' ;;
    esac
}
# ⚠️ `both` 的语义**必须保持原样**（linux + windows）：
#    它是这个脚本原来的默认值，很多地方按"两个平台"假设它。
#    加 macOS 时我一度把 both 改成三个平台，结果**默认调用直接失败**
#    （因为 darwin/aarch64 需要 arm64 ROM，而默认那份是 x86_64 SROM）——
#    等于把所有人的默认行为弄坏了。三平台要用 `all`。
# 把 PLATFORMS + DARWIN_ARCH 展开成**目标列表**。
#
# ⚠️ 展开规则（每条都有理由）：
#   · `both` 仍是 linux + windows —— 它是默认值，语义不能变（改坏了所有人的默认行为）；
#   · `all`  = linux + windows + **darwin 的两档** —— "全出"就该包含 mac 的两端，
#              这也是用户要的"x64 端和 arm64 端"；
#   · `darwin` 单列时，出哪档由 --darwin-arch 决定（aarch64 / x64 / both）；
#   · 目标名里的 `-aarch64`/`-x64` 也允许直接写（如 --platform darwin-x64）。
target_list() {
    local darwin_targets
    case "$DARWIN_ARCH" in
        both)    darwin_targets="darwin-aarch64 darwin-x86_64" ;;
        aarch64) darwin_targets="darwin-aarch64" ;;
        x64)     darwin_targets="darwin-x86_64" ;;
    esac
    case "$PLATFORMS" in
        both)    printf 'linux windows' ;;
        # all 固定出两档（不跟 --darwin-arch 走）——"全出"就该包含 mac 的两端
        all)     printf 'linux windows darwin-aarch64 darwin-x86_64' ;;
        darwin)  printf '%s' "$darwin_targets" ;;
        darwin-aarch64|darwin-x86_64)
                 printf '%s' "$PLATFORMS" ;;
        *)       printf '%s' "$PLATFORMS" ;;
    esac
}
# 兼容旧名（还有别的脚本/文档在叫它）
platform_list() { target_list; }

require_tools() {
    local t missing=""
    for t in zip unzip python3 curl sha1sum sha256sum flock; do
        command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
    done
    # 打了 darwin 平台但当前不是 macOS（例如在 Linux 构建机上打全平台包）时，
    # 缺的不会是"命令"，而是**Apple SDK / 签名**那一层 —— 所以单独说清楚。
    if [ -z "$missing" ] && [ "$PLATFORMS" = darwin ] && [ "$(uname -s)" != "Darwin" ]; then
        warn "只打 darwin 包但当前宿主不是 macOS —— 只能出**未签名**的包；
    真要交付给最终用户，签名与公证必须在 Mac 上做（见 docs/14-macos-host-notes.md §7）。"
    fi
    [ -z "$missing" ] || die "缺工具：$missing"
}

# macOS 上有几个 GNU 工具名不存在（sha1sum/sha256sum/flock/numfmt/stat -c）。
# 目前只在 darwin 平台的**元信息**里用到 sha1sum；打包流程本身仍建议在 Linux 上跑。
# 这里只做提示，不做替换 —— 免得为了"能在 Mac 上跑"把打包脚本改成两套。
require_mac_tools() {
    [ "$(uname -s)" = "Darwin" ] || return 0
    local t missing=""
    for t in sha1sum sha256sum flock; do
        command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
    done
    # ⚠️ 这段消息里**不能出现 $(...) 或 $VAR**：die 的实参是双引号字符串，
    #    shell 会先做命令替换/变量展开 —— 写成 $(brew --prefix) 的话，
    #    bash 在**解析期**就会报 `未预期的记号 "(" 附近有语法错误`，
    #    整个脚本连语法检查都过不了（实测踩到）。
    [ -z "$missing" ] || die "在 macOS 上打包需要这些 GNU 工具：$missing
    装法： brew install coreutils flock
    装完把 coreutils 的 gnubin 目录加到 PATH 前面（路径用 brew --prefix coreutils 查）。
    或者**在 Linux 构建机上打包** —— 推荐，这条流水线本来就在 Linux 上跑。"
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

# 这个目标该用哪份 ROM。
#
# 判据是 **guest 架构**（platform_guest_arch），不是平台名：
# 平台名与"用哪份 ROM"不是一回事 —— darwin 的两档就要用两份不同的 ROM，
# 而 darwin-x64 与 linux/windows 用的是同一份。
rom_dir_for_target() {   # rom_dir_for_target <目标> → 打印 ROM 目录
    case "$(platform_guest_arch "$1")" in
        arm64-v8a)
            # ⚠️ 优先级不能反：**用户显式给的优先于"约定的默认"**。
            #    第一版这里直接返回约定目录，于是 `--images <某个 arm64 ROM>` 被
            #    悄悄丢掉、改用 artifacts/ 里那份 —— 显式参数被忽略比报错更糟。
            if [ -n "$ROM_DIR_ARM64" ]; then printf '%s' "$ROM_DIR_ARM64"; return; fi
            # 只指了一份 ROM 且它确实是 arm64 的 → 就用它（"我只有这一份"的直觉）
            if [ "$IMAGES_EXPLICIT" = 1 ] && [ "$(rom_guest_arch "$ROM_DIR")" = arm64 ]; then
                printf '%s' "$ROM_DIR"; return
            fi
            printf '%s' "$ROM_DIR_ARM64_DEFAULT" ;;
        *)  printf '%s' "$ROM_DIR" ;;
    esac
}

# 这次调用会用到的**所有** ROM（去重）—— 自检、指纹、--list 都要遍历它
all_rom_dirs() {
    local t seen="" d
    for t in $(target_list); do
        d="$(rom_dir_for_target "$t")"
        case " $seen " in *" $d "*) ;; *) seen="$seen $d"; printf '%s\n' "$d" ;; esac
    done
}

check_rom() {
    # 逐个检查**这次会用到的每份 ROM**（可能两份：x86_64 桥 + arm64 原生）
    local d missing f
    while IFS= read -r d; do
        if [ ! -d "$d" ]; then
            # arm64 那份缺了要给"怎么打出来"的指引 —— 它不是默认产物
            if [ "$d" = "${ROM_DIR_ARM64:-$ROM_DIR_ARM64_DEFAULT}" ]; then
                die "arm64 ROM 交付目录不存在：$d
    目标 $(target_list) 里含 darwin-aarch64，它需要**原生 arm64** ROM（与默认那份不通用）。
    打出来：
        PRODUCT=arm64 ./scripts/build-rom.sh && PRODUCT=arm64 ./scripts/package-rom.sh
    或者用 --images-arm64 <目录> 指到别处。
    只想出 Intel Mac 那档： --darwin-arch x64"
            fi
            die "ROM 交付目录不存在：$d
    先打 ROM： ./scripts/package-rom.sh"
        fi
        missing=""
        for f in $ROM_REQUIRED; do [ -s "$d/$f" ] || missing="$missing $f"; done
        [ -s "$d/system/build.prop" ] || missing="$missing system/build.prop"
        [ -z "$missing" ] || die "ROM 交付目录不全（$d），缺：$missing
    重新打： ./scripts/package-rom.sh"
    done <<EOF
$(all_rom_dirs)
EOF
}

# rom_fingerprint [目录] —— 不给就用主 ROM
rom_fingerprint() {
    grep -m1 '^ro\.system\.build\.fingerprint' "${1:-$ROM_DIR}/MANIFEST.txt" 2>/dev/null | sed 's/.*= *//' || true
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

# sdk_archive <包路径> <host-os> [host-arch] → "url<TAB>size<TAB>sha1<TAB>版本<TAB>渠道"
#
# ⚠️ host-arch 是**必须**的第三维：macosx 这个 host-os 下有 **两个** archive ——
#    darwin_aarch64 与 darwin_x64（实测 37.2.12 两个都在，396 MB / 466 MB）。
#    只看 host-os 会挑到错的那份，而错的那份**装得上、但起不来**
#    （Apple Silicon 的包里没有 x86_64 后端，见 docs/13-macos-port.md §7.0.2）。
# ⚠️ 但**不能把 host-arch 当成硬条件**：platform-tools 的 darwin 包**没有** host-arch
#    字段（两种 Mac 共用一份）。所以规则是"清单给了就比，没给就放过"。
sdk_archive() {
    python3 - "$MANIFEST_XML" "$1" "$2" "$CHANNEL" "${3:-}" <<'PY'
import sys, xml.etree.ElementTree as ET
xml, want, hostos, channel, want_arch = sys.argv[1:6]
want_arch = (want_arch or "").lower()
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
        # 清单给了 host-arch 就必须匹配；没给（如 platform-tools 的 darwin 包）则放过
        ha = a.find("host-arch")
        ha = (ha.text or "").lower() if ha is not None else ""
        if want_arch and ha and ha not in (want_arch, "any"):
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

    # ⚠️ 这一段是踩出来的，别简化：
    #   · `--retry 3` **扛不住** HTTP/2 的 stream 级错误
    #     （实测：`curl: (92) HTTP/2 stream 1 was not closed cleanly: INTERNAL_ERROR`），
    #     因为那不在 curl 认得出来该重试的类别里 —— 加了等于没加；
    #   · 所以用**显式重试循环** + `--http1.1`（该镜像的 HTTP/2 不稳）+ `-C -`（断点续传，
    #     重试从断点接着下，不从头再来）；
    #   · `--speed-time 20 --speed-limit 20480` 让**卡住的**传输自己早点失败，
    #     否则会一直吊着，日志上看起来像"还在下载"（这个更像 bug 的 bug 最难查）。
    # 同一份逻辑在 macos/fetch-emulator.sh 里也有一份，两边改要一起改。
    local attempt rc=1
    for attempt in 1 2 3; do
        [ "$attempt" -gt 1 ] && warn "第 $attempt 次尝试（断点续传）"
        # shellcheck disable=SC2086
        curl -fL --http1.1 -C - --retry 2 --retry-delay 2 \
             --speed-time 20 --speed-limit 20480 \
             $prog -o "$out.part" "$url" && { rc=0; break; }
        rc=$?
        warn "下载中断（curl 退出码 $rc）：$(basename "$out")"
        sleep $((attempt * 3))
    done
    [ "$rc" = 0 ] || die "下载失败（试了 $attempt 次）：$url
    网络/镜像问题可以：① 重跑本命令（会断点续传）；
    ② 手工把包放到缓存目录 $CACHE_DIR/ 再重跑；
    ③ 用 --<平台>-emulator-zip <本地 zip> 直接指定。"
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
        rec="$(sdk_archive emulator "$hostos" "$(platform_hostarch "$plat")")" \
            || die "SDK 清单里没有 $hostos/$(platform_hostarch "$plat") 的 emulator 包"
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
    # ⚠️ 不要写成 `[ -s "$backend" ] || die "...$(...)..."` ——
    #    die 的实参是双引号字符串，里面的 $( ) 会**先被展开**；一旦里面还有引号/括号，
    #    bash 在解析期就报 `未预期的记号 "(" 附近有语法错误`（实测踩到，整个脚本过不了 bash -n）。
    #    要带条件信息就用真正的 if。
    if [ ! -s "$backend" ]; then
        local arch_note=""
        [ "$(target_platform "$plat")" = darwin ] && arch_note="（目标=$plat）"
        die "运行时里没有后端：$backend
    这个包带不动本 ROM（guest 是 $(platform_guest_arch "$plat")，别的架构后端不行）。
    平台=$plat$arch_note"
    fi

    # platform-tools（adb）：让"解压即用"不依赖宿主装 Android SDK
    local pt_url pt_sha pt_ver pt_zip pt_size prec
    if [ -n "${PT_ZIP_OVERRIDE[$plat]:-}" ]; then
        pt_zip="${PT_ZIP_OVERRIDE[$plat]}"; pt_url="(本地) $pt_zip"
        pt_sha="$(sha1sum "$pt_zip" | cut -d' ' -f1)"; pt_ver="-"
    else
        # platform-tools 的 darwin 包没有 host-arch 字段（两种 Mac 共用），
        # 所以这里**故意不传** arch —— 传了也不会命中不了，但语义上不该要求它。
        prec="$(sdk_archive platform-tools "$hostos")" || die "SDK 清单里没有 $hostos 的 platform-tools 包"
        IFS=$'\t' read -r pt_url pt_size pt_sha pt_ver _pt_chan <<<"$prec"
        case "$pt_url" in http*|ftp*) ;; *) pt_url="$SDK_MIRROR/$pt_url" ;; esac
        pt_zip="$CACHE_DIR/$(basename "$pt_url")"
        fetch_zip "$pt_url" "$pt_sha" "$pt_zip" "$pt_size"
    fi
    unzip -q -o "$pt_zip" -d "$rt"
    # ⚠️ darwin 的 adb 也叫 `adb`（不是 adb.exe），与 linux 相同 ——
    #    但**包名不同**（platform-tools_r<v>-darwin.zip），且两种 Mac 共用同一份。
    local adbname="adb"; [ "$(target_platform "$plat")" = windows ] && adbname="adb.exe"
    [ -s "$rt/platform-tools/$adbname" ] || die "platform-tools 里没有 $adbname"

    # 只用得上本平台的 x86_64 后端：别的架构后端（若包里有）删掉省体积
    local qdir
    for qdir in "$rt"/emulator/qemu/*/; do
        [ -d "$qdir" ] || continue
        case "$(basename "$qdir")" in
            "$(platform_backend_dir "$plat")") ;;
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
stage_release() {   # stage_release <目标> <ROM 目录>
    local plat="$1" rom="$2" tag root
    tag="$(platform_tag "$plat")"
    root="$STAGE_DIR/autosnap-$VERSION-$tag"
    rm -rf "$root"; mkdir -p "$root"

    log "[$plat] 镜像 → images/（硬链接，不复制实体）"
    cp -al "$rom" "$root/images" 2>/dev/null || { warn "硬链接不可用（跨文件系统？），退回复制"; cp -a "$rom" "$root/images"; }

    log "[$plat] 运行时 → runtime/"
    cp -a "$STAGE_DIR/runtime-$plat" "$root/runtime"

    log "[$plat] 入口脚本 + 模板"
    mkdir -p "$root/bin" "$root/templates" "$root/tools"
    # ⚠️ 这里是**平台名**（packaging/bin/linux|windows|darwin/），不是目标名 ——
    #    目标名会是 darwin-aarch64，照它去找会 cp 失败（实测踩到）。
    cp -a "$PACKAGING_DIR/bin/$(target_platform "$plat")/." "$root/bin/"
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
    # 桥接工具只进 linux 包：macOS 没有 -net-tap 等价物，Windows 侧也没实现
    # （三平台一致的口径，见 docs/13-macos-port.md §3.5）
    if [ "$(target_platform "$plat")" = linux ]; then
        cp -f "$X64_DIR/tools/net-bridge.sh" "$root/tools/" 2>/dev/null || true
        cp -f "$X64_DIR/tools/net-bridge-ifup.sh" "$root/tools/" 2>/dev/null || true
    fi
    chmod +x "$root"/tools/* 2>/dev/null || true
    rm -f "$root/tools/.gitkeep"

    STAGED_ROOT="$root"
}

# START-HERE.md 的占位符替换（多行块用 python3 做，不比 sed 打架）
render_start_here() {   # render_start_here <目标> <root> <ROM 目录>
    local plat="$1" root="$2" rom="$3" tag; tag="$(platform_tag "$plat")"
    # ⚠️ 第二个位置参数给的是 **平台名**（target_platform），不是目标名：
    #    这个 python 里到处是 `plat == "windows"` / `plat == "darwin"` 的分支，
    #    传目标名（darwin-aarch64）进去，两处会**静默走错**：
    #      · "Apple Silicon 提示" 变成空
    #      · 第 6 节的"加速"行写成 Windows 的
    #    —— 包能打出来、自检也过，只是文档内容是错的。目标名单独作为最后一个参数给。
    python3 - "$PACKAGING_DIR/START-HERE.md" "$root/START-HERE.md" "$(target_platform "$plat")" \
             "autosnap-$VERSION-$tag" "$VERSION" "$(platform_label "$plat")" \
             "$(date '+%Y-%m-%d %H:%M:%S %z')" "$(rom_fingerprint "$rom")" \
             "$(du -sh --apparent-size "$rom" | cut -f1)" "$rom" "$plat" <<'PY'
import os, sys
(tpl, out, plat, rootdir, ver, label, built, fp, imgsize, romdir_abs, target) = sys.argv[1:12]
# plat  = 平台名（linux / windows / darwin）—— 分支判断用它
# target= 目标名（含架构，如 darwin-aarch64）—— 判"哪一档"用它

# ⚠️ 这里**没有 tag 变量**：tag 已经折进 rootdir（autosnap-<版本>-<平台tag>）。
#    判宿主架构要用 rootdir.endswith("aarch64")，写 tag 会 NameError（实测踩到）。
def quickstart():
    if plat == "windows":
        return "\n".join(["```powershell", f"cd {rootdir}",
                          ".\\bin\\start-headless.ps1                 # 默认端口 5580，全新冷启动，等开机完成",
                          "```"])
    # linux 与 darwin 都是 bash 三件套（bin/ 下同名同语义，平台差异在 lib.sh 里）
    return "\n".join(["```bash", f"cd {rootdir}",
                      "./bin/start-headless.sh                 # 默认端口 5580，全新冷启动，等开机完成",
                      "```"])

def multi():
    if plat == "windows":
        return "\n".join([".\\bin\\start-headless.ps1 -Name vm2 -Port 5584    # 第二台（走 NAT）",
                          ".\\bin\\start-headless.ps1 -Name vm2 -Reuse        # 复用它的数据再起",
                          ".\\bin\\status.ps1                                # 看所有实例"])
    return "\n".join(["./bin/start-headless.sh --name vm2 --port 5584    # 第二台（走 NAT）",
                      "./bin/start-headless.sh --name vm2 --reuse        # 复用它的数据再起",
                      "./bin/status.sh                                   # 看所有实例"])

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

# 第 6 节的表体：按产品类型给不同的行。
# 判据是 images/system/build.prop 里的 abilist64（文件里的事实），
# 因为"有没有翻译层"这件事只有它说了算 —— 平台名与产品名都可能骗人。
def dist_rows(rt, rootdir, romdir_abs):
    # ⚠️ 判据从 **$ROM_DIR（绝对路径）** 读，不要用 rootdir 拼：
    #    rootdir 在渲染时可能是个相对路径，拼出来读不到文件 —— 而读不到时
    #    这个函数会落到"原生 arm64"那一支，把 x86_64 桥产品渲染成原生描述（实测踩到）。
    import os as _os, sys as _sys
    abilist = ""
    bp = _os.path.join(romdir_abs, "system", "build.prop")
    if _os.path.exists(bp):
        for line in open(bp, encoding="utf-8"):
            if line.startswith("ro.system.product.cpu.abilist64="):
                abilist = line.split("=", 1)[1].strip()
    if not abilist:
        # 取不到判据就**说出来**，并按平台+tag 保守回退，不假装知道
        _sys.stderr.write("警告：读不到 %s 的 abilist64，第 6 节按平台回退渲染\n" % bp)
        bridge = not (plat == "darwin" and rootdir.endswith("aarch64"))
    else:
        bridge = "x86_64" in abilist      # 有 x86_64 就是桥产品
    accel = "同架构才有加速"
    if plat == "darwin":
        accel_row = "| 加速 | macOS 用 Hypervisor.framework（`sysctl kern.hv_support` 必须为 1）。" \
                    "为 0（常见于虚拟机里的 macOS）会退到纯软件模拟，开机从分钟级变十几分钟 |"
    elif plat == "linux":
        accel_row = "| 加速 | Linux 用 KVM（/dev/kvm 可读写）。没有就退到纯软件模拟（很慢） |"
    else:
        accel_row = "| 加速 | Windows 用 WHPX。没有就退到纯软件模拟（很慢） |"

    if bridge:
        return "\n".join([
            "| **翻译层许可** | `images/` 里的 `libndk_translation*` 是 Google 专有二进制"
            "（随 SDK 系统镜像分发，SDK 许可**不含再分发**）。内部使用/开发无碍；"
            "**对外交付整机前必须过法务** |",
            "| ABI 覆盖 | 本 ROM 是 `%s`（纯 64 位）：**32 位 ARM（armeabi-v7a）应用装不上** |" % (abilist or "x86_64,arm64-v8a"),
            "| 性能 | 串行依赖浮点实测退化 21~23×（整数/哈希约 1.1×）——目标应用先做性能验收 |",
            "| 版本绑定 | 翻译层与 Android 版本绑定：Android 12（API 31）的载荷只能配 API 31 的框架 |",
            accel_row,
        ])
    return "\n".join([
        "| 翻译层许可 | **不适用** —— 本 ROM 是原生 arm64，不含 `libndk_translation`，"
        "没有 Google 专有二进制（这是它相对桥产品的优势之一） |",
        "| ABI 覆盖 | 本 ROM 是 `%s`（纯 64 位）：**32 位 ARM（armeabi-v7a）装不上**；"
        "**任何 x86/x86_64 应用也装不上**（原生 arm64 系统没有翻译层兜底） |" % (abilist or "arm64-v8a"),
        "| 性能 | **没有翻译层开销**（桥产品才有 21~23× 的串行浮点退化）——"
        "应用跑的是原生 arm64 机器码 |",
        "| 版本绑定 | 不适用（无翻译层）。内核/框架/应用都是 arm64，与 Android 版本的关系同普通 ROM |",
        accel_row,
    ])

adb = ".\\runtime\\platform-tools\\adb.exe" if plat == "windows" else "./runtime/platform-tools/adb"
subs = {
    "@VER@": ver, "@PLATFORM_LABEL@": label, "@BUILT_AT@": built, "@ROM_FINGERPRINT@": fp,
    "@RUNTIME_PKG@": rt.get("包名", "?"), "@RUNTIME_VER@": rt.get("版本", "?"),
    "@BUILD_ID@": rt.get("BuildId", "?"), "@IMAGES_SIZE@": imgsize,
    "@QUICKSTART@": quickstart(), "@MULTI_EXAMPLE@": multi(),
    "@STOP_CMD@": ".\\bin\\stop.ps1" if plat == "windows" else "./bin/stop.sh",
    "@VERIFY_CMD@": ".\\bin\\verify.ps1" if plat == "windows" else "./bin/verify.sh",
    "@ADB_EXAMPLE@": f"{adb} -s emulator-5580 shell getprop ro.product.cpu.abilist",
    "@ROOT_DIR@": rootdir,
    # 第 6 节（交付约束）按**产品类型**渲染 —— 判据取包内 build.prop 的 abilist64，
    # 不看平台名（darwin+x64 用的就是桥产品 ROM，平台名判不出来）。
    "@DIST_ROWS@": dist_rows(rt, rootdir, romdir_abs),
    # guest 架构说明：原来是写死在模板里的"guest 是 x86_64，另有 ARM64 用户态翻译层"，
    # 对 arm64 原生包是错的（那份没有翻译层，也不跑 x86 应用）。
    "@GUEST_DESC@": ("arm64-v8a 原生，**无翻译层** —— 跑不了纯 x86/x86_64 应用，"
                     "应用需自带 arm64-v8a 库")
                    if plat == "darwin" and target.endswith("aarch64")
                    else "x86_64，另有 ARM64 用户态翻译层（可跑 arm64 应用）",
    "@ENTRY_NAMES@": "start-headless / stop / status / verify",
    "@TOOLS_EXTRA@": "（另有 net-bridge.sh / net-bridge-ifup.sh：guest 桥接到物理 LAN）" if plat == "linux" else "",
    # macOS 专属提示：让 START-HERE 里直接写清"这台机器只能跑哪种 ROM"
    "@PLATFORM_HINT@": "\n> ⚠️ **这台是 Apple Silicon**：只能跑 arm64 原生 ROM。"
                       "现有 x86_64 ROM 在 Apple Silicon 上**没有**可用的模拟器后端"
                       "（不是慢，是根本没有那条路 —— 见包内 runtime/RUNTIME.txt 的后端路径）。\n"
                       if plat == "darwin" and target.endswith("aarch64")
                       else "\n> ⚠️ **这台是 Intel Mac**：可跑现有 x86_64 ROM；"
                            "arm64 原生 ROM 在 Intel 上同样没有后端。\n"
                       if plat == "darwin" else "",
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
write_manifest() {   # write_manifest <目标> <root> <ROM 目录>
    local plat="$1" root="$2" rom="$3" tag; tag="$(platform_tag "$plat")"
    local emu_zip emu_url emu_sha emu_ver emu_build _emubase pt_zip pt_url pt_sha pt_ver
    IFS='|' read -r emu_zip emu_url emu_sha emu_ver emu_build _emubase pt_zip pt_url pt_sha pt_ver \
        < "$STAGE_DIR/.runtime-$plat.meta"

    if [ "$VERIFY_IMAGES" = 1 ]; then
        log "[$plat] 校验镜像（sha256sum -c images/SHA256SUMS，$(du -sh "$root/images" | cut -f1)）"
        ( cd "$root/images" && sha256sum -c SHA256SUMS --quiet ) \
            || die "镜像与 SHA256SUMS 不符（$rom 可能被改过；--no-verify-images 可跳过）"
    fi

    log "[$plat] RELEASE.json"
    # ⚠️ 顺序要紧：先写 RELEASE.json，再算 SHA256SUMS ——
    #    否则 RELEASE.json 自己不在清单里（"包内每个文件都可校验"就不成立了）。
    local n
    n="$(python3 - "$root" "$VERSION" "$plat" "$tag" "$emu_zip" "$emu_url" "$emu_sha" "$emu_ver" "$emu_build" \
             "$pt_zip" "$pt_url" "$pt_sha" "$pt_ver" "$rom" "$PROJECT_ROOT" \
             "$(platform_backend "$plat")" <<'PY'
import json, os, subprocess, sys
(root, ver, plat, tag, emu_zip, emu_url, emu_sha, emu_ver, emu_build,
 pt_zip, pt_url, pt_sha, pt_ver, romdir, projroot, backend) = sys.argv[1:17]

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
        # ⚠️ 这里原来是硬编码的 linux/else 二元判断 —— darwin 会落到 windows 那一支，
        #    RELEASE.json 里就写着 windows 的后端路径（实测踩到）。
        #    后端路径由 bash 侧算好传进来（platform_backend），别在 python 里再判一次。
        "backend": "emulator/" + backend,
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
    adbname="adb"; [ "$(target_platform "$plat")" = windows ] && adbname="adb.exe"
    for must in \
        "$name/bin/start-headless.$([ "$(target_platform "$plat")" = windows ] && echo ps1 || echo sh)" \
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

# 按平台挑冒烟用的 adb：**macOS 上没有 /usr/bin/adb**（那是 linux 的路径），
# 不换的话 darwin 包在 Mac 上冒烟会永远走"没有可用的 adb，只做结构检查"这条静默分支。
resolve_smoke_adb() {   # resolve_smoke_adb <目标> → 打印 adb 路径（可能不存在）
    if [ -n "${SMOKE_ADB_EXPLICIT:-}" ]; then printf '%s' "$SMOKE_ADB_EXPLICIT"; return; fi
    case "$(target_platform "$1")" in
        darwin) printf '/usr/local/bin/adb' ;;
        *)      printf '%s' "$SMOKE_ADB" ;;
    esac
}

# 当前宿主能不能跑这个平台的产物？
#
# ⚠️ 这个守卫是必须的：在 **Linux 构建机上打 darwin 包**是常规操作（"一份 ROM 三个
#    平台"就是这么来的），但冒烟意味着**真去执行包里的模拟器** ——
#    那是 Mach-O 二进制，在 Linux 上只会 "cannot execute binary file"。
#    不判的话，冒烟会以一条与代码无关的报错失败，把打包整体拖挂。
host_can_smoke() {   # host_can_smoke <目标> → 0 能
    local plat="$1" us; us="$(uname -s)"
    case "$(target_platform "$plat")" in
        linux)  [ "$us" = "Linux" ] ;;
        darwin) [ "$us" = "Darwin" ] ;;
        windows) return 1 ;;   # windows 侧的冒烟在 Windows 机器上做（见 windows/README）
    esac
}

smoke_linux() {   # smoke_linux <zip> <平台>
    local zip="$1" SMOKE_PLAT="${2:-linux}" dir="$RUN_DIR/release-smoke" root pkg listing
    # ⚠️ 别写 `unzip -Z1 "$zip" | head -1`：head 拿到一行就退出，unzip 吃 SIGPIPE，
    #    pipefail 下整条命令非 0，`set -e` 会**静默**把打包脚本带走（踩过：冒烟一步都没跑）。
    #    先落成文件再取第一行，就没有管道早退这回事。
    listing="$RUN_DIR/.smoke-listing.txt"
    unzip -Z1 "$zip" > "$listing"
    root="$(head -1 "$listing" | cut -d/ -f1)"
    rm -f "$listing"
    if ! host_can_smoke "$SMOKE_PLAT"; then
        warn "[$SMOKE_PLAT] 当前宿主（$(uname -s)）跑不了这个平台的产物 —— 跳过冒烟，只做结构检查"
        warn "  这是正常的：在 Linux 上打 darwin 包时没法真启动 macOS 的模拟器。"
        warn "  真冒烟请在目标平台上跑：解压后 ./bin/start-headless.sh && ./bin/verify.sh"
        return 0
    fi
    local smoke_adb; smoke_adb="$(resolve_smoke_adb "$SMOKE_PLAT")"
    log "冒烟：解压并真启动（端口 $SMOKE_PORT，adb=$smoke_adb）"
    rm -rf "$dir"; mkdir -p "$dir"
    unzip -q "$zip" -d "$dir" || die "解压失败"
    pkg="$dir/$root"
    [ -d "$pkg" ] || die "解压后没有 $root 目录"
    if [ -x "$smoke_adb" ]; then
        AUTOSNAP_ADB="$smoke_adb" "$pkg/bin/start-headless.sh" --port "$SMOKE_PORT" --timeout 420 \
            || die "冒烟失败：从 release 包里起不来（看 $pkg/.run/emulator-$SMOKE_PORT.log）"
        AUTOSNAP_ADB="$smoke_adb" "$pkg/bin/verify.sh" --port "$SMOKE_PORT" || die "冒烟失败：验收没过"
        AUTOSNAP_ADB="$smoke_adb" "$pkg/bin/stop.sh" --port "$SMOKE_PORT" \
            || warn "停机没干净，手工看：$pkg/bin/stop.sh --port $SMOKE_PORT --force"
    else
        warn "没有可用的 adb（$smoke_adb），只做解压 + 结构检查，不启动"
        warn "  提示：darwin 包在 macOS 上默认找 /usr/local/bin/adb；"
        warn "        用 --smoke-adb /path/to/adb 指定（包内自带的那份也行）"
    fi
    log "冒烟目录：${dir#"$PROJECT_ROOT"/}（含启动日志与验收输出）"
    # ⚠️ 这里**不能**写成 `[ "$CLEAN_SMOKE" = 1 ] && rm -rf ...`：
    #    条件为假时这条 AND 列表返回 1，函数就返回 1，于是 set -e 在"清理 staging / 完成"
    #    之前把整个脚本带走 —— 冒烟明明全绿，调用者却拿到**失败**的退出码（实测踩过）。
    if [ "$CLEAN_SMOKE" = 1 ]; then
        rm -rf "$dir"; log "已清理冒烟目录"
    fi
    return 0
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
require_tools
resolve_version
check_rom

log "发布版本：$VERSION   平台：$PLATFORMS（darwin:$DARWIN_ARCH）   压缩级别：-$ZIP_LEVEL"
log "目标：$(target_list)"
# 每个用到的 ROM 各报一行（可能两份：x86_64 桥 + arm64 原生）
while IFS= read -r _rd; do
    log "ROM：${_rd#"$PROJECT_ROOT"/}（$(du -sh "$_rd" | cut -f1)，指纹 $(rom_fingerprint "$_rd")）"
done <<EOF
$(all_rom_dirs)
EOF

# ---------------------------------------------------------------------------
# 「目标平台」与「这份 ROM 的 guest 架构」必须配套 —— 不配套就**明确拒绝**，
# 不要产出一个装得上、起不来的包。
#
# ⚠️ 为什么需要这条：release.sh 一次调用只认**一份 ROM**（ROM_DIR 是单个目录），
#    而 darwin-aarch64 需要 **arm64 原生 ROM**、linux/windows/Intel-Mac 需要
#    现有的 **x86_64+翻译层 ROM**。两者要各打一次。不判的话会出现
#    "darwin-aarch64 包装着 x86_64 镜像"这种包 —— 用户解压后模拟器起不来，
#    而报错信息完全指不到真因（实测过：包里没有对应架构的 qemu 后端）。
#
# 判据取 ROM 的 build.prop（**文件里的事实**，不是我们以为的产品名）。
# ---------------------------------------------------------------------------
# rom_guest_arch [ROM 目录] → arm64 / x86_64 / unknown
#
# ⚠️ **必须接目录参数。** 原来只读全局 $ROM_DIR —— 而调用方
#    （assert_rom_matches_platforms）是按目标传各自 ROM 的，
#    参数被忽略的后果是"每个目标都拿主 ROM 去比"：
#    检查看起来跑了，结论却是错的（假失败，或更糟：该拦的没拦住）。
rom_guest_arch() {
    local rom="${1:-$ROM_DIR}"
    local abi abilist
    abi="$(sed -n 's/^ro\.product\.cpu\.abi=//p' "$rom/system/build.prop" 2>/dev/null | tail -1)"
    abilist="$(sed -n 's/^ro\.system\.product\.cpu\.abilist64=//p' "$rom/system/build.prop" 2>/dev/null | tail -1)"
    if [ "$abi" = "arm64-v8a" ] && ! printf '%s' "$abilist" | grep -q x86_64; then
        printf 'arm64'
    elif printf '%s' "$abilist" | grep -q x86_64; then
        printf 'x86_64'
    else
        printf 'unknown'
    fi
}

assert_rom_matches_platforms() {
    local t rom rom_arch need want bad="" ok=""
    for t in $(target_list); do
        rom="$(rom_dir_for_target "$t")"
        want="$(platform_guest_arch "$t")"
        rom_arch="$(rom_guest_arch "$rom")"
        if [ "$rom_arch" = unknown ]; then
            warn "[$t] 读不出 guest 架构（$rom/system/build.prop）—— 跳过这一项的配套检查"
            continue
        fi
        case "$want" in
            arm64-v8a) want=arm64 ;;
            *)         want=x86_64 ;;
        esac
        if [ "$want" != "$rom_arch" ]; then
            # ⚠️ 用 $'\n' 而不是 \n —— 双引号里的 \n 是**字面反斜杠 n**，
            #    die 出去就是一行里带着 \n 字样（实测踩到，很难看）。
            bad="$bad"$'\n'"    - $t 需要 guest=$want 的 ROM，而 $(basename "$rom") 是 guest=$rom_arch"
        else
            ok="$ok $t"
        fi
    done
    if [ -n "$bad" ]; then
        die "目标与 ROM 不配套：$bad

    规则：**darwin-aarch64 要原生 arm64 ROM，其余（linux/windows/darwin-x64）要 x86_64 桥 ROM。**
    两份 ROM 不通用，所以分别指定：
      --images       <x86_64 桥 ROM 目录>     # 默认取当前 PRODUCT 的产物
      --images-arm64 <arm64 原生 ROM 目录>    # 默认取 artifacts/rom-remote_control_arm64
    打 arm64 那份：
      PRODUCT=arm64 ./scripts/build-rom.sh && PRODUCT=arm64 ./scripts/package-rom.sh
    只出 Intel Mac 那档（不需要 arm64 ROM）： --darwin-arch x64
    见 docs/13-macos-port.md §4（三个产品的分工）。"
    fi
    log "目标与 ROM 配套检查通过：$ok"
}
assert_rom_matches_platforms

if [ "$LIST_ONLY" = 1 ]; then
    log "计划（--list，不做任何改动）"
    printf '  输出目录   %s\n' "${RELEASE_DIR#"$PROJECT_ROOT"/}"
    printf '  缓存目录   %s\n' "${CACHE_DIR#"$PROJECT_ROOT"/}"
    printf '  运行时渠道 %s（同一渠道下各平台取同一版本发布，build id 记进 RELEASE.json）\n' "$CHANNEL"
    if [ -s "$MANIFEST_XML" ]; then
        # ⚠️ 原来这里写死 `for p in linux windows` —— 加了 darwin 之后 --list 会
        #    **假装没有 darwin**（用户以为三个平台都列了）。改成跟 platform_list 走。
        for p in $(target_list); do
            # ⚠️ 第二参是 **host-os**（platform_hostos → macosx/linux/windows），
            #    不是平台名（darwin/linux/windows）—— 两者只差 darwin→macosx 这一处，
            #    传错的话 lookup 会**静默**返回空，界面显示"清单里没有这个组合"，
            #    看着像清单缺包，其实是参数传错（实测踩到，靠 bash -x 才定位到）。
            rec="$(sdk_archive emulator "$(platform_hostos "$p")" "$(platform_hostarch "$p")" 2>/dev/null || true)"
            if [ -n "$rec" ]; then
                IFS=$'\t' read -r u s sh v c <<<"$rec"
                printf '  %-14s %-8s %-40s（%s，%s MiB）\n' "$p" "$(platform_hostarch "$p")" \
                    "$(basename "$u")" "$v" "$((s / 1048576))"
            else
                printf '  %-14s %-8s （清单里没有这个组合）\n' "$p" "$(platform_hostarch "$p")"
            fi
        done
        printf '  目标与 ROM 的搭配：\n'
        for p in $(target_list); do
            # ⚠️ 这里必须借一个变量：bash **不允许** ${$(命令)#前缀} 这种嵌套
            #    （参数展开里不能再放命令替换）。写成 ${$(...)} 是语法错。
            _rd="$(rom_dir_for_target "$p")"
            printf '    %-14s guest=%-8s ← %s\n' "$p" "$(platform_guest_arch "$p")" \
                "${_rd#"$PROJECT_ROOT"/}"
        done
    else
        printf '  （还没有 SDK 清单缓存：正式跑一次会先下载 %s/repository2-3.xml）\n' "$SDK_MIRROR"
    fi
    printf '  打包内容   runtime/（无头模拟器 + adb） + images/ + templates/ + bin/ + START-HERE.md\n'
    while IFS= read -r _rd; do
        printf '    images ← %s（%s）\n' "${_rd#"$PROJECT_ROOT"/}" "$(du -sh --apparent-size "$_rd" | cut -f1)"
    done <<EOF
$(all_rom_dirs)
EOF
    exit 0
fi

mkdir -p "$RUN_DIR" "$CACHE_DIR" "$STAGE_DIR" "$RELEASE_DIR"
exec 9>"$RUN_DIR/release.lock"
flock -n 9 || die "另一个 release 打包实例正在跑（锁：$RUN_DIR/release.lock）"

if [ "$ZIP_ONLY" = 0 ]; then
    # 只有真的需要"按清单找包"时才去下载清单（全程用本地 zip 覆盖时可以完全离线）
    need_sdk=0
    for plat in $(target_list); do
        [ -n "${EMU_ZIP_OVERRIDE[$plat]:-}" ] || need_sdk=1
        [ -n "${PT_ZIP_OVERRIDE[$plat]:-}" ] || need_sdk=1
    done
    [ "$need_sdk" = 1 ] && fetch_manifest
    for plat in $(target_list); do prepare_runtime "$plat"; done
else
    log "--zip-only：复用 $STAGE_DIR 里已有的运行时与 staging"
fi

ZIPS=()
for plat in $(target_list); do
    if [ "$ZIP_ONLY" = 1 ]; then
        root="$STAGE_DIR/autosnap-$VERSION-$(platform_tag "$plat")"
        [ -d "$root" ] || die "--zip-only 但 staging 不在：$root"
    else
        # 这个目标用哪份 ROM（可能与其他目标不同 —— darwin-aarch64 用的是 arm64 那份）
        stage_release "$plat" "$(rom_dir_for_target "$plat")"; root="$STAGED_ROOT"
        render_start_here "$plat" "$root" "$(rom_dir_for_target "$plat")"
        write_manifest "$plat" "$root" "$(rom_dir_for_target "$plat")"
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
    # ⚠️ 原来只按 zip 名里有没有 "linux" 来决定冒不冒烟，而且只覆盖 linux。
    #    现在按 **platform_list 的实际顺序**逐个试，并把平台名传进去 ——
    #    darwin（以及将来的平台）也能被冒烟，能不能跑由 host_can_smoke 判。
    for plat in $(target_list); do
        for z in "${ZIPS[@]}"; do
            case "$(basename "$z")" in
                *"$(platform_tag "$plat")"*) smoke_linux "$z" "$plat" ;;
            esac
        done
    done
fi

if [ "$KEEP_STAGE" = 0 ] && [ "$STAGE_ONLY" = 0 ]; then
    log "清理 staging（--keep-stage 可保留）"
    rm -rf "$STAGE_DIR"/autosnap-* "$STAGE_DIR"/runtime-*
fi

log "完成。入口见包内 START-HERE.md；平台=${PLATFORMS}"
