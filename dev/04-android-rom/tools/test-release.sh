#!/usr/bin/env bash
# =============================================================================
# release 打包的体检：**不需要网络、不需要真镜像**，几十秒跑完。
#
#   bash tools/test-release.sh
#
# 验三件事：
#   [0] 所有脚本语法（bash -n + pwsh 语法分析）—— 这里能守住"写完没跑过"的脚本
#   [1] 假 ROM + 假模拟器 zip → **真跑 scripts/release.sh** → 断言两个 zip 的结构、
#       SHA256SUMS 能校验通过、RELEASE.json 字段对得上、START-HERE 占位符全替换了
#   [2] 解压出来的包能自述（Linux status.sh / Windows status.ps1 / Windows 工作目录构建）
#
# 真包（6 GB 镜像 + 真启动）走：./scripts/release.sh --smoke
# =============================================================================
set -uo pipefail

X64_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$X64_DIR/../.."
PWSH="$REPO_ROOT/.tmp/pwsh/pwsh"
TEST_SANDBOX_PARENT="${TEST_RELEASE_SANDBOX_PARENT:-$REPO_ROOT/.tmp}"
mkdir -p "$TEST_SANDBOX_PARENT"
SANDBOX="$(mktemp -d "$TEST_SANDBOX_PARENT/reltest.XXXXXX")"

pass=0; fail=0
chk() {   # chk <描述> <命令...>
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then printf '  \033[1;32m✓\033[0m %s\n' "$desc"; pass=$((pass+1))
    else printf '  \033[1;31m✗\033[0m %s\n' "$desc"; fail=$((fail+1)); fi
}
chk_out() {   # chk_out <描述> <期望子串> <命令...>
    local desc="$1" want="$2"; shift 2
    local out; out="$("$@" 2>&1)"
    if printf '%s' "$out" | grep -q -- "$want"; then printf '  \033[1;32m✓\033[0m %s\n' "$desc"; pass=$((pass+1))
    else
        printf '  \033[1;31m✗\033[0m %s\n    期望包含: %s\n    实际: %s\n' "$desc" "$want" "$(printf '%s' "$out" | head -3 | tr '\n' '|')"
        fail=$((fail+1))
    fi
}
ps() { DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 "$PWSH" -NoProfile "$@"; }

