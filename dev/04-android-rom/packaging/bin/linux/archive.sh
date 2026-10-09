#!/usr/bin/env bash
# =============================================================================
# 实例归档：export / import / inspect 的**共享实现**。
#
# 为什么抽出来（2026-10-09）
# -------------------------
# 这个仓库里有两套管理器，**手工维护的两套实现**：
#
#     scripts/emulator.sh                    ← 开发侧
#     packaging/bin/linux/emulator.sh        ← 交付包里的（用户真正跑的）
#
# 归档功能原先只在开发侧有。2026-10-09 把恢复修好、验证通过（格式 2）之后，
# 用户手里的包还是拿不到它。可选做法是"照交付版的风格再写一份"——那就是
# 同一个功能两份实现，而**这个仓库已经为这件事吃过苦**（见 docs/12 §10 与
# tools/check-lineage-parity.py）。所以抽成这一份，两边都 source 它。
#
# 接口约定（刻意不用各家的辅助函数名）
# -----------------------------------
# 只依赖两样东西：
#   1. `log` / `warn` / `die` —— 两套管理器里**同名同义**，已经存在；
#   2. 路径**由调用方显式传进来**（sysdir / datadir / 端口 / 名字），
#      共享代码不去猜 `inst_sysdir` 还是 `sysdir_for_port`。
# 这样它不需要任何适配层。
#
# 归档格式（**这是两套实现之间的契约，改动要同步**）
# ------------------------------------------------
#   ./INSTANCE-MANIFEST.json    清单：format / dataFile / instance / 镜像列表 …
#   ./host-service.env          服务属性（端口/鉴权/…）
#   ./host-service.token        服务令牌（600）
#   ./sysdir/…  ./datadir/…     实例状态
#
#   format 1：数据镜像**原样搬**（backing + 覆盖层）。恢复出来 guest 读不到数据
#             —— 文件层面逐字节忠实，但起来之后 /data 是空的。**仍然拒绝恢复**
#             除非显式 --unsafe。原因未查明，见 docs/12 §7。
#   format 2：数据镜像压平成**一张自包含** qcow2（`dataFile`），恢复时转回 raw。
#             **已验证：数据完好。** 见 docs/12 §7。
# =============================================================================

ARCHIVE_MANIFEST="INSTANCE-MANIFEST.json"
# 归档里那份压平后的数据镜像（格式 2 起）
ARCHIVE_DATA_FLAT="userdata-flat.qcow2"

# 数据镜像的两个名字（格式 1 原样搬的就是它们）
_ARCH_DATA_IMG="userdata-qemu.img"
_ARCH_DATA_OVL="userdata-qemu.img.qcow2"

# 路径/运行期相关、不进归档的文件（与开发侧 arch_exclude 保持一致）
archive_excluded() {
    case "$1" in
        hardware-qemu.ini|hardware-qemu.ini.lock|multiinstance.lock) return 0 ;;
        emu-launch-params.txt|bootcompleted.ini|version_num.cache) return 0 ;;
        *.qcow2.lock|config.ini) return 0 ;;
    esac
    return 1
}

# 找 qemu-img：$QEMU_IMG_BIN > 模拟器本体旁边 > 常见位置
archive_qemu_img() {
    if [ -n "${QEMU_IMG_BIN:-}" ] && [ -x "$QEMU_IMG_BIN" ]; then
        printf '%s' "$QEMU_IMG_BIN"; return 0
    fi
    local c
    for c in "${EMULATOR_BIN%/*}/qemu-img" \
             /opt/android/emulator-new/emulator/qemu-img; do
        [ -n "$c" ] && [ -x "$c" ] && { printf '%s' "$c"; return 0; }
    done
    die "找不到 qemu-img（export QEMU_IMG_BIN=...）—— 导出/恢复数据镜像要用它"
}

# 从归档里取一个成员的内容（顶层或 ./ 前缀两种都试）
archive_member() {   # <归档> <成员名>
    tar -xOf "$1" "./$2" 2>/dev/null || tar -xOf "$1" "$2" 2>/dev/null || true
}

# 从 manifest 里取一个字符串字段
archive_field() {    # <manifest 正文> <字段名>
    printf '%s' "$1" | sed -n "s/.*\"$2\": *\"\\([^\"]*\\)\".*/\\1/p"
}

