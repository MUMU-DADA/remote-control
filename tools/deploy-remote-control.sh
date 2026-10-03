#!/usr/bin/env bash
# =============================================================================
# deploy-remote-control.sh —— 模拟器每次重启后重新部署 remote-control
#
# 为什么需要它：当前模拟器的 /data **每次重启都是出厂状态**，
# 所以 /data/local/tmp/remote-control 每次都得重推一遍。
# 这不是"应该这样"，是绕过 —— 根因还没查到，见脚本末尾的「已知问题」。
#
# 用法:
#   bash tools/deploy-remote-control.sh                 # 默认 emulator-5580
#   bash tools/deploy-remote-control.sh emulator-5584
# =============================================================================
set -uo pipefail

SERIAL=${1:-emulator-5580}
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/aosp/out/target/product/emulator64_x86_64/system/bin"
ADB=${ADB:-/usr/bin/adb}
TMO=${TMO:-25}

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; }
warn() { printf '\033[1;33m  ! %s\033[0m\n' "$*"; }

# ⚠️ 每条 adb 都加超时。踩过两次挂死：
#    · adb root 之后 adbd 重启，后续调用会阻塞
#    · `adb shell "setsid ... &"` 后台进程挂着 adb 的管道，永不返回
sh_()  { timeout "$TMO" "$ADB" -s "$SERIAL" shell "$@"; }

step "检查前置"
[ -x "$BIN/remote-control" ] || { bad "找不到 $BIN/remote-control，先跑 tools/build-remote-control.sh"; exit 1; }
ok "二进制存在: $(stat -c%s "$BIN/remote-control") 字节"
timeout "$TMO" "$ADB" -s "$SERIAL" get-state >/dev/null 2>&1 \
    || { bad "$SERIAL 未连接"; exit 1; }
ok "$SERIAL 已连接"

step "等开机完成"
booted=""
for _ in $(seq 1 30); do
    booted=$(sh_ getprop sys.boot_completed 2>/dev/null | tr -d '\r')
    [ "$booted" = "1" ] && { ok "已开机"; break; }
    sleep 2
done
[ "$booted" = "1" ] || { bad "等不到开机完成"; exit 1; }

step "提权 + 关 SELinux"
# ⚠️ 关 SELinux 只是开发期方便 —— 生产形态必须走 init 起的 remote-control.rc
#    加专属 SELinux domain，见 dev/04-android-rom/device/remote_control_x64_arm64/sepolicy/。
timeout "$TMO" "$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
sleep 4
sh_ setenforce 0 2>/dev/null || true
ok "已 root / SELinux permissive"

step "推送"
timeout "$TMO" "$ADB" -s "$SERIAL" push "$BIN/remote-control"    /data/local/tmp/remote-control    >/dev/null \
    || { bad "推送 remote-control 失败"; exit 1; }
timeout "$TMO" "$ADB" -s "$SERIAL" push "$BIN/rcctl" /data/local/tmp/rcctl >/dev/null \
    || warn "推送 rcctl 失败（不影响服务）"
sh_ "chmod 755 /data/local/tmp/remote-control /data/local/tmp/rcctl" >/dev/null 2>&1
ok "已推送"

step "启动"
# ⚠️ 按**参数形状**杀，不要按二进制名杀。
#
#    改名那次实测踩到：这里原来写的是 pkill -f 'remote-control --socket'，
#    而当时还在跑的老进程叫 autod —— 匹配不到，它继续占着 8088；
#    新进程绑不上端口就退了。而"验证"只看端口有没有人应答，
#    于是老进程让验证**假通过**，看起来一切正常。
#
#    守护进程永远带 `socket /data/local/tmp/xxx.sock`，这个形状和它叫什么无关，
#    所以升级/改名都不会再漏杀。
#
#    ⚠️ 模式**不能以 `-` 开头**：Android 上是 toybox 的 pkill，它不认 `--`
#       这个选项终止符，写成 `pkill -f -- '--socket ...'` 会静默不匹配
#       （实测：旧进程照样活着占着 8088，新进程绑不上端口就退了）。
#       所以从中间一段开始匹配。
sh_ "pkill -f 'socket /data/local/tmp/'" >/dev/null 2>&1 || true
sleep 2

