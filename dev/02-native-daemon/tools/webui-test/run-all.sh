#!/bin/bash
# =============================================================================
# 跑完整的浏览器端到端套件，逐个报**退出码**。
# （**故意不写脚本个数**：以前写死的数字全部过期了 —— 仓库的规矩是数字问 runner 要。）
#
# 为什么要有一个 runner：
#   * 这些脚本各自输出格式不一样 —— 有 `通过 N/M`、有 `Passed N/M`、
#     还有只逐条打 ✓ 靠**退出码**表态的。分散跑很容易只看自己想看的那一行，
#     然后把红的当成"输出有点怪"。
#   * 之前没有 runner，README 里写的脚本数实际上没地方一键验证。
#     实测教训：这一套曾经在**页面 401** 的情况下报 8/9 —— 假绿。
#     所以这里的判据只看退出码 + 逐条 ✗，不看任何一行"看起来还行"的输出。
#
# 用法：
#   xvfb-run -a ./run-all.sh --host 127.0.0.1 --port 18088
#
# 令牌：交付镜像默认开鉴权。优先用 --token，其次环境变量 RC_TOKEN，
#       都没有就从设备上的配置文件读（需要 adb root，用 RC_SERIAL 指定设备）。
# =============================================================================
set -u

HOST="127.0.0.1"
PORT="8088"
TOKEN="${RC_TOKEN:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --host)  HOST="$2"; shift 2 ;;
        --port)  PORT="$2"; shift 2 ;;
        --token) TOKEN="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

cd "$(dirname "$0")" || exit 2
export RC_TOKEN="$TOKEN"
BASE="http://$HOST:$PORT"

# 脚本对参数的叫法不一样（多数 --host/--port，流那个是 --base）
SCRIPTS="coord_space_test.py filebrowser_test.py fillmode_test.py
         maxwidth_test.py mouse_test.py rotate_test.py text_test.py
         upload_test.py webui_test.py"

pass=0; fail=0; failed=""
for f in $SCRIPTS; do
    printf '%-24s ' "$f"
    out="$(timeout 240 python3 -u "$f" --host "$HOST" --port "$PORT" 2>&1)"
    rc=$?
    # 逐条失败也单独报出来 —— 有的脚本判据不完整，退出码可能仍是 0。
    #
    # ⚠️ 判据是「行首任意缩进 + ✗」，**不是**写死的两个空格。
    #    2026-10-09 实测：`upload_test.py` 打印 ✗ 用的是 4 个和 5 个空格
    #    （它自己的失败判定是 4 个），写死 `^  ✗` 就**永远罩不住它** ——
    #    那条"退出码可能仍是 0"的补充判据对它形同不存在。
    #
    #    约定：**脚本自己的判定**行首（可带缩进）是 ✗；**回显页面文字**要加前缀
    #    （`upload_test.py` 用的是 `│ `），免得把页面里的 ✗ 当成判定。
    bad="$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g' | grep -cE '^ *✗' || true)"
    summary="$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g' \
               | grep -E '通过 [0-9]+/|Passed [0-9]+/' | tail -1)"
    # ── 汇总行是**强制**的，而且按 N==M 判 ───────────────────────────────
    #
    # 为什么不能只看退出码：有的脚本判据不完整，退出码可能仍是 0。
    # 为什么不能只看 ✗ 字形：脚本会**回显页面文字**，里面本来就可能带 ✗
    #   （大文件那档「✗ 安装失败」是预期的）。回显走 wctest_echo.echo_block
    #   给每行加前缀，正是为了不让它被当成判定。
    #
    # 所以判据是：**退出码 = 0 且汇总行是 N/M 且 N == M**。
    # 没有汇总行 = 直接失败（"没有可判的东西"不等于"通过"）。
    n_of_m=""
    if [ -n "$summary" ]; then
        nums="$(printf '%s' "$summary" | grep -oE '[0-9]+/[0-9]+' | tail -1)"
        if [ -n "$nums" ]; then
            n_of_m="${nums%%/*}"; m_of_m="${nums##*/}"
            [ "$n_of_m" = "$m_of_m" ] || n_of_m="MISMATCH"
        fi
    fi
    if [ -z "$summary" ]; then
        printf '✗ exit=%s  **没有汇总行**（判不了 = 不算通过）\n' "$rc"
        fail=$((fail + 1)); failed="$failed $f"
    elif [ "$n_of_m" = "MISMATCH" ]; then
        printf '✗ exit=%s  汇总行说 %s（N != M）\n' "$rc" "$summary"
        fail=$((fail + 1)); failed="$failed $f"
    elif [ "$rc" -eq 0 ] && [ "$bad" -eq 0 ]; then
        printf '✓ exit=0  %s\n' "${summary:-（无汇总行，靠退出码）}"
        pass=$((pass + 1))
    else
        printf '✗ exit=%s  失败 %s 条  %s\n' "$rc" "$bad" "${summary:-}"
        printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g' | grep -E '^ *✗' | head -8
        fail=$((fail + 1)); failed="$failed $f"
    fi