# ---------------------------------------------------------------------------
# archive_export <实例名> <sysdir> <datadir> <归档路径> [--force]
#
# 调用方负责：实例已经**停稳**（运行中导出会拿到不一致的镜像）、镜像符号链接的处理。
# 这里只做：暂存 → 压平数据 → 写清单 → 打包 → 读回确认。
# ---------------------------------------------------------------------------
archive_export() {
    local name="$1" sysdir="$2" datadir="$3" out="$4" force="${5:-}"
    [ -d "$sysdir" ] || die "工作目录不存在：$sysdir"
    if [ -e "$out" ] && [ "$force" != "--force" ]; then
        die "已存在：$out（加 --force 覆盖）"
    fi

    # 空间检查：归档 ≈ 实占（稀疏空洞不写）
    local need free_mb
    need=$(du -sm "$sysdir" "$datadir" 2>/dev/null | awk '{s+=$1} END{print s+0}')
    free_mb=$(df -Pm "$(dirname "$out")" | awk 'NR==2{print $4}')
    [ "$free_mb" -gt $((need + 512)) ] || \
        die "空间不够：归档预计 ${need}MB，目标分区只剩 ${free_mb}MB"

    local stage; stage="$(dirname "$out")/.archive-stage-$$"
    rm -rf "$stage"; mkdir -p "$stage/sysdir" "$stage/datadir"

    local f b n_state=0
    for f in "$sysdir"/*; do
        [ -e "$f" ] || continue
        [ -L "$f" ] && continue                  # 指向 ROM 镜像的符号链接：不导
        b="$(basename "$f")"
        archive_excluded "$b" && continue
        [ "$b" = "$_ARCH_DATA_IMG" ] && continue # 走压平路线
        [ "$b" = "$_ARCH_DATA_OVL" ] && continue
        ln "$f" "$stage/sysdir/$b" 2>/dev/null || cp -a "$f" "$stage/sysdir/$b"
        n_state=$((n_state + 1))
    done
    if [ -d "$datadir" ]; then
        for f in "$datadir"/*; do
            [ -e "$f" ] || continue
            b="$(basename "$f")"
            ln "$f" "$stage/datadir/$b" 2>/dev/null || cp -a "$f" "$stage/datadir/$b"
        done
    fi

    # 压平数据镜像：以覆盖层为入口（读穿 backing 链）；没有覆盖层就从 backing 压。
    local qi src="" have_data=0
    if [ -f "$sysdir/$_ARCH_DATA_OVL" ]; then src="$sysdir/$_ARCH_DATA_OVL"
    elif [ -f "$sysdir/$_ARCH_DATA_IMG" ]; then src="$sysdir/$_ARCH_DATA_IMG"; fi
    if [ -n "$src" ]; then
        qi="$(archive_qemu_img)"
        log "压平数据镜像（$ARCHIVE_DATA_FLAT）…"
        if ! "$qi" convert -O qcow2 -c "$src" "$stage/sysdir/$ARCHIVE_DATA_FLAT" 2>/dev/null; then
            rm -rf "$stage"; die "qemu-img convert 失败：$src"
        fi
        have_data=1
    else
        # 没有数据镜像不是错：从没启动过的实例、测试里的最小实例都这样。
        # 导出仍然有意义（服务令牌、配置、状态文件），只是归档里没有数据。
        warn "这个实例没有数据镜像（$_ARCH_DATA_IMG／$_ARCH_DATA_OVL 都不在）—— 归档里不会有数据"
    fi

    # 服务属性/令牌：如果调用方定义了 archive_extra_stage()，就在这里调它，
    # 把 $stage 摆好（各家取法不同：交付版与开发侧的服务元数据来源不一样）。
    # 不定义也合法 —— 归档里就没有那两样，import 侧能容忍。
    if declare -f archive_extra_stage >/dev/null 2>&1; then
        archive_extra_stage "$stage"
    fi

    # 调用方在调用前设置：
    #   ARCHIVE_PRODUCT    产品名（写进清单）
    #   ARCHIVE_ROM_FP     ROM 指纹（写进清单）
    #   ARCHIVE_IMAGES     镜像文件名列表（空格分隔；写进清单，恢复时按名字重新链本地那份）
    #   以及先把 $stage/host-service.env、$stage/host-service.token 摆好（若需要）

    local bytes ts
    # ⚠️ 用**实占**（--block-size=1 而不是 -b）：-b 是表观大小，
    #    会记成 66G，而 import 拿它比剩余空间 —— 要求 66G 空闲才能导入一份
    #    实占 1.9G 的归档。import 要的是"解出来要写多少"。
    bytes=$(du -s --block-size=1 "$stage" 2>/dev/null | awk '{print $1+0}')
    ts="$(date -Iseconds)"

    {
        printf '{\n'
        printf '  "format": 2,\n'
        [ "$have_data" = 1 ] && printf '  "dataFile": "%s",\n' "$ARCHIVE_DATA_FLAT"
        printf '  "instance": "%s",\n' "$name"
        printf '  "exportedAt": "%s",\n' "$ts"
        printf '  "product": "%s",\n' "${ARCHIVE_PRODUCT:-unknown}"
        printf '  "romFingerprint": "%s",\n' "${ARCHIVE_ROM_FP:-unknown}"
        printf '  "imageFiles": [%s],\n' "$(printf '%s' "${ARCHIVE_IMAGES:-}" | tr ' ' '\n' | grep -v '^$' | sed 's/.*/"&"/' | paste -sd, -)"
        printf '  "stateFiles": %s,\n' "$n_state"
        printf '  "dirBytes": %s\n' "$bytes"
        printf '}\n'
    } > "$stage/$ARCHIVE_MANIFEST"

    local zflag=""
    case "$out" in *.gz|*.tgz) zflag="-z" ;; esac
    log "打包（状态实占约 $((need / 1024))G；稀疏空洞不写）"
    if ! tar --sparse --numeric-owner $zflag -cf "$out" -C "$stage" . ; then
        rm -rf "$stage"; die "tar 失败"
    fi
    chmod 600 "$out"
    rm -rf "$stage"

    # 读回确认 —— tar 说成功不等于归档能用
    local listed
    listed="$(tar -tf "$out" 2>/dev/null | wc -l)"
    [ "$listed" -gt 0 ] || die "归档读不回来（tar -tf 是空的）：$out"
    tar -tf "$out" 2>/dev/null | grep -q "$ARCHIVE_MANIFEST" || die "归档里没有 $ARCHIVE_MANIFEST"

    ARCHIVE_LAST="$out"; ARCHIVE_LAST_ENTRIES="$listed"
    ARCHIVE_LAST_BYTES="$bytes"; ARCHIVE_LAST_HAS_DATA="$have_data"
    return 0
}

