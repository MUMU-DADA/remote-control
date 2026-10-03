#!/usr/bin/env bash
# preflight.sh —— 在这台 Mac 上跑之前，把"能不能跑"一次性问清楚。
#
#   ./preflight.sh                     # 只检查，不改任何东西
#   ./preflight.sh --rom-dir DIR       # 顺带核对某个 ROM 交付目录
#   ./preflight.sh --rom-dir DIR --want-arch arm64
#
# 退出码：0 = 全部通过（可能有警告）；1 = 有硬性不满足项。
#
# 为什么需要它：macOS 上的失败多数**不是报错而是静默变慢或静默不解压**——
#   · 架构选错 → 模拟器起不来（后端只有本机架构）
#   · HVF 不可用 → 退到 TCG 全软件翻译，开机从 20 秒变几分钟（量级差，见 docs/09）
#   · unzip 丢可执行位 → "Permission denied"，看着像权限问题其实是解压行为
#   · 脚本没有可执行位 → 同上
#   · bash 3.2 → 某些脚本的 declare -A 会直接报错
# 这些在真跑之前都问得出来。
#
# ⚠️ macOS 自带 /bin/bash 是 3.2，本脚本只用 3.2 有的语法。
#
set -uo pipefail

ROM_DIR=""
WANT_ARCH=""
FAILED=0

while [ $# -gt 0 ]; do
    case "$1" in
        --rom-dir)   ROM_DIR="$2"; shift 2 ;;
        --want-arch) WANT_ARCH="$2"; shift 2 ;;
        -h|--help)   sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

pass() { printf '  \033[1;32m✓\033[0m %s\n' "$*"; }
warn() { printf '  \033[1;33m!\033[0m %s\n' "$*"; }
fail() { printf '  \033[1;31m✗\033[0m %s\n' "$*"; FAILED=1; }
hdr()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------------------
hdr "系统与架构"

OS="$(uname -s)"
MACH="$(uname -m)"
if [ "$OS" = "Darwin" ]; then pass "macOS $(sw_vers -productVersion 2>/dev/null || echo '?')（$(sw_vers -buildVersion 2>/dev/null || echo '?')）"
else fail "不是 macOS（uname -s = $OS）—— 这套脚本只用于 mac 宿主"; fi

case "$MACH" in
    arm64)  pass "宿主架构 arm64（Apple Silicon）→ 需要 **arm64 原生 ROM**"
            NOTE_ARCH="arm64" ;;
    x86_64) pass "宿主架构 x86_64（Intel Mac）→ 可用现有 x86_64 ROM"
            NOTE_ARCH="x64" ;;
    *)      fail "认不出的宿主架构：$MACH" ;;
esac

if [ -n "$WANT_ARCH" ] && [ -n "${NOTE_ARCH:-}" ] && [ "$WANT_ARCH" != "$NOTE_ARCH" ]; then
    fail "--want-arch $WANT_ARCH 与本机架构 $NOTE_ARCH 不符"
fi

# ---------------------------------------------------------------------------
hdr "虚拟化（决定开机是 20 秒还是几分钟）"

if [ "$OS" = "Darwin" ]; then
    HV="$(sysctl -n kern.hv_support 2>/dev/null || echo '?')"
    if [ "$HV" = "1" ]; then
        pass "kern.hv_support=1 → Hypervisor.framework 可用（模拟器会用 hvf 加速）"
    elif [ "$HV" = "0" ]; then
        fail "kern.hv_support=0 → 没有硬件虚拟化，模拟器只能退到 TCG 全软件翻译。
    量级代价：同架构 KVM 开机 23.8 秒，TCG 是 5–8 分钟（见 docs/09-why-not-full-arm64-sim.md）。
    常见原因：跑在虚拟机里的 macOS，或机型太老。"
    else
        warn "读不到 kern.hv_support（输出 '$HV'）"
    fi
fi

# ---------------------------------------------------------------------------
hdr "必需工具"

for t in python3 curl unzip; do
    if command -v "$t" >/dev/null 2>&1; then
        pass "$t：$(command -v "$t")"
    else
        case "$t" in
            python3) fail "缺 python3 —— macOS 上执行：xcode-select --install" ;;
            *)       fail "缺 $t" ;;
        esac
    fi
done

if command -v shasum >/dev/null 2>&1; then pass "shasum（校验用）"
elif command -v openssl >/dev/null 2>&1; then warn "没有 shasum，将用 openssl sha1 兜底"
else fail "既没有 shasum 也没有 openssl，无法校验下载"; fi

# 宿主 bash 版本：脚本本身用 3.2 语法，但要提醒**别的脚本**可能有 4+ 依赖
BVER="${BASH_VERSION:-?}"
case "$BVER" in
    3.2*) warn "宿主 bash 是 3.2（macOS 自带）—— 本脚本兼容，但 release.sh 里用了
    declare -A（需 bash 4+）。如果在 Mac 上跑打包，先 brew install bash。";;
    *)    pass "bash $BVER" ;;
esac

# ---------------------------------------------------------------------------
hdr "模拟器与 adb"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FOUND_EMU=""
for cand in "$SCRIPT_DIR/../sdk/emulator/emulator" \
            "${ANDROID_SDK_ROOT:-}/emulator/emulator" \
            "${ANDROID_HOME:-}/emulator/emulator" \
            "$HOME/Library/Android/sdk/emulator/emulator"; do
    [ -n "$cand" ] && [ -x "$cand" ] && { FOUND_EMU="$cand"; break; }
