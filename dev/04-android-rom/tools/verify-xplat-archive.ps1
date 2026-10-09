# verify-xplat-archive.ps1 —— 跨平台互认：**Linux 导出的归档，在 Windows 上恢复**。
#
# 两边共享同一份"归档格式"契约（见 packaging/bin/*/archive.* 的头部）：
#   ./INSTANCE-MANIFEST.json + ./host-service.env + ./sysdir/… + 压平的数据镜像
# 这个脚本验的就是那份契约真的能跨平台用 —— 而不是各自在自己平台上自洽。
#
# 用法：先在 Linux 侧造实例并导出（标记写进 /tmp/xplat-marker.txt），
#       把归档与本文件一起推过来，然后跑它。

$ErrorActionPreference = "Stop"
$base = "C:\Users\pc\Desktop\autosnap-test"
$pkg  = "$base\pkg"
$tar  = "$base\xplat.tar"
$qi   = "$pkg\runtime\emulator\qemu-img.exe"
$OFFSET = 1MB

foreach ($f in @($tar, "$base\xplat-marker.txt")) {
    if (-not (Test-Path -LiteralPath $f)) { Write-Host "[X] 缺文件：$f"; exit 1 }
}
$marker = (Get-Content -LiteralPath "$base\xplat-marker.txt" -Raw).Trim()
Write-Host "Linux 侧写入的标记：$marker"

Write-Host "[1] 归档内容（Linux 产的）"
& tar -tf $tar 2>&1 | ForEach-Object { Write-Host "    $_" }

Write-Host "[2] 在 Windows 上 import …"
Get-ChildItem "$pkg\.run" -Directory -Filter "sysdir-*" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne "sysdir-5580" -and $_.Name -ne "sysdir-5584" } |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$pkg\.run\instances\xp.env" -Force -ErrorAction SilentlyContinue
& powershell -NoProfile -ExecutionPolicy Bypass -File "$pkg\bin\emulator.ps1" import $tar -Name xp
if ($LASTEXITCODE -ne 0) { Write-Host "[X] import 失败"; exit 1 }

$xp = Get-ChildItem "$pkg\.run" -Directory -Filter "sysdir-*" |
      Where-Object { $_.Name -notin @("sysdir-5580", "sysdir-5584") } | Select-Object -First 1
if (-not $xp) { Write-Host "[X] 没建出工作目录"; exit 1 }
Write-Host ("    xp 工作目录：{0}" -f $xp.Name)

$img = Join-Path $xp.FullName "userdata-qemu.img"
if (-not (Test-Path -LiteralPath $img)) { Write-Host "[X] 没还原出 userdata-qemu.img"; exit 1 }
Write-Host ("    [OK] 还原出 raw：{0:N1} MB（表观）" -f ((Get-Item $img).Length / 1MB))

$svc = Get-Content -LiteralPath "$pkg\.run\instances\xp.env" -ErrorAction SilentlyContinue |
       Where-Object { $_ -match '^SERVICE_' }
if ($svc) { Write-Host ("    [OK] 服务属性跨平台搬过来了：{0}" -f (($svc | ForEach-Object { ($_ -split '=')[0] }) -join ", ")) }
else { Write-Host "    [!] 没有服务属性（Linux 侧那份 .env 里没有 SERVICE_* 就正常）" }

Write-Host "[3] 读回标记"
$restored = "$base\xplat-restored.raw"
$out = & $qi convert -O raw $img $restored 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "[X] 转回 raw 失败（exit=$LASTEXITCODE）"
    $out | Select-Object -Last 5 | ForEach-Object { Write-Host "    $_" }
    exit 1
}
$n = [System.Text.Encoding]::ASCII.GetByteCount($marker)
$fs = [System.IO.File]::OpenRead($restored); [void]$fs.Seek($OFFSET, 'Begin')
$buf = New-Object byte[] $n; [void]$fs.Read($buf, 0, $n); $fs.Close()
$got = ([System.Text.Encoding]::ASCII.GetString($buf)).TrimEnd([char]0)

Write-Host ""
if ($got -eq $marker) {
    Write-Host ("[OK] ★ 跨平台互认成立：Linux 导出的归档，在 Windows 恢复后偏移 1MB 处读到 `"$got`"")
} else {
    Write-Host ("[X] 对不上：期望 `"$marker`"，读到 `"$got`"")
}
Write-Host "DONE"
