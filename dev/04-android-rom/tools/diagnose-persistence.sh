#!/usr/bin/env bash
# 查"重启后 /data 变干净"的根因 —— 保留实例不删，把两次启动的证据都留下。
#
# 上一次已经把现象复现了：qcow2 在磁盘上（976MB）却读不回来。
# 这次要找出**为什么**。重点怀疑对象：
#   · encryptionkey.img.qcow2 —— FBE 的密钥变了的话 /data 就解不开，
#     系统会当"加密失败"重新格式化，而这不一定在 logcat 里留痕
#   · hardware-qemu.ini 两次是否一致（不一致的话模拟器可能丢弃旧覆盖层）
#   · 模拟器日志里有没有 wipe / format / "userdata" 的字样
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
EMU=./scripts/emulator.sh
ADB=/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb
MARK="diag-$$"
OUT=/tmp/persist-diag
rm -rf "$OUT"; mkdir -p "$OUT"

$EMU delete t3 -y >/dev/null 2>&1
$EMU create t3 >/dev/null 2>&1
PORT=$($EMU list | awk '$1=="t3"{print $2}')
S=".run/sysdir-$PORT"
echo "端口 $PORT   工作目录 $S"

snap() {   # snap <标签>
    local t="$1"
    { echo "--- $t ---"
      for f in userdata-qemu.img userdata-qemu.img.qcow2 encryptionkey.img encryptionkey.img.qcow2 cache.img cache.img.qcow2; do
          printf '%-28s %s\n' "$f" "$(stat -c %s "$S/$f" 2>/dev/null || echo -)"
      done
      echo "md5(encryptionkey.img.qcow2) = $(md5sum "$S/encryptionkey.img.qcow2" 2>/dev/null | cut -d' ' -f1)"
      echo "md5(hardware-qemu.ini)       = $(md5sum "$S/hardware-qemu.ini" 2>/dev/null | cut -d' ' -f1)"
    } | tee -a "$OUT/sizes.txt"
}

echo "===== 第一次启动 ====="
timeout 180 $EMU start t3 >/dev/null 2>&1
cp "$S/../emulator-$PORT.log" "$OUT/boot1-emulator.log" 2>/dev/null
$ADB -s "emulator-$PORT" shell "echo $MARK > /data/local/tmp/$MARK; sync"
echo "写入标记: $($ADB -s "emulator-$PORT" shell "cat /data/local/tmp/$MARK" | tr -d '\r')"
$ADB -s "emulator-$PORT" shell "df /data | tail -1" | tr -d '\r' | sed 's/^/  df: /'
snap "boot1 运行中"

echo "===== 关机 ====="
$EMU stop t3 2>&1 | tail -1
sleep 2
snap "停机后"

echo "===== 第二次启动 ====="
timeout 180 $EMU start t3 >/dev/null 2>&1
cp "$S/../emulator-$PORT.log" "$OUT/boot2-emulator.log" 2>/dev/null
echo "标记还在吗: [$($ADB -s "emulator-$PORT" shell "cat /data/local/tmp/$MARK 2>&1" | tr -d '\r')]"
$ADB -s "emulator-$PORT" shell "df /data | tail -1" | tr -d '\r' | sed 's/^/  df: /'
snap "boot2 运行中"

echo "===== 证据：两次启动日志里跟 userdata / 加密 有关的行 ====="
for n in 1 2; do
    echo "--- boot$n ---"
    grep -inE "userdata|wipe|format|encrypt|qcow|overlay|factory" "$OUT/boot$n-emulator.log" 2>/dev/null | head -12
done

echo "===== 证据：guest logcat 里的 vold / 解密 ====="
$ADB -s "emulator-$PORT" logcat -d 2>/dev/null | grep -iE "vold|decrypt|encrypt|format|wipe" | head -12

echo "===== 证据：hardware-qemu.ini 两次是否一致 ====="
grep -E "^disk\.(dataPartition|encryptionKey)" "$S/hardware-qemu.ini" 2>/dev/null | sed 's/^/  /'

echo
echo "实例 t3 保留在 $S（端口 $PORT），证据在 $OUT，需要继续查就别删"