echo "── [0] 脚本语法"
for f in "$X64_DIR"/scripts/release.sh "$X64_DIR"/packaging/bin/linux/*.sh \
         "$X64_DIR"/packaging/bin/darwin/*.sh; do
    chk "bash -n $(basename "$f")" bash -n "$f"
done
if [ -x "$PWSH" ]; then
    for f in "$X64_DIR"/packaging/bin/windows/*.ps1; do
        chk "$(basename "$f") 语法 OK" \
            env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 "$PWSH" -NoProfile -Command \
            "\$e=\$null; [void][System.Management.Automation.Language.Parser]::ParseFile('$f',[ref]\$null,[ref]\$e); if (\$e) { exit 1 }"
    done
else
    echo "  \033[1;33m[!]\033[0m 没有 pwsh（$PWSH），跳过 Windows 脚本语法检查"
fi

echo
echo "── [1] 假 ROM + 假运行时 → 真跑 release.sh"
mkdir -p "$SANDBOX"
printf 'FAKE-CONTROLLER-APK\n' > "$SANDBOX/controller.apk"

FAKE_ROM="$SANDBOX/rom-fake"
mkdir -p "$FAKE_ROM/system" "$FAKE_ROM/vendor"
for f in system-qemu.img vendor-qemu.img product-qemu.img ramdisk-qemu.img \
         kernel-ranchu encryptionkey.img userdata.img advancedFeatures.ini config.ini; do
    head -c 8192 /dev/urandom > "$FAKE_ROM/$f"
done
cp "$X64_DIR/emulator/config.ini" "$FAKE_ROM/config.ini"
cat > "$FAKE_ROM/system/build.prop" <<'EOF'
ro.product.device=remote_control_x64_arm64
ro.product.system.device=remote_control_x64_arm64
ro.system.product.cpu.abilist=x86_64,arm64-v8a
ro.system.product.cpu.abilist64=x86_64,arm64-v8a
EOF
# ⚠️ 顺序要和 package-rom.sh 一致：**先算 SHA256SUMS、再写 MANIFEST.txt**。
#    反过来的话假 ROM 的清单会包含 MANIFEST.txt，而真 ROM 的不会 ——
#    于是"整包清单漏掉 images/MANIFEST.txt"这类 bug 在假测试里测不出来（真实踩过）。
( cd "$FAKE_ROM" && find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS )
cat > "$FAKE_ROM/MANIFEST.txt" <<'EOF'
# lunch 目标：remote_control_x64_arm64-userdebug
# 产品：remote_control_x64_arm64（x86_64 guest + 用户态翻译层）
ro.system.build.fingerprint    = fake/product/fake:12/TEST/1:userdebug/test-keys
ro.product.system.device      = remote_control_x64_arm64
ro.system.product.cpu.abilist = x86_64,arm64-v8a
ro.system.product.cpu.abilist64 = x86_64,arm64-v8a
EOF

# 假模拟器包：结构与真包一致（emulator/ + qemu/<宿主>-x86_64/ + source.properties）
make_fake_emu_zip() {   # <输出> <后端相对路径> <另一个要删掉的后端> <启动器文件名>
    local out="$1" backend="$2" other="$3" launcher="$4" d="$SANDBOX/mkemu"
    rm -rf "$d"; mkdir -p "$d/emulator/qemu/$(dirname "$backend")" "$d/emulator/qemu/$(dirname "$other")"
    printf '#!/bin/sh\necho fake emulator\n' > "$d/emulator/$launcher"; chmod +x "$d/emulator/$launcher"
    printf 'fake backend\n' > "$d/emulator/qemu/$backend"; chmod +x "$d/emulator/qemu/$backend"
    printf 'other backend\n' > "$d/emulator/qemu/$other"
    printf 'Pkg.Revision=0.0.0-test\nPkg.BuildId=999999\n' > "$d/emulator/source.properties"
    ( cd "$d" && zip -qr "$out" . )
}
make_fake_pt_zip() {   # <输出> <adb 文件名>
    local out="$1" adbname="$2" d="$SANDBOX/mkpt"
    rm -rf "$d"; mkdir -p "$d/platform-tools"
    printf '#!/bin/sh\necho fake adb\n' > "$d/platform-tools/$adbname"; chmod +x "$d/platform-tools/$adbname"
    ( cd "$d" && zip -qr "$out" . )
}
make_fake_emu_zip "$SANDBOX/emu-linux.zip"   "linux-x86_64/qemu-system-x86_64-headless"   "windows-x86_64/qemu-system-x86_64.exe" "emulator"
make_fake_emu_zip "$SANDBOX/emu-windows.zip" "windows-x86_64/qemu-system-x86_64.exe"     "linux-x86_64/qemu-system-x86_64-headless" "emulator.exe"
make_fake_emu_zip "$SANDBOX/emu-darwin.zip"  "darwin-x86_64/qemu-system-x86_64-headless" "darwin-aarch64/qemu-system-aarch64-headless" "emulator"
make_fake_pt_zip  "$SANDBOX/pt-linux.zip"   "adb"
make_fake_pt_zip  "$SANDBOX/pt-windows.zip" "adb.exe"
make_fake_pt_zip  "$SANDBOX/pt-darwin.zip"  "adb"

# ⚠️ staging / 缓存指到沙箱里：绝不碰真项目的 .run/release-*
AUTOSNAP_ALLOW_TEST_APK_FIXTURE=1 AUTOSNAP_CONTROLLER_APK="$SANDBOX/controller.apk" \
RELEASE_STAGE_DIR="$SANDBOX/stage" RELEASE_CACHE_DIR="$SANDBOX/cache" \
"$X64_DIR/scripts/release.sh" --platform both --version testrun \
    --images "$FAKE_ROM" --out "$SANDBOX/release" --no-download --keep-stage \
    --linux-emulator-zip   "$SANDBOX/emu-linux.zip" \
    --windows-emulator-zip "$SANDBOX/emu-windows.zip" \
    --linux-platform-tools-zip   "$SANDBOX/pt-linux.zip" \
    --windows-platform-tools-zip "$SANDBOX/pt-windows.zip" \
    > "$SANDBOX/release.log" 2>&1
rc=$?
if [ "$rc" = 0 ]; then printf '  \033[1;32m✓\033[0m %s\n' "release.sh 跑通（两个平台）"; pass=$((pass+1))
else
    printf '  \033[1;31m✗\033[0m release.sh 失败（退出码 %s），日志尾部：\n' "$rc"
    tail -25 "$SANDBOX/release.log" | sed 's/^/    /'; fail=$((fail+1))
fi

AUTOSNAP_ALLOW_TEST_APK_FIXTURE=1 AUTOSNAP_CONTROLLER_APK="$SANDBOX/controller.apk" \
RELEASE_STAGE_DIR="$SANDBOX/stage" RELEASE_CACHE_DIR="$SANDBOX/cache" \
"$X64_DIR/scripts/release.sh" --platform both --version testrun \
    --images "$FAKE_ROM" --out "$SANDBOX/release" --no-download --zip-only --reuse-zip --keep-stage \
    > "$SANDBOX/reuse-zip.log" 2>&1
rc=$?
if [ "$rc" = 0 ] && ! grep -q '清单行数.*与文件数' "$SANDBOX/reuse-zip.log"; then
    printf '  \033[1;32m✓\033[0m %s\n' "zip-only 更新清单并按源码指纹复用 ZIP"; pass=$((pass+1))
else
    printf '  \033[1;31m✗\033[0m zip-only 复用失败（退出码 %s），日志尾部：\n' "$rc"
    tail -15 "$SANDBOX/reuse-zip.log" | sed 's/^/    /'; fail=$((fail+1))
fi

printf '\n# stale-manager-fixture\n' >> "$SANDBOX/stage/autosnap-testrun-linux-x86_64/bin/emulator.sh"
AUTOSNAP_ALLOW_TEST_APK_FIXTURE=1 AUTOSNAP_CONTROLLER_APK="$SANDBOX/controller.apk" \
RELEASE_STAGE_DIR="$SANDBOX/stage" RELEASE_CACHE_DIR="$SANDBOX/cache" \
"$X64_DIR/scripts/release.sh" --platform both --version testrun \
    --images "$FAKE_ROM" --out "$SANDBOX/release" --no-download --zip-only --reuse-zip --keep-stage \
    > "$SANDBOX/reuse-stale.log" 2>&1
rc=$?
if [ "$rc" != 0 ] && grep -q '与当前 staging 不一致' "$SANDBOX/reuse-stale.log"; then
    printf '  \033[1;32m✓\033[0m %s\n' "拒绝复用含旧管理脚本的 ZIP"; pass=$((pass+1))
else
    printf '  \033[1;31m✗\033[0m 未拒绝复用旧 ZIP（退出码 %s），日志尾部：\n' "$rc"
    tail -15 "$SANDBOX/reuse-stale.log" | sed 's/^/    /'; fail=$((fail+1))
fi

cp "$X64_DIR/packaging/bin/linux/emulator.sh" "$SANDBOX/stage/autosnap-testrun-linux-x86_64/bin/emulator.sh"
LINUX_ZIP="$SANDBOX/release/autosnap-testrun-linux-x86_64.zip"
cp "$LINUX_ZIP" "$SANDBOX/linux-clean.zip"
python3 - "$LINUX_ZIP" <<'PY'
import os, sys, zipfile
archive = sys.argv[1]
temp = archive + ".tampered"
with zipfile.ZipFile(archive) as source, zipfile.ZipFile(temp, "w") as target:
    for entry in source.infolist():
        data = source.read(entry.filename)
        if entry.filename.endswith("/bin/emulator.sh"):
            data += b"\n# tampered zip test fixture\n"
        target.writestr(entry, data)
os.replace(temp, archive)
PY
AUTOSNAP_ALLOW_TEST_APK_FIXTURE=1 AUTOSNAP_CONTROLLER_APK="$SANDBOX/controller.apk" \
RELEASE_STAGE_DIR="$SANDBOX/stage" RELEASE_CACHE_DIR="$SANDBOX/cache" \
"$X64_DIR/scripts/release.sh" --platform both --version testrun \
    --images "$FAKE_ROM" --out "$SANDBOX/release" --no-download --zip-only --reuse-zip --keep-stage \
    > "$SANDBOX/reuse-tampered.log" 2>&1
rc=$?
if [ "$rc" != 0 ] && grep -q 'ZIP content hash mismatch: bin/emulator.sh' "$SANDBOX/reuse-tampered.log"; then
    printf '  \033[1;32m✓\033[0m %s\n' "拒绝复用内容被篡改的 ZIP"; pass=$((pass+1))
else
    printf '  \033[1;31m✗\033[0m 未校验 ZIP 内文件内容（退出码 %s），日志尾部：\n' "$rc"
    tail -15 "$SANDBOX/reuse-tampered.log" | sed 's/^/    /'; fail=$((fail+1))
fi
cp "$SANDBOX/linux-clean.zip" "$LINUX_ZIP"

python3 - "$SANDBOX/stage/autosnap-testrun-linux-x86_64/RELEASE.json" <<'PY'
import json, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as source:
    manifest = json.load(source)
manifest["packageSourceSha256"] = "stale-stage-fixture"
with open(path, "w", encoding="utf-8") as target:
    json.dump(manifest, target)
PY
AUTOSNAP_ALLOW_TEST_APK_FIXTURE=1 AUTOSNAP_CONTROLLER_APK="$SANDBOX/controller.apk" \
RELEASE_STAGE_DIR="$SANDBOX/stage" RELEASE_CACHE_DIR="$SANDBOX/cache" \
"$X64_DIR/scripts/release.sh" --platform both --version testrun \
    --images "$FAKE_ROM" --out "$SANDBOX/release" --no-download --zip-only --keep-stage \
    > "$SANDBOX/zip-only-stale-stage.log" 2>&1
rc=$?
if [ "$rc" != 0 ] && grep -q 'staging 来自不同的打包源码' "$SANDBOX/zip-only-stale-stage.log"; then
    printf '  \033[1;32m✓\033[0m %s\n' "拒绝用过期 staging 冒充当前打包源码"; pass=$((pass+1))
else
    printf '  \033[1;31m✗\033[0m 未拒绝过期 staging（退出码 %s），日志尾部：\n' "$rc"
    tail -15 "$SANDBOX/zip-only-stale-stage.log" | sed 's/^/    /'; fail=$((fail+1))
fi

AUTOSNAP_ALLOW_TEST_APK_FIXTURE=1 AUTOSNAP_CONTROLLER_APK="$SANDBOX/controller.apk" \
RELEASE_STAGE_DIR="$SANDBOX/stage-darwin" RELEASE_CACHE_DIR="$SANDBOX/cache-darwin" \
"$X64_DIR/scripts/release.sh" --platform darwin --darwin-arch x64 --version testrun \
    --images "$FAKE_ROM" --out "$SANDBOX/release-darwin" --no-download \
    --darwin-emulator-zip "$SANDBOX/emu-darwin.zip" \
    --darwin-platform-tools-zip "$SANDBOX/pt-darwin.zip" \
    > "$SANDBOX/release-darwin.log" 2>&1
rc=$?
if [ "$rc" = 0 ]; then printf 'Darwin release.sh 跑通\n'; pass=$((pass+1))
else
    printf 'Darwin release.sh 失败（退出码 %s），日志尾部：\n' "$rc"
    tail -25 "$SANDBOX/release-darwin.log" | sed 's/^/    /'; fail=$((fail+1))
fi

LINUX_ZIP="$SANDBOX/release/autosnap-testrun-linux-x86_64.zip"
WIN_ZIP="$SANDBOX/release/autosnap-testrun-windows-x86_64.zip"
DARWIN_ZIP="$SANDBOX/release-darwin/autosnap-testrun-darwin-x86_64.zip"
LROOT="autosnap-testrun-linux-x86_64"
WROOT="autosnap-testrun-windows-x86_64"
DROOT="autosnap-testrun-darwin-x86_64"

chk "生成了 linux zip"  test -s "$LINUX_ZIP"
chk "生成了 windows zip" test -s "$WIN_ZIP"
chk "生成了 darwin zip" test -s "$DARWIN_ZIP"

# ⚠️ 清单先落成文件再比对：`unzip -Z1 | grep -q` 里 grep 一命中就退出、unzip 吃 SIGPIPE，
#    配上 pipefail 就是"明明有却报没有"（release.sh 里踩过同一个坑）。
unzip -Z1 "$LINUX_ZIP" > "$SANDBOX/linux.list"
unzip -Z1 "$WIN_ZIP"   > "$SANDBOX/win.list"
unzip -Z1 "$DARWIN_ZIP" > "$SANDBOX/darwin.list"

for spec in \
    "$LROOT/runtime/emulator/qemu/linux-x86_64/qemu-system-x86_64-headless" \
    "$LROOT/runtime/emulator/emulator" \
    "$LROOT/runtime/platform-tools/adb" \
    "$LROOT/runtime/RUNTIME.txt" \
    "$LROOT/images/system-qemu.img" \
    "$LROOT/images/system/build.prop" \
    "$LROOT/templates/config.ini" \
    "$LROOT/templates/instance.env" \
    "$LROOT/templates/README.md" \
    "$LROOT/bin/start-headless.sh" \
    "$LROOT/bin/emulator.sh" \
    "$LROOT/bin/reset.sh" \
    "$LROOT/bin/stop.sh" \
    "$LROOT/bin/status.sh" \
    "$LROOT/bin/verify.sh" \
    "$LROOT/bin/lib.sh" \
    "$LROOT/START-HERE.md" \
    "$LROOT/tools/remote-control-controller.apk" \
    "$LROOT/tools/CONTROLLER-APP.md" \
    "$LROOT/RELEASE.json" \
    "$LROOT/SHA256SUMS"; do
    chk "linux 包里有 ${spec#"$LROOT"/}" grep -qxF -- "$spec" "$SANDBOX/linux.list"
done
chk "linux 包里有桥接工具" grep -qxF -- "$LROOT/tools/net-bridge.sh" "$SANDBOX/linux.list"
chk "linux 包不带别的架构后端" bash -c "! grep -q 'qemu/windows-x86_64' '$SANDBOX/linux.list'"

for spec in \
    "$WROOT/runtime/emulator/qemu/windows-x86_64/qemu-system-x86_64.exe" \
    "$WROOT/runtime/emulator/emulator.exe" \
    "$WROOT/runtime/platform-tools/adb.exe" \
    "$WROOT/images/system-qemu.img" \
    "$WROOT/templates/config.ini" \
    "$WROOT/bin/start-headless.ps1" \
    "$WROOT/bin/emulator.ps1" \
    "$WROOT/bin/reset.ps1" \
    "$WROOT/bin/stop.ps1" \
    "$WROOT/bin/status.ps1" \
    "$WROOT/bin/verify.ps1" \
    "$WROOT/bin/common.ps1" \
    "$WROOT/RELEASE.json" \
    "$WROOT/tools/remote-control-controller.apk" \
    "$WROOT/tools/CONTROLLER-APP.md" \
    "$WROOT/SHA256SUMS"; do
    chk "windows 包里有 ${spec#"$WROOT"/}" grep -qxF -- "$spec" "$SANDBOX/win.list"
done
for spec in \
    "$DROOT/runtime/emulator/qemu/darwin-x86_64/qemu-system-x86_64-headless" \
    "$DROOT/bin/emulator.sh" \
    "$DROOT/bin/reset.sh" \
    "$DROOT/tools/remote-control-controller.apk" \
    "$DROOT/tools/CONTROLLER-APP.md" \
    "$DROOT/RELEASE.json" \
    "$DROOT/SHA256SUMS"; do
    chk "darwin 包里有 ${spec#"$DROOT"/}" grep -qxF -- "$spec" "$SANDBOX/darwin.list"
done
chk "windows 包不带 linux 后端" bash -c "! grep -q 'qemu/linux-x86_64' '$SANDBOX/win.list'"
chk "windows 包不带 net-bridge（Windows 侧没实现 -net-tap）" \
    bash -c "! grep -q 'tools/net-bridge.sh' '$SANDBOX/win.list'"
chk "linux 包带入当前上位应用 APK 字节" bash -c "unzip -p '$LINUX_ZIP' '$LROOT/tools/remote-control-controller.apk' | cmp - '$SANDBOX/controller.apk'"
chk "windows 包带入当前上位应用 APK 字节" bash -c "unzip -p '$WIN_ZIP' '$WROOT/tools/remote-control-controller.apk' | cmp - '$SANDBOX/controller.apk'"
chk "darwin 包带入当前上位应用 APK 字节" bash -c "unzip -p '$DARWIN_ZIP' '$DROOT/tools/remote-control-controller.apk' | cmp - '$SANDBOX/controller.apk'"

# 解压 → SHA256SUMS 必须能逐文件校验通过（这是"清单和实物对得上"的唯一证明）
mkdir -p "$SANDBOX/x"; ( cd "$SANDBOX/x" && unzip -q "$LINUX_ZIP" )
chk "包内 SHA256SUMS 逐文件校验通过（linux）" \
    bash -c "cd '$SANDBOX/x/$LROOT' && sha256sum -c SHA256SUMS --quiet"
mkdir -p "$SANDBOX/xw"; ( cd "$SANDBOX/xw" && unzip -q "$WIN_ZIP" )
mkdir -p "$SANDBOX/xd"; ( cd "$SANDBOX/xd" && unzip -q "$DARWIN_ZIP" )
chk "包内 SHA256SUMS 逐文件校验通过（windows）" \
    bash -c "cd '$SANDBOX/xw/$WROOT' && sha256sum -c SHA256SUMS --quiet"

# 清单必须覆盖包内**每一个**文件（SHA256SUMS 自己除外——它没法包含自己的哈希）
chk "linux 包内没有未覆盖文件" bash -c "[ -z \"\$(comm -23 <(cd '$SANDBOX/x/$LROOT' && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort) <(awk '{print \$2}' '$SANDBOX/x/$LROOT/SHA256SUMS' | sed 's|^\./||' | sort))\" ]"
chk "windows 包内没有未覆盖文件" bash -c "[ -z \"\$(comm -23 <(cd '$SANDBOX/xw/$WROOT' && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort) <(awk '{print \$2}' '$SANDBOX/xw/$WROOT/SHA256SUMS' | sed 's|^\./||' | sort))\" ]"

# START-HERE.md 的占位符必须全部替换
chk "START-HERE.md 没有残留占位符" bash -c "! grep -qE '@[A-Z_]+@' '$SANDBOX/x/$LROOT/START-HERE.md'"
chk_out "START-HERE.md 写进了假运行时版本" "0.0.0-test" cat "$SANDBOX/x/$LROOT/START-HERE.md"
chk_out "START-HERE.md 写进了 ROM 指纹" "fake/product/fake:12/TEST" cat "$SANDBOX/x/$LROOT/START-HERE.md"
chk_out "Linux 首读文档使用统一入口" 'emulator.sh start' cat "$SANDBOX/x/$LROOT/START-HERE.md"
chk_out "Windows 首读文档使用统一入口" 'emulator.ps1 start' cat "$SANDBOX/xw/$WROOT/START-HERE.md"
chk_out "上位应用安装说明标出当前集成限制" '尚未接通' cat "$SANDBOX/x/$LROOT/tools/CONTROLLER-APP.md"

# RELEASE.json 的字段
chk_out "RELEASE.json 可解析且平台正确" "linux-x86_64" \
    python3 -c "import json;d=json.load(open('$SANDBOX/x/$LROOT/RELEASE.json'));print(d['platform'])"
chk_out "RELEASE.json 记下了运行时 build id" "999999" \
    python3 -c "import json;print(json.load(open('$SANDBOX/x/$LROOT/RELEASE.json'))['runtime']['buildId'])"
chk_out "RELEASE.json 记下了 sha1" "$(sha1sum "$SANDBOX/emu-linux.zip" | cut -d' ' -f1)" \
    python3 -c "import json;print(json.load(open('$SANDBOX/x/$LROOT/RELEASE.json'))['runtime']['sha1'])"
chk_out "RELEASE.json 的 ROM 产品身份来自镜像清单" "remote_control_x64_arm64|remote_control_x64_arm64-userdebug|x86_64,arm64-v8a" \
    python3 -c "import json;d=json.load(open('$SANDBOX/x/$LROOT/RELEASE.json'))['rom'];print('|'.join([d['product'],d['lunch'],d['abilist']]))"
chk_out "RELEASE.json 将测试 APK 标为未验证夹具" "unverified-test-fixture|tools/remote-control-controller.apk|$(sha256sum "$SANDBOX/controller.apk" | cut -d' ' -f1)" \
    python3 -c "import json;d=json.load(open('$SANDBOX/x/$LROOT/RELEASE.json'))['controllerApp'];print('|'.join([d['packageName'],d['apk'],d['sha256']]))"
chk_out "RELEASE.json 区分测试 APK 与源码构建 APK" "test-fixture|unverified" \
    python3 -c "import json;d=json.load(open('$SANDBOX/x/$LROOT/RELEASE.json'))['controllerApp'];print(d['buildMethod']+'|'+d['signature'])"
chk "RELEASE.json records the release-source fingerprint" bash -c \
    "python3 -c \"import json;print(json.load(open('$SANDBOX/x/$LROOT/RELEASE.json'))['packageSourceSha256'])\" | grep -Eq '^[0-9a-f]{64}$'"
chk_out "RELEASE.json 声明统一管理命令" "create|clone|delete|reset|inspect" \
    python3 -c "import json;d=json.load(open('$SANDBOX/x/$LROOT/RELEASE.json'));print('|'.join(c for c in d['commands'] if c in ('create','clone','delete','reset','inspect')))"
chk_out "Linux RELEASE.json 入口使用 shell 脚本" "bin/start-headless.sh|bin/verify.sh" \
    python3 -c "import json;d=json.load(open('$SANDBOX/x/$LROOT/RELEASE.json'))['entrypoints'];print(d['start']+'|'+d['verify'])"
chk_out "Windows RELEASE.json 入口使用 PowerShell 脚本" 'bin\\start-headless.ps1|bin\\verify.ps1' \
    python3 -c "import json;d=json.load(open('$SANDBOX/xw/$WROOT/RELEASE.json'))['entrypoints'];print(d['start']+'|'+d['verify'])"
chk_out "RELEASE.json 的 sha256sumsLines 与实际行数一致" "1" \
    python3 -c "
import json
d=json.load(open('$SANDBOX/x/$LROOT/RELEASE.json'))
n=sum(1 for l in open('$SANDBOX/x/$LROOT/SHA256SUMS') if l.strip())
print(1 if int(d['files']['sha256sumsLines'])==n else '0 got=%d want=%d'%(n,d['files']['sha256sumsLines']))"

echo
echo "── [2] 解压出来的包能自述（不启动模拟器）"
RESET_RUN="$SANDBOX/run-reset"
mkdir -p "$RESET_RUN/instances" "$RESET_RUN/sysdir-5580/snapshots" "$RESET_RUN/datadir-5580"
printf 'PORT=5580\n' > "$RESET_RUN/instances/default.env"
printf 'app-data\n' > "$RESET_RUN/sysdir-5580/userdata-qemu.img.qcow2"
printf 'snapshot\n' > "$RESET_RUN/sysdir-5580/snapshots/state"
printf 'scratch\n' > "$RESET_RUN/datadir-5580/marker"
printf 'token\n' > "$RESET_RUN/instances/default.token"
ln -s "$SANDBOX/x/$LROOT/images/system-qemu.img" "$RESET_RUN/sysdir-5580/system-qemu.img"
chk_out "emulator.sh reset 明确完成重置" "已重置" \
    env AUTOSNAP_RUN_DIR="$RESET_RUN" "$SANDBOX/x/$LROOT/bin/emulator.sh" reset default --yes
chk "reset 删除数据和快照但保留镜像链接" bash -c \
    "test ! -e '$RESET_RUN/sysdir-5580/userdata-qemu.img.qcow2' && test ! -e '$RESET_RUN/sysdir-5580/snapshots' && test -L '$RESET_RUN/sysdir-5580/system-qemu.img' && test -e '$RESET_RUN/sysdir-5580/system-qemu.img'"
chk "reset 删除旧服务令牌并保留实例登记" bash -c \
    "test ! -e '$RESET_RUN/instances/default.token' && grep -qx 'PORT=5580' '$RESET_RUN/instances/default.env'"
chk_out "linux status.sh 能跑" "这个包里的 ROM" \
    env AUTOSNAP_RUN_DIR="$SANDBOX/run-linux" "$SANDBOX/x/$LROOT/bin/status.sh"
chk_out "linux status.sh 报出模板参数" "hw.ramSize" \
    env AUTOSNAP_RUN_DIR="$SANDBOX/run-linux" "$SANDBOX/x/$LROOT/bin/status.sh"
chk "Linux 生产 release 模板默认开启鉴权" \
    grep -qxF 'service.auth=1' "$SANDBOX/x/$LROOT/templates/config.ini"
chk "Windows 生产 release 模板默认开启鉴权" \
    grep -qxF 'service.auth=1' "$SANDBOX/xw/$WROOT/templates/config.ini"
chk "生产 release 模板保留空 token（首次启动生成）" \
    grep -qxF 'service.token=' "$SANDBOX/x/$LROOT/templates/config.ini"
chk_out "linux start-headless.sh --help" "无头启动" \
    "$SANDBOX/x/$LROOT/bin/start-headless.sh" --help
chk_out "Linux emulator.sh help 列出统一命令" "restart" "$SANDBOX/x/$LROOT/bin/emulator.sh" help
chk_out "Linux emulator.sh 拒绝未知命令" "未知命令" \
    "$SANDBOX/x/$LROOT/bin/emulator.sh" nonsense
chk_out "Linux --port 值不会被当成实例名" "端口必须" \
    env AUTOSNAP_RUN_DIR="$SANDBOX/run-manager-linux" "$SANDBOX/x/$LROOT/bin/emulator.sh" start --port 5581
chk_out "Linux manager rejects invalid low console ports" "5554..65534" \
    env AUTOSNAP_RUN_DIR="$SANDBOX/run-manager-lowport" "$SANDBOX/x/$LROOT/bin/emulator.sh" create low --port 2
MANAGER_RUN="$SANDBOX/run-manager-linux"
chk_out "Linux manager create 创建实例" "已创建实例 'source'" \
    env AUTOSNAP_RUN_DIR="$MANAGER_RUN" "$SANDBOX/x/$LROOT/bin/emulator.sh" create source --port 5680
chk_out "Linux manager refuses unregistered stop ports" "没有唯一登记" \
    env AUTOSNAP_RUN_DIR="$MANAGER_RUN" "$SANDBOX/x/$LROOT/bin/emulator.sh" stop --port 5684
mkdir -p "$SANDBOX/archive-fixture"
printf '{"format":1,"instance":"archive-fixture"}\n' > "$SANDBOX/archive-fixture/INSTANCE-MANIFEST.json"
printf 'guest state\n' > "$SANDBOX/archive-fixture/datadir-state"
tar -cf "$SANDBOX/archive-fixture.tar" -C "$SANDBOX/archive-fixture" .
chk_out "Linux manager inspect reads archive metadata without unpacking" '"instance":"archive-fixture"' \
    env AUTOSNAP_RUN_DIR="$MANAGER_RUN" "$SANDBOX/x/$LROOT/bin/emulator.sh" inspect "$SANDBOX/archive-fixture.tar"
printf 'user-state\n' > "$MANAGER_RUN/sysdir-5680/userdata-qemu.img.qcow2"
mkdir -p "$MANAGER_RUN/sysdir-5680/snapshots" "$MANAGER_RUN/datadir-5680"
printf 'snapshot\n' > "$MANAGER_RUN/sysdir-5680/snapshots/state"
printf 'app-data\n' > "$MANAGER_RUN/datadir-5680/marker"
printf 'HTTP_PORT=18088\nSERVICE_PORT=8088\nSERVICE_AUTH=1\n' >> "$MANAGER_RUN/instances/source.env"
printf 'source-token\n' > "$MANAGER_RUN/instances/source.token"
chmod 600 "$MANAGER_RUN/instances/source.token"
chk_out "Linux manager clone copies instance state" "已复制" \
    env AUTOSNAP_RUN_DIR="$MANAGER_RUN" "$SANDBOX/x/$LROOT/bin/emulator.sh" clone source copy --port 5682
chk "Linux clone retains shared image link and guest data" bash -c \
    "test -L '$MANAGER_RUN/sysdir-5682/system-qemu.img' && cmp '$MANAGER_RUN/sysdir-5680/userdata-qemu.img.qcow2' '$MANAGER_RUN/sysdir-5682/userdata-qemu.img.qcow2' && cmp '$MANAGER_RUN/datadir-5680/marker' '$MANAGER_RUN/datadir-5682/marker'"
chk "Linux clone drops snapshots and stale path state" bash -c \
    "test ! -e '$MANAGER_RUN/sysdir-5682/snapshots' && test ! -e '$MANAGER_RUN/sysdir-5682/hardware-qemu.ini'"
chk "Linux clone preserves auth metadata and token" bash -c \
    "grep -qx 'SERVICE_AUTH=1' '$MANAGER_RUN/instances/copy.env' && test -s '$MANAGER_RUN/instances/copy.token' && ! grep -q '^HTTP_PORT=' '$MANAGER_RUN/instances/copy.env'"
chk_out "Linux manager delete removes instance" "已删除实例 'source'" \
    env AUTOSNAP_RUN_DIR="$MANAGER_RUN" "$SANDBOX/x/$LROOT/bin/emulator.sh" delete source --yes
chk "Linux delete leaves shared ROM intact" test -s "$SANDBOX/x/$LROOT/images/system-qemu.img"
SMOKE_PORT="${TEST_RELEASE_SMOKE_PORT:-5698}"
chk_out "linux start-headless.sh 走到启动这一步（假模拟器起不来是预期的）" "启动：" \
    env AUTOSNAP_RUN_DIR="$SANDBOX/run-linux" "$SANDBOX/x/$LROOT/bin/start-headless.sh" --port "$SMOKE_PORT" --no-wait

if [ -x "$PWSH" ]; then
    chk_out "Windows emulator.ps1 help 列出统一命令" "restart" \
        env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 "$PWSH" -NoProfile -File \
        "$SANDBOX/xw/$WROOT/bin/emulator.ps1" help
    chk_out "Windows --port 值不会被当成实例名" "端口必须" \
        env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 AUTOSNAP_RUN_DIR="$SANDBOX/run-manager-win" \
        ANDROID_EMULATOR_HOME="$SANDBOX/win-emulator-home" "$PWSH" -NoProfile -File \
        "$SANDBOX/xw/$WROOT/bin/emulator.ps1" start --port 5581
    WIN_MANAGER_RUN="$SANDBOX/run-manager-win"
    chk_out "Windows manager create 创建实例" "已创建实例 'source'" \
        env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 AUTOSNAP_RUN_DIR="$WIN_MANAGER_RUN" \
        "$PWSH" -NoProfile -File "$SANDBOX/xw/$WROOT/bin/emulator.ps1" create source --port 5680
    printf 'user-state\n' > "$WIN_MANAGER_RUN/sysdir-5680/userdata-qemu.img.qcow2"
    mkdir -p "$WIN_MANAGER_RUN/sysdir-5680/snapshots" "$WIN_MANAGER_RUN/datadir-5680"
    printf 'snapshot\n' > "$WIN_MANAGER_RUN/sysdir-5680/snapshots/state"
    printf 'app-data\n' > "$WIN_MANAGER_RUN/datadir-5680/marker"
    printf 'SERVICE_PORT=8088\nSERVICE_AUTH=1\n' >> "$WIN_MANAGER_RUN/instances/source.env"
    printf 'source-token\n' > "$WIN_MANAGER_RUN/instances/source.token"
    chk_out "Windows manager clone copies instance state" "已复制" \
        env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 AUTOSNAP_RUN_DIR="$WIN_MANAGER_RUN" \
        "$PWSH" -NoProfile -File "$SANDBOX/xw/$WROOT/bin/emulator.ps1" clone source copy --port 5682
    chk "Windows clone retains guest data" bash -c \
        "cmp '$WIN_MANAGER_RUN/sysdir-5680/userdata-qemu.img.qcow2' '$WIN_MANAGER_RUN/sysdir-5682/userdata-qemu.img.qcow2' && cmp '$WIN_MANAGER_RUN/datadir-5680/marker' '$WIN_MANAGER_RUN/datadir-5682/marker'"
    chk "Windows clone drops snapshots and preserves auth token" bash -c \
        "test ! -e '$WIN_MANAGER_RUN/sysdir-5682/snapshots' && grep -qx 'SERVICE_AUTH=1' '$WIN_MANAGER_RUN/instances/copy.env' && test -s '$WIN_MANAGER_RUN/instances/copy.token'"
    PORT_ONLY_OUTPUT="$(env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 AUTOSNAP_RUN_DIR="$WIN_MANAGER_RUN" \
        "$PWSH" -NoProfile -File "$SANDBOX/xw/$WROOT/bin/emulator.ps1" stop -Port 5682 2>&1 || true)"
    chk_out "Windows manager resolves the instance from -Port" "没有模拟器在跑（实例 'copy'）" printf '%s' "$PORT_ONLY_OUTPUT"
    WRONG_PORT_OUTPUT="$(env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 AUTOSNAP_RUN_DIR="$WIN_MANAGER_RUN" \
        "$PWSH" -NoProfile -File "$SANDBOX/xw/$WROOT/bin/emulator.ps1" stop source -Port 5682 2>&1 || true)"
    chk_out "Windows manager refuses another instance's port" "登记的端口是 5680，不是 5682" printf '%s' "$WRONG_PORT_OUTPUT"
    chk_out "Windows manager delete removes instance" "已删除实例 'source'" \
        env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 AUTOSNAP_RUN_DIR="$WIN_MANAGER_RUN" \
        "$PWSH" -NoProfile -File "$SANDBOX/xw/$WROOT/bin/emulator.ps1" delete source --yes
    chk "Windows delete leaves shared ROM intact" test -s "$SANDBOX/xw/$WROOT/images/system-qemu.img"
    chk_out "Windows emulator.ps1 list 路由到状态页" "这个包里的 ROM" \
        env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 AUTOSNAP_RUN_DIR="$SANDBOX/run-win-list" \
        "$PWSH" -NoProfile -File "$SANDBOX/xw/$WROOT/bin/emulator.ps1" list
    chk_out "windows status.ps1 能跑" "这个包里的 ROM" \
        env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 AUTOSNAP_RUN_DIR="$SANDBOX/run-win" \
        "$PWSH" -NoProfile -File "$SANDBOX/xw/$WROOT/bin/status.ps1"

    # Windows 的工作目录构建（链镜像 / 不链 initrd / config.ini 是实文件）
    cat > "$SANDBOX/win-sysdir-test.ps1" <<PS1
\$ErrorActionPreference = "Stop"
. "$SANDBOX/xw/$WROOT/bin/common.ps1"
Build-SysDir 5592
\$sysdir = Get-SysDir 5592
if (-not (Test-Path (Join-Path \$sysdir "system-qemu.img"))) { Write-Host "缺 system-qemu.img"; exit 1 }
if (Test-Path (Join-Path \$sysdir "initrd")) { Write-Host "initrd 不该出现在工作目录"; exit 1 }
\$cfg = Join-Path \$sysdir "config.ini"
if (-not (Test-Path \$cfg)) { Write-Host "缺 config.ini"; exit 2 }
if ((Get-Item \$cfg).LinkType) { Write-Host "config.ini 必须是实文件（不能是链接）"; exit 3 }
if (-not (Test-Path (Join-Path \$sysdir "system"))) { Write-Host "缺 system\\ 目录链接"; exit 4 }
Write-Host "SYSDIR_OK"
PS1
    chk_out "windows 工作目录构建（镜像就位 / 不链 initrd / config.ini 实文件）" "SYSDIR_OK" \
        env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 AUTOSNAP_RUN_DIR="$SANDBOX/run-win" \
        "$PWSH" -NoProfile -File "$SANDBOX/win-sysdir-test.ps1"
else
    echo "  \033[1;33m[!]\033[0m 没有 pwsh，跳过 Windows 侧自述检查"
fi

echo
echo "── [3] test-release 模板默认关闭鉴权"
TEST_RELEASE_OUT="$SANDBOX/release-test"
TEST_RELEASE_STAGE="$SANDBOX/stage-test"
TEST_RELEASE_CACHE="$SANDBOX/cache-test"
AUTOSNAP_ALLOW_TEST_APK_FIXTURE=1 AUTOSNAP_CONTROLLER_APK="$SANDBOX/controller.apk" \
RELEASE_STAGE_DIR="$TEST_RELEASE_STAGE" RELEASE_CACHE_DIR="$TEST_RELEASE_CACHE" \
    "$X64_DIR/scripts/release.sh" --platform linux --version test-auth \
    --test-release --images "$FAKE_ROM" --out "$TEST_RELEASE_OUT" --no-download \
    --linux-emulator-zip "$SANDBOX/emu-linux.zip" \
    --linux-platform-tools-zip "$SANDBOX/pt-linux.zip" \
    > "$SANDBOX/test-release.log" 2>&1
TEST_RELEASE_ZIP="$TEST_RELEASE_OUT/autosnap-test-auth-linux-x86_64.zip"
mkdir -p "$SANDBOX/xt"; ( cd "$SANDBOX/xt" && unzip -q "$TEST_RELEASE_ZIP" )
TROOT="autosnap-test-auth-linux-x86_64"
chk "test-release 模板默认关闭鉴权" \
    grep -qxF 'service.auth=0' "$SANDBOX/xt/$TROOT/templates/config.ini"
chk "test-release 模板不预置 token" \
    grep -qxF 'service.token=' "$SANDBOX/xt/$TROOT/templates/config.ini"

echo
if [ "$fail" = 0 ]; then
    printf '\033[1;32m==>\033[0m 全部通过：%d 项（沙箱：%s）\n' "$pass" "${SANDBOX#"$REPO_ROOT"/}"
else
    printf '\033[1;31m==>\033[0m %d 项通过，%d 项失败（沙箱：%s，日志：%s）\n' "$pass" "$fail" "${SANDBOX#"$REPO_ROOT"/}" "${SANDBOX#"$REPO_ROOT"/}/release.log"
    exit 1
fi
