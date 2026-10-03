#!/usr/bin/env bash
# =============================================================================
# macOS 支持的体检：**不需要网络、不需要真 Mac、不需要真镜像**，一分钟跑完。
#
#   bash tools/test-macos-port.sh
#
# 为什么要有它：macOS 那一层的逻辑（后端按架构选包、平台与 ROM 配套、mac 版 lib.sh 的
# 平台函数、START-HERE 按产品类型渲染）**在 Linux 构建机上全能验**，
# 唯一的障碍是这些代码平时只有"在 Mac 上跑"才会被走到 —— 这个脚本就是那台假 Mac。
#
# 验五组：
#   [0] 静态：语法 + macOS 专属的 bash 3.2 限制（无 declare -A / ${v^^} / mapfile）
#        + 不许再有指向旧目录名 packaging/bin/macos 的引用
#   [1] 平台分派：五个派生函数 × 三个平台（tag / label / host-os / host-arch / 后端 / guest 架构）
#   [2] 平台与 ROM 配套：四种组合的接受/拒绝（判据取 build.prop，不是产品名）
#   [3] 真跑 release.sh 打 darwin 包（假 zip 覆盖，全程离线）→ 断言包结构与渲染结果
#   [4] 回归：同一个假 ROM 打 linux 包，第 6 节必须仍是"桥产品"的行
#   [5] mac 版 lib.sh 的平台函数：命令 stub 伪装成 macOS（sysctl/system_profiler/ps/df）
#
# 真包与真启动走：./scripts/release.sh --platform darwin --smoke（要一台真 Mac）
# =============================================================================
set -uo pipefail

