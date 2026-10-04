#!/usr/bin/env bash
# Restore raw image holes after ZIP extraction.  Logical bytes never change.

image_storage_bytes() {   # <path> -> logical bytes, allocated bytes
    local path="$1" logical blocks
    if [ "$(uname -s)" = Darwin ]; then
        logical="$(stat -f %z "$path")"; blocks="$(stat -f %b "$path")"
    else
        logical="$(stat -c %s "$path")"; blocks="$(stat -c %b "$path")"
    fi
    printf '%s %s\n' "$logical" "$((blocks * 512))"
}

optimize_image_storage() {   # <images directory> <qemu-img path>
    local dir="$1" qemu_img="$2" image sizes logical allocated tmp mode saved=0
    [ -x "$qemu_img" ] || return 0
    for image in "$dir"/*.img; do
        [ -f "$image" ] && [ ! -L "$image" ] || continue
        sizes="$(image_storage_bytes "$image")" || continue
        read -r logical allocated <<< "$sizes"
        # Already sparse files and small metadata images need no scan.  ZIP
        # extraction allocates the whole image, which is the case repaired here.
        [ "$logical" -ge 67108864 ] && [ "$allocated" -ge "$logical" ] || continue
        tmp="$(mktemp "$image.sparse.XXXXXX")" || continue
        if "$qemu_img" convert -f raw -O raw -S 4k "$image" "$tmp"; then
            if [ "$(uname -s)" = Darwin ]; then mode="$(stat -f %Lp "$image")"
            else mode="$(stat -c %a "$image")"; fi
            chmod "$mode" "$tmp"
            touch -r "$image" "$tmp"
            sizes="$(image_storage_bytes "$tmp")"
            read -r logical allocated <<< "$sizes"
            # Replacing a read-only base by identical bytes is safe for running
            # readers, whose open descriptors retain the old inode.  Conversion
            # errors (including a QEMU write lock) leave the original in place.
            if mv -f "$tmp" "$image"; then
                saved=$((saved + logical - allocated))
            fi
        fi
        [ ! -f "$tmp" ] || unlink "$tmp"
    done
    if [ "$saved" -gt 0 ]; then
        printf '已回收镜像零块 %s MiB（镜像内容和容量不变）\n' "$((saved / 1048576))"
    fi
}

