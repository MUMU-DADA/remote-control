#!/usr/bin/env bash
# 在**源码树里**起一台 macOS 实例（开发用）。
#
#   ./macos/run-darwin.sh                       # default 实例，端口 5580
#   ./macos/run-darwin.sh --reuse                # 保留上次的数据
#   ./macos/run-darwin.sh --port 5584 --name vm2
#   ./macos/run-darwin.sh --no-wait
#
# 为什么是薄封装而不是"再写一套"：
#   起/停/查/验的逻辑已经在 packaging/bin/darwin/ 里写好并**离线测过 91 项**
#   （tools/test-macos-port.sh）。源码树与发布包的区别只有**路径约定**：
#       发布包：bin/ 与 images/ runtime/ templates/ 同级
#       源码树：脚本在 packaging/bin/darwin/，镜像与运行时在别处
#   所以这里只做一件事：把 AUTOSNAP_* 指到源码树的位置，再交给包内脚本。
#   参数照抄一份的结果一定是"两边漂移"—— 这个项目已经在别处吃过这个亏。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
X64_DIR="$(cd "$HERE/.." && pwd)"
PKG_BIN="$X64_DIR/packaging/bin/darwin"

die() { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 运行时：macos/fetch-emulator.sh 的默认落点
# ---------------------------------------------------------------------------
RUNTIME="$HERE/sdk"
[ -x "$RUNTIME/emulator/emulator" ] || die "找不到模拟器：$RUNTIME/emulator/emulator
    先跑： ./macos/fetch-emulator.sh"

# ---------------------------------------------------------------------------
# 镜像：优先用 release 里铺好的包（最接近最终用户看到的形态），
# 其次用 .run/release-stage，最后才用产物目录。
#
# ⚠️ 产物目录那条是**兜底**：它可能带着模拟器上次写进去的运行期文件
#    （userdata-qemu.img 是表观 32 GB 的稀疏文件，而且是跑过的用户数据）。
#    包内脚本会用 .run/sysdir-<port>/ 软链过去，所以不会写穿它 ——
#    但**不要**自己拿产物目录当 -sysdir 启动（见 docs/13-macos-port.md §7.0.12）。
# ---------------------------------------------------------------------------
IMAGES=""
for c in "$X64_DIR"/../release/autosnap-*/images \
         "$X64_DIR"/.run/release-stage/autosnap-*/images \
         "$X64_DIR"/artifacts/rom-remote_control_arm64 \
         "$X64_DIR"/artifacts/rom-remote_control_x64_arm64; do
    if [ -d "$c" ]; then IMAGES="$c"; break; fi
done
[ -n "$IMAGES" ] || die "找不到镜像。先跑其一：
    ./macos/fetch-images.sh          # 从构建机拉
    ../scripts/package-rom.sh        # 或在本地打一份"

# ---------------------------------------------------------------------------
# 架构自检：两种 Mac 各自只有**一个**后端，配错镜像就是"装得上、起不来"，
# 而且那类报错完全指不到真因（实测过）。
# ---------------------------------------------------------------------------
ABI="$(sed -n 's/^ro\.system\.product\.cpu\.abilist64=//p' "$IMAGES/system/build.prop" 2>/dev/null | tail -1)"
MACH="$(uname -m)"
case "$MACH:$ABI" in
    arm64:*x86_64*)
        die "本机是 Apple Silicon，但镜像的 abilist64 是「$ABI」（含 x86_64）。
    Apple Silicon 的模拟器包**没有** x86_64 后端 —— 这份镜像起不来。
    换 arm64 那份： cd .. && PRODUCT=arm64 ./scripts/package-rom.sh" ;;
    x86_64:arm64-v8a)
        die "本机是 Intel Mac，但镜像是原生 arm64 —— Intel 的包里没有 aarch64 后端。" ;;
esac

printf '\033[1;34m==>\033[0m 源码树模式\n' >&2
printf '      镜像   %s（abilist64=%s）\n' "$IMAGES" "${ABI:-?}" >&2
printf '      运行时 %s\n' "$RUNTIME" >&2
printf '      脚本   %s\n' "$PKG_BIN" >&2

# 交给包内脚本（参数原样透传）
#
# ⚠️ AUTOSNAP_TEMPLATES 指的是**含 config.ini 的那个目录**：
#    源码树里硬件配置的唯一真源是 emulator/config.ini（common.sh 的 EMULATOR_CONFIG 就是它）。
#    packaging/templates/ 里**只有 README**（config.ini 是 release.sh 打包时才拷进去的），
#    指过去会 "模板缺失：…/packaging/templates/config.ini"（实测踩到）。
AUTOSNAP_RUNTIME="$RUNTIME" \
AUTOSNAP_IMAGES="$IMAGES" \
AUTOSNAP_TEMPLATES="$X64_DIR/emulator" \
AUTOSNAP_EMULATOR="$RUNTIME/emulator/emulator" \
AUTOSNAP_ADB="$RUNTIME/platform-tools/adb" \
exec "$PKG_BIN/start-headless.sh" "$@"