done

f="webui_stream_test.py"
printf '%-24s ' "$f"
# ⚠️ **带 --h264**。这一段默认是跳过的，而它恰恰逮到过一个真回归：
#    H.264 流从 maxWidth=360 改到 480 时服务端照旧发 360（只能缩不能放），
#    页面等到超时。不打开这一段，那次回归就会一路绿着过去。
out="$(timeout 300 python3 -u "$f" --base "$BASE" --h264 2>&1)"; rc=$?
summary="$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g' \
           | grep -E 'Passed [0-9]+/' | tail -1)"
# 同循环里那条规则：汇总行强制，且 N == M。
_snums="$(printf '%s' "$summary" | grep -oE '[0-9]+/[0-9]+' | tail -1)"
# ⚠️ 失败时**必须把脚本自己的输出打出来**。
#    2026-10-09 实测：这一段原来只打汇总行，而脚本输出被 `out="$(…)"` 吞掉了 ——
#    于是看到「Passed 24/25」却**不知道是哪一项挂了**，只能手工重跑一遍
#    （而重跑恰好可能是好的，白折腾）。报失败而说不出为什么，等于没报。
show_detail() {
    printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g' | grep -E '✗|FAIL|失败' | head -8
}
if [ -z "$summary" ] || [ -z "$_snums" ]; then
    printf '✗ exit=%s  **没有可判的汇总行**（判不了 = 不算通过）\n' "$rc"
    show_detail
    fail=$((fail + 1)); failed="$failed $f"
elif [ "${_snums%%/*}" != "${_snums##*/}" ]; then
    printf '✗ exit=%s  汇总行说 %s（N != M）\n' "$rc" "$summary"
    show_detail
    fail=$((fail + 1)); failed="$failed $f"
elif [ "$rc" -eq 0 ]; then
    printf '✓ exit=0  %s\n' "$summary"
    pass=$((pass + 1))
else
    printf '✗ exit=%s  %s\n' "$rc" "$summary"
    fail=$((fail + 1)); failed="$failed $f"
fi

# 文件管理的边界（越界必须被拒）。
#
# ⚠️ 它以前不在这个 runner 里，而且**没有令牌** —— 于是每一条"越界被拒"
#    都拿到 401 被记成 ✓：13 个 ✓ 里没有一条真验过边界。
#    补上令牌之后它才第一次真正跑起来，顺带暴露出 /sdcard（FUSE）上
#    重命名**一直失败**（renameat2 的 RENAME_NOREPLACE 在 FUSE 上返回 EINVAL）。
if [ -f fileops_boundary.sh ]; then
    printf '%-24s ' "fileops_boundary.sh"
    out="$(timeout 120 env REMOTE_CONTROL_BASE="$BASE" REMOTE_CONTROL_TOKEN="$TOKEN" \
           REMOTE_CONTROL_SERIAL="${RC_SERIAL:-}" bash fileops_boundary.sh 2>&1)"; rc=$?
    summary="$(printf '%s' "$out" | grep -E '通过 [0-9]+ 项' | tail -1)"
    # 它的汇总写的是「通过 N 项，失败 M 项」—— 判 **M == 0**，
    # 而不是"退出码是 0 就算过"。
    _failed_n="$(printf '%s' "$summary" | grep -oE '失败 [0-9]+ 项' | grep -oE '[0-9]+' | tail -1)"
    if [ -z "$summary" ] || [ -z "$_failed_n" ]; then
        printf '✗ exit=%s  **没有可判的汇总行**（判不了 = 不算通过）\n' "$rc"
        fail=$((fail + 1)); failed="$failed fileops_boundary.sh"
    elif [ "$_failed_n" != "0" ]; then
        printf '✗ exit=%s  %s\n' "$rc" "$summary"
        printf '%s' "$out" | grep -E '✗' | head -6
        fail=$((fail + 1)); failed="$failed fileops_boundary.sh"
    elif [ "$rc" -eq 0 ]; then
        printf '✓ exit=0  %s\n' "$summary"
        pass=$((pass + 1))
    else
        printf '✗ exit=%s  %s\n' "$rc" "$summary"
        printf '%s' "$out" | grep -E '✗' | head -6
        fail=$((fail + 1)); failed="$failed fileops_boundary.sh"
    fi
fi

echo
echo "  通过 $pass / $((pass + fail))"
[ -n "$failed" ] && echo "  失败：$failed"
[ "$fail" -eq 0 ]
