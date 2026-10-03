#!/usr/bin/env bash
# fetch-images.sh —— 把 ROM 交付目录从构建机拉到这台 Mac。
#
#   ./fetch-images.sh                                   # 默认从 192.168.0.108 拉
#   ./fetch-images.sh --remote user@host --remote-dir /path/to/rom-xxx
#   ./fetch-images.sh --dest ./images --force
#   ./fetch-images.sh --local /Volumes/usb/rom-xxx       # 从已挂载的本地目录拷（不走网络）
#   ./fetch-images.sh --dequarantine                     # 拉完顺手去掉 quarantine 标记
#
# 算法与 windows/fetch-images.ps1 一致（比对远端大小 → scp → 按 SHA256SUMS 校验），
# 但把 Windows 版里几处**静默跳过**改成了**显式失败**（理由见下）。
#
# ⚠️ 必需的是 **-qemu 家族**：
#   system/vendor/product-qemu.img = 带 GPT 分区表的包装版（模拟器靠它做 /dev/block/by-name/*）
#   ramdisk-qemu.img = (cat ramdisk.img vendor_ramdisk.img) 合并版（first-stage 的 fstab 在里面）
#   只拉裸 ext4 的 *.img，模拟器会以 "partition(s) not found in /sys" /
#   "failed to find device default fstab" 卡死并无限重启（见 docs/02-build-traps.md 坑 8）。
#
# macOS 专有处理：
#   · sha256 用 shasum -a 256（没有 sha256sum）
#   · 本地目录拷贝用 ditto —— 它是 macOS 上唯一能把扩展属性/资源叉也带对、
#     且不会像 cp -R 那样在符号链接上出岔子的工具
#   · quarantine 只在**显式 --dequarantine** 时才清。默认只报告：
#     清 quarantine 等于告诉系统"这个来源我信"，不该由脚本替你决定
#     （来源可能是从网上下载的 zip）。
#
set -uo pipefail

REMOTE="root@192.168.0.108"
REMOTE_DIR="/root/AutoSnapshotAndroid/dev/04-android-rom/artifacts/rom-remote_control_x64_arm64"
DEST="$(cd "$(dirname "$0")" && pwd)/../images"
LOCAL_SRC=""
FORCE=0
SKIP_VERIFY=0
DEQ=0
PROBE_REMOTE="/root/AutoSnapshotAndroid/dev/04-android-rom/artifacts/arm64-probe.apk"

while [ $# -gt 0 ]; do
    case "$1" in
        --remote)      REMOTE="$2"; shift 2 ;;
        --remote-dir)  REMOTE_DIR="$2"; shift 2 ;;
        --dest)        DEST="$2"; shift 2 ;;
        --local)       LOCAL_SRC="$2"; shift 2 ;;
        --dequarantine) DEQ=1; shift ;;
        --force)       FORCE=1; shift ;;
        --skip-verify) SKIP_VERIFY=1; shift ;;
        -h|--help)     sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m  ! %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*" >&2; exit 1; }

REQUIRED="system-qemu.img vendor-qemu.img product-qemu.img ramdisk-qemu.img kernel-ranchu encryptionkey.img userdata.img advancedFeatures.ini config.ini"
OPTIONAL="system_ext-qemu.img initrd system.img vendor.img product.img ramdisk.img build.prop source.properties dtb.img vbmeta.img SHA256SUMS MANIFEST.txt"

sha256_of() { shasum -a 256 "$1" 2>/dev/null | awk '{print $1}'; }
size_of()   { stat -f%z "$1" 2>/dev/null || stat -c %s "$1" 2>/dev/null; }

mkdir -p "$DEST" "$DEST/system"

