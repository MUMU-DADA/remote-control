# verify-repo-side.ps1 —— 真跑**开发侧**（windows\emulator.ps1）的 export / import。
#
# 交付版那份已经验过了；仓库侧是另一份实现（这一路的形状就是"两套手工维护"），
# 所以它也得**真跑**一遍 —— 只验语法不算数。
#
# 与交付版的 CLI 差异（各自随各家）：
#   export <名> -Out <归档>
#   import <归档> <新名字>        ← 位置 2 是新名字（交付版是 -Name）

$ErrorActionPreference = "Stop"
$base = "C:\Users\pc\Desktop\autosnap-test"
$mgr  = "$base\windows\emulator.ps1"
$run  = "$base\windows\.run"
$sys  = "$run\sysdir-5600"
$qi   = "$base\pkg\runtime\emulator\qemu-img.exe"
$OFFSET = 1MB

# 开发侧读这三个环境变量（没有 $script:Runtime/$script:Images）
$env:AUTOSNAP_IMAGES  = "$base\pkg\images"
$env:AUTOSNAP_RUNTIME = "$base\pkg\runtime"
$env:AUTOSNAP_CONFIG  = "$base\packaging\templates\config.ini"

New-Item -ItemType Directory -Force -Path "$run\instances", $sys | Out-Null
Set-Content -LiteralPath "$run\instances\tz.env" -Encoding ASCII -Value @(
    "# 开发侧 export/import 测试", "PORT=5600", "SERVICE_PORT=8088", "SERVICE_AUTH=1")
Set-Content -LiteralPath "$sys\emulator-user.ini" -Value "repo-side-keepme" -Encoding ASCII

$marker = "AUTOSNAP-REPO-" + (Get-Date -Format "HHmmss")
$raw = "$base\repo-base.raw"
$fs = [System.IO.File]::Create($raw); $fs.SetLength(64MB); $fs.Close()
$bytes = [System.Text.Encoding]::ASCII.GetBytes($marker)
$fs = [System.IO.File]::OpenWrite($raw); [void]$fs.Seek($OFFSET, 'Begin'); $fs.Write($bytes, 0, $bytes.Length); $fs.Close()
$qo = & $qi convert -O qcow2 -c $raw "$sys\userdata-qemu.img.qcow2" 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "[X] 造覆盖层失败（exit=$LASTEXITCODE）"; $qo | Select-Object -Last 3 | ForEach-Object { Write-Host "    $_" }; exit 1 }
Copy-Item -LiteralPath $raw -Destination "$sys\userdata-qemu.img" -Force
Write-Host "标记 = $marker"

$tar = "$base\repo-export.tar"
Remove-Item -LiteralPath $tar -Force -ErrorAction SilentlyContinue
Write-Host "[1] 开发侧 export tz …"
& powershell -NoProfile -ExecutionPolicy Bypass -File $mgr export tz -Out $tar -Force
if ($LASTEXITCODE -ne 0) { Write-Host "[X] 开发侧 export 失败"; exit 1 }
$members = @(& tar -tf $tar 2>&1 | ForEach-Object { ([string]$_).Trim() })
if ($members -contains "./sysdir/userdata-flat.qcow2") { Write-Host "    [OK] 归档里有压平后的数据镜像" }
else { Write-Host ("    [X] 没有压平后的数据镜像；成员：{0}" -f ($members -join ", ")) }

Write-Host "[2] 开发侧 import → tz2 …"
Remove-Item -LiteralPath "$run\instances\tz2.env" -Force -ErrorAction SilentlyContinue
Get-ChildItem $run -Directory -Filter "sysdir-*" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne "sysdir-5600" } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
& powershell -NoProfile -ExecutionPolicy Bypass -File $mgr import $tar tz2
if ($LASTEXITCODE -ne 0) { Write-Host "[X] 开发侧 import 失败"; exit 1 }

$tz2 = Get-ChildItem $run -Directory -Filter "sysdir-*" | Where-Object { $_.Name -ne "sysdir-5600" } | Select-Object -First 1
if (-not $tz2) { Write-Host "[X] 没建出工作目录"; exit 1 }
Write-Host ("    {0} 内容：{1}" -f $tz2.Name, ((Get-ChildItem $tz2.FullName -Force | ForEach-Object { $_.Name }) -join ", "))
$img = Join-Path $tz2.FullName "userdata-qemu.img"
if (-not (Test-Path -LiteralPath $img)) { Write-Host "[X] 没还原出 userdata-qemu.img"; exit 1 }
if (Test-Path -LiteralPath (Join-Path $tz2.FullName "userdata-qemu.img.qcow2")) { Write-Host "    [X] 覆盖层还在" } else { Write-Host "    [OK] 单文件恢复（无覆盖层）" }
if (Test-Path -LiteralPath (Join-Path $tz2.FullName "config.ini")) { Write-Host "    [OK] config.ini 已按本机模板重写" } else { Write-Host "    [X] 没有 config.ini" }
$linked = @(Get-ChildItem $tz2.FullName -Force | Where-Object { $_.LinkType })
if ($linked.Count -gt 0) { Write-Host ("    [OK] 镜像重新链上了：{0}" -f (($linked | ForEach-Object { $_.Name }) -join ", ")) }
else { Write-Host "    [!] 没有链上镜像（images 目录里没有同名文件就正常）" }

$restored = "$base\repo-restored.raw"
$ro = & $qi convert -O raw $img $restored 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "[X] 转回 raw 失败（exit=$LASTEXITCODE）"; $ro | Select-Object -Last 3 | ForEach-Object { Write-Host "    $_" }; exit 1 }
$fs = [System.IO.File]::OpenRead($restored); [void]$fs.Seek($OFFSET, 'Begin')
$buf = New-Object byte[] $bytes.Length; [void]$fs.Read($buf, 0, $buf.Length); $fs.Close()
$got = ([System.Text.Encoding]::ASCII.GetString($buf)).TrimEnd([char]0)
Write-Host ""
if ($got -eq $marker) { Write-Host "[OK] ★ 开发侧数据搬过来了：偏移 1MB 处读到 `"$got`"" }
else { Write-Host "[X] 对不上：期望 `"$marker`"，读到 `"$got`"" }
Write-Host "DONE"
