#!/usr/bin/env bash
# verify.sh（macOS 版）—— 验收：这份 ROM 在**这台 Mac** 上是不是该有的样子。
#
#   ./verify.sh                       # 自动按镜像判断产品类型
#   ./verify.sh --host                # 只做宿主侧检查（不需要设备在跑）
#   ./verify.sh --device              # 连设备一起查（默认两者都做）
#   ./verify.sh --port 5580
#
# 与 bin/linux/verify.sh 的差别（**有意的**）：
#   Linux 版有「翻译层已注册」「arm64 应用映射库条数」这两组断言 —— 那是给
#   **x86_64 + libndk_translation** 那个产品用的。本脚本把它们改成**按产品分派**：
#     · 原生 arm64 产品：断言**没有**翻译层残留，且服务是 aarch64
#     · x86_64 桥产品：  断言翻译层在场，且服务是 x86_64
#   两种产品在同一份脚本里都成立，判据从**镜像文件**读，不信设备的自述。
#
# ⚠️ 判据取"文件里的事实"而不是"接口说成功"——这是上游项目被坑三次换来的纪律：
#    接口说成功而设备纹丝不动的情况出现过三次（见根 README）。
#
set -uo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

MODE="both"
PORT=""
PRODUCT_HINT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --host)    MODE="host"; shift ;;
        --device)  MODE="device"; shift ;;
        --port)    PORT="$2"; shift 2 ;;
        --product) PRODUCT_HINT="$2"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
done

CHECKS=0; FAILS=0
chk() {   # chk <描述> <实际值> <期望（正则或字面）>
    CHECKS=$((CHECKS+1))
    local desc="$1" got="$2" want="$3"
    if printf '%s' "$got" | grep -qE "^(${want})$"; then
        printf '  \033[1;32m✓\033[0m %-34s %s\n' "$desc" "$got"
    else
        FAILS=$((FAILS+1))
        printf '  \033[1;31m✗\033[0m %-34s %s \033[1;31m(期望 %s)\033[0m\n' "$desc" "$got" "$want"
    fi
}
chk_ok() {  # chk_ok <描述> <布尔：0=通过>
    CHECKS=$((CHECKS+1))
    if [ "$2" -eq 0 ]; then printf '  \033[1;32m✓\033[0m %s\n' "$1"
    else FAILS=$((FAILS+1)); printf '  \033[1;31m✗\033[0m %s\n' "$1"; fi
}

hr() { printf '\n\033[1;36m── %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------------------
# 0) 宿主
# ---------------------------------------------------------------------------
hr "0. 宿主"
printf '  macOS %s / %s（后端目录 %s）\n' "$(sw_vers -productVersion 2>/dev/null || echo '?')" "$HOST_MACH" "$QEMU_DIR"
chk_ok "Hypervisor.framework 可用（kern.hv_support=1）" "$(hv_usable && echo 0 || echo 1)"
if ! hv_usable; then
    warn "没有硬件虚拟化 → 模拟器会退到 TCG 全软件翻译，开机从 20 秒变几分钟（量级差）"
fi
require_runtime

# ---------------------------------------------------------------------------
# 1) 镜像：这是什么产品？该有什么？
# ---------------------------------------------------------------------------
hr "1. 镜像与产品类型"
require_images

ABI="$(sed -n 's/^ro\.product\.cpu\.abi=//p' "$IMAGES/system/build.prop" | tail -1)"
ABILIST64="$(sed -n 's/^ro\.system\.product\.cpu\.abilist64=//p' "$IMAGES/system/build.prop" | tail -1)"
DISPLAY_ABI="$(sed -n 's/^SystemImage\.Abi=//p' "$IMAGES/source.properties" 2>/dev/null | tail -1)"
NATIVE_BRIDGE="$(sed -n 's/^ro\.dalvik\.vm\.native\.bridge=//p' "$IMAGES/vendor/build.prop" 2>/dev/null | tail -1)"

# 产品判定：翻译层在不在 + guest ABI
if [ -n "$PRODUCT_HINT" ]; then
    PRODUCT="$PRODUCT_HINT"
elif [ "$ABI" = "arm64-v8a" ] && ! printf '%s' "$ABILIST64" | grep -q x86_64; then
    PRODUCT="arm64"
else
    PRODUCT="x64_arm64"
fi
ok "判定产品：$PRODUCT（guest abi=$ABI，abilist64=$ABILIST64）"

chk "guest ABI" "$ABI" "arm64-v8a|x86_64"
chk "source.properties 的 SystemImage.Abi 与 guest 一致" "$DISPLAY_ABI" "$ABI"

if [ "$PRODUCT" = "arm64" ]; then
    chk "abilist64 不含 x86_64（原生 arm64 不该有）" "$ABILIST64" "arm64-v8a"
    chk "native.bridge 为空或 0" "${NATIVE_BRIDGE:-0}" "0|"
    chk_ok "镜像里没有 libndk_translation 载荷" \
        "$([ -z "$(ls "$IMAGES"/system/lib64/libndk_translation*.so 2>/dev/null)" ] && echo 0 || echo 1)"
    chk_ok "镜像里没有 arm64 子目录（那是翻译层的落点）" \
        "$([ ! -e "$IMAGES/system/lib64/arm64" ] && echo 0 || echo 1)"
else
    chk "abilist64 含 arm64-v8a（翻译层带来的）" "$ABILIST64" ".*arm64-v8a.*"
    chk "native.bridge 指向翻译层" "$NATIVE_BRIDGE" "libndk_translation.so"
fi

# ---------------------------------------------------------------------------
# 2) 服务产物：架构必须与 guest 一致
#
# ⚠️ 这一组是本脚本的核心：它读的是**文件头里的 ELF 架构**，
#    而不是设备的自述、也不是接口返回的 ok。镜像里混进错架构的二进制，
#    症状是运行时 "exec format error"，很难查。
# ---------------------------------------------------------------------------
hr "2. 服务产物架构"
elf_arch() {   # elf_arch <文件> → aarch64 / x86-64 / ?
    # 用 od 读 ELF header 的 e_machine（偏移 18，2 字节小端）：0x3E=x86-64, 0xB7=aarch64
    local f="$1" m
    [ -s "$f" ] || { printf '缺失'; return; }
    m="$(od -An -tx1 -j18 -N2 "$f" 2>/dev/null | tr -d ' \n')"
    case "$m" in
        3e00) printf 'x86-64' ;;
        b700) printf 'aarch64' ;;
        *)    printf '未知(%s)' "$m" ;;
    esac
}

