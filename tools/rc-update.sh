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
#   bash tools/rc-update.sh rollback             切回**上一版**（previous）+ 重启服务
#   bash tools/rc-update.sh clear [--purge]      清指针，回到镜像自带的版本
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

# 访问令牌。交付镜像**默认开鉴权**，而不带令牌的 /api/v1/info 只会拿到 401
# —— buildId 取不到，等待循环就永远判"没换过来"（实测：switch 明明成功了，
# 壳日志已经在跑新槽，脚本却报"buildId 还是 …"）。
# 优先用环境变量，其次从设备上的配置读（需要 adb root）。
RC_TOKEN=${RC_TOKEN:-}
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

    # ⚠️ **必须重新打标签**，否则这一版根本跑不起来。
    #
    #    rename 只换目录项，**标签跟着 inode 走** —— 从 /data/local/tmp 搬过来的
    #    文件带的是 shell_data_file，而壳跑在 remote_control_loader 域，
    #    连 getattr 都不允许：
    #        avc: denied { getattr } … scontext=u:r:remote_control_loader:s0
    #                                     tcontext=u:object_r:shell_data_file:s0
    #    壳的做法是"读不到就判这份载荷不可用 → current 回退到镜像自带版本 → 试下一个
    #    候选"，于是 switch 看起来"成功"（指针确实切了、服务确实重启了），
    #    实际跑的**还是旧的镜像版本** —— 只有翻 launcher.log 才看得到。
    #    实测就是这么卡住的。
    #
    #    用 restorecon（按 file_contexts 走）而不是 chcon 写死标签：
    #    标签属于策略，不该在脚本里抄一份。
    sh_ "restorecon $DROOT/releases/$sha/remote-control" >/dev/null 2>&1 || true
    # 注意别按列号取：ls -laZ 的列会随实现/有无 ACL 变，取到时间戳就闹笑话。
    local lbl; lbl=$(sh_ "ls -laZ $DROOT/releases/$sha/remote-control 2>/dev/null | grep -o 'u:object_r:[^ ]*' | head -1" | tr -d '\r')
    case "$lbl" in
        *remote_control_data_file*) : ;;
        *) die "载荷标签不对（$lbl）—— 壳会读不到它并回退到镜像版本；检查 file_contexts 里 $DROOT 的条目" ;;
    esac
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
    # ⚠️ 取 **previous**，不是 last-good。
    #
    #    launcher 自己那段注释说得最清楚（launcher.cpp:466）：
    #      "启动新提交版本时 last-good 会先被更新，只有 previous 才能保留
    #       紧邻的上一版"
    #    所以"刚 switch 过去、发现新版不对、想退回上一版"这个最常见的用法，
    #    用 last-good 会**退回到刚切过去的那一版**，等于什么都没干 ——
    #    实测：switch 到 3a80cfa6… 之后 rollback，又回到了 3a80cfa6…。
    #    launcher 自己的回退顺序就是 previous → last-good → 镜像原版，
    #    这里跟它保持一致。
    local cur good
    cur=$(sh_ "cat $DROOT/current 2>/dev/null" | tr -d '\r')
    good=$(sh_ "cat $DROOT/previous 2>/dev/null" | tr -d '\r')
    if [ -z "$good" ] || [ "$good" = "$cur" ]; then
        good=$(sh_ "cat $DROOT/last-good 2>/dev/null" | tr -d '\r')
    fi
    [ -n "$good" ] || die "没有 previous / last-good —— 服务还没成功跑起来过，没有可回退的版本"
    if [ "$good" = "$cur" ]; then
        die "可回退的版本就是当前这一版（$good）—— 没有更早的版本可退"
    fi
    [ "$(sh_ "test -x $DROOT/$good && echo yes" | tr -d '\r')" = "yes" ] \
        || die "要回退的版本不在了：$good"

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
# 取设备上正在跑的 buildId。
#
# 交付镜像默认开鉴权，不带令牌只会拿到 401 —— 于是"等待换版本"永远判失败
# （实测：switch 明明成功了，壳日志已经在跑新槽，脚本却报"buildId 还是 …"）。
# 令牌优先取环境变量，其次从设备配置里读（需要 adb root）。
device_build_id() {
    local tok="$RC_TOKEN"
    if [ -z "$tok" ]; then
        tok=$(sh_ "grep -m1 '^token=' $DROOT/remote-control.conf 2>/dev/null | cut -d= -f2-" | tr -d '\r')
    fi
    curl -s --max-time 5 ${tok:+-H "Authorization: Bearer $tok"} \
        "http://$RC_HOST:$RC_PORT/api/v1/info" 2>/dev/null \
        | python3 -c 'import json,sys; print(json.load(sys.stdin).get("buildId",""))' 2>/dev/null
}

