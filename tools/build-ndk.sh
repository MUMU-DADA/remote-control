#!/usr/bin/env bash
# =============================================================================
# build-ndk.sh —— 用 NDK 编译 remote-control（**不需要 AOSP 源码树**）
#
# 这条路径的价值：AOSP 同步要 ~110 GB、首次编译要 1 小时；NDK 只要 2.5 GB
# 和几十秒。用它可以在**任意一台 root 的安卓设备**上把服务跑起来，
# 验证除 SurfaceFlinger 直连之外的全部环节。
#
# 截图走 capture_screencap.cpp（exec 设备自带的 screencap）
# 触控走 inject_uinput.cpp（/dev/uinput）
#
# 用法:
#   bash tools/build-ndk.sh                  # 默认 arm64-v8a + API 31
#   API=29 bash tools/build-ndk.sh           # 换 API level
#   ABI=x86_64 bash tools/build-ndk.sh       # 换架构（模拟器用）
#
# 产物: dev/02-native-daemon/out/ndk/<ABI>/{remote-control,rcctl}
# =============================================================================
set -euo pipefail

PROJECT_DIR=/root/AutoSnapshotAndroid
SRC="$PROJECT_DIR/dev/02-native-daemon"
OUT="$SRC/out/ndk"

NDK_ROOT=${NDK_ROOT:-/opt/android/android-ndk-r26d}
ABI=${ABI:-arm64-v8a}
API=${API:-31}                # Android 12 = API 31
JOBS=${JOBS:-$(nproc)}

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; }

# -----------------------------------------------------------------------------
step "定位 NDK"
# -----------------------------------------------------------------------------
if [ ! -d "$NDK_ROOT" ]; then
    bad "找不到 NDK: $NDK_ROOT"
    echo "  下载: wget https://dl.google.com/android/repository/android-ndk-r26d-linux.zip"
    echo "  解压: unzip -q android-ndk-r26d-linux.zip -d /opt/android/"
    exit 1
fi
ok "$NDK_ROOT"

HOST_TAG=linux-x86_64
TOOLCHAIN="$NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG"
[ -d "$TOOLCHAIN" ] || { bad "工具链不存在: $TOOLCHAIN"; exit 1; }

case "$ABI" in
    arm64-v8a)   TRIPLE=aarch64-linux-android ;;
    armeabi-v7a) TRIPLE=armv7a-linux-androideabi ;;
    x86_64)      TRIPLE=x86_64-linux-android ;;
    x86)         TRIPLE=i686-linux-android ;;
    *) bad "不支持的 ABI: $ABI"; exit 1 ;;
esac

CXX="$TOOLCHAIN/bin/${TRIPLE}${API}-clang++"
CC="$TOOLCHAIN/bin/${TRIPLE}${API}-clang"
[ -x "$CXX" ] || { bad "找不到编译器: $CXX"; exit 1; }
ok "$ABI / API $API → ${TRIPLE}${API}-clang++"

# -----------------------------------------------------------------------------
step "检查源码"
# -----------------------------------------------------------------------------
DAEMON="$SRC/daemon"
JSONCPP_DIR=${JSONCPP_DIR:-"$PROJECT_DIR/aosp/external/jsoncpp"}
if [ ! -f "$JSONCPP_DIR/include/json/json.h" ]; then
    bad "找不到 jsoncpp；用 JSONCPP_DIR=<jsoncpp 1.9.4 源码目录> 指定依赖"
    exit 1
fi

# 平台构建才需要的文件，NDK 构建**必须排除**：
#   capture_surfaceflinger.cpp  依赖 libgui
#   inject_binder.cpp           依赖 libbinder + AIDL 生成的 C++ 头（且故意 #error）
for f in main.cpp socket_server.cpp dispatch.cpp selftest.cpp \
         capture_screencap.cpp inject.cpp inject_uinput.cpp \
         appops.cpp subprocess.cpp fileops.cpp http_client.cpp \
         service_state.cpp log_buffer.cpp http_server.cpp rest_api.cpp \
         json_parser.cpp png_encoder.cpp keyboard.cpp clipops.cpp \
         webui.cpp image_encoder.cpp frame_hub.cpp h264_encoder.cpp \
         encode_pool.cpp \
         jpeg_encoder.cpp \
         webp_encoder.cpp \
         websocket.cpp \
         config_file.cpp; do
    [ -f "$DAEMON/$f" ] || { bad "缺源文件: daemon/$f"; exit 1; }
