#!/usr/bin/env bash
# =============================================================================
# build-apk.sh —— 构建上位应用（autod 控制台）
#
# 为什么不走 Soong：Soong 要占 AOSP 的 out/，而那个目录经常被别的构建占着
# （防重入检查会直接拒绝）。这里用独立的 SDK build-tools，随时能编。
#
# 需要的工具（默认从 /opt/android/btools 找，可用 BT 覆盖）：
#   aapt2  d8  apksigner  zipalign        + javac（JDK）
# 以及一个 android.jar：
#   默认用 AOSP 树里的 prebuilts/sdk/31/public/android.jar
#
# 用法:
#   bash build-apk.sh              # 构建 + 签名
#   bash build-apk.sh --install    # 构建后 adb 安装
# =============================================================================
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$APP_DIR/../.." && pwd)"
BT="${BT:-/opt/android/btools/android-13}"
ANDROID_JAR="${ANDROID_JAR:-$PROJECT_ROOT/aosp/prebuilts/sdk/31/public/android.jar}"
OUT="$APP_DIR/build"
PKG=com.autod.controller

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; }

# -----------------------------------------------------------------------------
step "检查工具链"
# -----------------------------------------------------------------------------
for t in aapt2 d8 apksigner zipalign; do
    [ -x "$BT/$t" ] || { bad "缺 $BT/$t"; exit 1; }
done
ok "build-tools: $BT"

# JDK：优先用 AOSP 树自带的（prebuilts/jdk/jdk11），
# 宿主机上不一定装了 JDK，而这份肯定在（repo sync 时就下来了）。
if [ -z "${JAVA_HOME:-}" ]; then
    for j in "$PROJECT_ROOT/aosp/prebuilts/jdk/jdk11/linux-x86" \
             "$PROJECT_ROOT/aosp/prebuilts/jdk/jdk9/linux-x86"; do
        if [ -x "$j/bin/javac" ]; then
            JAVA_HOME="$j"
            break
        fi
    done
fi
if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/javac" ]; then
    JAVAC="$JAVA_HOME/bin/javac"
    KEYTOOL="$JAVA_HOME/bin/keytool"
else
    command -v javac >/dev/null || { bad "找不到 JDK（既没有 AOSP 自带的，也没有系统装的）"; exit 1; }
    JAVAC="$(command -v javac)"
    KEYTOOL="$(command -v keytool)"
fi
# d8/apksigner 是 shell 脚本，内部 `exec java ...`。
# 必须把 JDK 的 bin 放到 PATH 最前面，否则可能撞上同名的目录或别的 java
# （实测报 "exec: java: 无法执行：是一个目录"）。
if [ -n "${JAVA_HOME:-}" ]; then
    export JAVA_HOME
    export PATH="$JAVA_HOME/bin:$PATH"
fi
ok "javac: $("$JAVAC" -version 2>&1 | head -1)  ($JAVAC)"
ok "java : $(command -v java || echo '（PATH 里没有）')"

[ -f "$ANDROID_JAR" ] || { bad "缺 android.jar: $ANDROID_JAR"; exit 1; }
ok "android.jar: $(stat -c%s "$ANDROID_JAR") 字节"

# -----------------------------------------------------------------------------
step "编译资源与清单"
# -----------------------------------------------------------------------------
rm -rf "$OUT"
mkdir -p "$OUT/gen" "$OUT/classes"

# 没有 res/ 目录也要走 aapt2 link —— 它负责把清单编成二进制并产出 APK 骨架
"$BT/aapt2" link \
    -o "$OUT/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest "$APP_DIR/AndroidManifest.xml" \
    --java "$OUT/gen" \
    --min-sdk-version 31 \
    --target-sdk-version 31 \
    --auto-add-overlay
ok "资源与清单已编译"

