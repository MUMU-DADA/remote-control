#!/usr/bin/env bash
# =============================================================================
# windows/emulator.ps1 的**实跑**验证（在 Linux 上用 pwsh 跑）
#
# 为什么要在 Linux 上跑：这台机器上没有 Windows，而"写完没跑过的脚本"
# 正是这个项目反复吃亏的地方。能在这里验的是**文件与生命周期逻辑** ——
# 建实例、链镜像、复制、重置、删除，这些出错会直接毁数据。
# 不能验的是进程管理（Get-CimInstance / Start-Process 是 Windows 专有），
# 那部分靠 API 语义保证，脚本里逐处写了注释。
#
# 沙箱：整个 windows/ 目录复制到 .tmp 下跑，绝不碰真的 images/ 和实例。
#
# 用法： bash tools/test-windows-emulator.sh
# =============================================================================
set -uo pipefail

X64_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$X64_DIR/../.."
PWSH="$REPO_ROOT/.tmp/pwsh/pwsh"
SANDBOX="$REPO_ROOT/.tmp/pstest"

pass=0; fail=0
chk() {   # chk <描述> <条件命令...>
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then printf '  \033[1;32m✓\033[0m %s\n' "$desc"; pass=$((pass+1))
    else printf '  \033[1;31m✗\033[0m %s\n' "$desc"; fail=$((fail+1)); fi
}
chk_out() {   # chk_out <描述> <期望子串> <命令...>
    local desc="$1" want="$2"; shift 2
    local out; out="$("$@" 2>&1)"
    if printf '%s' "$out" | grep -q -- "$want"; then printf '  \033[1;32m✓\033[0m %s\n' "$desc"; pass=$((pass+1))
    else printf '  \033[1;31m✗\033[0m %s\n    期望包含: %s\n    实际: %s\n' "$desc" "$want" "$(printf '%s' "$out" | head -3 | tr '\n' '|')"; fail=$((fail+1)); fi
}

[ -x "$PWSH" ] || { echo "[x] 没有 pwsh：$PWSH（下载见对话记录）"; exit 1; }

ps() { DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 "$PWSH" -NoProfile -File "$SANDBOX/win/emulator.ps1" "$@"; }