done
ok "28 个源文件齐备"

# -----------------------------------------------------------------------------
# -----------------------------------------------------------------------------
step "编译内置的 libwebp"
# -----------------------------------------------------------------------------
# WebP 和 JPEG 不一样：JPEG 可以 dlopen 设备上已有的 libjpeg，
# WebP 设备上没有（实测 Android 12 就没有 libwebp.so），
# 所以只能把源码整个编进来。
#
# 98 个 .c 文件用 C 编译器单独编成 .o，再和 C++ 那部分链接 ——
# 不能混在一条命令行里，C 和 C++ 的 flags 不一样。
WEBP_DIR="$DAEMON/vendor/webp"
WEBP_OBJ="$OUT/$ABI/webpobj"
mkdir -p "$WEBP_OBJ"

# 这几个开关来自 AOSP 的 libwebp-encode：
#   ANDROID            走 Android 的内存/日志适配
#   WEBP_SWAP_16BIT_CSP  Android 的 16 位通道顺序
#   WEBP_USE_THREAD    允许多线程（我们固定 thread_level=0，但代码要在）
# ⚠️ 不要自己加 -DWEBP_USE_SSE2 之类 —— dsp.h 会按编译器的预定义宏
#    （__SSE2__ / __ARM_NEON）自己判断，重复定义会报 macro redefined。
WEBP_CFLAGS=(-O2 -DANDROID -DWEBP_SWAP_16BIT_CSP -DWEBP_USE_THREAD -I"$WEBP_DIR")

