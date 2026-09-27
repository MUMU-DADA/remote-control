#!/usr/bin/env bash
# =============================================================================
# integrate-sepolicy.sh —— 把 autod 的 SELinux 策略接入 AOSP 树
#
# 阶段 2 用。阶段 1（以 root 跑在 /data/local/tmp）不需要 sepolicy。
#
# 做什么：
#   1. daemon/autod.te        → system/sepolicy/private/autod.te
#   2. sepolicy/file_contexts → 追加到 system/sepolicy/private/file_contexts
#   3. 可选：编 sepolicy 验证语法
#
# 用法:
#   bash tools/integrate-sepolicy.sh           # 接入 + 编译验证
#   bash tools/integrate-sepolicy.sh --check   # 只编 sepolicy，不改文件
#   bash tools/integrate-sepolicy.sh --revert  # 撤销接入
# =============================================================================
set -euo pipefail

PROJECT_DIR=/root/AutoSnapshotAndroid
SRC="$PROJECT_DIR/dev/02-native-daemon/sepolicy"
AOSP="$PROJECT_DIR/aosp"
CONTAINER=${CONTAINER:-autod-builder}
TARGET=${TARGET:-aosp_cf_x86_64_phone-userdebug}

TE_DST="$AOSP/system/sepolicy/private/autod.te"
FC_DST="$AOSP/system/sepolicy/private/file_contexts"
FC_MARK="# === autod (AutoSnapshotAndroid) ==="

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; }
warn() { printf '\033[1;33m  ! %s\033[0m\n' "$*"; }

MODE=apply
case "${1:-}" in
    --check)  MODE=check ;;
    --revert) MODE=revert ;;
esac

# -----------------------------------------------------------------------------
if [ "$MODE" = "revert" ]; then
    step "撤销"
    rm -f "$TE_DST" && ok "已删除 $TE_DST" || warn "删除失败"
    if grep -q "$FC_MARK" "$FC_DST" 2>/dev/null; then
        # 删掉标记行到下一个空行/文件尾之间的内容
        python3 - "$FC_DST" "$FC_MARK" <<'PYEOF'
import sys
path, mark = sys.argv[1], sys.argv[2]
lines = open(path, encoding='utf-8').read().splitlines(keepends=True)
out, skip = [], False
for ln in lines:
    if ln.strip() == mark:
        skip = True
        continue
    if skip:
        if ln.strip() == '':
            skip = False
        continue
    out.append(ln)
open(path, 'w', encoding='utf-8').write(''.join(out))
print("  已从 file_contexts 移除 autod 条目")
PYEOF
    else
        warn "file_contexts 里没有 autod 标记，跳过"
    fi
    exit 0
fi

# -----------------------------------------------------------------------------
step "前置检查"
# -----------------------------------------------------------------------------
[ -f "$SRC/autod.te" ]        || { bad "缺 $SRC/autod.te"; exit 1; }
[ -f "$SRC/file_contexts" ]   || { bad "缺 $SRC/file_contexts"; exit 1; }
[ -d "$AOSP/system/sepolicy/private" ] || { bad "system/sepolicy 未同步"; exit 1; }
ok "源文件与 AOSP sepolicy 都在"

# -----------------------------------------------------------------------------
if [ "$MODE" = "apply" ]; then
    step "接入策略"
    cp "$SRC/autod.te" "$TE_DST"
    ok "autod.te → system/sepolicy/private/"

    if grep -q "$FC_MARK" "$FC_DST"; then
        warn "file_contexts 里已有 autod 标记，先撤销再接入"
        echo "  bash tools/integrate-sepolicy.sh --revert"
        exit 1
    fi
    {
        echo ""
        echo "$FC_MARK"
        grep -v '^#' "$SRC/file_contexts" | grep -v '^$' | sed 's/^[[:space:]]*//'
    } >> "$FC_DST"
    ok "file_contexts 已追加"
    echo
    echo "  新增的标签："
    grep -v '^#' "$SRC/file_contexts" | grep -v '^$' | sed 's/^/    /'
fi

# -----------------------------------------------------------------------------
step "编译 sepolicy 验证"
# -----------------------------------------------------------------------------
# 这一步会暴露两类问题：
#   - 语法错误
#   - neverallow 违反（AOSP 的全局约束，必须逐条调和）
if ! docker inspect -f '{{.State.Running}}' "$CONTAINER" 2>/dev/null | grep -q true; then
    bad "容器 $CONTAINER 未运行"
    exit 1
fi

if bash "$(dirname "$0")/check-no-build.sh"; then
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

  • "unknown type uhid_device" 之类
      → 类型名在目标版本里变了，对照 system/sepolicy 重新确认。
        autod.te 里的每条规则的来源都写在文件头注释里。

  • "duplicate type declaration"
      → 同一个 type 被声明了两次，检查是否与已有 domain 重名。

─── 定位手段 ───
  在 autod.te 里放开 `permissive autod;`，init 会打印所有本来会被拒绝的
  操作而不真的拦，可以一次性收集全部 avc 报错：
    adb shell dmesg | grep 'avc:.*autod' > avc.log
    prebuilts/build-tools/linux-x86/bin/audit2allow -i avc.log
  ⚠️ permissive 会导致 CTS 失败，定位完必须删掉。
EOF
    exit "$RC"
fi

step "完成"
ok "sepolicy 编译通过"
echo
echo "下一步："
echo "  bash tools/integrate-aosp.sh       # 确保 autod 源码在树里"
echo "  bash tools/build-cuttlefish.sh     # 或 bash tools/build-autod.sh"