restart_service() {
    local want_sha="$1"
    step "重启服务"
    if [ "$(sh_ "getprop init.svc.$SVC" | tr -d '\r')" != "running" ]; then
        warn "init.svc.$SVC 不是 running —— 说明服务不是 init 拉起来的（开发期手工跑的？）"
        warn "ctl.restart 不会有任何效果。生产形态请走 remote-control.rc。"
    fi
    sh_ "setprop ctl.restart $SVC" >/dev/null 2>&1 || true

    step "等服务换过来"
    local got="" http_ok=0
    for i in $(seq 1 20); do
        got=$(device_build_id)
        [ -n "$got" ] && http_ok=1
        [ "$got" = "$want_sha" ] && break
        sleep 1
    done

    if [ "$got" = "$want_sha" ]; then
        ok "已在跑新版本（buildId ${got:0:12}…，约 $((i)) 秒）"
    elif [ "$http_ok" = 0 ]; then
        # ⚠️ 区分"连不上"和"版本不对"。以前两种情况都只报
        #    "buildId 还是 …（期望 …）"，看起来像版本没换上去 ——
        #    而实际常常是**根本没连上**（RC_HOST 默认是文档示例里的
        #    192.168.0.110，模拟器走 adb forward 时是 127.0.0.1）。
        #    实测：switch 明明成功了（壳日志已经在跑新槽、/info 的
        #    buildId 就是新 sha），脚本却报"buildId 还是 …"。
        bad "HTTP 连不上 http://$RC_HOST:$RC_PORT —— 无法确认版本，但**不代表没换成功**"
        echo "  用对的地址再确认： RC_HOST=<主机> RC_PORT=<端口> $0 verify"
        echo "  （模拟器经 adb forward 时一般是 RC_HOST=127.0.0.1 RC_PORT=18088）"
        echo "  看壳日志：   adb -s $SERIAL shell cat $DROOT/launcher.log"
        echo "  看服务日志： adb -s $SERIAL shell tail -20 $DROOT/remote-control.log"
        echo "  看壳日志：   adb -s $SERIAL shell cat $DROOT/launcher.log"
        return 1
    fi
}

# 清掉指针，回到**镜像自带**的版本。
#
# 为什么必须有这个命令：热替换槽在 /data 里，而 `/data` 不随重装 ROM 重置 ——
# 于是"用过一次 rc-update 之后，再重编重装 ROM 却发现什么都没变"。
# 壳是按 `current` 优先选的，这是设计如此（你切过去就是要它跑），
# 但**没有一条路能明确地说"我不要槽了，回去跑镜像那份"**：
# rollback 只会退到 previous/last-good，两者都可能是槽里的版本。
#
#   用法:  rc-update.sh clear            # 只清指针（槽留着，随时能再切回来）
#          rc-update.sh clear --purge    # 连槽里的载荷一起删
cmd_clear() {
    need_device
    local purge=0
    [ "${1:-}" = "--purge" ] && purge=1

    step "清指针"
    local cur; cur=$(sh_ "cat $DROOT/current 2>/dev/null" | tr -d '\r')
    if [ -z "$cur" ]; then
        printf '    current 本来就是空的（跑的就是镜像自带的版本）\n' 
    else
        printf '    清掉 current=%s\n' "$cur"
    fi
    for f in current previous last-good; do
        sh_ "printf '' > $DROOT/$f" >/dev/null 2>&1 || true
    done
    ok "指针已清空 —— 下次拉起会用镜像自带的那份"

    if [ "$purge" = 1 ]; then
        step "清槽"
        sh_ "rm -rf $DROOT/releases" >/dev/null 2>&1 || true
        ok "槽已清空"
    else
        printf '    （槽还留着；要一起删： %s clear --purge）\n' "$0"
    fi

    step "重启服务"
    sh_ "setprop ctl.restart $SVC" >/dev/null 2>&1 || true
    # 这里**不能**像 switch 那样"等 buildId 变" —— 回到镜像版时 buildId
    # 本来就该和之前一样（可能还是同一个 sha）。等的是"服务重新能应答"。
    local live=""
    for i in $(seq 1 15); do
        live=$(device_build_id)
        [ -n "$live" ] && break
        sleep 1
    done
    if [ -n "$live" ]; then
        ok "服务已回来（buildId ${live:0:12}…）"
    else
        warn "没能确认服务状态（HTTP 连不上就查启动日志： $0 status）"
    fi
    printf '    壳日志里应当写着"镜像自带，不做哈希校验"：\n'
    printf '      adb -s %s shell tail -2 %s/launcher.log\n' "$SERIAL" "$DROOT"
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
    local live; live=$(device_build_id)
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
    clear)    shift; cmd_clear "$@" ;;
    list)     cmd_list ;;
    verify)   shift; cmd_verify "$@" ;;
    status)   cmd_status ;;
    *)  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
