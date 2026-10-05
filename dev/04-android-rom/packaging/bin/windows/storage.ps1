# Host disk accounting and lossless NTFS sparse recovery.
# ZIP does not carry sparse-file extents. An extracted disk image can therefore
# occupy its whole logical size until zero ranges are reclaimed below.
# Reclaim only zero ranges, with an exclusive file handle, before QEMU starts.

function Initialize-SparseFileApi {
    if ("Autosnap.SparseFile" -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Autosnap {
    public static class SparseFile {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetCompressedFileSizeW(string name, out uint high);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool DeviceIoControl(SafeFileHandle file, uint code,
            IntPtr input, uint inputSize, IntPtr output, uint outputSize,
            out uint returned, IntPtr overlapped);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool DeviceIoControl(SafeFileHandle file, uint code,
            ref ZeroRange input, uint inputSize, IntPtr output, uint outputSize,
            out uint returned, IntPtr overlapped);
        [StructLayout(LayoutKind.Sequential)]
        private struct ZeroRange { public long Start; public long End; }

        public static long AllocatedBytes(string name) {
            uint high;
            uint low = GetCompressedFileSizeW(name, out high);
            int error = Marshal.GetLastWin32Error();
            if (low == UInt32.MaxValue && error != 0) throw new Win32Exception(error);
            return (long)(((ulong)high << 32) | low);
        }

        private static void Punch(SafeFileHandle file, long start, long end) {
            if (end <= start) return;
            ZeroRange range = new ZeroRange { Start = start, End = end };
            uint returned;
            if (!DeviceIoControl(file, 0x000980c8, ref range, 16, IntPtr.Zero, 0,
                                 out returned, IntPtr.Zero)) {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
        }

        public static long CompactZeroes(string name) {
            long before;
            using (FileStream file = new FileStream(name, FileMode.Open,
                    FileAccess.ReadWrite, FileShare.None, 1024 * 1024)) {
                before = AllocatedBytes(name);
                // Files with reclaimed holes need no scan.  An interrupted
                // scan might set the sparse attribute while leaving it dense,
                // so use actual allocation rather than the attribute alone.
                if (before < file.Length - 64 * 1024) return 0;
                uint returned;
                if (!DeviceIoControl(file.SafeFileHandle, 0x000900c4, IntPtr.Zero, 0,
                                     IntPtr.Zero, 0, out returned, IntPtr.Zero)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }
                // 64 KiB is the NTFS sparse allocation granularity.  Check
                // complete units so a partially zero block is never discarded.
                byte[] buffer = new byte[64 * 1024];
                long offset = 0, zeroStart = -1;
                while (offset < file.Length) {
                    int wanted = (int)Math.Min(buffer.Length, file.Length - offset);
                    int got = 0;
                    while (got < wanted) {
                        int n = file.Read(buffer, got, wanted - got);
                        if (n == 0) throw new EndOfStreamException(name);
                        got += n;
                    }
                    bool zero = wanted == buffer.Length;
                    for (int i = 0; zero && i < got; ++i) zero = buffer[i] == 0;
                    if (zero) {
                        if (zeroStart < 0) zeroStart = offset;
                    } else if (zeroStart >= 0) {
                        Punch(file.SafeFileHandle, zeroStart, offset);
                        zeroStart = -1;
                    }
                    offset += got;
                }
                if (zeroStart >= 0) Punch(file.SafeFileHandle, zeroStart, offset);
                file.Flush(true);
            }
            return Math.Max(0, before - AllocatedBytes(name));
        }
    }
}
'@
}

function Get-AllocatedFileBytes {
    param([string]$Path)
    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        Initialize-SparseFileApi
        return [Autosnap.SparseFile]::AllocatedBytes($item.FullName)
    }
    # PowerShell-on-Unix tests can use stat without requiring kernel32.
    $value = & stat -c %b -- $item.FullName 2>$null
    if ($LASTEXITCODE -eq 0) { return [int64]$value * 512 }
    return [int64]$item.Length
}

function Optimize-ImageStorage {
    param([string]$Directory)
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { return }
    Initialize-SparseFileApi
    [int64]$saved = 0
    foreach ($item in Get-ChildItem -LiteralPath $Directory -Filter *.img -File) {
        # sysdir contains links/junctions back to the immutable package images;
        # never compact through one, or a launch would mutate the shared base.
        if ($item.LinkType -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { continue }
        if ($item.Length -lt 64MB) { continue }
        try {
            $saved += [Autosnap.SparseFile]::CompactZeroes($item.FullName)
        } catch {
            # A live QEMU, a read-only directory, or a filesystem without sparse
            # support is left untouched.  This is a storage optimisation only.
            Write-Warning "无法回收镜像零块 $($item.Name)：$($_.Exception.Message)"
        }
    }
    if ($saved -gt 0) {
        Write-Host ("已回收镜像零块 {0:N2} GiB（镜像内容和容量不变）" -f ($saved / 1GB))
    }
}

