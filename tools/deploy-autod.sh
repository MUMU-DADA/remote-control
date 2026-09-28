#!/usr/bin/env bash
# =============================================================================
# deploy-autod.sh —— 模拟器每次重启后重新部署 autod
#
# 为什么需要它：当前模拟器的 /data **每次重启都是出厂状态**，
# 所以 /data/local/tmp/autod 每次都得重推一遍。
# 这不是"应该这样"，是绕过 —— 根因还没查到，见脚本末尾的「已知问题」。
#
# 用法:
#   bash tools/deploy-autod.sh                 # 默认 emulator-5580
#   bash tools/deploy-autod.sh emulator-5584
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
[ -x "$BIN/autod" ] || { bad "找不到 $BIN/autod，先跑 tools/build-autod.sh"; exit 1; }
ok "二进制存在: $(stat -c%s "$BIN/autod") 字节"
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
# ⚠️ 关 SELinux 只是开发期方便 —— 生产形态必须走 init 起的 autod.rc
#    加专属 SELinux domain，见 dev/02-native-daemon/sepolicy/。
timeout "$TMO" "$ADB" -s "$SERIAL" root >/dev/null 2>&1 || true
sleep 4
sh_ setenforce 0 2>/dev/null || true
ok "已 root / SELinux permissive"

step "推送"
timeout "$TMO" "$ADB" -s "$SERIAL" push "$BIN/autod"    /data/local/tmp/autod    >/dev/null \
    || { bad "推送 autod 失败"; exit 1; }
timeout "$TMO" "$ADB" -s "$SERIAL" push "$BIN/autodctl" /data/local/tmp/autodctl >/dev/null \
    || warn "推送 autodctl 失败（不影响服务）"
sh_ "chmod 755 /data/local/tmp/autod /data/local/tmp/autodctl" >/dev/null 2>&1
ok "已推送"

step "启动"
sh_ "pkill -f 'autod --socket'" >/dev/null 2>&1 || true
sleep 2

# 启动脚本在**本地**生成再 push —— 不在 adb shell 里拼多行命令，
# 引号层数一多 remote shell 就会等输入，把整条 adb 挂死。
TMPD=$(mktemp -d)
cat > "$TMPD/start-autod.sh" <<'SH'
#!/system/bin/sh
exec /data/local/tmp/autod --socket /data/local/tmp/autod.sock --http-bind 0.0.0.0 --foreground
SH
timeout "$TMO" "$ADB" -s "$SERIAL" push "$TMPD/start-autod.sh" \
        /data/local/tmp/start-autod.sh >/dev/null
rm -rf "$TMPD"
sh_ "chmod 755 /data/local/tmp/start-autod.sh" >/dev/null 2>&1

# 这条**预期**会超时（见 sh_ 上面的说明），加 || true 别让它中断脚本
timeout 12 "$ADB" -s "$SERIAL" shell \
    "setsid /data/local/tmp/start-autod.sh </dev/null >/dev/null 2>&1 &" >/dev/null 2>&1 || true
sleep 7

step "验证"
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
    echo "  手动看一下: adb -s $SERIAL shell logcat -d -s autod | tail"
fi

cat <<'EOF'

───────────────────────────────────────────────────────────────
已知问题：模拟器 /data 不持久（根因未查明）

  · run-linux.sh **不加 --reuse** 会 rm -rf 工作目录
    （脚本注释：「每次都是一台全新机器」，是设计如此）
  · 但**加了 --reuse 数据还是丢**：
    写入都进 userdata-qemu.img.qcow2，那个覆盖层在磁盘上**是保留的**
    （1.5GB 且还在长），可 guest 看不到里面的内容；
    裸镜像 userdata-qemu.img（48G）用 debugfs 离线看过，只有出厂内容，
    时间戳还是创建时的。logcat 里没有 /data 被格式化的记录。
  · 所以每次重启都要重跑本脚本。
EOF
