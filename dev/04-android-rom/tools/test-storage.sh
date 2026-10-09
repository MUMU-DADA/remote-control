#!/usr/bin/env bash
# Lossless storage regression: dense archive extraction -> sparse raw disk.
set -euo pipefail
X64_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_IMG="${QEMU_IMG:-}"
if [ -z "$QEMU_IMG" ]; then
    for candidate in "$X64_DIR/.run/release-stage/runtime-linux/emulator/qemu-img" \
        /opt/android/emulator-new/emulator/qemu-img /opt/android/emu31b/emulator/qemu-img; do
        if [ -x "$candidate" ]; then QEMU_IMG="$candidate"; break; fi
    done
fi
[ -x "$QEMU_IMG" ] || { printf 'qemu-img unavailable; set QEMU_IMG to the bundled runtime tool\n' >&2; exit 1; }

python3 - "$X64_DIR" "$QEMU_IMG" <<'PY'
import hashlib, pathlib, stat, subprocess, sys, tempfile, zipfile
root, qemu = pathlib.Path(sys.argv[1]), sys.argv[2]

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()

with tempfile.TemporaryDirectory(prefix='autosnap-storage-') as td:
    work = pathlib.Path(td)
    image = work / 'source.img'
    # Non-zero data at both ends and in the middle protects GPT-like metadata
    # and partition payloads; dense zeroes reproduce standard ZIP extraction.
    with image.open('wb') as f:
        for i in range(96):
            f.write(bytes([i + 1]) * 1048576 if i in (0, 47, 95) else b'\0' * 1048576)
    original = digest(image)
    archive = work / 'images.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z: z.write(image, 'system-qemu.img')
    for platform in ('linux', 'darwin'):
        output = work / platform
        with zipfile.ZipFile(archive) as z: z.extractall(output)
        target = output / 'system-qemu.img'
        target.chmod(0o640)
        before = target.stat()
        assert before.st_blocks * 512 > 90 * 1048576, 'fixture extraction was not dense'
        helper = root / 'packaging/bin' / platform / 'storage.sh'
        cmd = ['bash', '-c', 'set -euo pipefail; . "$1"; optimize_image_storage "$2" "$3"',
               'storage-test', str(helper), str(output), qemu]
        subprocess.run(cmd, check=True)
        after = target.stat()
        assert digest(target) == original, 'sparse recovery changed disk bytes'
        assert after.st_size == before.st_size, 'sparse recovery changed disk geometry'
        assert stat.S_IMODE(after.st_mode) == 0o640, 'sparse recovery changed permissions'
        assert after.st_blocks * 512 < 5 * 1048576, 'zero extents were not reclaimed'
        subprocess.run(cmd, check=True)
        assert target.stat().st_ino == after.st_ino, 'already sparse image was copied again'
        print('PASS %s archive recovery: %.1f MiB -> %.1f MiB, bytes/size/mode preserved; repeat is a no-op' %
              (platform, before.st_blocks / 2048, after.st_blocks / 2048))
PY

PWSH="$(command -v pwsh || true)"
[ -n "$PWSH" ] || { [ ! -x "$X64_DIR/../../.tmp/pwsh/pwsh" ] || PWSH="$X64_DIR/../../.tmp/pwsh/pwsh"; }
if [ -n "$PWSH" ]; then
    DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 "$PWSH" -NoProfile -File "$X64_DIR/tools/test-storage.ps1"
else
    # ⚠️ 这里原来**没有 else** —— 缺 pwsh 就静默少跑一整套 PowerShell 测试，
    #    而脚本照样退出 0、日志里一个字都不提。2026-10-09 实测：把 .tmp/pwsh
    #    挪走后跑这个脚本，退出码 0，`grep -icE "skip|跳过|pwsh"` 命中 **0**。
    #    **"跑过了"和"跑了"是两回事** —— 它的兄弟 test-script-safety.sh 一直是
    #    明说 [SKIP] 的，这里对齐。
    printf '  [SKIP] 没有 pwsh —— Windows 侧的存储测试（test-storage.ps1）整套没跑\n'
    printf '         注意：上面的 PASS 只覆盖 Python 那部分，**不代表整套通过**。\n'
    printf '         补上它：把 PowerShell 7.4.6 linux-x64 解包到 .tmp/pwsh/，\n'
    printf '         确切命令见 tools/test-windows-emulator.sh 头部（本机还需\n'
    printf '         DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1，否则启动即崩）。\n'
fi

