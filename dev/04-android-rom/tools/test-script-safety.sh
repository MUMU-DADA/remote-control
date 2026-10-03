#!/usr/bin/env bash
# Isolated regressions for script paths that can delete or replace emulator state.
set -uo pipefail

X64_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/autosnap-safety.XXXXXX")"
if [ "${SAFETY_TEST_KEEP:-0}" != 1 ]; then trap 'rm -rf "$TEST_TMP"' EXIT; else printf 'Sandbox: %s\n' "$TEST_TMP"; fi

PASS=0; FAIL=0
okc()  { printf '  [OK] %s\n' "$1"; PASS=$((PASS+1)); }
badc() { printf '  [!!] %s\n' "$1" >&2; FAIL=$((FAIL+1)); }
has() { if printf '%s' "$2" | grep -qF -- "$3"; then okc "$1"; else badc "$1"; fi; }

make_images() {
    local d="$1" f
    mkdir -p "$d/system"
    for f in system-qemu.img vendor-qemu.img product-qemu.img ramdisk-qemu.img \
             kernel-ranchu encryptionkey.img userdata.img advancedFeatures.ini config.ini; do
        printf x > "$d/$f"
    done
    printf 'ro.product.device=remote_control_x64_arm64\n' > "$d/system/build.prop"
}

echo "-- verifier does not uninstall unrelated third-party apps"
VERIFY_ROOT="$TEST_TMP/verify"
mkdir -p "$VERIFY_ROOT/bin/linux" "$VERIFY_ROOT/tools" "$VERIFY_ROOT/mock-shell" "$VERIFY_ROOT/mock-bin"
cp "$X64_DIR/packaging/bin/linux/verify.sh" "$VERIFY_ROOT/bin/linux/verify.sh"
chmod +x "$VERIFY_ROOT/bin/linux/verify.sh"
printf x > "$VERIFY_ROOT/tools/arm64-probe.apk"
printf '#!/bin/sh\n' > "$VERIFY_ROOT/tools/arm64-probe"; chmod +x "$VERIFY_ROOT/tools/arm64-probe"
cat > "$VERIFY_ROOT/bin/linux/lib.sh" <<'EOF'
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLS="$ROOT/tools"
RUN_DIR="$MOCK_RUN"
DEFAULT_NAME=default
ADB="$MOCK_ADB"
log() { printf '%s\n' "$*"; }; ok() { printf '%s\n' "$*"; }; bad() { printf '%s\n' "$*"; }; warn() { :; }
die() { printf '%s\n' "$*" >&2; exit 1; }
instance_port() { printf 5580; }
serial_for_port() { printf 'emulator-%s' "$1"; }
emu_pid_for_port() { printf 4242; }
require_adb() { [ -x "$ADB" ] || die 'mock adb missing'; }
EOF
cat > "$VERIFY_ROOT/mock-shell/getprop" <<'EOF'
#!/bin/sh
case "$1" in
  ro.build.version.sdk) echo 31 ;;
  ro.product.device) echo remote_control_x64_arm64 ;;
  ro.product.cpu.abilist) echo x86_64,arm64-v8a ;;
  ro.dalvik.vm.native.bridge) echo libndk_translation.so ;;
  ro.enable.native.bridge.exec) echo 1 ;;
  *) echo ;;
esac
EOF
cat > "$VERIFY_ROOT/mock-shell/pm" <<'EOF'
#!/bin/sh
case "$1 $2" in
  'list packages')
    echo package:com.example.userapp
    [ -e "$MOCK_PROBE_INSTALLED" ] && echo package:org.autosnap.arm64probe
    ;;
  'install --abi') touch "$MOCK_PROBE_INSTALLED"; echo Success ;;
  'dump org.autosnap.arm64probe') echo 'primaryCpuAbi=arm64-v8a' ;;
  uninstall*) echo called >> "$MOCK_UNINSTALL_LOG"; echo Success ;;
esac
EOF
cat > "$VERIFY_ROOT/mock-shell/monkey" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$VERIFY_ROOT/mock-shell/pidof" <<'EOF'
#!/bin/sh
echo 123
EOF
cat > "$VERIFY_ROOT/mock-shell/ls" <<'EOF'
#!/bin/sh
case "$*" in
  */proc/sys/fs/binfmt_misc*) printf 'arm64_dyn\narm64_exe\n' ;;
  *) exec /bin/ls "$@" ;;
esac
EOF
cat > "$VERIFY_ROOT/mock-shell/grep" <<'EOF'
#!/bin/sh
case "$*" in
  *"/proc/123/maps"*) echo 1 ;;
  *) exec /bin/grep "$@" ;;
