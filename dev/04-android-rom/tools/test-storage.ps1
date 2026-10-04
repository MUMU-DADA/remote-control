# Runs native sparse recovery on Windows; compiles the native helper elsewhere.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "../packaging/bin/windows/storage.ps1")
Initialize-SparseFileApi
Write-Host "PASS Windows sparse helper C# compilation"
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    Write-Host "SKIP NTFS allocated-block checks: Windows host required"
    exit 0
}

$temp = Join-Path ([IO.Path]::GetTempPath()) ("autosnap-storage-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temp | Out-Null
$path = Join-Path $temp "system-qemu.img"
try {
    $stream = [IO.File]::Create($path)
    try {
        $zeroes = New-Object byte[] (1MB)
        $payload = New-Object byte[] (1MB)
        for ($i = 0; $i -lt $payload.Length; ++$i) { $payload[$i] = 0x55 }
        for ($i = 0; $i -lt 96; ++$i) {
            $b = if ($i -eq 0 -or $i -eq 47 -or $i -eq 95) { $payload } else { $zeroes }
            $stream.Write($b, 0, $b.Length)
        }
    } finally { $stream.Dispose() }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    $size = (Get-Item -LiteralPath $path).Length
    $before = Get-AllocatedFileBytes $path
    Optimize-ImageStorage $temp
    $after = Get-AllocatedFileBytes $path
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $hash) { throw "Disk bytes changed" }
    if ((Get-Item -LiteralPath $path).Length -ne $size) { throw "Disk geometry changed" }
    if ($after -ge 5MB) { throw "NTFS zero extents were not reclaimed" }
    $savedAgain = [Autosnap.SparseFile]::CompactZeroes($path)
    if ($savedAgain -ne 0) { throw "Already sparse image was scanned again" }
    Write-Host ("PASS NTFS sparse recovery: {0:N1} MiB -> {1:N1} MiB; bytes and size preserved" -f ($before / 1MB), ($after / 1MB))

    $lock = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $lockedRejected = $false
    try {
        try { [Autosnap.SparseFile]::CompactZeroes($path) | Out-Null } catch { $lockedRejected = $true }
    } finally { $lock.Dispose() }
    if (-not $lockedRejected) { throw "Locked image was accessed" }
    Write-Host "PASS NTFS recovery refuses a locked image"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force
}

