#!/usr/bin/env bash
# =============================================================================
# build-remote-control.sh —— 在构建容器里编译 remote-control / rcctl
#
# 前置: 先跑 tools/integrate-aosp.sh 把源码接进 AOSP 树
#
# 用法:
#   bash tools/build-remote-control.sh              # 编 remote-control + rcctl
#   bash tools/build-remote-control.sh --stub       # 额外编 remote_control_stub（桩截图）
#   TARGET=aosp_cf_x86_64_phone bash tools/build-remote-control.sh
#
# 首次编译需要 30-90 分钟（要编 libgui/libbinder 等依赖）。
# 之后改 remote-control 自己的代码，增量编译是分钟级。
#
# 建议后台跑:
#   setsid nohup bash tools/build-remote-control.sh > /var/log/remote-control-build.log 2>&1 &
#   tail -f /var/log/remote-control-build.log
# =============================================================================
set -euo pipefail

CONTAINER=${CONTAINER:-remote-control-builder}
TARGET=${TARGET:-aosp_arm64-userdebug}
JOBS=${JOBS:-12}          # 31 GiB 内存，经验值约 2 GB/任务

# 源树位置（用来和树内副本比新鲜度）
SRC_DIR=${SRC_DIR:-"$(cd "$(dirname "$0")/.." && pwd)/dev/02-native-daemon"}

MODULES="remote-control rcctl"
if [ "${1:-}" = "--stub" ]; then
    MODULES="$MODULES remote_control_stub"
fi

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; }
warn() { printf '\033[1;33m  ! %s\033[0m\n' "$*"; }

# -----------------------------------------------------------------------------
step "检查前置条件"
# -----------------------------------------------------------------------------
docker inspect -f '{{.State.Running}}' "$CONTAINER" 2>/dev/null | grep -q true \
    || { bad "容器 $CONTAINER 未运行，先跑 tools/setup-host.sh"; exit 1; }
ok "容器运行中"

docker exec "$CONTAINER" test -f /aosp/frameworks/native/cmds/remote-control/daemon/Android.bp \
    || { bad "源码未接入，先跑 tools/integrate-aosp.sh"; exit 1; }

# ⚠️ 光"文件在"不够 —— integrate-aosp.sh 是 **rsync 复制**，不是软链。
#    改了 dev/ 下的源码但没重新接入的话，这里会顺利通过，然后编译出
#    一个**跟改动无关的旧二进制**，而且 ninja 一句 "No need to regenerate"
#    就结束，看起来完全像是编译成功了。
#
#    实测被这个坑掉过两次完整构建：改完代码跑 build-remote-control.sh，
#    产物时间戳纹丝不动，还以为是自己改错了。
#
#    所以这里比对源树和树内副本的最新修改时间，不新鲜就直接叫停。
if [ "${SKIP_STALE_CHECK:-0}" != "1" ]; then
    newest_src=$(find "$SRC_DIR" -type f \
                     \( -name '*.cpp' -o -name '*.h' -o -name '*.bp' \
                        -o -name '*.rc' -o -name '*.te' \) \
                     -printf '%T@\n' 2>/dev/null | sort -rn | head -1)
    # ⚠️ 比的是树内**最新文件的 mtime**，不是那个目录的 mtime。
    #    rsync -a 会把源目录的 mtime 一并带过去，拿目录 mtime 来比
    #    会得出"源比树新 10 小时"这种假警报。
    tree_mtime=$(docker exec "$CONTAINER" sh -c \
                     "find /aosp/frameworks/native/cmds/remote-control -type f \
                          \\( -name '*.cpp' -o -name '*.h' -o -name '*.bp' \
                             -o -name '*.rc' -o -name '*.te' \\) \
                          -printf '%T@\\n' | sort -rn | head -1" 2>/dev/null \
                 | cut -d. -f1)
    tree_mtime=${tree_mtime:-0}
    if [ -n "$newest_src" ]; then
        newest_int=${newest_src%%.*}
        if [ "$newest_int" -gt "$tree_mtime" ]; then
            warn "树内源码比 dev/ 旧（差 $(( newest_int - tree_mtime )) 秒）"
            bad  "先跑 tools/integrate-aosp.sh —— 否则编出来的是旧二进制"
            echo "  急着编: SKIP_STALE_CHECK=1 bash tools/build-remote-control.sh"
            exit 1
        fi
    fi
fi
ok "源码已接入且是最新的"

docker exec "$CONTAINER" test -f /aosp/build/envsetup.sh \
    || { bad "AOSP 树不完整（缺 build/envsetup.sh）"; exit 1; }
