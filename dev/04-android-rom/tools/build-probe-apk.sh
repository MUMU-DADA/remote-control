#!/usr/bin/env bash
# 打包验收用探针 APK：**只带 arm64-v8a 一个 ABI** 的原生库。
#
#   ./build-probe-apk.sh              # 产出 artifacts/arm64-probe.apk
#
# 用途：在 x86_64 ROM 上安装并运行它 —— 能装（ABI 被接受）+ 能跑（真的执行 aarch64 机器码），
#       就是"arm64 应用兼容"这一目标的最小充分证据。
#
# 工具链全部来自 AOSP 树与 NDK，不依赖 Android Studio：
#   aapt2 / apksigner（out/host/linux-x86/bin）
#   d8.jar（prebuilts/sdk/tools/linux/lib）
#   android.jar（prebuilts/sdk/31/public）
#   JDK（prebuilts/jdk/jdk11/linux-x86）—— 宿主没装 java 也能跑
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../scripts" && pwd)/common.sh"

SRC="$X64_DIR/tools/arm64-probe"
OUT_DIR="$X64_DIR/artifacts"
BUILD="$X64_DIR/.run/probe-build"
APK_OUT="$OUT_DIR/arm64-probe.apk"

JDK="${JDK:-$AOSP_DIR/prebuilts/jdk/jdk11/linux-x86}"
AAPT2="${AAPT2:-$AOSP_DIR/out/host/linux-x86/bin/aapt2}"
APKSIGNER="${APKSIGNER:-$AOSP_DIR/out/host/linux-x86/bin/apksigner}"
D8_JAR="${D8_JAR:-$AOSP_DIR/prebuilts/sdk/tools/linux/lib/d8.jar}"
ANDROID_JAR="${ANDROID_JAR:-$AOSP_DIR/prebuilts/sdk/31/public/android.jar}"
NDK_DIR="${NDK_DIR:-/opt/android/android-ndk-r26d}"
CLANG="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang"

for f in "$JDK/bin/javac" "$AAPT2" "$APKSIGNER" "$D8_JAR" "$ANDROID_JAR" "$CLANG"; do
    [ -e "$f" ] || die "工具缺失：$f"
done

mkdir -p "$OUT_DIR" "$BUILD/classes" "$BUILD/dex" "$BUILD/stage/lib/arm64-v8a"

log "1/6 编译 arm64-v8a 原生库"
"$CLANG" -shared -fPIC -O2 -Wall \
    -o "$BUILD/stage/lib/arm64-v8a/libarm64probe.so" "$SRC/arm64probe.c" -llog
file -b "$BUILD/stage/lib/arm64-v8a/libarm64probe.so" | grep -q 'ARM aarch64' \
    || die "原生库不是 aarch64 —— ABI 目标搞错了"
log "    $(file -b "$BUILD/stage/lib/arm64-v8a/libarm64probe.so" | cut -c1-60)"

log "2/6 javac（用 AOSP 自带 JDK，宿主不需要装 java）"
"$JDK/bin/javac" -source 8 -target 8 -nowarn \
    -bootclasspath "$ANDROID_JAR" -classpath "$ANDROID_JAR" \
    -d "$BUILD/classes" "$SRC/MainActivity.java" 2>&1 | grep -v "bootstrap class path" || true
[ -f "$BUILD/classes/org/remote_control/arm64probe/MainActivity.class" ] || die "javac 没产出 class"

log "3/6 d8 → classes.dex"
"$JDK/bin/java" -cp "$D8_JAR" com.android.tools.r8.D8 \
    --release --min-api 30 --lib "$ANDROID_JAR" \
    --output "$BUILD/dex" $(find "$BUILD/classes" -name '*.class')
[ -f "$BUILD/dex/classes.dex" ] || die "d8 没产出 classes.dex"

log "4/6 aapt2 link（无资源，只有 manifest）"
"$AAPT2" link -I "$ANDROID_JAR" --manifest "$SRC/AndroidManifest.xml" \
    -o "$BUILD/base.apk" --min-sdk-version 30 --target-sdk-version 31

log "5/6 组装 + 对齐"
# ⚠️ targetSdk ≥ 30 的硬要求：resources.arsc 必须**不压缩**且 4 字节对齐，
#    否则安装直接失败：
#      Failure [-124: ... requires the resources.arsc of installed APKs to be
#      stored uncompressed and aligned on a 4-byte boundary]
#    所以 arsc 单独用 -0（store）打进去，其余按常规压缩。
( cd "$BUILD/stage" && unzip -q -o ../base.apk && cp ../dex/classes.dex . \
  && rm -f ../unsigned.apk \
  && zip -q -X -0 ../unsigned.apk resources.arsc \
  && zip -q -X -9 -r ../unsigned.apk AndroidManifest.xml classes.dex lib )
if [ -x "$AOSP_DIR/out/host/linux-x86/bin/zipalign" ]; then
    # -p：对未压缩的 .so 做页对齐（本探针 extractNativeLibs=true，属额外保险）
    "$AOSP_DIR/out/host/linux-x86/bin/zipalign" -f -p 4 "$BUILD/unsigned.apk" "$BUILD/aligned.apk"
else
    cp "$BUILD/unsigned.apk" "$BUILD/aligned.apk"
fi

log "6/6 签名"
KS="$BUILD/debug.keystore"
if [ ! -f "$KS" ]; then
    "$JDK/bin/keytool" -genkeypair -keystore "$KS" -storepass android -keypass android \
        -alias androiddebugkey -dname "CN=remote-control Debug,O=remote-control,C=CN" \
        -keyalg RSA -keysize 2048 -validity 10000 >/dev/null 2>&1
fi
PATH="$JDK/bin:$PATH" "$APKSIGNER" sign \
    --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --out "$APK_OUT" "$BUILD/aligned.apk"
PATH="$JDK/bin:$PATH" "$APKSIGNER" verify --print-certs "$APK_OUT" | head -3

# 自检：APK 里必须只有 arm64-v8a 一个 ABI，且带 classes.dex
log "自检"
unzip -l "$APK_OUT" | grep -E "classes.dex|lib/" | sed 's/^/    /'
n_abi=$(unzip -l "$APK_OUT" | grep -c "lib/" || true)
# 自检：resources.arsc 必须是 stored（Defl:N 表示被压缩了 → targetSdk 30+ 装不上）
unzip -v "$APK_OUT" | awk '$NF=="resources.arsc"{print "    resources.arsc 压缩方式:", $2, $3}' 
[ "$n_abi" = 1 ] || warn "APK 里有 $n_abi 个原生库条目（期望 1 个，纯 arm64）"

log "完成： ${APK_OUT#"$PROJECT_ROOT"/}  （$(du -h "$APK_OUT" | cut -f1)）"
log "用法： ./scripts/run-linux.sh --apk $APK_OUT"