# ---------------------------------------------------------------------------
# archive_preflight <归档> [--unsafe]
#
# 只做"读清单 + 格式闸 + 空间检查"，**不落任何盘**。
# 调用方必须在 `instance_register` **之前**调它 —— 否则被拒的导入会留下
# 半个实例（注册了但没内容），而验证脚本正是在检查这一点。
# 结果放 ARCHIVE_FMT / ARCHIVE_MF / ARCHIVE_NEED_MB。
# ---------------------------------------------------------------------------
archive_preflight() {
    local f="$1" unsafe="${2:-}"
    [ -s "$f" ] || die "没有这个归档：$f"
    ARCHIVE_MF="$(archive_member "$f" "$ARCHIVE_MANIFEST")"
    [ -n "$ARCHIVE_MF" ] || die "归档里没有 $ARCHIVE_MANIFEST —— 不是本工具导出的？"
    ARCHIVE_FMT="$(printf '%s' "$ARCHIVE_MF" | sed -n 's/.*"format": *\([0-9]*\).*/\1/p')"
    ARCHIVE_FMT="${ARCHIVE_FMT:-1}"     # 没有 format 字段 = 老归档 = 当作 1
    if [ "$ARCHIVE_FMT" -lt 2 ] && [ "$unsafe" != "--unsafe" ]; then
        warn "这是**格式 1** 的老归档：那条路线恢复出来的实例 /data 是空的（归档本身没问题）"
        warn "详见 docs/12-emulator-control.md §7。确实要用加 --unsafe。"
        die "已拒绝执行（这是保护，不是故障）"
    fi
    [ "$ARCHIVE_FMT" -lt 2 ] && warn "以 --unsafe 运行格式 1 的归档：恢复出来的机器可能没有原来的数据"
    ARCHIVE_NEED_MB="$(printf '%s' "$ARCHIVE_MF" | sed -n 's/.*"dirBytes": *\([0-9]*\).*/\1/p')"
    ARCHIVE_NEED_MB=$(( ${ARCHIVE_NEED_MB:-0} / 1048576 ))
    return 0
}

