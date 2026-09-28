#!/usr/bin/env bash
# =============================================================================
# rc-update.sh —— remote-control 的热替换通道
#
# 为什么需要它：init **不重读 rc**，/system 又没有持久写入通道 ——
# 所以"服务起来之后还能换二进制"靠的是 /system 里一个不动的壳
# （remote-control-launch）+ /data 里的版本槽：
#
#     /data/misc/remote-control/
#         ├── current -> releases/<sha256>/remote-control   指针（普通文本文件）
#         ├── last-good                                     上一版能跑的 sha
#         └── releases/<sha256>/remote-control              载荷
#
# 换版本 = push 新二进制 + 切指针 + `setprop ctl.restart remote-control`，
# 秒级生效，不重启设备、不重编镜像。
#
# 用法:
#   bash tools/rc-update.sh push  <二进制>       校验 → 入槽（**不动指针**，安全）
#   bash tools/rc-update.sh switch [<sha>|latest] 原子切指针 + 重启服务
#   bash tools/rc-update.sh rollback             切回 last-good + 重启服务
#   bash tools/rc-update.sh list                 列出所有版本与当前指针
#   bash tools/rc-update.sh verify [<二进制>]    比对 buildId 与本地二进制
#   bash tools/rc-update.sh status               设备侧槽目录一览
#
# 环境:
#   SERIAL=emulator-5580   目标设备
#   RC_HOST=192.168.0.110  HTTP 地址（verify 用）
# =============================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERIAL=${SERIAL:-emulator-5580}
RC_HOST=${RC_HOST:-192.168.0.110}
RC_PORT=${RC_PORT:-8088}
ADB=${ADB:-adb}
TMO=${TMO:-30}

DROOT=/data/misc/remote-control
SVC=remote-control

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; }
warn() { printf '\033[1;33m  ! %s\033[0m\n' "$*"; }
die()  { bad "$*"; exit 1; }

# ⚠️ 每条 adb 都要超时：`adb root` 之后 adbd 重启会阻塞后续调用，
#    而 `adb shell "setsid ... &"` 会因为后台进程挂着管道永不返回。
sh_() { timeout "$TMO" "$ADB" -s "$SERIAL" shell "$@"; }