ok "envsetup.sh 存在"

# 防重入：两个编译同时跑会抢 out/.lock，后来者会报
#   "Tried to lock out/.lock, but timed out polling every 1s until 10s"
# 这个报错完全看不出真正的原因，所以在这里主动拦住。
if bash "$(dirname "$0")/check-no-build.sh"; then
    bad "容器里已有编译在跑，等它结束再启动（避免抢 out/.lock）"
    echo "  查看: docker exec $CONTAINER pgrep -af \"soong_ui|ninja\""
    echo "  等待: while docker exec $CONTAINER pgrep -f soong_ui >/dev/null; do sleep 10; done"
    exit 1
fi
ok "无其他编译在跑"

# -----------------------------------------------------------------------------
step "编译 TARGET=$TARGET  MODULES=$MODULES"
# -----------------------------------------------------------------------------
# 日志同时写文件，方便失败后翻看
LOG=/tmp/remote-control-build-$(date +%Y%m%d-%H%M%S).log

set +e
docker exec "$CONTAINER" bash -lc "
    set -e
    cd /aosp
    source build/envsetup.sh >/dev/null 2>&1
    lunch $TARGET >/dev/null 2>&1
    echo \"TARGET_PRODUCT=\$TARGET_PRODUCT\"
    echo \"ANDROID_PRODUCT_OUT=\$ANDROID_PRODUCT_OUT\"
    m -j$JOBS $MODULES
" 2>&1 | tee "$LOG"
RC=${PIPESTATUS[0]}
set -e

# -----------------------------------------------------------------------------
if [ "$RC" -ne 0 ]; then
    printf '\n\033[1;31m编译失败（退出码 %d）\033[0m\n' "$RC"
    echo
    echo "日志: $LOG"
    echo
    echo "─── 错误摘要 ───"
    grep -nE "error:|Error:|FAILED:" "$LOG" | head -30 || echo "（未匹配到标准错误格式，看完整日志）"
    echo
    echo "─── 常见原因 ───"
    cat <<'EOF'
  • "unknown module libgui_headers"
      → 去掉 Android.bp 里的 header_libs（较老版本把 gui 头文件直接
        放在 libgui 的 export_include_dirs 里）
  • "fatal error: 'gui/XXX.h' file not found"
      → Android 12 的 gui 头文件位置与实现不符，对照
        docs/03-reference.md 核实
  • "fatal error: 'jni.h' file not found"
      → <android/bitmap.h> 需要 jni.h，而它由 libjnigraphics 通过
        export_header_lib_headers 传递。确认 client/Android.bp 的
        shared_libs 里是 "libjnigraphics" 而不是 "libandroid"。
        对照 AOSP 的 frameworks/base/cmds/screencap/Android.bp。
  • "Path is outside directory: ../xxx"
      → Soong 的 srcs/init_rc 不允许 ".."，文件必须与 Android.bp 同目录
  • "depends on undefined module XXX" 但那个模块明明存在
      → Soong 的 glob 文件过期（out/soong/.bootstrap/build-globs.ninja）。
        如果源码是边同步边编译的就会这样。重跑一次即可。
  • "duplicate symbol: CreateInjectorBackend"
      → 两个后端被链进了同一个二进制，检查 srcs 只留一个
  • "./../init/remote-control.rc: No such file"
      → init_rc 的相对路径问题，确认 init/ 与 daemon/ 同级
EOF
    exit "$RC"
fi

# -----------------------------------------------------------------------------
step "完成"
# -----------------------------------------------------------------------------
docker exec "$CONTAINER" bash -lc '
    cd /aosp
    source build/envsetup.sh >/dev/null 2>&1
    lunch '"$TARGET"' >/dev/null 2>&1
    for f in remote-control rcctl remote_control_stub; do
        p="$ANDROID_PRODUCT_OUT/system/bin/$f"
        if [ -f "$p" ]; then
            printf "  %-14s %s  (%s)\n" "$f" "$p" "$(stat -c%s "$p") 字节"
            file "$p" 2>/dev/null | sed "s|^|                 |"
        fi
    done
'

echo
echo "日志: $LOG"
echo
echo "下一步（部署到设备）:"
echo "  DEV=/root/AutoSnapshotAndroid/aosp/out/target/product/<product>/system/bin"
echo "  adb push \$DEV/remote-control    /data/local/tmp/"
echo "  adb push \$DEV/rcctl /data/local/tmp/"
echo "  adb shell chmod 755 /data/local/tmp/remote-control /data/local/tmp/rcctl"