# ---------------------------------------------------------------------------
# archive_import <归档> <sysdir> <datadir> <名字> [--unsafe]
#
# 调用方负责：名字不重名、端口已选好并注册、服务属性/令牌的落盘。
# 这里只做：读清单 → 按格式闸 → 解包 → 还原数据 → 清掉运行期文件。
# ---------------------------------------------------------------------------
archive_import() {
    local f="$1" sysdir="$2" datadir="$3" name="$4" unsafe="${5:-}"
    # 闸与空间检查走 preflight（调用方本该在注册实例前先调它；这里再调一次是幂等的）
    archive_preflight "$f" "$unsafe"
    local mf="$ARCHIVE_MF" fmt="$ARCHIVE_FMT" need_mb="$ARCHIVE_NEED_MB"
    local free_mb
    free_mb=$(df -Pm "$(dirname "$sysdir")" | awk 'NR==2{print $4}')
    [ "$free_mb" -gt $((need_mb + 512)) ] || \
        die "空间不够：需要约 ${need_mb}MB，只剩 ${free_mb}MB"

    mkdir -p "$sysdir" "$datadir"

    # 解包：把 ./sysdir 与 ./datadir 直接落到目标位置（不经过中间目录，
    # 免得 64GiB 的稀疏文件被 cp 一遍 —— cp -a 会保稀疏，但白费一遍 IO）。
    local port; port="$(basename "$sysdir" | sed 's/^sysdir-//')"
    log "解包 → $(basename "$sysdir")（约 $((need_mb/1024))G）"
    if ! tar --sparse --numeric-owner -xf "$f" -C "$(dirname "$sysdir")" \
             --exclude="./host-service.env" --exclude="./host-service.token" \
             --transform "s,^\\./sysdir,sysdir-$port,; s,^\\./datadir,datadir-$port," \
             2>/dev/null; then
        # --transform 不可用（非 GNU tar）时退回两步走
        local tmp="$(dirname "$sysdir")/.import-tmp-$$"
        rm -rf "$tmp"; mkdir -p "$tmp"
        tar -xf "$f" -C "$tmp"
        [ -d "$tmp/sysdir" ] && cp -a "$tmp/sysdir/." "$sysdir/"
        [ -d "$tmp/datadir" ] && cp -a "$tmp/datadir/." "$datadir/"
        rm -rf "$tmp"
    fi
    rm -f "$sysdir/$ARCHIVE_MANIFEST" 2>/dev/null

    # ── 格式 2：把压平镜像转回 raw 当 backing（探针验证过的路线）──────────
    local dfile; dfile="$(printf '%s' "$mf" | sed -n 's/.*"dataFile": *"\([^"]*\)".*/\1/p')"
    dfile="${dfile:-$ARCHIVE_DATA_FLAT}"
    local has_dfile=0
    printf '%s' "$mf" | grep -q '"dataFile"' && has_dfile=1
    if [ "$fmt" -ge 2 ] && [ "$has_dfile" = 1 ]; then
        [ -f "$sysdir/$dfile" ] || die "归档是格式 2，但里面没有 $dfile —— 归档不完整？"
        log "还原数据镜像（$dfile → $_ARCH_DATA_IMG）…"
        rm -f "$sysdir/$_ARCH_DATA_IMG" "$sysdir/$_ARCH_DATA_OVL"
        "$(archive_qemu_img)" convert -O raw "$sysdir/$dfile" "$sysdir/$_ARCH_DATA_IMG" 2>/dev/null \
            || die "qemu-img convert 失败：$sysdir/$dfile"
        rm -f "$sysdir/$dfile"
    fi

    # 清掉运行期/路径相关的东西（config.ini 由调用方按本地真源重写）
    rm -f "$sysdir/hardware-qemu.ini" "$sysdir/hardware-qemu.ini.lock" \
          "$sysdir/multiinstance.lock" "$sysdir/emu-launch-params.txt" \
          "$sysdir/bootcompleted.ini" "$sysdir/version_num.cache" "$sysdir"/*.qcow2.lock 2>/dev/null

    if [ ! -s "$sysdir/$_ARCH_DATA_IMG" ]; then
        [ "$fmt" -ge 2 ] && [ "$has_dfile" = 1 ] && \
            die "格式 2 的归档恢复后没有 $_ARCH_DATA_IMG —— 不该发生"
        warn "归档里没有数据镜像（导出时实例就没有）"
    fi

    ARCHIVE_FMT="$fmt"
    ARCHIVE_NEED_MB="$need_mb"
    # 给调用方：按这份名单重新链本地 ROM 里的同名文件
    ARCHIVE_IMAGES="$(printf '%s' "$mf" | sed -n 's/.*"imageFiles": *\[\([^]]*\)\].*/\1/p' \
                     | tr -d '"' | tr ',' ' ')"
    return 0
}