done

if [ -n "$FOUND_EMU" ]; then
    pass "emulator：$FOUND_EMU"
    QDIR="$(dirname "$FOUND_EMU")/qemu"
    if [ -d "$QDIR" ]; then
        BACKENDS="$(ls "$QDIR" 2>/dev/null | tr '\n' ' ')"
        pass "qemu 后端：$BACKENDS"
        case "${NOTE_ARCH:-}" in
            arm64) ls "$QDIR" | grep -q aarch64 || fail "包里没有 aarch64 后端 —— 这份模拟器跑不了 arm64 ROM" ;;
            x64)   ls "$QDIR" | grep -q x86_64  || fail "包里没有 x86_64 后端 —— 这份模拟器跑不了现有 x86_64 ROM" ;;
        esac
    else
        fail "emulator 旁边没有 qemu/ 目录（可能解压不完整）"
    fi
    EXEC_OK=1
    [ -x "$FOUND_EMU" ] && [ -r "$FOUND_EMU" ] || EXEC_OK=0
    [ "$EXEC_OK" = 1 ] && pass "emulator 有可执行位" \
        || fail "emulator 没有可执行位 —— unzip 在 macOS 上不保留权限，执行：
    chmod +x '$FOUND_EMU' \"\$(dirname '$FOUND_EMU')\"/qemu/*/qemu-system-*"
else
    warn "还没准备模拟器 —— 先跑 macos/fetch-emulator.sh"
fi

ADB=""
for cand in "$SCRIPT_DIR/../sdk/platform-tools/adb" \
            "${ANDROID_SDK_ROOT:-}/platform-tools/adb" \
            "$HOME/Library/Android/sdk/platform-tools/adb"; do
    [ -n "$cand" ] && [ -x "$cand" ] && { ADB="$cand"; break; }
done
if [ -n "$ADB" ]; then
    pass "adb：$ADB"
    "$ADB" version 2>/dev/null | head -1 | sed 's/^/      /'
else
    warn "没有 adb（platform-tools 未就绪）"
fi

# ---------------------------------------------------------------------------
hdr "磁盘与 Gatekeeper"

# 数据盘要有地方放 datadir：32 G 的 userdata + 快照
if [ -n "$ROM_DIR" ] && [ -d "$ROM_DIR" ]; then
    AVAIL_KB="$(df -Pk "$ROM_DIR" 2>/dev/null | awk 'NR==2{print $4}')"
else
    AVAIL_KB="$(df -Pk "$HOME" 2>/dev/null | awk 'NR==2{print $4}')"
fi
if [ -n "$AVAIL_KB" ]; then
    AVAIL_GB=$(( AVAIL_KB / 1048576 ))
    if [ "$AVAIL_GB" -ge 60 ]; then pass "可用空间 ${AVAIL_GB} GiB"
    elif [ "$AVAIL_GB" -ge 40 ]; then warn "可用空间 ${AVAIL_GB} GiB —— 够跑一台，但快照/多实例要省着用"
    else fail "可用空间只有 ${AVAIL_GB} GiB —— 数据分区本身就要 32 G，再加镜像与快照会爆"; fi
fi

if [ -n "$ROM_DIR" ] && [ -d "$ROM_DIR" ]; then
    # ⚠️ xattr 在"该文件没有扩展属性"时返回非 0；脚本开了 set -o pipefail，
    #    直接放进 $( ) 会让 `XA="$(...)"` 带上非 0 退出码。这里显式吃掉。
    XA="$(xattr -l "$ROM_DIR" 2>/dev/null | grep -c quarantine)" || XA=0
    if [ "${XA:-0}" != "0" ]; then
        fail "ROM 目录带 com.apple.quarantine（从网上下载的痕迹）—— 模拟器会被 Gatekeeper 拦。
    解除： xattr -dr com.apple.quarantine '$ROM_DIR'"
    else
        pass "ROM 目录没有 quarantine 标记"
    fi
    MISSING=""
    for f in system-qemu.img ramdisk-qemu.img kernel-ranchu userdata.img config.ini; do
        [ -s "$ROM_DIR/$f" ] || MISSING="$MISSING $f"
    done
    if [ -z "$MISSING" ]; then pass "ROM 关键文件齐全（在 $ROM_DIR）"
    else fail "ROM 目录缺文件：$MISSING"; fi
else
    [ -n "$ROM_DIR" ] && warn "--rom-dir 不存在：$ROM_DIR"
fi

# ---------------------------------------------------------------------------
hdr "结论"
if [ "$FAILED" -eq 0 ]; then
    printf '  \033[1;32m全部通过。\033[0m'
    case "${NOTE_ARCH:-}" in
        arm64) printf ' 注意：这台是 Apple Silicon，**必须用 arm64 原生 ROM**（现有 x86_64 ROM 跑不了）。\n' ;;
        x64)   printf ' 这台是 Intel Mac，现有 x86_64 ROM 理论上可跑（HVF 路径尚未实测）。\n' ;;
        *)     printf '\n' ;;
    esac
    exit 0
else
    printf '  \033[1;31m有硬性不满足项（见上面的 ✗）。\033[0m\n'
    exit 1
fi
