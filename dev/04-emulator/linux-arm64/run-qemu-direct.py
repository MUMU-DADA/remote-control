#!/usr/bin/env python3
"""用模拟器自带的 QEMU fork 手工启动 arm64 镜像 —— 去掉它硬加的 PCI 设备。

背景：emulator 会给 arm64 guest 塞 16 个 PCI 设备（virtio-serial-pci、
virtio_input_multi_touch_pci_*、virtio-wifi-pci、virtio-vsock-pci），
但 arm 的 ranchu 机器没有 PCI 总线 → QEMU 致命退出（PCI bus not available for hda）。

这里从 emulator -verbose 的 argv 记录里重建命令行：
  * 丢掉所有 *-pci* 设备（mmio 版 virtio-serial 仍在，virtconsole 照样能用）
  * -serial null 换成 -serial stdio，以便直接看到 guest 内核输出
"""
import re
import subprocess
import sys
from pathlib import Path

DRY = '--dry-run' in sys.argv
_args = [a for a in sys.argv[1:] if not a.startswith('--')]
LOG = _args[0] if _args else '/tmp/t6.log'
OUT = _args[1] if len(_args) > 1 else '/tmp/rawqemu.log'
SECONDS = int(_args[2]) if len(_args) > 2 else 45

text = Path(LOG).read_text(encoding='utf-8', errors='replace')
pairs = sorted(((int(m.group(1)), m.group(2)) for m in re.finditer(r'argv\[(\d+)\] = "(.*)"', text)))
argv = [a for _, a in pairs]
if not argv:
    sys.exit('没从日志里解析到 argv')

qemu = argv[0]
assert 'qemu-system-aarch64' in qemu, '第一条不是 arm64 的 qemu：' + qemu

dropped, out = [], [qemu]
i = 1
while i < len(argv):
    a = argv[i]
    if a == '-device' and i + 1 < len(argv):
        dev = argv[i + 1]
        if '-pci' in dev or '_pci' in dev:     # arm ranchu 没有 PCI 总线（两种命名都要拦）
            dropped.append(dev)
            i += 2
            continue
        out += ['-device', dev]
        i += 2
        continue
    if a == '-serial' and i + 1 < len(argv):
        dropped.append('-serial ' + argv[i + 1])   # 这个 fork 的解析器不认 -serial
        i += 2
        continue
    out.append(a)
    i += 1

print('丢弃的参数：')
for d in dropped:
    print('   ', d[:100])
print(f'\n重建后的命令（{len(out)} 个参数）：')
print('  ' + qemu)
for k in range(1, len(out) - 1, 2):
    print(f'    {out[k]} {out[k+1][:88]}')
if DRY:
    sys.exit(0)

env = dict(__import__('os').environ)
# QEMU 的库在 emulator 包内
env['LD_LIBRARY_PATH'] = str(Path(qemu).parent.parent.parent / 'lib64') + ':' + env.get('LD_LIBRARY_PATH', '')
print(f'\n启动（最多 {SECONDS}s）…')
with open(OUT, 'w') as fh:
    p = subprocess.Popen(out, stdout=fh, stderr=subprocess.STDOUT, env=env, cwd='/tmp')
    try:
        p.wait(timeout=SECONDS)
        print('进程自行退出，代码', p.returncode)
    except subprocess.TimeoutExpired:
        print(f'{SECONDS}s 后仍在运行 → 说明没有立刻挂掉 ✓')
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()

data = Path(OUT).read_text(encoding='utf-8', errors='replace')
lines = [l for l in data.splitlines() if l.strip()]
print(f'\n输出共 {len(lines)} 行，尾部 25 行：')
for l in lines[-25:]:
    print('   ', l[:150])