X64_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$X64_DIR/../.."
SANDBOX="$REPO_ROOT/.tmp/mactest"
PASS=0; FAIL=0
okc()  { printf '  \033[1;32m✓\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
badc() { printf '  \033[1;31m✗\033[0m %s\n' "$1" >&2; FAIL=$((FAIL+1)); }
chk()  { if [ "$2" = "$3" ]; then okc "$1（$3）"; else badc "$1：期望 $3，实得 $2"; fi; }
has()  { if printf '%s' "$2" | grep -qF -- "$3"; then okc "$1"; else badc "$1：没找到 [$3]"; fi; }
hasnt(){ if printf '%s' "$2" | grep -qF -- "$3"; then badc "$1：不该出现 [$3]"; else okc "$1"; fi; }

rm -rf "$SANDBOX"; mkdir -p "$SANDBOX"

# =============================================================================
echo "── [0] 静态检查"
# =============================================================================
for f in "$X64_DIR"/scripts/*.sh "$X64_DIR"/macos/*.sh "$X64_DIR"/packaging/bin/darwin/*.sh \
         "$X64_DIR"/packaging/bin/linux/*.sh; do
    [ -f "$f" ] || continue
    if bash -n "$f" 2>/dev/null; then okc "$(basename "$f") 语法"; else badc "$(basename "$f") 语法"; fi
done

# macOS 自带 /bin/bash 是 3.2：这些语法在 3.2 上不存在，写了就是"在 Mac 上直接挂"
#
# ⚠️ 必须先**剔掉注释行**再查：这几条在 lib.sh 的注释里被当成"我们不用它"的例子写着
#    （"只用 bash 3.2 语法：无 declare -A、无 ${var^^}、无 mapfile"），
#    不剔注释就会把说明文字判成违规 —— 第一次跑本脚本就是这样误报的。
#    另：单引号里的字面量（如 grep '^[a-z]*()'）也会误报，这里接受的代价是
#    这几条只用来防"真的写了 bash 4 语法"，宁可偶尔漏也不要天天误报。
strip_comments() { grep -vE '^[[:space:]]*#' "$1"; }
for f in "$X64_DIR"/packaging/bin/darwin/*.sh; do
    body="$(strip_comments "$f")"
    hasnt "$(basename "$f") 不用 declare -A" "$body" "declare -A"
    hasnt "$(basename "$f") 不用 mapfile"    "$body" "mapfile"
    if printf '%s' "$body" | grep -qE '\$\{[A-Za-z_]+(\^\^|,,)'; then
        badc "$(basename "$f") 用了 \${v^^}/\${v,,}（bash 4+）"
    else
        okc "$(basename "$f") 不用 \${v^^}"
    fi
done

# 目录改名之后不许再有旧引用（release.sh 按平台名找 bin/<plat>/）
#
# ⚠️ 两个边界，第一次写都踩了：
#   · 排除本文件自己 —— 这串字面量就写在这行里，不排除每次必然自指误报；
#   · 只查**代码**，不查文档 —— docs/13-macos-port.md 里「packaging/bin/macos → darwin」
#     是记录这次改名的历史说明，不是残留引用。把它算进去就是天天误报，
#     然后所有人学会忽略这个检查 —— 那比没有检查更糟。
STALE="$(grep -rln "packaging/bin/macos" \
            "$X64_DIR/scripts" "$X64_DIR/macos" "$X64_DIR/packaging" "$X64_DIR/tools" 2>/dev/null \
         | grep -v "test-macos-port.sh" || true)"
if [ -z "$STALE" ]; then okc "代码里没有指向 packaging/bin/macos 的残留引用"
else badc "还有代码引用旧目录名：$STALE"; fi

if [ -f "$X64_DIR/packaging/bin/darwin/lib.sh" ]; then okc "packaging/bin/darwin/ 存在"
else badc "packaging/bin/darwin/ 不存在（release.sh 会按平台名找它）"; fi

# =============================================================================
echo
echo "── [1] 平台分派（五个派生函数 × 三个平台）"
# =============================================================================
# 用 --list 的行为间接验：能列出该平台的包名，说明 tag/hostos/backend 都拼对了
# ⚠️ 抽函数块，**不要 source common.sh**：common.sh 在非 Linux/未配置时会 die，
#    而 die 会 exit 掉整个子 shell → 四项断言全空（第一次跑就是这样，看着像函数坏了）。
#    这几张分派表是自包含的（只依赖 PLATFORMS / DARWIN_ARCH 两个变量），直接抽出来 eval。
DISPATCH_SRC="$SANDBOX/dispatch.sh"
sed -n '/^platform_backend_dir() {/,/^platform_list() {/p' "$X64_DIR/scripts/release.sh" | head -n -1 > "$DISPATCH_SRC"

disp() {   # disp <平台> [darwin-arch] → "tag|后端目录|hostos|hostarch|guest架构|后端路径"
    PLATFORMS="$1" DARWIN_ARCH="${2:-aarch64}" bash -c '
        set -uo pipefail
        source "'"$DISPATCH_SRC"'"
        printf "%s|%s|%s|%s|%s|%s" "$(platform_tag "$1")" "$(platform_backend_dir "$1")" \
            "$(platform_hostos "$1")" "$(platform_hostarch "$1")" "$(platform_guest_arch "$1")" \
            "$(platform_backend "$1")"
    ' _ "$1" 2>/dev/null
}
if [ -s "$DISPATCH_SRC" ] && grep -q "^platform_backend\|^platform_backend()" "$DISPATCH_SRC"; then
    okc "分派函数块抽出来了（$(wc -l < "$DISPATCH_SRC") 行）"
else
    badc "分派函数块没抽出来 —— 检查 release.sh 里那几张表是否还是连续的一块"
fi
chk "linux"   "$(disp linux -)"            "linux-x86_64|linux-x86_64|linux|x64|x86_64|qemu/linux-x86_64/qemu-system-x86_64-headless"
chk "windows" "$(disp windows -)"          "windows-x86_64|windows-x86_64|windows|x64|x86_64|qemu/windows-x86_64/qemu-system-x86_64.exe"
chk "darwin/aarch64" "$(disp darwin aarch64)" "darwin-aarch64|darwin-aarch64|macosx|aarch64|arm64-v8a|qemu/darwin-aarch64/qemu-system-aarch64-headless"
chk "darwin/x64"     "$(disp darwin x64)"     "darwin-x64|darwin-x64|macosx|x64|x86_64|qemu/darwin-x64/qemu-system-x86_64-headless"

# =============================================================================
echo
echo "── [2] 平台与 ROM 配套（四种组合）"
# =============================================================================
mk_fake_rom() {   # mk_fake_rom <目录> <abilist64>
    local d="$1" abilist="$2"
    rm -rf "$d"; mkdir -p "$d/system" "$d/vendor"
    for f in system-qemu.img vendor-qemu.img product-qemu.img ramdisk-qemu.img \
             kernel-ranchu encryptionkey.img userdata.img advancedFeatures.ini; do
        head -c 4096 /dev/urandom > "$d/$f"
    done
    cp "$X64_DIR/emulator/config.ini" "$d/config.ini"
    { echo "ro.product.cpu.abi=$(printf '%s' "$abilist" | cut -d, -f1)"
      echo "ro.system.product.cpu.abilist64=$abilist"
      echo "ro.product.device=fake_device"; } > "$d/system/build.prop"
    ( cd "$d" && find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS )
    echo "ro.system.build.fingerprint=fake/$abilist:12/TEST/1:userdebug/test-keys" > "$d/MANIFEST.txt"
}
ROM_ARM="$SANDBOX/rom-arm64";   mk_fake_rom "$ROM_ARM" "arm64-v8a"
ROM_X86="$SANDBOX/rom-x86_64";  mk_fake_rom "$ROM_X86" "x86_64,arm64-v8a"

pair_ok() {   # pair_ok <描述> <期望 0/1> <平台> <arch> <ROM>
    local desc="$1" want="$2" plat="$3" arch="$4" rom="$5"
    RELEASE_STAGE_DIR="$SANDBOX/stage" RELEASE_CACHE_DIR="$SANDBOX/cache" \
      bash "$X64_DIR/scripts/release.sh" --platform "$plat" ${arch:+--darwin-arch "$arch"} \
        --images "$rom" --list --out "$SANDBOX/out" > "$SANDBOX/pair.log" 2>&1
    local got=0; grep -q "不配套" "$SANDBOX/pair.log" && got=1
    if [ "$got" = "$want" ]; then
        okc "$desc（$( [ "$want" = 0 ] && echo 接受 || echo 拒绝 )）"
    else
        badc "$desc：期望 $( [ "$want" = 0 ] && echo 接受 || echo 拒绝 )，实际相反"
        sed 's/^/      /' "$SANDBOX/pair.log" | head -5 >&2
    fi
}
pair_ok "darwin/aarch64 + arm64 ROM"  0 darwin aarch64 "$ROM_ARM"
pair_ok "darwin/aarch64 + x86_64 ROM" 1 darwin aarch64 "$ROM_X86"
pair_ok "darwin/x64 + x86_64 ROM"     0 darwin x64     "$ROM_X86"
pair_ok "linux + arm64 ROM"           1 linux  ""      "$ROM_ARM"
pair_ok "both + x86_64 ROM"           0 both   ""      "$ROM_X86"
pair_ok "all + x86_64 ROM"            1 all    ""      "$ROM_X86"

# =============================================================================
echo
echo "── [3] 真跑 release.sh 打 darwin 包（假 zip，离线）"
# =============================================================================
make_fake_emu_zip() {   # <输出> <宿主架构> <启动器>
    local out="$1" arch="$2" launcher="$3" d="$SANDBOX/mkemu-a"
    rm -rf "$d"; mkdir -p "$d/emulator/qemu/darwin-$arch" "$d/emulator/qemu/darwin-otherarch"
    printf '#!/bin/sh\necho fake emulator\n' > "$d/emulator/$launcher"; chmod +x "$d/emulator/$launcher"
    local be="qemu-system-aarch64-headless"
    [ "$arch" = x64 ] && be="qemu-system-x86_64-headless"
    printf 'fake\n' > "$d/emulator/qemu/darwin-$arch/$be"; chmod +x "$d/emulator/qemu/darwin-$arch/$be"
    printf 'fake\n' > "$d/emulator/qemu/darwin-otherarch/qemu-system-other-headless"
    printf 'Pkg.Revision=0.0.0-test\nPkg.BuildId=999999\n' > "$d/emulator/source.properties"
    ( cd "$d" && zip -qr "$out" . )
}
make_fake_pt_zip() {   # <输出>
    local out="$1" d="$SANDBOX/mkpt-a"
    rm -rf "$d"; mkdir -p "$d/platform-tools"
    printf '#!/bin/sh\necho fake adb\n' > "$d/platform-tools/adb"; chmod +x "$d/platform-tools/adb"
    ( cd "$d" && zip -qr "$out" . )
}
make_fake_emu_zip "$SANDBOX/emu-darwin-arm.zip" aarch64 emulator
make_fake_pt_zip  "$SANDBOX/pt-darwin.zip"

RELEASE_STAGE_DIR="$SANDBOX/stage" RELEASE_CACHE_DIR="$SANDBOX/cache" \
  bash "$X64_DIR/scripts/release.sh" --platform darwin --version mactest \
    --images "$ROM_ARM" --out "$SANDBOX/release" --no-download \
    --darwin-emulator-zip "$SANDBOX/emu-darwin-arm.zip" \
    --darwin-platform-tools-zip "$SANDBOX/pt-darwin.zip" \
    > "$SANDBOX/darwin.log" 2>&1
if [ $? -eq 0 ]; then okc "release.sh --platform darwin 跑通"; else
    badc "release.sh --platform darwin 失败"; tail -12 "$SANDBOX/darwin.log" | sed 's/^/      /' >&2
fi

DZIP="$(ls "$SANDBOX/release"/*darwin-aarch64*.zip 2>/dev/null | head -1)"
if [ -n "$DZIP" ]; then okc "产出了 darwin zip：$(basename "$DZIP")"; else badc "没产出 darwin zip"; fi

if [ -n "$DZIP" ]; then
    mkdir -p "$SANDBOX/dx"; ( cd "$SANDBOX/dx" && unzip -q "$DZIP" )
    DROOT="$(ls -d "$SANDBOX/dx"/autosnap-* | head -1)"

    # bin/ 必须是 mac 那五个（不是 linux 的、也不是空的）
    NB="$(ls "$DROOT/bin" 2>/dev/null | wc -l)"
    chk "包内 bin/ 文件数" "$NB" "5"
    for s in lib.sh start-headless.sh stop.sh status.sh verify.sh; do
        if [ -f "$DROOT/bin/$s" ]; then okc "包内有 bin/$s"; else badc "包内缺 bin/$s"; fi
    done
    [ -x "$DROOT/bin/start-headless.sh" ] && okc "bin/ 有可执行位" || badc "bin/ 缺可执行位"

    # 后端路径必须是 darwin-aarch64（不是 windows/linux）
    has "RUNTIME.txt 后端是 darwin-aarch64" "$(cat "$DROOT/runtime/RUNTIME.txt" 2>/dev/null)" \
        "emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless"
    hasnt "runtime/ 里没有别的平台后端" "$(ls "$DROOT/runtime/emulator/qemu" 2>/dev/null)" "windows-x86_64"

    # RELEASE.json：backend 曾经被硬编码成 windows（这轮修的）
    RJ="$(python3 -c '
import json,sys
d=json.load(open(sys.argv[1]))
r=d.get("runtime",{})
print(d.get("platform",""), r.get("backend",""))
' "$DROOT/RELEASE.json" 2>/dev/null)"
    has "RELEASE.json 平台" "$RJ" "darwin-aarch64"
    has "RELEASE.json 后端是 darwin-aarch64" "$RJ" "emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless"

    # START-HERE：占位符全替换 + 命令是 bash + 第 6 节按产品类型渲染
    SH="$(cat "$DROOT/START-HERE.md")"
    hasnt "START-HERE 无未替换占位符" "$SH" "@"
    has "START-HERE 快速开始是 bash" "$SH" "./bin/start-headless.sh"
    hasnt "START-HERE 不出现 powershell" "$SH" "start-headless.ps1"
    has "START-HERE 第 6 节：无翻译层" "$SH" "**不适用**"
    has "START-HERE 第 6 节：ABI 只有 arm64-v8a" "$SH" "本 ROM 是 \`arm64-v8a\`"
    has "START-HERE 平台提示：Apple Silicon" "$SH" "Apple Silicon"
    has "START-HERE 验收描述已平台无关" "$SH" "宿主 / 产品类型 / 服务产物架构"
fi

# =============================================================================
echo
echo "── [4] 回归：同一个假 ROM 打 linux 包，第 6 节必须是桥产品的行"
# =============================================================================
make_fake_emu_zip_lin() {
    local out="$1" d="$SANDBOX/mkemu-l"
    rm -rf "$d"; mkdir -p "$d/emulator/qemu/linux-x86_64" "$d/emulator/qemu/windows-x86_64"
    printf '#!/bin/sh\necho fake\n' > "$d/emulator/emulator"; chmod +x "$d/emulator/emulator"
    printf 'fake\n' > "$d/emulator/qemu/linux-x86_64/qemu-system-x86_64-headless"
    printf 'fake\n' > "$d/emulator/qemu/windows-x86_64/qemu-system-x86_64.exe"
    printf 'Pkg.Revision=0.0.0-test\nPkg.BuildId=999999\n' > "$d/emulator/source.properties"
    ( cd "$d" && zip -qr "$out" . )
}
make_fake_emu_zip_lin "$SANDBOX/emu-lin.zip"
cp "$SANDBOX/pt-darwin.zip" "$SANDBOX/pt-lin.zip"   # adb 名一样，结构够用

RELEASE_STAGE_DIR="$SANDBOX/stage2" RELEASE_CACHE_DIR="$SANDBOX/cache" \
  bash "$X64_DIR/scripts/release.sh" --platform linux --version mactest \
    --images "$ROM_X86" --out "$SANDBOX/release2" --no-download \
    --linux-emulator-zip "$SANDBOX/emu-lin.zip" \
    --linux-platform-tools-zip "$SANDBOX/pt-lin.zip" \
    > "$SANDBOX/linux.log" 2>&1 && okc "release.sh --platform linux（回归）跑通" \
    || { badc "linux 回归失败"; tail -8 "$SANDBOX/linux.log" | sed 's/^/      /' >&2; }

LZIP="$(ls "$SANDBOX/release2"/*linux*.zip 2>/dev/null | head -1)"
if [ -n "$LZIP" ]; then
    mkdir -p "$SANDBOX/lx"; ( cd "$SANDBOX/lx" && unzip -q "$LZIP" )
    LROOT="$(ls -d "$SANDBOX/lx"/autosnap-* | head -1)"
    LSH="$(cat "$LROOT/START-HERE.md")"
    has   "linux 包第 6 节：翻译层许可那段在" "$LSH" "libndk_translation"
    has   "linux 包第 6 节：ABI 是 x86_64,arm64-v8a" "$LSH" "本 ROM 是 \`x86_64,arm64-v8a\`"
    hasnt "linux 包第 6 节：没被误渲染成原生" "$LSH" "**不适用** —— 本 ROM 是原生 arm64"
    hasnt "linux 包没有 Apple Silicon 提示" "$LSH" "Apple Silicon"
fi

# =============================================================================
echo
echo "── [5] mac 版 lib.sh 的平台函数（命令 stub 伪装 macOS）"
# =============================================================================
STUB="$SANDBOX/stubbin"; mkdir -p "$STUB"
# ⚠️ uname 是最关键的一个 stub：lib.sh 开头就有平台守卫（uname -s 不是 Darwin 就拒绝加载）。
#    第一次写这个测试台时漏了 uname，结果 [5] 全空 —— 而"全空"看着像函数坏了，
#    其实是守卫在正常工作。所以下面既测"有 stub 时函数对"，也测"没 stub 时守卫拦住"。
cat > "$STUB/uname" <<'EOF'
#!/bin/sh
case "${1:-}" in
    -s) echo "${STUB_UNAME_S:-Darwin}" ;;
    -m) echo "${STUB_UNAME_M:-arm64}" ;;
    *)  echo "${STUB_UNAME_S:-Darwin}" ;;
esac
EOF
chmod +x "$STUB/uname"
printf '#!/bin/sh\necho "${STUB_HV:-1}"\n'                      > "$STUB/sysctl";          chmod +x "$STUB/sysctl"
printf '#!/bin/sh\necho 15.3.1\n'                              > "$STUB/sw_vers";         chmod +x "$STUB/sw_vers"
printf '#!/bin/sh\necho "${STUB_METAL:-Metal Support: Metal 3}"\n' > "$STUB/system_profiler"; chmod +x "$STUB/system_profiler"
printf '#!/bin/sh\ncat <<EOF\n  PID COMMAND         ARGS\n${STUB_PS:-}\nEOF\n' > "$STUB/ps"; chmod +x "$STUB/ps"

# 造一个最小的"发行包"布局给 lib.sh 用
PKG="$SANDBOX/pkg"; mkdir -p "$PKG/runtime/emulator/qemu/darwin-aarch64" "$PKG/runtime/platform-tools" \
                              "$PKG/images" "$PKG/templates"
printf '#!/bin/sh\n' > "$PKG/runtime/emulator/emulator"; chmod +x "$PKG/runtime/emulator/emulator"
printf '#!/bin/sh\n' > "$PKG/runtime/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless"
chmod +x "$PKG/runtime/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless"
printf '#!/bin/sh\n' > "$PKG/runtime/platform-tools/adb"; chmod +x "$PKG/runtime/platform-tools/adb"
cp "$X64_DIR/emulator/config.ini" "$PKG/templates/config.ini"
for f in system-qemu.img kernel-ranchu initrd; do head -c 2048 /dev/urandom > "$PKG/images/$f"; done

mlib() {   # mlib <shell 片段>：在 stub PATH 下 source lib.sh 并执行片段
    PATH="$STUB:$PATH" STUB_HV="${STUB_HV:-1}" STUB_METAL="${STUB_METAL:-Metal Support: Metal 3}" \
    STUB_PS="${STUB_PS:-}" STUB_UNAME_S="${STUB_UNAME_S:-Darwin}" STUB_UNAME_M="${STUB_UNAME_M:-arm64}" \
    AUTOSNAP_RUNTIME="$PKG/runtime" AUTOSNAP_IMAGES="$PKG/images" \
    AUTOSNAP_TEMPLATES="$PKG/templates" AUTOSNAP_RUN_DIR="$SANDBOX/run" \
    bash -c '
        set -uo pipefail
        source "'"$X64_DIR"'/packaging/bin/darwin/lib.sh" >/dev/null 2>&1
        '"$1"
}
# 守卫本身也要验：在 Linux 上（不给 uname stub）必须**明确拒绝**加载并指出该用哪一套，
# 而不是稀里糊涂跑起来。这条是"错了要响"的纪律，值得占一个断言。
GUARD="$(AUTOSNAP_RUNTIME="$PKG/runtime" AUTOSNAP_TEMPLATES="$PKG/templates" \
         AUTOSNAP_RUN_DIR="$SANDBOX/run" \
         bash -c 'source "'"$X64_DIR"'/packaging/bin/darwin/lib.sh" 2>&1; echo "rc=$?"' </dev/null | tr -d '\n')"
has "非 macOS 上 lib.sh 拒绝加载" "$GUARD" "这套脚本只用于 macOS"
chk "hv_usable（kern.hv_support=1）"      "$(STUB_HV=1 mlib 'hv_usable && echo yes || echo no')" "yes"
chk "hv_usable（kern.hv_support=0）"      "$(STUB_HV=0 mlib 'hv_usable && echo yes || echo no')" "no"
# ⚠️ 必须用 auto：resolve_gpu_mode 对非 auto 的入参是**直接透传**（不探测）。
#    拿 "host" 去测"没 Metal 时会退软渲染"必然测不出东西（第一次跑就是这样）。
chk "GPU 自适应（有 Metal → host）"          "$(mlib 'resolve_gpu_mode auto')" "host"
chk "GPU 自适应（无 Metal → 软渲染）"        "$(STUB_METAL='Display Type: Fake Display' mlib 'resolve_gpu_mode auto')" "swiftshader_indirect"
chk "GPU 显式指定时透传（不做探测）"          "$(STUB_METAL='Display Type: Fake Display' mlib 'resolve_gpu_mode host')" "host"
chk "后端路径按本机架构" "$(mlib 'backend_path')" "$PKG/runtime/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless"

# 进程枚举：伪装 ps 输出，含"调用者自己的命令行"（必须被排除）
PSLINE_A="  4242 qemu-system-aarch64-headless  /x/emulator -sysdir /s -port 5580 -no-window"
PSLINE_B="  5151 bash -c pgrep -f \"qemu-system.* -port 5580\""
chk "emu_pid_for_port 命中真 qemu" \
    "$(STUB_PS="$PSLINE_A
$PSLINE_B" mlib 'emu_pid_for_port 5580')" "4242"
chk "emu_pid_for_port 不误算 5582" \
    "$(STUB_PS="$PSLINE_A" mlib 'emu_pid_for_port 5582')" ""

# 工作目录：initrd/config.ini 必须是实文件，大件才是软链
build_out="$(mlib 'build_sysdir 5580 fresh >/dev/null 2>&1; sysdir_for_port 5580')"
if [ -n "$build_out" ]; then
    [ -L "$build_out/initrd" ] && badc "initrd 被软链了（模拟器会写穿 images/）" || okc "initrd 是实文件"
    [ -L "$build_out/config.ini" ] && badc "config.ini 被软链了" || okc "config.ini 是实文件"
    [ -L "$build_out/system-qemu.img" ] && okc "大件是软链（不白占空间）" || badc "大件没链成软链"
else
    badc "build_sysdir 没跑出结果"
fi

# verify.sh 的架构判据必须是"读文件头"，不是"信设备自述"
VSH="$(cat "$X64_DIR/packaging/bin/darwin/verify.sh")"
has "verify.sh 用 od 读 ELF 头" "$VSH" "od -An -tx1"
has "verify.sh 认 aarch64 的 e_machine" "$VSH" "b700"
has "verify.sh 认 x86-64 的 e_machine"  "$VSH" "3e00"

# =============================================================================
echo
echo "── [6] macos/run-darwin.sh（源码树入口）的架构自检"
# =============================================================================
# 两种 Mac 各自只有**一个**后端，配错镜像会"装得上、起不来"，而且报错指不到真因。
# 这个薄封装的价值就在**启动前**把这件事判掉 —— 所以它值得一个断言。
RUNW="$X64_DIR/macos/run-darwin.sh"
if [ -f "$RUNW" ]; then
    okc "macos/run-darwin.sh 存在"
    # 三种输入下的三种输出。注意都在**没有真 sdk / 真 artifacts** 的前提下，
    # 所以前两条走的是"缺依赖就明确报错"而不是静默失败。
    o1="$(PATH="$STUB:$PATH" STUB_UNAME_M=arm64 bash "$RUNW" --no-wait 2>&1 | head -2 | tr -d '\n')"
    if printf '%s' "$o1" | grep -q "找不到"; then
        okc "缺 sdk/镜像时明确报错，并指出先跑哪个脚本"
    else
        badc "run-darwin.sh 缺依赖时的输出不像预期：$o1"
    fi
    has "run-darwin.sh 会比对本机架构与镜像 abilist64" "$(cat "$RUNW")" "abilist64"
    has "run-darwin.sh 对 Apple Silicon 有专门提示"     "$(cat "$RUNW")" "Apple Silicon"
    has "run-darwin.sh 是薄封装（交给包内脚本）"        "$(cat "$RUNW")" 'exec "$PKG_BIN/start-headless.sh"'
    # ⚠️ 必须先剔注释再查：run-darwin.sh 的注释里就写着「不要自己拿产物目录当 -sysdir 启动」。
    #    "检查代码里出现的字符串"一律先 strip_comments —— 这个假阳性模式今天出现三次了
    #    （bash 4 语法、旧目录名、-sysdir），每次都是注释把检查骗了。
    hasnt "run-darwin.sh 没有自己拼 -sysdir 参数"       "$(strip_comments "$RUNW")" "-sysdir"
    # 模板路径这条踩过：packaging/templates/ 里只有 README，config.ini 的真源是 emulator/
    has "run-darwin.sh 的模板指向 emulator/（config.ini 的真源）" "$(cat "$RUNW")" 'AUTOSNAP_TEMPLATES="$X64_DIR/emulator"'
else
    badc "macos/run-darwin.sh 不存在"
fi

# =============================================================================
echo
if [ "$FAIL" = 0 ]; then
    printf '\033[1;32m==>\033[0m 全部通过：%d 项（沙箱：%s）\n' "$PASS" "${SANDBOX#"$REPO_ROOT"/}"
    exit 0
else
    printf '\033[1;31m==>\033[0m 失败 %d 项 / 通过 %d 项（沙箱：%s）\n' "$FAIL" "$PASS" "${SANDBOX#"$REPO_ROOT"/}"
    exit 1
fi
