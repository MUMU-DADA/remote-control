#!/usr/bin/env bash
# 翻译层动态依赖自检：翻译器与 proxy 库所需的库/符号，在**我们自己的 system 里**是否满足。
#
#   ./check-bridge-symbols.sh                 # 用当前构建产物 out/.../system
#   ./check-bridge-symbols.sh /path/to/system # 或用别的 system 目录
#
# 为什么需要它：翻译层是从 Google 官方镜像搬来的，编它的框架版本可能比我们的树新/旧。
# 宿主侧 proxy 库（x86_64）会去链我们 system 里的 libbinder / libandroid_runtime 等，
# 一旦有符号对不上，表现是"应用 dlopen 失败 / 一启动就崩"，而不是构建报错。
# 这个脚本把这类问题提前到"构建后、启动前"暴露。
#
# 实现上踩过三个坑（都已处理，别改回去）：
#   1. readelf 会跟随 LANG 输出中文（"共享库：[libm.so]"）→ 全程 LC_ALL=C；
#   2. Android 的动态符号带版本后缀（__cxa_finalize@LIBC）→ 两边都去掉 @ 后缀再比；
#   3. libc/libm/libdl/liblog 在 runtime APEX 里，不在 system/lib64
#      → 从 system/apex/*.apex（本质是 zip）里解出 lib64/ 一并作为符号来源。
#
set -euo pipefail
export LC_ALL=C
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../scripts" && pwd)/common.sh"

SYS="${1:-$PRODUCT_OUT/system}"
BRIDGE="$DEVICE_DST/autosnap_x64_arm64/bridge/system"
[ -d "$SYS/lib64" ] || die "找不到 system/lib64：$SYS"
[ -d "$BRIDGE/lib64" ] || die "找不到翻译层：$BRIDGE（先 ./apply-overlay.sh）"
command -v readelf >/dev/null || die "缺 readelf（binutils）"
command -v nm >/dev/null || die "缺 nm（binutils）"
command -v unzip >/dev/null || die "缺 unzip"

# 临时文件放数据盘（放 /tmp 会把 19 GB 的根分区写满——踩过）
mkdir -p "$RUN_DIR"
tmp=$(mktemp -d "$RUN_DIR/symcheck.XXXXXX"); trap 'rm -rf "$tmp"' EXIT

# ---------------------------------------------------------------------------
# 符号来源目录
SEARCH_DIRS=( "$SYS/lib64" "$SYS/../vendor/lib64" "$SYS/../system_ext/lib64"
              "$SYS/../product/lib64" "$SYS/../odm/lib64" )

# 展平的 APEX（产品产物里是 *.apex，本质 zip，里面才有 libc/libm/libdl/liblog）
n_apex=0
for a in "$SYS"/apex/*.apex; do
    [ -f "$a" ] || continue
    d="$tmp/apex/$(basename "$a" .apex)"
    mkdir -p "$d"
    if unzip -o -q "$a" 'lib64/*' -d "$d" 2>/dev/null; then
        [ -d "$d/lib64" ] && { SEARCH_DIRS+=( "$d/lib64" ); n_apex=$((n_apex+1)); }
    fi
done

SYSLIBS=()
for d in "${SEARCH_DIRS[@]}"; do
    [ -d "$d" ] || continue
    for f in "$d"/*.so "$d"/*.so.*; do [ -f "$f" ] && SYSLIBS+=( "$f" ); done
done

log "符号来源：$((${#SYSLIBS[@]})) 个库（含 $n_apex 个 APEX 解包目录）"
log "待检对象：$(ls "$BRIDGE/lib64"/libndk_translation*.so | wc -l) 个翻译层库"

# ---------------------------------------------------------------------------
strip_ver() { sed 's/@.*$//'; }                       # 去掉 @LIBC 之类的版本后缀
need_of()   { readelf -d "$1" 2>/dev/null | awk '/NEEDED/{gsub(/[\[\]]/,"",$NF); print $NF}' || true; }
undef_of()  { nm -D --undefined-only "$1" 2>/dev/null | awk '{print $NF}' | grep -v '^$' | strip_ver | sort -u || true; }
defs_of()   { nm -D --defined-only   "$1" 2>/dev/null | awk '{print $NF}' | grep -v '^$' | strip_ver | sort -u || true; }

: > "$tmp/defined"
for l in "${SYSLIBS[@]}"; do defs_of "$l" >> "$tmp/defined"; done
sort -u "$tmp/defined" -o "$tmp/defined"
log "可用导出符号：$(wc -l < "$tmp/defined") 个"

# ---------------------------------------------------------------------------
missing_libs=0; missing_syms=0; miss_report=""
for so in "$BRIDGE/lib64"/libndk_translation*.so; do
    name=$(basename "$so")
    for n in $(need_of "$so"); do
        found=""
        for d in "${SEARCH_DIRS[@]}"; do [ -e "$d/$n" ] && { found=1; break; }; done
        if [ -z "$found" ]; then
            printf '  \033[1;31m[缺库]\033[0m %-52s → %s\n' "$name" "$n"
            missing_libs=$((missing_libs+1))
        fi
    done
    undef_of "$so" > "$tmp/u"
    comm -23 "$tmp/u" "$tmp/defined" > "$tmp/miss"
    if [ -s "$tmp/miss" ]; then
        n=$(wc -l < "$tmp/miss")
        printf '  \033[1;33m[未解析]\033[0m %-50s %s 个（前 3：%s）\n' \
            "$name" "$n" "$(head -3 "$tmp/miss" | tr '\n' ' ')"
        missing_syms=$((missing_syms+n))
        miss_report="$miss_report$name: $(head -20 "$tmp/miss" | tr '\n' ' ')\n"
    fi
done

echo
printf '%s' "$miss_report" > "$RUN_DIR/symcheck-missing.txt" 2>/dev/null || true
if [ "$missing_libs" = 0 ] && [ "$missing_syms" = 0 ]; then
    log "翻译层依赖自检通过 ✓（缺库 0，未解析符号 0）"
else
    warn "缺库 $missing_libs 个；未解析符号 $missing_syms 个（明细：.run/symcheck-missing.txt）"
    warn "注意：少数符号由 vendor/odm 命名空间或运行时 dlopen 提供，"
    warn "      这里报「未解析」不一定就是真问题——最终以启动后的验收为准。"
    exit 1
fi