need_device() {
    timeout "$TMO" "$ADB" -s "$SERIAL" get-state >/dev/null 2>&1 \
        || die "$SERIAL 未连接"
    [ "$(sh_ getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ] \
        || die "$SERIAL 还没开机完成"
}

# ── push ────────────────────────────────────────────────────────────────────
cmd_push() {
    local src="${1:?用法: rc-update.sh push <二进制>}"
    [ -f "$src" ] || die "找不到 $src"

    local sha; sha=$(sha256sum "$src" | cut -d' ' -f1)
    ok "本地 $(basename "$src")"
    printf '    sha256 %s\n' "$sha"
    printf '    大小   %s 字节\n' "$(stat -c%s "$src")"

    # ⚠️ 先推到 /data/local/tmp 再在设备上 mv 进槽。
    #
    #    直接 push 覆盖 releases/<sha>/remote-control 会踩 ETXTBSY ——
    #    那个文件正在被跑着（Linux 不允许写一个正在执行的文件的 inode）。
    #    推到别处再 rename，同分区 rename 是原子的，而且换的是目录项，
    #    正在跑的进程继续用旧 inode，互不影响。
    local tmp="/data/local/tmp/.rc-payload-$$"
    timeout "$TMO" "$ADB" -s "$SERIAL" push "$src" "$tmp" >/dev/null \
        || die "push 失败"

    # 设备上再核一次哈希 —— 传输坏掉的话必须在这里拦住，
    # 而不是等壳 exec 失败（那时只会看到"版本起不来"）。
    local got; got=$(sh_ "sha256sum $tmp 2>/dev/null | cut -d' ' -f1" | tr -d '\r')
    [ "$got" = "$sha" ] || { sh_ "rm -f $tmp"; die "设备上哈希不符：$got"; }
    ok "设备上哈希一致"

    sh_ "mkdir -p $DROOT/releases/$sha && mv $tmp $DROOT/releases/$sha/remote-control && chmod 755 $DROOT/releases/$sha/remote-control && chown shell:shell $DROOT/releases/$sha/remote-control" \
        || die "入槽失败"
    ok "已入槽：releases/$sha/remote-control"
    printf '    下一步   bash tools/rc-update.sh switch %s\n' "$sha"
}

# 原子写指针：先写 .new 再 mv。半截写入会让壳读到一个不存在的路径。
write_pointer() {
    sh_ "cd $DROOT && printf '%s\n' '$1' > current.new && mv current.new current" \
        || die "写指针失败"
}

cmd_switch() {
    need_device
    local want="${1:-latest}"

    if [ "$want" = "latest" ]; then
        # 按 mtime 取最新的一版
        want=$(sh_ "ls -1t $DROOT/releases 2>/dev/null | head -1" | tr -d '\r')
        [ -n "$want" ] || die "槽里一个版本都没有（先 push）"
    fi

    local rel="releases/$want/remote-control"
    [ "$(sh_ "test -x $DROOT/$rel && echo yes" | tr -d '\r')" = "yes" ] \
        || die "槽里没有这一版：$rel"

    step "切换指针"
    printf '    %s → %s\n' "$(sh_ "cat $DROOT/current 2>/dev/null" | tr -d '\r')" "$rel"
    write_pointer "$rel"
    ok "指针已切换"

    restart_service "$want"
}

cmd_rollback() {
    need_device
    local good; good=$(sh_ "cat $DROOT/last-good 2>/dev/null" | tr -d '\r')
    [ -n "$good" ] || die "没有 last-good —— 服务还没成功跑起来过，没有可回退的版本"
    [ "$(sh_ "test -x $DROOT/$good && echo yes" | tr -d '\r')" = "yes" ] \
        || die "last-good 指向的版本不在了：$good"

    step "回退"
    printf '    → %s\n' "$good"
    write_pointer "$good"
    ok "指针已回退"
    restart_service "$(basename "$(dirname "$good")")"
}

# 重启服务并等它自报新版本。
#
# ⚠️ 判据是 **buildId 变了**，不是"进程还在"。
#    "推上去了但跑的还是旧进程"是这个项目反复踩过的坑
#    （integrate-aosp.sh / build-remote-control.sh 的新鲜度检查是同一个道理）。
restart_service() {
    local want_sha="$1"
    step "重启服务"
    if [ "$(sh_ "getprop init.svc.$SVC" | tr -d '\r')" != "running" ]; then
        warn "init.svc.$SVC 不是 running —— 说明服务不是 init 拉起来的（开发期手工跑的？）"
        warn "ctl.restart 不会有任何效果。生产形态请走 remote-control.rc。"
    fi
    sh_ "setprop ctl.restart $SVC" >/dev/null 2>&1 || true

    step "等服务换过来"
    local got=""
    for i in $(seq 1 20); do
        got=$(curl -s --max-time 3 "http://$RC_HOST:$RC_PORT/api/v1/info" 2>/dev/null \
              | python3 -c 'import json,sys; print(json.load(sys.stdin).get("buildId",""))' 2>/dev/null)
        [ "$got" = "$want_sha" ] && break
        sleep 1
    done

    if [ "$got" = "$want_sha" ]; then
        ok "已在跑新版本（buildId ${got:0:12}…，约 $((i)) 秒）"
    else
        bad "buildId 还是 ${got:0:12}…（期望 ${want_sha:0:12}…）"
        echo "  看服务日志： adb -s $SERIAL shell tail -20 $DROOT/remote-control.log"
        echo "  看壳日志：   adb -s $SERIAL shell cat $DROOT/launcher.log"
        return 1
    fi
}

cmd_list() {
    need_device
    step "设备上的版本槽"
    printf '    current   %s\n' "$(sh_ "cat $DROOT/current 2>/dev/null || echo '(空 → 用镜像自带)'" | tr -d '\r')"
    printf '    last-good %s\n' "$(sh_ "cat $DROOT/last-good 2>/dev/null || echo '(无)'" | tr -d '\r')"
    echo
    sh_ "ls -1t $DROOT/releases 2>/dev/null" | tr -d '\r' | while read -r sha; do
        [ -n "$sha" ] || continue
        printf '    %s  %s\n' "${sha:0:16}…" \
            "$(sh_ "stat -c '%y %s 字节' $DROOT/releases/$sha/remote-control 2>/dev/null" | tr -d '\r' | cut -c1-40)"
    done
}

cmd_verify() {
    local src="${1:-}"
    echo "服务自报 buildId:"
    local live; live=$(curl -s --max-time 5 "http://$RC_HOST:$RC_PORT/api/v1/info" 2>/dev/null \
        | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("buildId",""))' 2>/dev/null)
    printf '    %s\n' "${live:-（拿不到 —— 服务没在跑？）}"

    if [ -n "$src" ]; then
        [ -f "$src" ] || die "找不到 $src"
        local sha; sha=$(sha256sum "$src" | cut -d' ' -f1)
        echo "本地 $(basename "$src"):"
        printf '    %s\n' "$sha"
        if [ "$live" = "$sha" ]; then
            ok "一致 —— 跑的就是这个文件"
        else
            bad "不一致 —— 设备上跑的不是这个文件"
            return 1
        fi
    fi
}

cmd_status() {
    need_device
    step "服务状态"
    printf '    init.svc.%s = %s\n' "$SVC" "$(sh_ "getprop init.svc.$SVC" | tr -d '\r')"
    printf '    pidof       = %s\n' "$(sh_ "pidof $SVC" | tr -d '\r')"
    echo
    sh_ "ls -la $DROOT 2>/dev/null" | tr -d '\r' | sed 's/^/    /'
    echo
    step "壳日志尾部"
    sh_ "tail -12 $DROOT/launcher.log 2>/dev/null" | tr -d '\r' | sed 's/^/    /'
}

case "${1:-}" in
    push)     shift; cmd_push "$@" ;;
    switch)   shift; cmd_switch "$@" ;;
    rollback) cmd_rollback ;;
    list)     cmd_list ;;
    verify)   shift; cmd_verify "$@" ;;
    status)   cmd_status ;;
    *)  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