# -----------------------------------------------------------------------------
step "编译 Java"
# -----------------------------------------------------------------------------
find "$APP_DIR/java" -name '*.java' > "$OUT/sources.txt"
find "$OUT/gen" -name '*.java' >> "$OUT/sources.txt"
# 两个约束叠在一起，只有这一种组合能过：
#
#  1. 必须是 -source/-target 8 —— JDK 11 的 javac 在 target ≥ 9 时不允许
#     -bootclasspath，而 8 正好也是 Android 期望的字节码级别（d8 再转 dex）。
#
#  2. ⚠️ **不能**用 -bootclasspath android.jar。
#     用 lambda 时 javac 需要 java.lang.invoke.LambdaMetafactory，
#     而用 -bootclasspath 把 JDK 的 java.* 顶掉之后，android.jar 里的那个
#     签名对不上，会报：
#        致命错误: 找不到方法 metafactory(...)
#     改用 -classpath 让 javac 用 JDK 自己的实现编译，lambda 由 d8 脱糖。
#     代价是编译期能看到 JDK 专有 API —— 本项目只用了 android.* 和
#     Android 上确实存在的 java.*，可控。
set +e
"$JAVAC" -source 8 -target 8 -nowarn -Xlint:-options \
    -classpath "$ANDROID_JAR" \
    -d "$OUT/classes" \
    @"$OUT/sources.txt" > "$OUT/javac.log" 2>&1
JAVAC_RC=$?
set -e
if [ $JAVAC_RC -ne 0 ]; then
    bad "javac 失败:"
    grep -E "error|错误" "$OUT/javac.log" | head -15
    exit 1
fi
ok "Java 已编译（$(wc -l < "$OUT/sources.txt") 个源文件）"

# -----------------------------------------------------------------------------
step "转 dex"
# -----------------------------------------------------------------------------
# class 文件数量可能很多，用 @argfile 而不是命令行展开
find "$OUT/classes" -name '*.class' > "$OUT/classlist.txt"
set +e
"$BT/d8" --min-api 31 --output "$OUT" @"$OUT/classlist.txt" > "$OUT/d8.log" 2>&1
D8_RC=$?
set -e
if [ $D8_RC -ne 0 ] || [ ! -f "$OUT/classes.dex" ]; then
    bad "d8 失败（退出码 $D8_RC）:"
    tail -20 "$OUT/d8.log"
    exit 1
fi
ok "classes.dex: $(stat -c%s "$OUT/classes.dex") 字节"

# -----------------------------------------------------------------------------
step "打包"
# -----------------------------------------------------------------------------
cp "$OUT/base.apk" "$OUT/app-unsigned.apk"
(cd "$OUT" && zip -q app-unsigned.apk classes.dex)
ok "已加入 classes.dex"

"$BT/zipalign" -f -p 4 "$OUT/app-unsigned.apk" "$OUT/app-aligned.apk"
ok "已 4 字节对齐"

# -----------------------------------------------------------------------------
step "签名"
# -----------------------------------------------------------------------------
KS="$APP_DIR/debug.keystore"
if [ ! -f "$KS" ]; then
    # 调试用密钥。生产环境应换成自己的密钥，并把应用装成系统应用。
    "$KEYTOOL" -genkeypair -keystore "$KS" -alias autod -keyalg RSA -keysize 2048 \
        -validity 10000 -storepass android -keypass android \
        -dname "CN=autod debug, OU=dev, O=AutoSnapshotAndroid, L=, S=, C=CN" \
        >/dev/null 2>&1 || { bad "keytool 生成密钥失败"; exit 1; }
    ok "已生成调试密钥 $KS"
else
    ok "复用已有密钥"
fi

"$BT/apksigner" sign \
    --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias autod \
    --v1-signing-enabled true --v2-signing-enabled true \
    --out "$OUT/autod-controller.apk" "$OUT/app-aligned.apk"
ok "已签名"

"$BT/apksigner" verify --print-certs "$OUT/autod-controller.apk" 2>/dev/null | head -3

# -----------------------------------------------------------------------------
step "产物"
# -----------------------------------------------------------------------------
APK="$OUT/autod-controller.apk"
printf '  %-40s %8s 字节\n' "autod-controller.apk" "$(stat -c%s "$APK")"
"$BT/aapt2" dump badging "$APK" 2>/dev/null | head -3

echo
echo "安装："
echo "  adb push $APK /data/local/tmp/"
echo "  adb shell pm install -r /data/local/tmp/autod-controller.apk"
echo
echo "⚠️ daemon 的 socket 默认是 0660 root:root，应用以自己的 UID 连不上。"
echo "   起 daemon 时加 --socket-mode 0666，或装成系统应用后调整策略。"

if [ "${1:-}" = "--install" ]; then
    step "安装到设备"
    ADB="${ADB:-adb}"
    "$ADB" push "$APK" /data/local/tmp/ >/dev/null
    "$ADB" shell pm install -r /data/local/tmp/autod-controller.apk
    ok "已安装"
fi
