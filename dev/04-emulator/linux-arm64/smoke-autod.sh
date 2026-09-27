#!/usr/bin/env bash
# 阶段 1 冒烟测试：把 autod / autodctl 推进模拟器，验证截图与触控链路。
#
#   ./smoke-autod.sh                        # 用产物目录里的 autod / autodctl（默认 arm64）
#   ./smoke-autod.sh --fast                 # 用 x86_64 通道的产物
#   ./smoke-autod.sh /path/autod /path/autodctl
#   ./smoke-autod.sh --no-frame-check       # 跳过“截图是不是全黑”的分析
#   ./smoke-autod.sh --stop                 # 停掉设备上的 autod
#
# 走的是 02-native-daemon/README.md 的阶段 1 路径：
#   推 /data/local/tmp → root 前台跑 → 不碰 /system、不碰 SELinux。
#
# 想走 dev/02-native-daemon/tools/deploy_cuttlefish.sh 那种推 /system/bin 的路径，
# 先用 ./run-emulator.sh --writable-system 启动模拟器（否则 adb remount 必然失败）。

# 先扫 ABI 开关，再 source common.sh
for _a in "$@"; do
    case "$_a" in
        --fast|--x86_64) EMU_ABI=x86_64 ;;
        --arm64)         EMU_ABI=arm64 ;;
    esac
done
export EMU_ABI

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

FRAME_CHECK=1
AUTOD_BIN=""
AUTODCTL_BIN=""

while [ $# -gt 0 ]; do
    case "$1" in
        --fast|--x86_64|--arm64) ;;                    # 已在 source 之前处理
        --no-frame-check) FRAME_CHECK=0 ;;
        --stop)
            ADB_BIN="$(resolve_adb)"
            "$ADB_BIN" -s "emulator-$EMULATOR_PORT" shell 'pkill -f "autod --socket"' 2>/dev/null || true
            log "已停掉设备上的 autod"
            exit 0 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        -*)
            die "未知参数：$1（-h 看用法）" ;;
        *)
            if [ -z "$AUTOD_BIN" ]; then AUTOD_BIN="$1"
            elif [ -z "$AUTODCTL_BIN" ]; then AUTODCTL_BIN="$1"
            else die "多余参数：$1"; fi ;;
    esac
    shift
done

SERIAL="emulator-$EMULATOR_PORT"
ADB_BIN="$(resolve_adb)"
TMP=/data/local/tmp
SOCK="$TMP/autod.sock"
SHOT="$TMP/autod-shot.png"
SHOT_RAW="$TMP/autod-shot.ppm"
OUT_DIR="$RUN_DIR/shots"
mkdir -p "$OUT_DIR"

AUTOD_BIN="${AUTOD_BIN:-$PRODUCT_OUT/system/bin/autod}"
AUTODCTL_BIN="${AUTODCTL_BIN:-$PRODUCT_OUT/system/bin/autodctl}"

# ---------------------------------------------------------------------------
log "检查设备与二进制"

"$ADB_BIN" -s "$SERIAL" get-state >/dev/null 2>&1 || \
    die "$SERIAL 不在线。先跑： ./run-emulator.sh"

uid="$("$ADB_BIN" -s "$SERIAL" shell id -u 2>/dev/null | tr -d '\r')"
if [ "$uid" != "0" ]; then
    "$ADB_BIN" -s "$SERIAL" root >/dev/null 2>&1 || true
    "$ADB_BIN" -s "$SERIAL" wait-for-device
    uid="$("$ADB_BIN" -s "$SERIAL" shell id -u 2>/dev/null | tr -d '\r')"
fi
[ "$uid" = "0" ] || die "拿不到 root（uid=$uid）。模拟器镜像得是 userdebug/eng"

for f in "$AUTOD_BIN" "$AUTODCTL_BIN"; do
    [ -f "$f" ] || die "找不到二进制：$f
    先编： ./build-images.sh --modules     （产物在 out/target/product/$PRODUCT_DEVICE/system/bin/）"
