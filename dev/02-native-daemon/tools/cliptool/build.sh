#!/usr/bin/env bash
# =============================================================================
# build.sh —— 构建剪贴板辅助工具（cliptool.jar）
#
# 产物是一个只含 classes.dex 的 zip，由 remote-control **以 shell 身份**通过
# app_process 运行。为什么需要它、为什么是 shell 身份，见
# ../../daemon/clipops.h 的文件头。
#
# 用法:
#   bash build.sh            # 构建
#   bash build.sh --install  # 构建后推到设备
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$HERE/../../../.." && pwd)"
AOSP="$PROJECT_ROOT/aosp"
BT="${BT:-/opt/android/btools/android-13}"
OUT="$HERE/build"

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; }

# -----------------------------------------------------------------------------
step "工具链"
# -----------------------------------------------------------------------------
ANDROID_JAR="$AOSP/prebuilts/sdk/31/public/android.jar"
[ -f "$ANDROID_JAR" ] || { bad "缺 android.jar: $ANDROID_JAR"; exit 1; }
ok "android.jar $(stat -c%s "$ANDROID_JAR") 字节"

# JDK：优先用 AOSP 自带的（宿主机不一定装了）
if [ -z "${JAVA_HOME:-}" ]; then
    for j in "$AOSP/prebuilts/jdk/jdk11/linux-x86" \
             "$AOSP/prebuilts/jdk/jdk9/linux-x86"; do
        [ -x "$j/bin/javac" ] && { JAVA_HOME="$j"; break; }
    done
fi
[ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/javac" ] \
    || { bad "找不到 JDK"; exit 1; }
export JAVA_HOME
# d8 是 shell 脚本，内部 `exec java ...`，必须把 JDK bin 放 PATH 最前
export PATH="$JAVA_HOME/bin:$PATH"
ok "javac $("$JAVA_HOME/bin/javac" -version 2>&1 | head -1)"

for t in d8; do
    [ -x "$BT/$t" ] || { bad "缺 $BT/$t"; exit 1; }
done
ok "d8 $BT/d8"

# -----------------------------------------------------------------------------
step "编译"
# -----------------------------------------------------------------------------
rm -rf "$OUT"
mkdir -p "$OUT/classes"

# -source/-target 8：JDK 11 在 target ≥ 9 时不接受 -bootclasspath，
# 而 8 也正是 Android 期望的字节码级别（d8 再转 dex）。
# 不用 -bootclasspath android.jar —— lambda 需要 JDK 的 LambdaMetafactory。
"$JAVA_HOME/bin/javac" -source 8 -target 8 -nowarn -Xlint:-options \
    -classpath "$ANDROID_JAR" \
    -d "$OUT/classes" \
    "$HERE/ClipTool.java" 2>&1 | grep -v "^注:" || true
ok "javac 完成"

"$BT/d8" --min-api 31 --output "$OUT" "$OUT/classes/com/remote-control/clip/ClipTool.class"
[ -f "$OUT/classes.dex" ] || { bad "d8 没产出 classes.dex"; exit 1; }
ok "classes.dex $(stat -c%s "$OUT/classes.dex") 字节"

# app_process 的 CLASSPATH 要一个 zip；只放 dex 不打包成标准 jar 也行
(cd "$OUT" && zip -q cliptool.jar classes.dex)
ok "cliptool.jar $(stat -c%s "$OUT/cliptool.jar") 字节"

# -----------------------------------------------------------------------------
step "完成"
# -----------------------------------------------------------------------------
cat <<EOF

推到设备：
  adb push $OUT/cliptool.jar /data/local/tmp/
  adb shell chmod 644 /data/local/tmp/cliptool.jar

验证（注意：**必须以 shell 身份**，root 会被 ClipboardService 拒绝）：
  adb shell 'CLASSPATH=/data/local/tmp/cliptool.jar \\
      app_process /system/bin com.remotecontrol.clip.ClipTool info'

remote-control 会自动在 /data/local/tmp/cliptool.jar 找到它；
放在别处就设 REMOTE_CONTROL_CLIPTOOL 环境变量。
EOF

if [ "${1:-}" = "--install" ]; then
    step "安装到设备"
    ADB="${ADB:-adb}"
    "$ADB" push "$OUT/cliptool.jar" /data/local/tmp/ >/dev/null
    "$ADB" shell chmod 644 /data/local/tmp/cliptool.jar
    ok "已推送"
fi