esac
EOF
chmod +x "$VERIFY_ROOT/mock-shell"/*
cat > "$VERIFY_ROOT/adb" <<'EOF'
#!/bin/sh
shift 2
case "$1" in
  wait-for-device|push) exit 0 ;;
  logcat)
    [ "${2:-}" = -d ] && echo 'PROBE_RESULT arm64-v8a native ok'
    exit 0
    ;;
  shell)
    shift
    cmd="$*"
    case "$cmd" in
      /data/local/tmp/arm64-probe) echo ARM64_OK ;;
      *) PATH="$MOCK_SHELL:$PATH" sh -c "$cmd" ;;
    esac
    ;;
esac
EOF
chmod +x "$VERIFY_ROOT/adb"
cat > "$VERIFY_ROOT/mock-bin/sleep" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$VERIFY_ROOT/mock-bin/sleep"
MOCK_RUN="$VERIFY_ROOT/run" MOCK_ADB="$VERIFY_ROOT/adb" MOCK_SHELL="$VERIFY_ROOT/mock-shell" \
MOCK_PROBE_INSTALLED="$VERIFY_ROOT/probe-installed" MOCK_UNINSTALL_LOG="$VERIFY_ROOT/uninstall.log" \
PATH="$VERIFY_ROOT/mock-bin:$PATH" \
    "$VERIFY_ROOT/bin/linux/verify.sh" > "$VERIFY_ROOT/verify.log" 2>&1
if grep -q '验收全部通过' "$VERIFY_ROOT/verify.log" && [ ! -e "$VERIFY_ROOT/uninstall.log" ]; then
    okc "Linux verify completes without uninstalling an app"
else
    badc "Linux verify mock regression: $(tail -3 "$VERIFY_ROOT/verify.log" | tr '\n' '|')"
fi

echo "-- explicit port collision preserves the other instance's data"
for platform in linux darwin; do
    PKG="$TEST_TMP/$platform"
    mkdir -p "$PKG/bin/$platform" "$PKG/runtime/emulator/qemu" "$PKG/runtime/platform-tools" \
             "$PKG/templates" "$PKG/images" "$PKG/run/instances"
    cp "$X64_DIR/packaging/bin/$platform/start-headless.sh" "$PKG/bin/$platform/start-headless.sh"
    cp "$X64_DIR/packaging/bin/$platform/lib.sh" "$PKG/bin/$platform/lib.sh"
    chmod +x "$PKG/bin/$platform/start-headless.sh" "$PKG/bin/$platform/lib.sh"
    if [ "$platform" = linux ]; then backend=linux-x86_64/qemu-system-x86_64-headless
    else backend=darwin-aarch64/qemu-system-aarch64-headless; fi
    mkdir -p "$PKG/runtime/emulator/qemu/$(dirname "$backend")"
    printf '#!/bin/sh\nexit 1\n' > "$PKG/runtime/emulator/emulator"; chmod +x "$PKG/runtime/emulator/emulator"
    printf '#!/bin/sh\nexit 1\n' > "$PKG/runtime/emulator/qemu/$backend"; chmod +x "$PKG/runtime/emulator/qemu/$backend"
    printf '#!/bin/sh\nexit 0\n' > "$PKG/runtime/platform-tools/adb"; chmod +x "$PKG/runtime/platform-tools/adb"
    printf 'hw.lcd.width=720\n' > "$PKG/templates/config.ini"
    make_images "$PKG/images"
    printf '#!/bin/sh\necho Darwin\n' > "$TEST_TMP/uname"
    chmod +x "$TEST_TMP/uname"
    if [ "$platform" = darwin ]; then
        cat > "$TEST_TMP/uname" <<'EOF'
#!/bin/sh
case "${1:-}" in -s) echo Darwin ;; -m) echo arm64 ;; *) echo Darwin ;; esac
EOF
        chmod +x "$TEST_TMP/uname"
    fi
    printf 'PORT=5582\n' > "$PKG/run/instances/owner.env"
    mkdir -p "$PKG/run/sysdir-5582" "$PKG/run/datadir-5582"
    printf owner-sys > "$PKG/run/sysdir-5582/marker"
    printf owner-data > "$PKG/run/datadir-5582/marker"
    start="$PKG/bin/$platform/start-headless.sh"
    if [ "$platform" = darwin ]; then start="$PKG/bin/darwin/start-headless.sh"; fi
    if PATH="$TEST_TMP:$PATH" AUTOSNAP_RUNTIME="$PKG/runtime" AUTOSNAP_IMAGES="$PKG/images" \
       AUTOSNAP_TEMPLATES="$PKG/templates" AUTOSNAP_CONFIG="$PKG/templates/config.ini" \
       AUTOSNAP_RUN_DIR="$PKG/run" AUTOSNAP_ADB="$PKG/runtime/platform-tools/adb" \
       "$start" --name newcomer --port 5582 --accel off --no-wait > "$PKG/collision.log" 2>&1; then
        badc "$platform explicit collision unexpectedly succeeded"
    elif grep -q '已登记给实例' "$PKG/collision.log" && \
         [ "$(cat "$PKG/run/sysdir-5582/marker")" = owner-sys ] && \
         [ "$(cat "$PKG/run/datadir-5582/marker")" = owner-data ] && \
         [ ! -e "$PKG/run/instances/newcomer.env" ]; then
        okc "$platform collision preserves registered instance data"
    else
        badc "$platform collision did not fail closed: $(cat "$PKG/collision.log" 2>/dev/null | tail -3 | tr '\n' '|')"
    fi

    mkdir -p "$PKG/run/datadir-5584"
    printf orphan-data > "$PKG/run/datadir-5584/marker"
    if PATH="$TEST_TMP:$PATH" AUTOSNAP_RUNTIME="$PKG/runtime" AUTOSNAP_IMAGES="$PKG/images" \
       AUTOSNAP_TEMPLATES="$PKG/templates" AUTOSNAP_CONFIG="$PKG/templates/config.ini" \
       AUTOSNAP_RUN_DIR="$PKG/run" AUTOSNAP_ADB="$PKG/runtime/platform-tools/adb" \
       "$start" --name newcomer --port 5584 --accel off --no-wait > "$PKG/orphan.log" 2>&1; then
        badc "$platform unregistered directory unexpectedly reused"
    elif grep -q '拒绝覆盖未登记数据' "$PKG/orphan.log" && \
         [ "$(cat "$PKG/run/datadir-5584/marker")" = orphan-data ] && \
         [ ! -e "$PKG/run/instances/newcomer.env" ]; then
        okc "$platform orphan directory is retained and unregistered"
    else
        badc "$platform orphan directory was not rejected: $(cat "$PKG/orphan.log" 2>/dev/null | tail -3 | tr '\n' '|')"
    fi
done

PWSH="$(command -v pwsh || true)"
if [ -z "$PWSH" ] && [ -x "$X64_DIR/../../.tmp/pwsh/pwsh" ]; then
    PWSH="$X64_DIR/../../.tmp/pwsh/pwsh"
fi
if [ -n "$PWSH" ]; then
    echo "-- Windows explicit port collision preserves existing data"
    PKG="$TEST_TMP/windows"
    mkdir -p "$PKG/bin/windows" "$PKG/runtime/emulator/qemu/windows-x86_64" \
             "$PKG/runtime/platform-tools" "$PKG/templates" "$PKG/images/system" "$PKG/run/instances"
    cp "$X64_DIR/packaging/bin/windows/common.ps1" "$X64_DIR/packaging/bin/windows/start-headless.ps1" "$PKG/bin/windows/"
    printf x > "$PKG/runtime/emulator/emulator.exe"
    printf x > "$PKG/runtime/emulator/qemu/windows-x86_64/qemu-system-x86_64.exe"
    printf x > "$PKG/runtime/platform-tools/adb.exe"
    printf 'hw.lcd.width=720\n' > "$PKG/templates/config.ini"
    make_images "$PKG/images"
    printf 'PORT=5582\n' > "$PKG/run/instances/owner.env"
    mkdir -p "$PKG/run/sysdir-5582" "$PKG/run/datadir-5582"
    printf owner-data > "$PKG/run/datadir-5582/marker"
    if env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 AUTOSNAP_RUNTIME="$PKG/runtime" \
       AUTOSNAP_IMAGES="$PKG/images" AUTOSNAP_TEMPLATES="$PKG/templates" \
       AUTOSNAP_CONFIG="$PKG/templates/config.ini" AUTOSNAP_RUN_DIR="$PKG/run" \
       AUTOSNAP_ADB="$PKG/runtime/platform-tools/adb.exe" \
       "$PWSH" -NoProfile -File "$PKG/bin/windows/start-headless.ps1" \
       -Name newcomer -Port 5582 -NoAccel -NoWait > "$PKG/collision.log" 2>&1; then
        badc "Windows explicit collision unexpectedly succeeded"
    elif grep -q '已登记给实例' "$PKG/collision.log" && \
         [ "$(cat "$PKG/run/datadir-5582/marker")" = owner-data ] && \
         [ ! -e "$PKG/run/instances/newcomer.env" ]; then
        okc "Windows collision preserves registered instance data"
    else
        badc "Windows collision did not fail closed"
    fi
else
    printf '  [SKIP] pwsh unavailable; Windows runtime mock skipped\n'
fi

echo "-- package-rom leaves source and active sysdir data intact"
TEST_TREE="$TEST_TMP/checkout"
mkdir -p "$TEST_TREE/aosp" "$TEST_TREE/dev/04-android-rom/scripts" \
         "$TEST_TREE/dev/04-android-rom/emulator" "$TEST_TREE/product-out/system"
PKGROOT="$TEST_TREE/dev/04-android-rom"
cp "$X64_DIR/scripts/package-rom.sh" "$X64_DIR/scripts/common.sh" "$PKGROOT/scripts/"
cp "$X64_DIR/emulator/config.ini" "$PKGROOT/emulator/config.ini"
PRODUCT_OUT_TEST="$TEST_TREE/product-out"
make_images "$PRODUCT_OUT_TEST"
DEST_TEST="$PKGROOT/artifacts/rom-remote_control_x64_arm64"
run_package() {
    PRODUCT_OUT="$1" TMPDIR_OVERRIDE="$TEST_TREE/tmp" \
        "$PKGROOT/scripts/package-rom.sh"
}
start_emulator_holder() {
    bash -c 'exec -a qemu-system-x86_64-headless python3 -c "import time; time.sleep(30)" holder -sysdir "$1"' \
        safety-holder "$1" &
    HOLDER_PID=$!
}

printf active-data > "$PRODUCT_OUT_TEST/userdata-qemu.img"
start_emulator_holder "$PRODUCT_OUT_TEST"
sleep 0.1
if run_package "$PRODUCT_OUT_TEST" > "$TEST_TREE/active-product.log" 2>&1; then
    badc "package-rom ran while PRODUCT_OUT was in use"
elif grep -q '正在以 .* 作为 -sysdir' "$TEST_TREE/active-product.log" && \
     [ "$(cat "$PRODUCT_OUT_TEST/userdata-qemu.img")" = active-data ]; then
    okc "package-rom refuses active PRODUCT_OUT and retains its data"
else
    badc "package-rom did not protect active PRODUCT_OUT: $(tail -4 "$TEST_TREE/active-product.log" | tr '\n' '|')"
fi
kill "$HOLDER_PID" 2>/dev/null || true; wait "$HOLDER_PID" 2>/dev/null || true

mkdir -p "$DEST_TEST"
printf active-output > "$DEST_TEST/marker"
start_emulator_holder "$DEST_TEST"
sleep 0.1
if run_package "$PRODUCT_OUT_TEST" > "$TEST_TREE/active-dest.log" 2>&1; then
    badc "package-rom ran while its output directory was in use"
elif grep -q '正在以 .* 作为 -sysdir' "$TEST_TREE/active-dest.log" && \
     [ "$(cat "$DEST_TEST/marker")" = active-output ]; then
    okc "package-rom refuses active delivery directory and retains its data"
else
    badc "package-rom did not protect active delivery directory: $(tail -4 "$TEST_TREE/active-dest.log" | tr '\n' '|')"
fi
kill "$HOLDER_PID" 2>/dev/null || true; wait "$HOLDER_PID" 2>/dev/null || true

mkdir -p "$DEST_TEST/system"
printf same-path > "$DEST_TEST/marker"
make_images "$DEST_TEST"
if run_package "$DEST_TEST" > "$TEST_TREE/same-path.log" 2>&1; then
    badc "package-rom accepted PRODUCT_OUT equal to destination"
elif grep -q 'PRODUCT_OUT 与交付目录相同' "$TEST_TREE/same-path.log" && \
     [ "$(cat "$DEST_TEST/marker")" = same-path ]; then
    okc "package-rom rejects identical source and destination paths"
else
    badc "package-rom did not protect identical source and destination"
fi

mkdir -p "$PRODUCT_OUT_TEST/build.avd"
printf keep-me > "$PRODUCT_OUT_TEST/build.avd/marker"
run_package "$PRODUCT_OUT_TEST" > "$TEST_TREE/package.log" 2>&1
if [ "$(cat "$PRODUCT_OUT_TEST/userdata-qemu.img")" = active-data ] && \
   [ "$(cat "$PRODUCT_OUT_TEST/build.avd/marker")" = keep-me ] && \
   [ ! -e "$DEST_TEST/userdata-qemu.img" ] && [ ! -e "$DEST_TEST/build.avd" ]; then
    okc "package-rom preserves source runtime data and excludes it from release output"
else
    badc "package-rom source/output allowlist regression"
fi

echo "-- Windows clone copy failures are not suppressed"
if grep -qE 'Copy-Item .*SilentlyContinue' "$X64_DIR/windows/emulator.ps1"; then
    badc "Windows clone still suppresses Copy-Item errors"
else
    okc "Windows clone copies use terminating errors"
fi

echo
printf '  Passed %d, failed %d (sandbox: %s)\n' "$PASS" "$FAIL" "$TEST_TMP"
[ "$FAIL" -eq 0 ]
