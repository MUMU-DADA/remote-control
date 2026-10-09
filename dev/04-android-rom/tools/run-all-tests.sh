#!/usr/bin/env bash
# =============================================================================
# 一把跑完 dev/04-android-rom/tools/ 下的测试
#
# 为什么要有它：这些脚本原来只散在 README / docs 里被提到，**没有任何一处
# 把它们串起来跑**。"全套通过"于是成了人工动作 —— 而 2026-10-09 实测，
# 人工动作漏掉了一个真失败：test-release.sh 因为沙箱端口写死 5580、撞上
# 本机真在跑的开发实例，144 通过 / **3 失败**，一直没人发现。
#
# ⚠️ 设计上唯一重要的一条：**不许静默漏测**。
#    · 测试是**按文件发现**的，不是硬编码清单；
#    · 发现到的每个文件**必须**在下面的分类表里出现，否则本脚本直接报错退出；
#    · 跳过的一定会打印出来，并说明原因。
#    这样新加一个 test-*.sh 却忘了接进来，会立刻炸，而不是被无声忽略。
#
# 用法：
#   bash tools/run-all-tests.sh              # 只跑宿主侧的（不需要模拟器）
#   bash tools/run-all-tests.sh --with-device # 连需要模拟器的 verify-* 一起跑
#   bash tools/run-all-tests.sh --list        # 只列出分类，不跑
# =============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE/.." || exit 1

WITH_DEVICE=0
LIST_ONLY=0
for a in "$@"; do
    case "$a" in
        --with-device) WITH_DEVICE=1 ;;
        --list)        LIST_ONLY=1 ;;
        -h|--help)     sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "未知参数：$a"; exit 2 ;;
    esac
done

# ── 分类表 ──────────────────────────────────────────────────────────────
# host   = 纯宿主侧，任何机器都能跑
# device = 会自己 create/start 真实例（不是"需要你先把模拟器跑起来" ——
#          它们自带启动），代价是各自要开关机几次、几分钟到十几分钟，
#          而且要占几 GB 到几十 GB 磁盘。默认跳过只是因为它重，不是因为它缺前提。
declare -A KIND=(
    [test-macos-port.sh]=host
    [test-packaging-windows.py]=host
    [test-release.sh]=host
    [test-script-safety.sh]=host
    [test-storage.sh]=host
    [test-windows-emulator.sh]=host
    [test-lifecycle-unix.py]=host
    [verify-rom.sh]=host
    [verify-clone-independent.sh]=device
    [verify-data-persistence.sh]=device
    [verify-export-import.sh]=device
    [verify-kill-is-hard-poweroff.sh]=device
    [verify-partition-resize.sh]=device
)

# ── 发现 ────────────────────────────────────────────────────────────────
shopt -s nullglob
found=()
for f in tools/test-*.sh tools/test-*.py tools/verify-*.sh; do
    found+=("$(basename "$f")")
done
shopt -u nullglob

if [ "${#found[@]}" -eq 0 ]; then
    echo "[x] tools/ 下一个测试都没发现 —— 这不正常，检查路径"; exit 1
fi

# **关键**：发现到的每个文件都必须在表里。漏了就炸，不许无声跳过。
unclassified=()
for f in "${found[@]}"; do
    [ -n "${KIND[$f]:-}" ] || unclassified+=("$f")
done
if [ "${#unclassified[@]}" -gt 0 ]; then
    echo "[x] 这些测试脚本没有分类，拒绝静默跳过它们："
    for f in "${unclassified[@]}"; do echo "      $f"; done
    echo "    处理：在 $0 的 KIND 表里给它标 host 或 device。"
    exit 1
fi

host_list=(); device_list=()
for f in "${found[@]}"; do
    if [ "${KIND[$f]}" = host ]; then host_list+=("$f"); else device_list+=("$f"); fi
done

echo "宿主侧（一定会跑）：${#host_list[@]} 个"
printf '  %s\n' "${host_list[@]}"
if [ "$WITH_DEVICE" = 1 ]; then _devnote="--with-device 已开，会跑"; else _devnote="默认跳过，加 --with-device 才跑"; fi
echo "会自己起真实例、比较重（$_devnote）：${#device_list[@]} 个"
printf '  %s\n' "${device_list[@]}"
[ "$LIST_ONLY" = 1 ] && exit 0
echo

# ── 跑 ──────────────────────────────────────────────────────────────────
pass=0; fail=0; skip=0
declare -a failed_names=() skipped_names=()

run_one() {
    local f="$1" log; log="$(mktemp)"
    printf '════ %s\n' "$f"
    local t0; t0=$(date +%s)
    # 按扩展名选解释器 —— 第一版一律用 bash 跑，两个 .py 直接
    # "import: 未找到命令" 挂了（那是 runner 的 bug，不是测试的）。
    local runner=bash
    case "$f" in *.py) runner=python3 ;; esac
    if "$runner" "tools/$f" >"$log" 2>&1; then
        printf '  \033[1;32m✓\033[0m 通过（%s 秒）\n' "$(( $(date +%s) - t0 ))"
        tail -2 "$log" | sed 's/^/    /'
        pass=$((pass + 1))
    else
        local rc=$?
        printf '  \033[1;31m✗\033[0m 失败（退出码 %s，%s 秒）\n' "$rc" "$(( $(date +%s) - t0 ))"
        echo "    最后 12 行："
        tail -12 "$log" | sed 's/^/      /'
        fail=$((fail + 1)); failed_names+=("$f")
    fi
    rm -f "$log"
}

for f in "${host_list[@]}"; do run_one "$f"; done

for f in "${device_list[@]}"; do
    if [ "$WITH_DEVICE" = 1 ]; then
        run_one "$f"
    else
        printf '════ %s\n' "$f"
        printf '  \033[1;33m-\033[0m SKIP：会自己起真实例、比较重（加 --with-device 才跑）\n'
        skip=$((skip + 1)); skipped_names+=("$f")
    fi
done

echo
echo "───────────────────────────────"
printf '通过 %d，失败 %d，跳过 %d\n' "$pass" "$fail" "$skip"
if [ "$skip" -gt 0 ]; then
    echo "跳过的（**不代表通过**）："
    printf '  - %s\n' "${skipped_names[@]}"
fi
[ "$fail" -eq 0 ] || { echo "失败的："; printf '  - %s\n' "${failed_names[@]}"; exit 1; }
exit 0