webp_n=0 webp_fail=0
while IFS= read -r cf; do
    obj="$WEBP_OBJ/$(echo "${cf#$WEBP_DIR/}" | tr '/' '_' | sed 's/\.c$/.o/')"
    if [ ! -f "$obj" ] || [ "$cf" -nt "$obj" ]; then
        if ! "$CC" "${WEBP_CFLAGS[@]}" -c "$cf" -o "$obj" 2>"$WEBP_OBJ/err.txt"; then
            webp_fail=$((webp_fail + 1))
            [ $webp_fail -le 3 ] && sed 's/^/    /' "$WEBP_OBJ/err.txt" | head -3
        fi
    fi
    webp_n=$((webp_n + 1))
done < <(find "$WEBP_DIR/src" -name '*.c' | sort)

if [ $webp_fail -gt 0 ]; then
    printf '\033[1;31m  ✗ libwebp: %s 个文件里有 %s 个编不过\033[0m\n' \
           "$webp_n" "$webp_fail"
    exit 1
fi
ok "libwebp: $webp_n 个源文件 → $(ls "$WEBP_OBJ"/*.o 2>/dev/null | wc -l) 个 .o"

# -----------------------------------------------------------------------------
step "编译 remote-control"
# -----------------------------------------------------------------------------
mkdir -p "$OUT/$ABI"

# 关键编译参数：
#   -DREMOTE_CONTROL_NDK_BUILD    走 NDK 形态（无 Binder / 无 init socket）
#   不定义 REMOTE_CONTROL_FULL_PLATFORM
#
# 注意不要用 __ANDROID__ 判断平台形态 —— NDK 构建时它同样是定义的。
COMMON_FLAGS=(
    -std=c++20
    -O2
    -Wall -Wextra -Wno-unused-parameter
    -fPIE -pie
    -DREMOTE_CONTROL_NDK_BUILD=1
    -I"$DAEMON"
    -I"$JSONCPP_DIR/include"
    -DJSON_USE_EXCEPTION=0
)

"$CXX" "${COMMON_FLAGS[@]}" -I"$WEBP_DIR" \
    -o "$OUT/$ABI/remote-control" \
    "$DAEMON/main.cpp" \
    "$DAEMON/socket_server.cpp" \
    "$DAEMON/dispatch.cpp" \
    "$DAEMON/selftest.cpp" \
    "$DAEMON/capture_screencap.cpp" \
    "$DAEMON/inject.cpp" \
    "$DAEMON/inject_uinput.cpp" \
    "$DAEMON/appops.cpp" \
    "$DAEMON/subprocess.cpp" \
    "$DAEMON/fileops.cpp" \
    "$DAEMON/http_client.cpp" \
    "$DAEMON/service_state.cpp" \
    "$DAEMON/log_buffer.cpp" \
    "$DAEMON/http_server.cpp" \
    "$DAEMON/rest_api.cpp" \
    "$DAEMON/json_parser.cpp" \
    "$DAEMON/png_encoder.cpp" \
    "$DAEMON/keyboard.cpp" \
    "$DAEMON/clipops.cpp" \
    "$DAEMON/webui.cpp" \
    "$DAEMON/image_encoder.cpp" \
    "$DAEMON/frame_hub.cpp" \
    "$DAEMON/encode_pool.cpp" \
    "$DAEMON/h264_encoder.cpp" \
    "$DAEMON/jpeg_encoder.cpp" \
    "$DAEMON/webp_encoder.cpp" \
    "$WEBP_OBJ"/*.o \
    "$DAEMON/websocket.cpp" \
    "$DAEMON/config_file.cpp" \
    "$DAEMON/sha256.cpp" \
    "$JSONCPP_DIR/src/lib_json/json_reader.cpp" \
    "$JSONCPP_DIR/src/lib_json/json_value.cpp" \
    "$JSONCPP_DIR/src/lib_json/json_writer.cpp" \
    -llog -lmediandk -static-libstdc++ -lm -pthread

ok "remote-control → $OUT/$ABI/remote-control"

# -----------------------------------------------------------------------------
step "编译 rcctl"
# -----------------------------------------------------------------------------
# 设备端客户端。用 AndroidBitmap_compress 编码 PNG。
# rcctl 仍然直接链 libjnigraphics —— 它是调试工具，只在开发机上跑，
# 不需要照顾老设备（remote-control 本体已经改成 dlopen 了）。
"$CXX" "${COMMON_FLAGS[@]}" \
    -o "$OUT/$ABI/rcctl" \
    "$SRC/client/rcctl.cpp" \
    -ljnigraphics -llog -static-libstdc++

ok "rcctl → $OUT/$ABI/rcctl"

# -----------------------------------------------------------------------------
step "产物"
# -----------------------------------------------------------------------------
for f in remote-control rcctl; do
    p="$OUT/$ABI/$f"
    printf "  %-10s %9s 字节  %s\n" "$f" "$(stat -c%s "$p")" \
        "$(file -b "$p" | cut -c1-60)"
done

# -----------------------------------------------------------------------------
step "部署（推到设备）"
# -----------------------------------------------------------------------------
cat <<EOF
  ABI=$ABI  API=$API

  adb push $OUT/$ABI/remote-control    /data/local/tmp/
  adb push $OUT/$ABI/rcctl /data/local/tmp/
  adb shell chmod 755 /data/local/tmp/remote-control /data/local/tmp/rcctl

  # 需要 root（/dev/uinput 默认 0600）
  adb shell su -c '/data/local/tmp/remote-control --socket /data/local/tmp/remote-control.sock --foreground &'
  # 触控范围要显式给（screencap 后端拿不到显示尺寸）
  adb shell 'wm size'      # 先看分辨率
  adb shell su -c '/data/local/tmp/remote-control --socket /data/local/tmp/remote-control.sock --touch-range 1080x2400'

  adb shell /data/local/tmp/rcctl --socket /data/local/tmp/remote-control.sock info
  adb shell /data/local/tmp/rcctl --socket /data/local/tmp/remote-control.sock capture -o /data/local/tmp/shot.png
  adb shell /data/local/tmp/rcctl --socket /data/local/tmp/remote-control.sock tap 540 1200

注意:
  • screencap 后端每次抓帧 fork+exec 一个进程，约 100-300 ms
    （AOSP 的 SurfaceFlinger 后端只要 20-35 ms）
  • 触控范围由启动探针自动探测（多花一次抓帧，约 100-300ms）
    探针失败时会降到 32767x32767 并给出警告，那时才需要 --touch-range
  • 这是「让服务尽早在真机上跑起来」的路径，最终形态还是 AOSP 构建
EOF
