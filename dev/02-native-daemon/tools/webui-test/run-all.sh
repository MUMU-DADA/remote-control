#!/bin/bash
# =============================================================================
# 跑完整的浏览器端到端套件（10 个脚本），逐个报**退出码**。
#
# 为什么要有一个 runner：
#   * 这十个脚本各自输出格式不一样 —— 有 `通过 N/M`、有 `Passed N/M`、
#     还有只逐条打 ✓ 靠**退出码**表态的。分散跑很容易只看自己想看的那一行，
#     然后把红的当成"输出有点怪"。
#   * 之前没有 runner，README 里写的"10 个脚本"实际上没地方一键验证。
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

# 十个脚本对参数的叫法不一样（多数 --host/--port，流那个是 --base）
SCRIPTS="coord_space_test.py filebrowser_test.py fillmode_test.py
         maxwidth_test.py mouse_test.py rotate_test.py text_test.py
         upload_test.py webui_test.py"

pass=0; fail=0; failed=""
for f in $SCRIPTS; do
    printf '%-24s ' "$f"
    out="$(timeout 240 python3 -u "$f" --host "$HOST" --port "$PORT" 2>&1)"
    rc=$?
    # 逐条失败也单独报出来 —— 有的脚本判据不完整，退出码可能仍是 0
    bad="$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g' | grep -c '^  ✗' || true)"
    summary="$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g' \
               | grep -E '通过 [0-9]+/|Passed [0-9]+/' | tail -1)"
    if [ "$rc" -eq 0 ] && [ "$bad" -eq 0 ]; then
        printf '✓ exit=0  %s\n' "${summary:-（无汇总行，靠退出码）}"
        pass=$((pass + 1))
    else
        printf '✗ exit=%s  失败 %s 条  %s\n' "$rc" "$bad" "${summary:-}"
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
if [ "$rc" -eq 0 ]; then
    printf '✓ exit=0  %s\n' "${summary:-（无汇总行）}"
    pass=$((pass + 1))
else
    printf '✗ exit=%s  %s\n' "$rc" "${summary:-}"
    fail=$((fail + 1)); failed="$failed $f"
fi

echo
echo "  通过 $pass / $((pass + fail))"
[ -n "$failed" ] && echo "  失败：$failed"
[ "$fail" -eq 0 ]
