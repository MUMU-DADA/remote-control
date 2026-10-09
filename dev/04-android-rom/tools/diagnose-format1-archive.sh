#!/usr/bin/env bash
# 造一份**格式 1** 的归档：就是 2026-10-09 之前 export 产出的那种布局
# （数据镜像原样搬：userdata-qemu.img + userdata-qemu.img.qcow2）。
#
# 为什么要造：格式 1 那条路线恢复出来 /data 是空的，而**用户手里可能就有这种归档**。
# 新代码已经不生产它了，要复现那个问题只能手工造一份 —— 布局照旧代码：
#   · 暂存用硬链接；符号链接（指向 ROM 的镜像）不导；config.ini/锁/运行期文件不导
#   · tar --sparse --numeric-owner
#   · manifest 里 "format": 1，**没有** dataFile
#
# 用法：make-format1-archive.sh <实例名> <归档路径>
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

NAME="${1:?实例名}"
OUT="${2:?归档路径}"
MANIFEST="INSTANCE-MANIFEST.json"
EXCLUDED='hardware-qemu.ini|hardware-qemu.ini.lock|multiinstance.lock|emu-launch-params.txt|bootcompleted.ini|version_num.cache|config.ini|.*\.qcow2\.lock'

PORT="$(./scripts/emulator.sh list 2>/dev/null | awk -v n="$NAME" '$1==n{print $2}')"
[ -n "$PORT" ] || { echo "[x] 没有叫 '$NAME' 的实例"; exit 1; }
SYSDIR=".run/sysdir-$PORT"
[ -d "$SYSDIR" ] || { echo "[x] 工作目录不存在：$SYSDIR"; exit 1; }
./scripts/emulator.sh list 2>/dev/null | awk -v n="$NAME" '$1==n && /运行中/{found=1} END{exit !found}' \
    && { echo "[x] '$NAME' 还在跑 —— 先停掉再导出（运行中导出会拿到不一致的镜像）"; exit 1; }

STAGE="$(dirname "$OUT")/.fmt1-stage-$$"
rm -rf "$STAGE"; mkdir -p "$STAGE/sysdir" "$STAGE/datadir"

n=0
for f in "$SYSDIR"/*; do
    [ -e "$f" ] || continue
    [ -L "$f" ] && continue                       # 指向 ROM 的符号链接：不导
    b="$(basename "$f")"
    printf '%s' "$b" | grep -qE "^($EXCLUDED)$" && continue
    ln "$f" "$STAGE/sysdir/$b" 2>/dev/null || cp -a "$f" "$STAGE/sysdir/$b"
    n=$((n + 1))
done
for f in ".run/datadir-$PORT"/*; do
    [ -e "$f" ] || continue
    b="$(basename "$f")"
    ln "$f" "$STAGE/datadir/$b" 2>/dev/null || cp -a "$f" "$STAGE/datadir/$b"
done

bytes=$(du -s --block-size=1 "$STAGE" 2>/dev/null | awk '{print $1+0}')
{
    printf '{\n'
    printf '  "format": 1,\n'
    printf '  "instance": "%s",\n' "$NAME"
    printf '  "exportedAt": "%s",\n' "$(date -Iseconds)"
    printf '  "product": "remote_control_x64_arm64",\n'
    printf '  "romFingerprint": "unknown",\n'
    printf '  "stateFiles": %s,\n' "$n"
    printf '  "dirBytes": %s\n' "$bytes"
    printf '}\n'
} > "$STAGE/$MANIFEST"

# 服务属性/令牌（旧代码也带）
./scripts/emulator.sh list >/dev/null 2>&1
: > "$STAGE/host-service.env"
for key in SERVICE_PORT SERVICE_ENABLED SERVICE_AUTH SERVICE_BIND SERVICE_ADB; do
    v="$(grep -E "^$key=" ".run/instances/$NAME.env" 2>/dev/null | head -1)"
    [ -n "$v" ] && printf '%s\n' "$v" >> "$STAGE/host-service.env"
done
[ -s ".run/instances/$NAME.token" ] && { (umask 077; cp ".run/instances/$NAME.token" "$STAGE/host-service.token"); chmod 600 "$STAGE/host-service.token"; }

tar --sparse --numeric-owner -cf "$OUT" -C "$STAGE" . || { rm -rf "$STAGE"; echo "[x] tar 失败"; exit 1; }
chmod 600 "$OUT"
rm -rf "$STAGE"

echo "已造出格式 1 归档：$OUT"
echo "  条目     $(tar -tf "$OUT" | wc -l) 个"
echo "  数据镜像 $(tar -tf "$OUT" | grep -cE 'userdata-qemu\.img(\.qcow2)?$') 个（应为 2：raw + 覆盖层）"
echo "  format   $(tar -xOf "$OUT" ./$MANIFEST | sed -n 's/.*"format": *\([0-9]*\).*/\1/p')"