done
log "设备 uid=0 ✓   autod=$(basename "$AUTOD_BIN")  autodctl=$(basename "$AUTODCTL_BIN")"

# ---------------------------------------------------------------------------
log "推送二进制到 $TMP（阶段 1：不碰 /system）"
"$ADB_BIN" -s "$SERIAL" push "$AUTOD_BIN"    "$TMP/autod"    >/dev/null
"$ADB_BIN" -s "$SERIAL" push "$AUTODCTL_BIN" "$TMP/autodctl" >/dev/null
"$ADB_BIN" -s "$SERIAL" shell "chmod 755 $TMP/autod $TMP/autodctl"

# ---------------------------------------------------------------------------
# 先跑自检。
#
# `autod --selftest` 逐项检查运行环境（/dev/uinput 权限、截图后端、
# 触控后端、显示尺寸），每项失败都给排查方向。
#
# 放在起服务之前：环境不对的话，后面的截图/点击会一串失败，
# 真正的原因会被淹没在后面的报错里。
printf '\n--- autod --selftest ---\n'
if "$ADB_BIN" -s "$SERIAL" shell "$TMP/autod --selftest" 2>&1; then
    log "自检通过 ✓"
else
    warn "自检有失败项（见上）—— 后面的功能测试大概率也会失败"
    warn "先按上面的 → 提示逐项排查，不要往下看"
fi

# ---------------------------------------------------------------------------
log "启动 autod（前台模式、root 身份、跳过 sepolicy）"
"$ADB_BIN" -s "$SERIAL" shell "pkill -f 'autod --socket' 2>/dev/null; rm -f $SOCK; sleep 1" >/dev/null 2>&1 || true
"$ADB_BIN" -s "$SERIAL" shell \
    "nohup $TMP/autod --socket $SOCK --foreground --verbose > $TMP/autod.log 2>&1 &"

for _ in $(seq 1 20); do
    "$ADB_BIN" -s "$SERIAL" shell "test -S $SOCK" 2>/dev/null && break
    sleep 1
done
if ! "$ADB_BIN" -s "$SERIAL" shell "test -S $SOCK" 2>/dev/null; then
    echo "--- autod 日志 ---" >&2
    "$ADB_BIN" -s "$SERIAL" shell "cat $TMP/autod.log" >&2 || true
    echo "--- logcat ---" >&2
    "$ADB_BIN" -s "$SERIAL" logcat -d -s autod:* >&2 2>/dev/null || true
    die "socket $SOCK 没建起来，autod 启动失败"
fi
log "socket 就绪 ✓"

ctl() { "$ADB_BIN" -s "$SERIAL" shell "$TMP/autodctl --socket $SOCK $*"; }

# ---------------------------------------------------------------------------
printf '\n--- info ---\n'
ctl info || warn "info 失败"

# ---------------------------------------------------------------------------
# 截图：两条路都走一遍
#   1) 默认 PNG —— 设备上真实走 AndroidBitmap_compress，和 screencap 同一条路
#   2) --raw PPM —— 未压缩，方便在开发机上直接判定“是不是全黑”
printf '\n--- capture (PNG) ---\n'
if ctl "capture -o $SHOT"; then
    "$ADB_BIN" -s "$SERIAL" pull "$SHOT" "$OUT_DIR/autod-shot.png" >/dev/null 2>&1 \
        && log "已拉回 ${OUT_DIR#"$PROJECT_ROOT"/}/autod-shot.png" \
        || warn "拉取 PNG 失败"
fi

if [ "$FRAME_CHECK" = 1 ]; then
    printf '\n--- capture (PPM，用于自动判定画面) ---\n'
    if ctl "capture --raw -o $SHOT_RAW"; then
        "$ADB_BIN" -s "$SERIAL" pull "$SHOT_RAW" "$OUT_DIR/autod-shot.ppm" >/dev/null 2>&1
        if [ -s "$OUT_DIR/autod-shot.ppm" ]; then
            python3 - "$OUT_DIR/autod-shot.ppm" <<'PY'
