#!/usr/bin/env bash
# Clear guest data and snapshots while preserving shared ROM images.
set -euo pipefail
BIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$BIN_DIR/lib.sh"

NAME=""; PORT=""; TIMEOUT=60; YES=0; FORCE=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --name|-n) NAME="${2:?实例名}"; shift ;;
        --port) PORT="${2:?端口}"; shift ;;
        --timeout) TIMEOUT="${2:?秒数}"; shift ;;
        --yes|-y) YES=1 ;;
        --force|-f) FORCE=1 ;;
        -h|--help)
            printf '用法：./bin/reset.sh [--name NAME | --port PORT] [--yes] [--force]\n'
            exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
    shift
done

if [ -n "$PORT" ]; then
    owner="$(instance_names_for_port "$PORT")"
    [ -n "$owner" ] || die "端口 $PORT 没有已登记实例；拒绝清理未登记数据"
    [ -z "$NAME" ] && NAME="$owner"
    [ "$owner" = "$NAME" ] || die "端口 $PORT 属于实例 '$owner'，不是 '$NAME'"
else
    [ -n "$NAME" ] || NAME="$DEFAULT_NAME"
    instance_exists "$NAME" || die "实例 '$NAME' 没登记过端口"
    PORT="$(instance_port "$NAME")"
fi
[ -n "$PORT" ] || die "实例 '$NAME' 没登记过端口"

if [ "$YES" = 0 ]; then
    printf '[!] 重置 %s 会清空已装应用、应用数据、共享存储和快照。\n' "$NAME"
    printf '    输入 yes 继续：'
    read -r answer
    [ "$answer" = yes ] || die "已取消"
fi

stop_args=(--name "$NAME" --timeout "$TIMEOUT")
[ "$FORCE" = 0 ] || stop_args+=(--force)
"$BIN_DIR/stop.sh" "${stop_args[@]}"

sysdir="$(sysdir_for_port "$PORT")"
datadir="$(datadir_for_port "$PORT")"
for f in userdata-qemu.img userdata-qemu.img.qcow2 userdata-qemu.img.qcow2.lock \
         cache.img cache.img.qcow2 cache.img.qcow2.lock encryptionkey.img.qcow2 \
         bootcompleted.ini hardware-qemu.ini hardware-qemu.ini.lock multiinstance.lock \
         emu-launch-params.txt version_num.cache read-snapshot.txt; do
    rm -f "$sysdir/$f"
done
for d in build.avd snapshots tmpAdbCmds; do
    if [ -L "$sysdir/$d" ]; then rm -f "$sysdir/$d"
    else rm -rf "$sysdir/$d"; fi
done
if [ -L "$datadir" ]; then rm -f "$datadir"
else rm -rf "$datadir"; fi
mkdir -p "$datadir"
rm -f "$(service_token_file "$NAME")"
instance_register "$NAME" "$PORT"
ok "已重置 '$NAME'（下次启动使用当前模板）"
