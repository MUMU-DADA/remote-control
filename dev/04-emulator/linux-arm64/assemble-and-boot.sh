#!/usr/bin/env bash
# 编译完成后的一键收尾：取新镜像 → 打 encryption 补丁 → 重组 super → 写回 → 启动。
#
#   ./assemble-and-boot.sh          # 组装并启动
#   ./assemble-and-boot.sh --no-boot # 只组装
#
# 背景（详见 README 第 23~25 轮）：
#  * 镜像手术只能"替换/删除已有文件"，**不能新增**（debugfs 新增的文件 guest 的 init 看不到）；
#  * 所以 HAL 的完整性必须靠**干净重编**来保证；
#  * 重编后只需再打两处补丁：kernel-ranchu 的 ramoops→noramop（内核二进制）与
#    system.img 的 encryption=Require→Attempt（替换已有 init.rc）。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/common.sh"

AOSP=/root/AutoSnapshotAndroid/aosp
IMG="$AOSP/out/target/product/emulator64_arm64"
SURG="$RUN_DIR/surgery2"
HOSTBIN="$AOSP/out/host/linux-x86/bin"
LPM="$HOSTBIN/lpmake"
LPD="$HOSTBIN/lpdump"
DO_BOOT=1
[ "${1:-}" = "--no-boot" ] && DO_BOOT=0

mkdir -p "$SURG"
cd "$SURG"

# ---- 1) 四个分区镜像就位（编译产出就是纯 ext4，直接用） --------------------
for f in system.img system_ext.img product.img vendor.img; do
    [ -f "$IMG/$f" ] || die "缺少 $IMG/$f（先跑一次 m）"
done
log "四个分区镜像就绪："
ls -la "$IMG"/{system,system_ext,product,vendor}.img | awk '{printf "  %10s  %s\n", $5, $9}'

# ---- 2) system.img：encryption=Require → Attempt（替换已有文件，有效） -----
log "给 system.img 打 encryption 补丁"
python3 - "$IMG/system.img" <<'PY'
import subprocess, sys
F = sys.argv[1]
r = subprocess.run(["debugfs", "-R", "cat /system/etc/init/hw/init.rc", F], capture_output=True)
orig = r.stdout
if not orig:
    print("  !! 读不到 init.rc"); sys.exit(1)
new = orig.replace(b"encryption=Require", b"encryption=Attempt")
print("  init.rc %d → %d 字节" % (len(orig), len(new)))
open("/tmp/initrc.patched", "wb").write(new)
subprocess.run(["debugfs", "-w", "-R", "rm /system/etc/init/hw/init.rc", F], capture_output=True)
subprocess.run(["debugfs", "-w", "-R", "write /tmp/initrc.patched /system/etc/init/hw/init.rc", F], capture_output=True)
r2 = subprocess.run(["debugfs", "-R", "cat /system/etc/init/hw/init.rc", F], capture_output=True)
print("  回读：Require=%d Attempt=%d" % (r2.stdout.count(b"encryption=Require"), r2.stdout.count(b"encryption=Attempt")))
PY

# ---- 3) kernel-ranchu：ramoops → noramop（消除 pstore panic） --------------
log "给 kernel-ranchu 打 ramoops 补丁"
python3 - "$IMG/kernel-ranchu" <<'PY'
import gzip, subprocess, sys
P = sys.argv[1]
raw = gzip.decompress(open(P, "rb").read())
n = raw.count(b"ramoops")
raw = raw.replace(b"ramoops", b"noramop")
open("/tmp/kernel.patched", "wb").write(gzip.compress(raw, 9))
print("  ramoops → noramop：%d 处；剩余 %d" % (n, raw.count(b"ramoops")))
PY
cp -f /tmp/kernel.patched "$IMG/kernel-ranchu"

# ---- 4) 用四个镜像重建 super，并写回 --------------------------------------
log "重建 super"
"$LPM" --device-size 4303355904 --metadata-size 65536 --metadata-slots 2 \
    --group emulator_dynamic_partitions:4294967296 \
    --partition system:readonly:$(stat -c%s "$IMG/system.img"):emulator_dynamic_partitions --image system="$IMG/system.img" \
    --partition system_ext:readonly:$(stat -c%s "$IMG/system_ext.img"):emulator_dynamic_partitions --image system_ext="$IMG/system_ext.img" \
    --partition product:readonly:$(stat -c%s "$IMG/product.img"):emulator_dynamic_partitions --image product="$IMG/product.img" \
    --partition vendor:readonly:$(stat -c%s "$IMG/vendor.img"):emulator_dynamic_partitions --image vendor="$IMG/vendor.img" \
    --out "$SURG/newsuper.img" > "$SURG/lpmake.log" 2>&1 || { tail -5 "$SURG/lpmake.log"; die "lpmake 失败"; }
cp -f "$SURG/newsuper.img" "$IMG/super.img"
log "super 已写回：$(stat -c%s "$IMG/super.img") 字节"
"$LPD" "$IMG/super.img" 2>/dev/null | grep -E "Name:|^ +[0-9]+ \.\." | sed 's/^/  /'

[ "$DO_BOOT" = "0" ] && { log "只组装，不启动"; exit 0; }

# ---- 5) 启动 --------------------------------------------------------------
log "启动（ranchu + cortex-a53 + 三个 QEMU 二进制补丁）"
timeout 8 "$ADB" -s emulator-5556 emu kill >/dev/null 2>&1 || true
sleep 2
rm -f "$IMG"/userdata-qemu.img* "$IMG"/encryptionkey.img.qcow2 "$IMG"/cache.img.qcow2 2>/dev/null || true
rm -f "$RUN_DIR/boot-watch.status"
RUN="$RUN_DIR/$(date +%H%M%S)"
mkdir -p "$RUN"
setsid nohup env ANDROID_PRODUCT_OUT="$IMG" ANDROID_BUILD_TOP="$AOSP" \
    "$AOSP/prebuilts/android-emulator/linux-x86_64/emulator" -sysdir "$IMG" -datadir "$RUN" \
    -no-window -gpu swiftshader_indirect \
    -no-audio -no-snapshot -no-boot-anim -accel off -memory 4096 -cores 4 -port 5556 -show-kernel \
    -selinux permissive \
    -feature -VirtconsoleLogcat,-VirtioInput,-VirtioMouse,-VirtioWifi,-VirtioVsockPipe \
    -qemu -cpu cortex-a53 > "$RUN/run.log" 2>&1 &
sleep 3
setsid nohup "$HERE/watch-boot.sh" emulator-5556 > "$RUN/watch.log" 2>&1 &
log "已启动；日志：$RUN/run.log（守护会在 boot_completed 时截图）"
