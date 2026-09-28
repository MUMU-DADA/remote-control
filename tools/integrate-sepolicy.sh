#!/usr/bin/env bash
# =============================================================================
# integrate-sepolicy.sh —— 接入并校验 remote-control 的 SELinux 策略
#
# 策略的**唯一真源在设备树**：
#
#     dev/04-x64-android/device/autosnap_x64_arm64/sepolicy/
#       ├── remote_control.te             主服务域
#       ├── remote_control_controller.te  控制器应用域
#       └── file_contexts                 文件标签
#
# 生效路径：
#     apply-overlay.sh 把它同步到 aosp/device/autosnap/autosnap_x64_arm64/sepolicy/
#     BoardConfig.mk 的 BOARD_SEPOLICY_DIRS 引用上面那个路径
#
# ⚠️⚠️ **不要放 system/sepolicy/private/**（这个脚本原来就是往那拷的）。
#
#    往平台策略树里加文件会让 AOSP 的 sepolicy_freeze_test **挂掉**：
#    它 diff 当前树与 prebuilts/api/31.0/，
#        Only in system/sepolicy/private: remote_control.te
#    于是 ninja 就停在那里，整个 ROM 编不出来。
#
#    设备/产品自己的策略本来就该走 BOARD_SEPOLICY_DIRS ——
#    同一份 BoardConfig 里 goldfish 的 x86 策略就是这么接的。
#    官方策略树只放平台的东西，别往里塞私货。
#
# 用法:
#   bash tools/integrate-sepolicy.sh           # 同步设备树 + 编译验证
#   bash tools/integrate-sepolicy.sh --check   # 只编译验证（不改任何文件）
# =============================================================================
set -uo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TE_SRC="$PROJECT_DIR/dev/04-x64-android/device/autosnap_x64_arm64/sepolicy"
AOSP="$PROJECT_DIR/aosp"
CONTAINER=${CONTAINER:-remote-control-builder}
TARGET=${TARGET:-autosnap_x64_arm64-userdebug}

TE_FILES=(remote_control.te remote_control_controller.te)

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; }
warn() { printf '\033[1;33m  ! %s\033[0m\n' "$*"; }

MODE=apply
case "${1:-}" in
    --check)  MODE=check ;;
    -h|--help) sed -n '2,38p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
esac

# -----------------------------------------------------------------------------
step "前置检查"
for te in "${TE_FILES[@]}"; do
    [ -f "$TE_SRC/$te" ] || { bad "缺 $TE_SRC/$te"; exit 1; }
done
[ -f "$TE_SRC/file_contexts" ] || { bad "缺 $TE_SRC/file_contexts"; exit 1; }
[ -d "$AOSP/system/sepolicy/private" ] || { bad "system/sepolicy 未同步"; exit 1; }
ok "策略源在设备树：${TE_SRC#"$PROJECT_DIR"/}"

# 顺带确认一条最容易犯的错：平台策略树里不许有我们的文件
if ls "$AOSP/system/sepolicy/private/" 2>/dev/null | grep -q "^remote_control"; then
    bad "平台策略树里还有我们的 .te —— sepolicy_freeze_test 会挂"
    echo "  删掉： rm -f $AOSP/system/sepolicy/private/remote_control*.te"
    exit 1
fi
ok "平台策略树是干净的（没有往里塞东西）"

# -----------------------------------------------------------------------------
if [ "$MODE" = "apply" ]; then
    step "同步设备树进 AOSP"
    bash "$PROJECT_DIR/dev/04-x64-android/scripts/apply-overlay.sh" 2>&1 | tail -4
    ok "已同步（BoardConfig 里的 BOARD_SEPOLICY_DIRS 指向它）"
fi

# -----------------------------------------------------------------------------
step "编译 sepolicy 验证"
# -----------------------------------------------------------------------------
# 这一步会暴露两类问题 —— 两类都真踩过：
#   · 语法错误   例：`allow init x:{ sock_file create unlink };`
#                —— `X:{ ... }` 是**类**的列表，权限不能写进去
#   · neverallow 违反   AOSP 的全局约束，必须逐条调和
if ! docker inspect -f '{{.State.Running}}' "$CONTAINER" 2>/dev/null | grep -q true; then
    bad "容器 $CONTAINER 未运行"
    exit 1
fi

if bash "$PROJECT_DIR/tools/check-no-build.sh" >/dev/null 2>&1; then
    warn "容器里已有编译在跑，跳过 sepolicy 编译验证"
    echo "  稍后单独跑：bash tools/integrate-sepolicy.sh --check"
    exit 0
fi

set +e
docker exec "$CONTAINER" bash -lc "
    cd /aosp
    source build/envsetup.sh >/dev/null 2>&1
    lunch $TARGET >/dev/null 2>&1
    m -j8 selinux_policy
" 2>&1 | tail -40
RC=${PIPESTATUS[0]}
set -e

if [ "$RC" -ne 0 ]; then
    bad "sepolicy 编译失败"
    cat <<'EOF'

─── 常见原因 ───
  • "neverallow ... violated"
      → AOSP 有全局 neverallow 约束。报错会给两条冲突的规则，
        通常需要给你自己的规则加例外，或用更窄的权限集。
        不要为了让 it 通过而放宽别的 domain。

  • "syntax error ... at token ';'"
      → 看它指的行。最常见的两个：
          `allow A B:{ 类 权限... };`   权限不能写在 { } 里，那是**类**的列表
          `allow A B:类 权限;`          权限要加 { }

  • "Only in system/sepolicy/private: ..."
      → 你往平台策略树里放文件了，见本文件开头那段。挪回设备树。

  • "unknown type xxx"
      → 类型名在这个 AOSP 版本里变了，对照 system/sepolicy 重新确认。
        remote_control.te 里每条规则的来源都写在文件头注释里。

  • "duplicate type declaration"
      → 同一个 type 声明了两次，或与已有 domain 重名。

─── 定位手段 ───
  在 remote_control.te 里放开 `permissive remote_control;`，init 会打印所有本来会被拒绝的
  操作而不真的拦，可以一次性收集全部 avc 报错：
    adb shell dmesg | grep 'avc:.*remote_control' > avc.log
    prebuilts/build-tools/linux-x86/bin/audit2allow -i avc.log
  ⚠️ permissive 会导致 CTS 失败，定位完必须删掉。
EOF
    exit 1
fi

ok "sepolicy 编译通过（语法 + neverallow 都干净）"
echo
echo "  策略在设备树： ${TE_SRC#"$PROJECT_DIR"/}/"
echo "  文件标签（装了 .te 之后由 BoardConfig 的 BOARD_SEPOLICY_DIRS 生效）："
grep -vE '^\s*#|^\s*$' "$TE_SRC/file_contexts" | sed 's/^/    /'