# 服务二进制在哪里：镜像的 system.img 是 ext4，宿主上看不见内部；
# 但交付目录带了 system/build.prop，而**服务二进制在 system.img 里**。
# 所以这里分两种情况：
#   · 若提供了解开的 system 目录（AUTOSNAP_SYSTEM_DIR），直接查文件
#   · 否则从 system.img 里按已知路径尝试提取（用 mke2fs 的 debugfs 不一定有，
#     退而求其次：只报"无法直接检查"，不假装通过）
SYS_DIR="${AUTOSNAP_SYSTEM_DIR:-}"
if [ -n "$SYS_DIR" ] && [ -d "$SYS_DIR" ]; then
    WANT_ARCH=$([ "$PRODUCT" = "arm64" ] && printf 'aarch64' || printf 'x86-64')
    for b in remote-control rcctl remote-control-launch; do
        chk "$b 架构" "$(elf_arch "$SYS_DIR/bin/$b")" "$WANT_ARCH"
    done
    chk_ok "system/etc/init/remote-control.rc 在" "$([ -s "$SYS_DIR/etc/init/remote-control.rc" ] && echo 0 || echo 1)"
else
    warn "未提供 AUTOSNAP_SYSTEM_DIR（解开的 system 目录）—— 跳过产物架构检查。
    想查的话：从交付目录解 system.img，或设 AUTOSNAP_SYSTEM_DIR=/path/to/system"
    # 至少核对 system.img 里的 abi 线索（build.prop 我们有了）——不假装通过
    CHECKS=$((CHECKS+1))
    printf '  \033[1;33m⊘\033[0m %s\n' "产物 ELF 架构（需要 AUTOSNAP_SYSTEM_DIR，未检查）"
fi

# ---------------------------------------------------------------------------
# 3) 设备侧（可选）
# ---------------------------------------------------------------------------
if [ "$MODE" = "host" ]; then
    hr "结论"
    printf '  宿主侧检查 %d 项，失败 %d 项\n' "$CHECKS" "$FAILS"
    [ "$FAILS" -eq 0 ] && { ok "宿主侧全部通过"; exit 0; } || { bad "有失败项"; exit 1; }
fi

require_adb
[ -n "$PORT" ] || PORT="$(instance_port "$DEFAULT_NAME")"
[ -n "$PORT" ] || die "没有登记实例可查（先 start-headless.sh，或用 --port 指定）"
SERIAL="$(serial_for_port "$PORT")"

hr "3. 设备（$SERIAL）"
"$ADB" -s "$SERIAL" get-state >/dev/null 2>&1 || die "设备 $SERIAL 不在线"
chk "boot_completed" "$(adb_prop "$PORT" sys.boot_completed)" "1"
chk "设备 guest ABI" "$(adb_prop "$PORT" ro.product.cpu.abi)" "$ABI"
chk "设备内核架构（uname -m）" "$("$ADB" -s "$SERIAL" shell uname -m 2>/dev/null | tr -d '\r' | sed -n '1p')" \
    "$([ "$PRODUCT" = "arm64" ] && printf 'aarch64' || printf 'x86_64')"
chk "ro.product.model" "$(adb_prop "$PORT" ro.product.model)" "remote-control.*"

hr "4. 服务在设备上"
chk_ok "remote-control 进程在跑" \
    "$("$ADB" -s "$SERIAL" shell 'ps -A 2>/dev/null | grep -c "[r]emote-control"' 2>/dev/null | tr -d '\r' | grep -qE '^[1-9]' && echo 0 || echo 1)"
chk_ok "/dev/uinput 存在（触控注入的前提）" \
    "$("$ADB" -s "$SERIAL" shell '[ -e /dev/uinput ] && echo 0 || echo 1' 2>/dev/null | tr -d '\r' | grep -q '^0$' && echo 0 || echo 1)"

# 服务能不能真的应答（比"进程在"更强）
HTTP_PORT="$(adb_prop "$PORT" persist.remote_control.http_port)"
if [ -n "$HTTP_PORT" ]; then
    chk_ok "HTTP 端口 $HTTP_PORT 有应答" \
        "$("$ADB" -s "$SERIAL" shell "echo -n > /dev/tcp/127.0.0.1/$HTTP_PORT" 2>/dev/null && echo 0 || echo 1)"
else
    warn "拿不到 HTTP 端口属性；如需验接口，用 adb forward 后 tools/check-api-docs.py"
fi

hr "结论"
printf '  共 %d 项检查，失败 %d 项\n' "$CHECKS" "$FAILS"
if [ "$FAILS" -eq 0 ]; then ok "全部通过"; exit 0; else bad "有 $FAILS 项失败"; exit 1; fi