# ---------------------------------------------------------------------------
# 模式 A：从本地目录拷（U 盘 / 网络盘 / 已解压的 zip）
# ---------------------------------------------------------------------------
if [ -n "$LOCAL_SRC" ]; then
    [ -d "$LOCAL_SRC" ] || die "--local 目录不存在：$LOCAL_SRC"
    log "本地来源：$LOCAL_SRC → $DEST"
    # ditto 是 macOS 上带扩展属性/资源叉最稳的拷贝工具；但它**只存在于 macOS**，
    # 在别的系统上（例如开发机跑测试）要能回退，否则脚本在那里直接死。
    # 对镜像文件（.img/.bin）而言两者等价 —— 它们没有资源叉。
    if command -v ditto >/dev/null 2>&1; then
        ditto "$LOCAL_SRC" "$DEST" || die "ditto 失败"
        ok "已拷贝（ditto）"
    else
        cp -a "$LOCAL_SRC"/. "$DEST"/ || die "cp -a 失败"
        ok "已拷贝（cp -a；此系统没有 ditto）"
    fi
else
    # -----------------------------------------------------------------------
    # 模式 B：从构建机拉（scp + 比对大小）
    # -----------------------------------------------------------------------
    command -v ssh >/dev/null 2>&1 || die "需要 ssh"
    command -v scp >/dev/null 2>&1 || die "需要 scp"

    log "远端：$REMOTE:$REMOTE_DIR"
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$REMOTE" true 2>/dev/null \
        || die "连不上 $REMOTE（先确认免密登录：ssh $REMOTE true）"

    remote_size() {   # 远端文件大小；不存在则空
        ssh -o BatchMode=yes "$REMOTE" "stat -c %s '$REMOTE_DIR/$1' 2>/dev/null" 2>/dev/null | tr -d '\r\n'
    }

    TODO=""
    MISSING_REQ=""
    for f in $REQUIRED $OPTIONAL; do
        rs="$(remote_size "$f")"
        if [ -z "$rs" ]; then
            case " $REQUIRED " in *" $f "*) MISSING_REQ="$MISSING_REQ $f" ;; esac
            continue
        fi
        if [ -s "$DEST/$f" ] && [ "$FORCE" -eq 0 ] && [ "$rs" = "$(size_of "$DEST/$f")" ]; then
            printf '    skip %-24s %s 字节（大小一致）\n' "$f" "$rs"
            continue
        fi
        TODO="$TODO $f"
    done

    # ⚠️ 与 Windows 版不同：远端缺必需文件时**必须失败**，不能只打个 warning 就继续。
    #    之前那版会走到最后才提一句"缺少：…"，中间的解压/校验阶段可能已经给出更迷惑的报错。
    [ -z "$MISSING_REQ" ] || die "远端缺必需镜像：$MISSING_REQ
    先在构建机上跑： ./scripts/build-rom.sh && ./scripts/package-rom.sh"

    if [ -n "$TODO" ]; then
        n=$(printf '%s' "$TODO" | wc -w)
        log "拉取 $n 个文件（可能几 GB，耐心等）"
        # shellcheck disable=SC2086
        scp -q -o BatchMode=yes $(for f in $TODO; do printf '%s ' "$REMOTE:$REMOTE_DIR/$f"; done) "$DEST/" \
            || die "scp 失败"
        ok "拉取完成"
    else
        ok "本地已是最新（全部文件大小一致）"
    fi

    # system/build.prop：模拟器靠它识别 guest 架构，缺了会**退回宿主架构**（静默出错）
    if [ ! -s "$DEST/system/build.prop" ]; then
        log "补 system/build.prop"
        scp -q -o BatchMode=yes "$REMOTE:$REMOTE_DIR/system/build.prop" "$DEST/system/build.prop" \
            || die "拉 system/build.prop 失败 —— 缺它模拟器会按宿主架构布置机器"
    fi

    # 探针 APK（arm64 验收用）
    if [ ! -s "$DEST/arm64-probe.apk" ] || [ "$FORCE" -eq 1 ]; then
        src=""
        [ -n "$(remote_size arm64-probe.apk)" ] && src="$REMOTE_DIR/arm64-probe.apk"
        [ -z "$src" ] && [ -n "$(ssh -o BatchMode=yes "$REMOTE" "stat -c %s '$PROBE_REMOTE' 2>/dev/null" 2>/dev/null | tr -d '\r\n')" ] \
            && src="$PROBE_REMOTE"
        if [ -n "$src" ]; then
            log "拉自建 arm64 探针 APK"
            scp -q -o BatchMode=yes "$REMOTE:$src" "$DEST/" || warn "探针 APK 拉取失败（不影响启动）"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# initrd：模拟器 -initrd 指向它。