# 启动脚本在**本地**生成再 push —— 不在 adb shell 里拼多行命令，
# 引号层数一多 remote shell 就会等输入，把整条 adb 挂死。
TMPD=$(mktemp -d)
cat > "$TMPD/start-remote-control.sh" <<'SH'
#!/system/bin/sh
exec /data/local/tmp/remote-control --socket /data/local/tmp/remote-control.sock --http-bind 0.0.0.0 --foreground
SH
timeout "$TMO" "$ADB" -s "$SERIAL" push "$TMPD/start-remote-control.sh" \
        /data/local/tmp/start-remote-control.sh >/dev/null
rm -rf "$TMPD"
sh_ "chmod 755 /data/local/tmp/start-remote-control.sh" >/dev/null 2>&1

# 这条**预期**会超时（见 sh_ 上面的说明），加 || true 别让它中断脚本
timeout 12 "$ADB" -s "$SERIAL" shell \
    "setsid /data/local/tmp/start-remote-control.sh </dev/null >/dev/null 2>&1 &" >/dev/null 2>&1 || true
sleep 7

step "验证"
# ⚠️ 先确认**跑的是刚推上去的那个进程**，再看接口。
#    只看端口有没有应答是不够的 —— 任何还活着的老进程都能让它"通过"。
pid=$(sh_ "pidof remote-control" 2>/dev/null | tr -d '\r')
if [ -z "$pid" ]; then
    bad "remote-control 没在跑（八成没绑上端口 —— 端口被占？）"
    echo "  日志尾部："
    sh_ "tail -5 /sdcard/remote-control.log 2>/dev/null" 2>/dev/null | sed 's/^/    /' || true
    echo "  还占着 8088 的进程："
    sh_ "netstat -tlnp 2>/dev/null | grep 8088" 2>/dev/null | sed 's/^/    /' || true
    exit 1
fi
ok "进程在跑（pid $pid）"

ip=$(sh_ "ip -4 addr show eth0 2>/dev/null | grep -oE 'inet [0-9.]+' | cut -d' ' -f2" 2>/dev/null | tr -d '\r')
ip=${ip:-127.0.0.1}
info=$(curl -s --max-time 8 "http://$ip:8088/api/v1/info" 2>/dev/null || true)
if [ -n "$info" ]; then
    echo "$info" | python3 -c "
import json,sys
d=json.load(sys.stdin)
tw, th = d['touchWidth'], d['touchHeight']
pw, ph = d['primaryWidth'], d['primaryHeight']
print(f'  显示 {pw}x{ph}   触控 {tw}x{th}')
print('  ✓ 显示尺寸和触控范围一致' if (tw,th)==(pw,ph)
      else f'  ! 两者不一致 —— 触控会偏（见 docs/05-design-notes.md）')"
    ok "服务就绪"
    printf '\n\033[1;32m  控制台: http://%s:8088/\033[0m\n' "$ip"
else
    warn "拿不到 /api/v1/info"
    echo "  设备 IP: $ip"
    echo "  手动看一下: adb -s $SERIAL shell logcat -d -s remote-control | tail"
fi

cat <<'EOF'

───────────────────────────────────────────────────────────────
关于模拟器 /data 的持久性（根因已查明）

  早年记的"模拟器 /data 不持久"其实是两件事叠在一起，现在都清楚了：

  1. **run-linux.sh 不带 --reuse 会 rm -rf 工作目录** —— 那是设计如此
     （"每次都是一台全新机器"），不是 bug。日常开关机请用
     dev/04-android-rom/scripts/emulator.sh，它从不删工作目录。

  2. **`adb emu kill` 是硬断电，不是优雅关机** —— 它让 QEMU 立刻终止，
     guest 没机会卸载文件系统。实测（同一台实例三组对照，
     见 dev/04-android-rom/tools/verify-kill-is-hard-poweroff.sh）：

       写入后 sync 再关      → 重启后文件在
       写入后不 sync 直接关  → 重启后文件没了
       写入后等 15 秒再关    → 还是没了（guest 回写比想象中懒）

     所以现在的 stop/kill 都会**先 sync 再 kill**。手动关机的话，
     记得先 `adb -s <序列号> shell sync`。

  · run-linux.sh **不加 --reuse** 会 rm -rf 工作目录
    （脚本注释：「每次都是一台全新机器」，是设计如此）
  · 但**加了 --reuse 数据还是丢**：
    写入都进 userdata-qemu.img.qcow2，那个覆盖层在磁盘上**是保留的**
    （1.5GB 且还在长），可 guest 看不到里面的内容；
    裸镜像 userdata-qemu.img（48G）用 debugfs 离线看过，只有出厂内容，
    时间戳还是创建时的。logcat 里没有 /data 被格式化的记录。
  · 所以每次重启都要重跑本脚本。
EOF
