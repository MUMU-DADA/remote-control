# verify-win-archive-data.ps1 —— 验 Windows 侧备份恢复的**数据路径**。
#
# 为什么单独一个：之前的端到端验的是"实例**没有**数据镜像"那条路径
# （那是最省的一条，不需要 qemu-img）。真正的主力路径是：
#
#     导出：userdata-qemu.img.qcow2（覆盖层）→ qemu-img convert -O qcow2 -c → 归档里的 userdata-flat.qcow2
#     导入：userdata-flat.qcow2 → qemu-img convert -O raw → userdata-qemu.img
#
# 做法：造一个 64MB 的 raw，在 1MB 偏移写一个已知标记；转成 qcow2 覆盖层放进实例；
# 导出、导入，再把**导入出来的**镜像转回 raw，到**同一个偏移**读回标记比对。
# 比对的是内容，不是"文件在不在" —— 后者抓不到"数据没搬过来"。

$ErrorActionPreference = "Stop"
$base = "C:\Users\pc\Desktop\autosnap-test"
$pkg  = "$base\pkg"
$sys  = "$pkg\.run\sysdir-5580"
$qi   = "$pkg\runtime\emulator\qemu-img.exe"
$OFFSET = 1MB

if (-not (Test-Path -LiteralPath $qi)) { Write-Host "[X] 找不到 qemu-img：$qi"; exit 1 }

# ── 1) 造带标记的镜像 ────────────────────────────────────────────────────
$marker = "AUTOSNAP-DATA-" + (Get-Date -Format "HHmmss")
$raw = "$base\base.raw"
$fs = [System.IO.File]::Create($raw); $fs.SetLength(64MB); $fs.Close()
$bytes = [System.Text.Encoding]::ASCII.GetBytes($marker)
$fs = [System.IO.File]::OpenWrite($raw); [void]$fs.Seek($OFFSET, 'Begin'); $fs.Write($bytes, 0, $bytes.Length); $fs.Close()
Write-Host ("[1] 造好 raw：{0}  标记 = {1}（偏移 1MB）" -f $raw, $marker)

# 覆盖层（真实例就是这个形状：backing 一份 raw + 一个 qcow2 覆盖层）
# ⚠️ **不要把 qemu-img 的输出吞掉** —— 第一版写了 `*>&1 | Out-Null`，
#    结果失败时只剩一个裸退出码 `-1073741515`（= 0xC0000135 STATUS_DLL_NOT_FOUND，
#    少拷了 qemu-img 旁边的 DLL），完全看不出原因。
$out = & $qi convert -O qcow2 -c $raw "$sys\userdata-qemu.img.qcow2" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "[X] 造覆盖层失败（exit=$LASTEXITCODE）"
    $out | Select-Object -Last 5 | ForEach-Object { Write-Host "    $_" }
    exit 1
}
Copy-Item -LiteralPath $raw -Destination "$sys\userdata-qemu.img" -Force
Write-Host ("[2] 已放进实例：backing + 覆盖层（{0:N1} MB qcow2）" -f ((Get-Item "$sys\userdata-qemu.img.qcow2").Length / 1MB))

# ── 2) 导出（应压平） ────────────────────────────────────────────────────
$tar = "$base\data-export.tar"
Remove-Item -LiteralPath $tar -Force -ErrorAction SilentlyContinue
Write-Host "[3] export t1 …"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$pkg\bin\emulator.ps1" export t1 -Out $tar -Force
if ($LASTEXITCODE -ne 0) { Write-Host "[X] export 失败"; exit 1 }

$members = @(& tar -tf $tar 2>&1 | ForEach-Object { ([string]$_).Trim() })
if ($members -contains "./sysdir/userdata-flat.qcow2") {
    Write-Host "    [OK] 归档里有压平后的 userdata-flat.qcow2（数据路径走对了）"
} else {
    Write-Host "    [X] 归档里没有 userdata-flat.qcow2 —— 数据没被压平带出来"
    Write-Host ("    实际成员：{0}" -f ($members -join ", "))
}
$leaked = @($members | Where-Object { $_ -match "userdata-qemu\.img" })
if ($leaked.Count -gt 0) { Write-Host ("    [X] 原始数据镜像泄进归档了：{0}" -f ($leaked -join ", ")) }
else { Write-Host "    [OK] 原始数据镜像没有泄进归档（只带压平那一份）" }

# ── 3) 导入（应转回 raw） ────────────────────────────────────────────────
Get-ChildItem "$pkg\.run" -Directory -Filter "sysdir-*" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne "sysdir-5580" } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$pkg\.run\instances\t9.env" -Force -ErrorAction SilentlyContinue
Write-Host "[4] import → t9 …"
# 交付版的 import 只收一个位置参数（归档），新名字走 -Name（仓库侧那份是位置 2 —— 两边 CLI 各随各家）
& powershell -NoProfile -ExecutionPolicy Bypass -File "$pkg\bin\emulator.ps1" import $tar -Name t9
if ($LASTEXITCODE -ne 0) { Write-Host "[X] import 失败"; exit 1 }

$t9 = Get-ChildItem "$pkg\.run" -Directory -Filter "sysdir-*" |
      Where-Object { $_.Name -ne "sysdir-5580" } | Select-Object -First 1
if (-not $t9) { Write-Host "[X] t9 的工作目录没建出来"; exit 1 }
$img = Join-Path $t9.FullName "userdata-qemu.img"
if (Test-Path -LiteralPath $img) {
    Write-Host ("    [OK] 还原出 raw：{0:N1} MB（表观）" -f ((Get-Item $img).Length / 1MB))
} else {
    Write-Host "    [X] 没有还原出 userdata-qemu.img"; exit 1
}
if (Test-Path -LiteralPath (Join-Path $t9.FullName "userdata-qemu.img.qcow2")) {
    Write-Host "    [X] 覆盖层还在（格式 2 恢复后不该有）"
} else {
    Write-Host "    [OK] 没有覆盖层（单文件恢复）"
}

# ── 4) 读回标记比对 ──────────────────────────────────────────────────────
$restored = "$base\restored.raw"
$out2 = & $qi convert -O raw $img $restored 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "[X] 把还原出来的镜像转回 raw 失败（exit=$LASTEXITCODE）"
    $out2 | Select-Object -Last 5 | ForEach-Object { Write-Host "    $_" }
    exit 1
}
$fs = [System.IO.File]::OpenRead($restored); [void]$fs.Seek($OFFSET, 'Begin')
$buf = New-Object byte[] $bytes.Length
[void]$fs.Read($buf, 0, $buf.Length); $fs.Close()
$got = ([System.Text.Encoding]::ASCII.GetString($buf)).TrimEnd([char]0)

Write-Host ""
if ($got -eq $marker) {
    Write-Host ("[OK] ★ 数据搬过来了：偏移 1MB 处读到 `"$got`"，与写入的标记一致")
} else {
    Write-Host ("[X] 数据对不上：期望 `"$marker`"，读到 `"$got`"")
}
Write-Host "DONE"
