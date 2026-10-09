# run-archive-test.ps1 —— 真跑一遍 Windows 的 export / import，并核对结果。
#
# 合成实例 t1 里故意放了四个**该被排除**的文件（config.ini / bootcompleted.ini /
# version_num.cache / multiinstance.lock），导入之后它们**不该**出现在 t2 里 ——
# 这是反向测试，只看"文件在不在"是抓不到排除规则写错的。

$root = "C:\Users\pc\Desktop\autosnap-test\pkg"   # 真实包布局：bin\ 与 images\ 平级
$bin  = "$root\bin\emulator.ps1"
$out  = "C:\Users\pc\Desktop\autosnap-test\t1-export.tar"
Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$root\.run\instances\t2.env" -Force -ErrorAction SilentlyContinue
Get-ChildItem "$root\.run" -Directory -Filter "sysdir-*" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne "sysdir-5580" } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

function Run-Manager {
    # ⚠️ 参数**不能叫 $Args** —— 那是 PowerShell 的自动变量，会互相打架：
    #    实测表现为"第一个参数没传进去"，命令变成 help、打印用法还退出码 0。
    param([string[]]$ManagerArgs)
    & powershell -NoProfile -ExecutionPolicy Bypass -File $bin @ManagerArgs
    Write-Host ("  [exit={0}]" -f $LASTEXITCODE)
}

Write-Host "===== 1) export t1 ====="
Run-Manager @("export", "t1", "-Out", $out)

Write-Host ""
Write-Host "===== 2) 归档里有什么 ====="
& tar -tf $out 2>&1 | Sort-Object | ForEach-Object { Write-Host "  $_" }

Write-Host ""
Write-Host "===== 3) 清单 ====="
& tar -xOf $out "./INSTANCE-MANIFEST.json"
Write-Host ""

Write-Host "===== 4) import -> t2 ====="
Run-Manager @("import", $out, "-Name", "t2")

Write-Host ""
Write-Host "===== 5) 核对 ====="
$t2 = Get-ChildItem "$root\.run" -Directory -Filter "sysdir-*" -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -ne "sysdir-5580" }
if (-not $t2) {
    Write-Host "  [X] t2 的工作目录没建出来"
} else {
    Write-Host ("  t2 工作目录：{0}" -f $t2.Name)
    $names = @(Get-ChildItem $t2.FullName -Force | Select-Object -ExpandProperty Name)
    Write-Host ("  内容：{0}" -f ($names -join ", "))

    foreach ($must in @("emulator-user.ini", "hardware-properties.ini")) {
        if ($names -contains $must) { Write-Host "  [OK] 该搬的搬过来了：$must" }
        else { Write-Host "  [X] 该搬的没搬：$must" }
    }
    # ⚠️ config.ini **不在**这个清单里：它**应该**存在 —— 但内容是导入时按
    #    本机模板重写的，不是归档里那份。第一版把它当成"该排除"来判，
    #    结果是一个**假红**（文件在=报错），而真正要判的是**内容**。
    foreach ($no in @("bootcompleted.ini", "version_num.cache", "multiinstance.lock")) {
        if ($names -contains $no) { Write-Host "  [X] 不该来的来了：$no" }
        else { Write-Host "  [OK] 正确排除：$no" }
    }
    $cfg = Join-Path $t2.FullName "config.ini"
    if (Test-Path -LiteralPath $cfg) {
        $c = Get-Content -LiteralPath $cfg -Raw
        if ($c -match "SHOULD-NOT-TRAVEL") { Write-Host "  [X] config.ini 是从归档搬来的（应该用本机模板重写）" }
        else { Write-Host "  [OK] config.ini 是按本机模板重写的" }
    } else {
        Write-Host "  [!] config.ini 不存在（导入由调用方负责重写）"
    }
    $linked = @(Get-ChildItem $t2.FullName -Force | Where-Object { $_.LinkType })
    if ($linked.Count -gt 0) { Write-Host ("  [OK] 镜像重新链上了：{0}" -f (($linked | ForEach-Object { $_.Name }) -join ", ")) }
    else { Write-Host "  [X] 镜像没有重新链上" }
}

Write-Host ""
Write-Host "===== 6) t2 的登记（端口/服务属性）====="
Get-Content "$root\.run\instances\t2.env" -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $_" }
Write-Host "DONE"
