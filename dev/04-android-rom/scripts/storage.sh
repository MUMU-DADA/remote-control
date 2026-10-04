#!/usr/bin/env bash
# Large image helpers shared by ROM packaging and storage checks.
#
# Android's *_qemu.img files are raw GPT disks.  They have a multi-gigabyte
# logical size but usually contain long zero ranges around the small ext4
# payload.  Preserve those ranges as holes when copying; the guest sees the
# same bytes and geometry while the host does not pay for zero blocks.

copy_sparse_file() {
    local src="$1" dst="$2"
    if cp --help 2>&1 | grep -q -- '--sparse'; then
        cp --sparse=always -f "$src" "$dst"
    else
        # BSD/macOS cp has no sparse switch.  Keep this fallback portable;
        # callers on such systems can use the Windows/macOS native sparse
        # helper when the destination filesystem supports one.
        cp -f "$src" "$dst"
    fi
}