import random, sys
path = sys.argv[1]
with open(path, 'rb') as f:
    hdr = b''
    while hdr.count(b'\n') < 3:
        c = f.read(1)
        if not c:
            sys.exit("PPM 头不完整")
        hdr += c
    parts = hdr.split()
    if parts[0] != b'P6':
        sys.exit("不是 P6 PPM：%r" % parts[0])
    w, h = int(parts[1]), int(parts[2])
    off = f.tell()
    f.seek(0, 2)
    expect = off + w * h * 3
    actual = f.tell()
    if actual < expect:
        sys.exit("PPM 被截断：%d < %d" % (actual, expect))

    random.seed(0)
    n, lo, hi, total, colors = 20000, 255, 0, 0, set()
    for _ in range(n):
        x, y = random.randrange(w), random.randrange(h)
        f.seek(off + (y * w + x) * 3)
        r, g, b = f.read(3)
        lo = min(lo, r, g, b)
        hi = max(hi, r, g, b)
        total += (r + g + b) // 3
        colors.add((r >> 4, g >> 4, b >> 4))

print("  分辨率   : %dx%d" % (w, h))
print("  采样     : %d 点（随机 seek，非全量解码）" % n)
print("  亮度范围 : %d ~ %d   均值 %.1f" % (lo, hi, total / n))
print("  颜色密度 : %d / 4096 个 4bit 分箱" % len(colors))
if hi == 0:
    print("  判定     : \033[1;31m全黑\033[0m —— 截图通道拿到了缓冲但内容是黑的，"
          "检查显示后端（-gpu 必须是 swiftshader_indirect）")
elif len(colors) < 8:
    print("  判定     : \033[1;33m近乎纯色\033[0m —— 可能是开机动画之外的空白页，或 SurfaceFlinger 没合成")
elif len(colors) > 200:
    print("  判定     : \033[1;32m有真实画面\033[0m ✓")
else:
    print("  判定     : \033[1;33m内容偏少\033[0m —— 打开一个 App 再截一次更可靠")
PY
        else
            warn "PPM 没拉回来，跳过画面判定"
        fi
    fi
fi

# ---------------------------------------------------------------------------
read -r W H <<<"$("$ADB_BIN" -s "$SERIAL" shell wm size | tr -d '\r' | awk -F'[: x]+' '{print $(NF-1), $NF}')"
if [ -n "${W:-}" ] && [ -n "${H:-}" ]; then
    printf '\n--- tap 屏幕中心 (%d, %d) ---\n' $((W/2)) $((H/2))
    ctl "tap $((W/2)) $((H/2))" || warn "tap 失败"

    printf '\n--- swipe 中线向上 ---\n'
    ctl "swipe $((W/2)) $((H*3/4)) $((W/2)) $((H/4)) --ms 300" || warn "swipe 失败"
else
    warn "拿不到屏幕尺寸，跳过触控测试"
fi

# ---------------------------------------------------------------------------
printf '\n'
log "冒烟测试结束"
cat <<EOF

产物：
  ${OUT_DIR#"$PROJECT_ROOT"/}/autod-shot.png     ← 用图片查看器打开，确认是真实画面
  ${OUT_DIR#"$PROJECT_ROOT"/}/autod-shot.ppm     ← 上面自动判定用的原始帧

接下来人工确认（脚本判断不了的事）：
  1. PNG 里是不是**桌面/界面**，而不是黑屏或纯色
  2. 触控真的生效了吗 —— 打开设置 App，再 tap 一个开关看它有没有变
     （模拟器里 uinput 设备是可见的，这本身就是方案已知代价之一）

排查：
  $ADB_BIN -s $SERIAL logcat -s autod:*
  $ADB_BIN -s $SERIAL shell cat $TMP/autod.log
  $ADB_BIN -s $SERIAL shell dmesg | grep -i avc      # SELinux 拒绝
EOF