echo "── [0] 所有 .ps1 先过一遍语法分析"
# ⚠️ 这一节是补出来的：run-windows.ps1 里有一行
#     Write-Host "... $(((& $adb ... ) -join "").Trim())"
#   双引号字符串里的 $( ) 里再套 ""，PowerShell 词法分析当场崩
#    （The string is missing the terminator）—— 也就是说那个脚本
#    **从来没能运行过**，而没人发现，因为没人能在这儿跑 Windows。
#    语法分析不需要 Windows，所以这一节能一直守着。
for f in "$X64_DIR"/windows/*.ps1; do
    chk "$(basename "$f") 语法 OK" \
        env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 "$PWSH" -NoProfile -Command \
        "\$e=\$null; [void][System.Management.Automation.Language.Parser]::ParseFile('$f',[ref]\$null,[ref]\$e); if (\$e) { exit 1 }"
done

echo
echo "==> 搭沙箱 $SANDBOX"
rm -rf "$SANDBOX"; mkdir -p "$SANDBOX/win" "$SANDBOX/emulator"

cp "$X64_DIR/emulator/config.ini" "$SANDBOX/emulator/config.ini"
cp "$X64_DIR/windows/emulator.ps1" "$SANDBOX/win/emulator.ps1"

config_value() {
    awk -F= -v key="$1" '$1 == key { gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit }' \
        "$X64_DIR/emulator/config.ini"
}
CONFIG_MEMORY_MB="$(config_value hw.ramSize)"
CONFIG_CORES="$(config_value hw.cpu.ncore)"
[ -n "$CONFIG_MEMORY_MB" ] && [ -n "$CONFIG_CORES" ] || {
    echo "[x] config.ini 缺少 hw.ramSize 或 hw.cpu.ncore"
    exit 1
}

# 假的 images\：几个"镜像"文件 + 一个目录（目录在真环境里是 system\ / vendor\）
mkdir -p "$SANDBOX/win/images/system"
for f in system-qemu.img vendor-qemu.img userdata.img kernel-ranchu initrd config.ini; do
    head -c 200000 /dev/zero > "$SANDBOX/win/images/$f"
done
echo "build.prop" > "$SANDBOX/win/images/system/build.prop"

# 假 adb（就是个 shell 脚本，pwsh 在 Linux 上照样能 execve 它）
mkdir -p "$SANDBOX/win/sdk/platform-tools"
cat > "$SANDBOX/win/sdk/platform-tools/adb.exe" <<'EOF'
#!/bin/sh
case "$*" in
  *get-state*)        echo device ;;
  *sys.boot_completed*) echo 1 ;;
  *"emu kill"*)       exit 0 ;;
  *)                  exit 0 ;;
esac
EOF
chmod +x "$SANDBOX/win/sdk/platform-tools/adb.exe"

IMG_HASH_BEFORE="$(cd "$SANDBOX/win/images" && sha256sum * 2>/dev/null | sha256sum)"

# 从登记文件读回端口（不写死 —— 端口是自动分配的）
port_of() { sed -n 's/^PORT=//p' "$SANDBOX/win/.run/instances/$1.env" | tr -d '\r'; }

echo
echo "── [1] create"
# 先建 default（占 5580，和真机器上的布局一致），dev2 才会拿到 5582
CREATE_OUT="$(ps create default 2>&1)"
printf '%s' "$CREATE_OUT" | grep -q "1280x720 @320dpi  横屏" \
    && { printf '  \033[1;32m✓\033[0m %s\n' "create 打印 1280x720 @320dpi 横屏"; pass=$((pass+1)); } \
    || { printf '  \033[1;31m✗\033[0m %s\n' "create 打印 1280x720 @320dpi 横屏"; fail=$((fail+1)); }
printf '%s' "$CREATE_OUT" | grep -q "${CONFIG_MEMORY_MB} MB / ${CONFIG_CORES} 核" \
    && { printf '  \033[1;32m✓\033[0m %s\n' "create 打印内存/核数（都来自 config.ini）"; pass=$((pass+1)); } \
    || { printf '  \033[1;31m✗\033[0m %s\n' "create 打印内存/核数（都来自 config.ini）"; fail=$((fail+1)); }
printf '%s' "$CREATE_OUT" | grep -q "32G" \
    && { printf '  \033[1;32m✓\033[0m %s\n' "create 打印数据分区 32G"; pass=$((pass+1)); } \
    || { printf '  \033[1;31m✗\033[0m %s\n' "create 打印数据分区 32G"; fail=$((fail+1)); }
chk "default 的端口是 5580" test "$(port_of default)" = "5580"
chk_out "create dev2" "已创建" ps create dev2
chk_out "status dev2 读到真源的 ${CONFIG_MEMORY_MB}" "$CONFIG_MEMORY_MB" ps status dev2
chk "实例登记文件在" test -s "$SANDBOX/win/.run/instances/dev2.env"
chk "dev2 分到 5582（5580 被 default 占了）" test "$(port_of dev2)" = "5582"
chk "登记文件里 PORT= 一行是对的" grep -q "^PORT=5582" "$SANDBOX/win/.run/instances/dev2.env"
chk "中文注释没被写成 ???" bash -c "! grep -q '???' '$SANDBOX/win/.run/instances/dev2.env'"
chk "工作目录建好了" test -d "$SANDBOX/win/.run/sysdir-5582"
chk "datadir 建好了" test -d "$SANDBOX/win/.run/datadir-5582"

S="$SANDBOX/win/.run/sysdir-5582"
chk "镜像 system-qemu.img 是链接（不是复制）" test -L "$S/system-qemu.img"
chk "目录 system 是链接" test -L "$S/system"
chk "initrd 是**实文件**（模拟器要重写它，不能链）" bash -c "[ -f '$S/initrd' ] && [ ! -L '$S/initrd' ]"
chk "config.ini 是**实文件**（要按真源覆盖，不能链）" bash -c "[ -f '$S/config.ini' ] && [ ! -L '$S/config.ini' ]"
chk "sysdir 里的 config.ini 和真源一致" cmp -s "$S/config.ini" "$X64_DIR/emulator/config.ini"
chk_out "重复 create 会拒绝" "已经存在" ps create dev2

echo
echo "── [2] list / status"
chk_out "list 里有 dev2 和端口" "dev2" ps list
chk_out "list 显示已停止" "已停止" ps list
chk_out "status 读到真源的 ${CONFIG_MEMORY_MB}" "$CONFIG_MEMORY_MB" ps status dev2
chk_out "status 读到真源的 32G" "32G" ps status dev2
chk_out "status 读到真源的 ${CONFIG_CORES} 核" "hw.cpu.ncore             ${CONFIG_CORES}" ps status dev2

echo
echo "── [3] clone（连状态一起复制）"
# 造点"状态"：模拟器会在工作目录里拉这些
head -c 5000 /dev/zero > "$S/userdata-qemu.img"
head -c 3000 /dev/zero > "$S/userdata-qemu.img.qcow2"
echo "abs-path=/old/sysdir-5582" > "$S/hardware-qemu.ini"
echo "stale" > "$S/emu-launch-params.txt"
mkdir -p "$S/snapshots/snap1"; echo x > "$S/snapshots/snap1/ram.bin"
mkdir -p "$S/build.avd"; echo y > "$S/build.avd/x"

chk_out "clone dev2 → dev3" "已复制" ps clone dev2 dev3
D="$SANDBOX/win/.run/sysdir-5584"
chk "dev3 登记了" test -s "$SANDBOX/win/.run/instances/dev3.env"
chk "状态文件 userdata-qemu.img.qcow2 复制过来了" test -s "$D/userdata-qemu.img.qcow2"
chk "build.avd 目录复制过来了" test -e "$D/build.avd/x"
chk "**hardware-qemu.ini 没有复制**（里面是绝对路径，会两台共用一份 /data）" test ! -e "$D/hardware-qemu.ini"
chk "emu-launch-params.txt 清掉了" test ! -e "$D/emu-launch-params.txt"
chk "snapshots 没复制（快照绑定了原路径）" test ! -e "$D/snapshots"
chk "dev3 的镜像依然是链接" test -L "$D/system-qemu.img"
chk "dev3 和 dev2 的 qcow2 是**两个文件**" bash -c "[ \"\$(stat -c %i '$S/userdata-qemu.img.qcow2')\" != \"\$(stat -c %i '$D/userdata-qemu.img.qcow2')\" ]"

echo
echo "── [4] reset"
chk_out "reset dev2" "已重置" ps reset dev2 -Yes
chk "userdata-qemu.img 清掉了" test ! -e "$S/userdata-qemu.img"
chk "qcow2 清掉了" test ! -e "$S/userdata-qemu.img.qcow2"
chk "hardware-qemu.ini 清掉了（改过 config.ini 后 reset 就生效）" test ! -e "$S/hardware-qemu.ini"
chk "build.avd 清掉了" test ! -e "$S/build.avd"
chk "snapshots 清掉了" test ! -e "$S/snapshots"
chk "实例还在（reset 不删实例）" test -s "$SANDBOX/win/.run/instances/dev2.env"
chk "镜像链接还在" test -L "$S/system-qemu.img"

echo
echo "── [5] delete"
chk_out "delete dev3" "已删除" ps delete dev3 -Yes
chk "dev3 登记没了" test ! -e "$SANDBOX/win/.run/instances/dev3.env"
chk "dev3 工作目录没了" test ! -d "$D"
chk_out "delete dev2" "已删除" ps delete dev2 -Yes
chk "dev2 工作目录没了" test ! -d "$S"
chk_out "delete default" "已删除" ps delete default -Yes

echo
echo "── [6] 关键：共享的 images\\ 从头到尾没被动过"
IMG_HASH_AFTER="$(cd "$SANDBOX/win/images" && sha256sum * 2>/dev/null | sha256sum)"
chk "images\\ 内容校验和不变（delete/reset 没顺着链接删到目标）" \
    test "$IMG_HASH_BEFORE" = "$IMG_HASH_AFTER"
chk "images\\ 文件还在" test -s "$SANDBOX/win/images/system-qemu.img"

echo
echo "── [7] 边界"
chk_out "未知命令会被拒" "未知命令" ps frobnicate
chk_out "不存在的实例会报清楚" "没有叫" ps status nosuch

echo
printf '  ───────────────────────────────\n'
printf '  通过 %d 项，失败 %d 项\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