# ⚠️ QEMU 拿到**不存在**的 initrd 会"主循环立刻结束且不报错"——极难查（docs/02-build-traps.md）。
#    所以这里不满足于"文件在"，而是真的**读回确认非空**。
if [ ! -s "$DEST/initrd" ]; then
    if [ -s "$DEST/ramdisk-qemu.img" ]; then
        cp -f "$DEST/ramdisk-qemu.img" "$DEST/initrd" && ok "已由 ramdisk-qemu.img（合并版）生成 initrd"
    elif [ -s "$DEST/ramdisk.img" ]; then
        cp -f "$DEST/ramdisk.img" "$DEST/initrd" && warn "已由裸 ramdisk.img 生成 initrd（不是合并版，可能缺 first-stage fstab）"
    else
        die "既没有 initrd 也没有 ramdisk*.img —— 模拟器起来会静默退出"
    fi
fi
[ -s "$DEST/initrd" ] || die "initrd 是空的"
ok "initrd：$(size_of "$DEST/initrd") 字节"

# ---------------------------------------------------------------------------
# 校验
# ---------------------------------------------------------------------------
if [ "$SKIP_VERIFY" -eq 0 ] && [ -s "$DEST/SHA256SUMS" ]; then
    log "按 SHA256SUMS 校验"
    bad=0; checked=0
    while IFS= read -r line; do
        # 典型行： <64hex>  ./文件名
        want="$(printf '%s' "$line" | awk '{print $1}')"
        rel="$(printf '%s' "$line" | awk '{print $2}' | sed 's|^\./||')"
        [ "${#want}" -eq 64 ] || continue
        [ -n "$rel" ] || continue
        if [ ! -f "$DEST/$rel" ]; then warn "缺文件：$rel"; bad=$((bad+1)); continue; fi
        got="$(sha256_of "$DEST/$rel")"
        checked=$((checked+1))
        [ "$got" = "$want" ] || { warn "校验不符：$rel"; bad=$((bad+1)); }
    done < "$DEST/SHA256SUMS"
    if [ "$bad" -eq 0 ]; then ok "校验通过（$checked 个文件）"
    else die "校验失败：$bad 个文件有问题（重拉： $0 --force）"; fi
else
    [ "$SKIP_VERIFY" -eq 1 ] && warn "已按 --skip-verify 跳过校验"
fi

# ---------------------------------------------------------------------------
# quarantine：只报告，或在显式要求时清除
# ---------------------------------------------------------------------------
if [ "$(uname -s)" = "Darwin" ]; then
    _q="$(xattr -l "$DEST" 2>/dev/null | grep -c quarantine)" || _q=0
    if [ "${_q:-0}" != "0" ]; then
        if [ "$DEQ" -eq 1 ]; then
            log "清除 quarantine 标记（你显式要求了 --dequarantine）"
            xattr -dr com.apple.quarantine "$DEST" && ok "已清除"
        else
            warn "镜像带 com.apple.quarantine（来自下载的归档）—— 模拟器可能被 Gatekeeper 拦。
    确认来源可信后执行： xattr -dr com.apple.quarantine '$DEST'
    或重跑本脚本加 --dequarantine"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# 收尾：显式列出缺了什么
# ---------------------------------------------------------------------------
log "清单（$DEST）"
ls -la "$DEST" | awk 'NF>=9 {printf "    %-26s %s\n", $9, $5}'

MISSING=""
for f in $REQUIRED; do [ -s "$DEST/$f" ] || MISSING="$MISSING $f"; done
if [ -z "$MISSING" ]; then ok "必需文件齐全 ✓  可以起机器了"
else die "仍缺必需文件：$MISSING"; fi
