# setup-win-pkg.ps1 —— 按**真实包布局**铺一个最小可跑环境，用来验 export/import。
#
# 关键：`release.sh` 是把 `packaging/bin/windows/.` **直接铺到包的 `bin\`** 下，
# 不是 `bin\windows\`。管理器用 `$PSScriptRoot\..` 当包根 —— 放深一层它就会
# 去 `bin\.run` / `bin\images` 找东西（2026-10-09 实测踩到，才开始按真实布局铺）。
#
# 不需要 ROM 与运行时：走"实例没有数据镜像"这条路径时用不上 qemu-img。

$ErrorActionPreference = "Stop"
$base = "C:\Users\pc\Desktop\autosnap-test"
$src  = "$base\packaging\bin\windows"
$pkg  = "$base\pkg"

Remove-Item -LiteralPath $pkg -Recurse -Force -ErrorAction SilentlyContinue
foreach ($d in @("$pkg\bin", "$pkg\images", "$pkg\templates", "$pkg\.run\instances", "$pkg\.run\sysdir-5580")) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
Copy-Item -Path "$src\*" -Destination "$pkg\bin" -Force
Copy-Item -LiteralPath "$base\packaging\templates\config.ini" -Destination "$pkg\templates\config.ini" -Force

# 哑镜像
foreach ($n in @("system-qemu.img", "vendor-qemu.img", "userdata.img", "encryptionkey.img")) {
    Set-Content -LiteralPath "$pkg\images\$n" -Value "dummy-$n" -Encoding ASCII
}
Set-Content -LiteralPath "$pkg\images\SHA256SUMS" -Value "deadbeef  system-qemu.img" -Encoding ASCII

# 合成实例
Set-Content -LiteralPath "$pkg\.run\instances\t1.env" -Encoding ASCII -Value @(
    "# 合成实例（验证 export/import 用）",
    "PORT=5580", "SERVICE_PORT=8088", "SERVICE_ENABLED=1", "SERVICE_AUTH=1", "SERVICE_BIND=0.0.0.0")

Set-Content -LiteralPath "$pkg\.run\sysdir-5580\emulator-user.ini"     -Value "keepme-please" -Encoding ASCII
Set-Content -LiteralPath "$pkg\.run\sysdir-5580\hardware-properties.ini" -Value "also-keep"   -Encoding ASCII
# 四个**该被排除**的（反向测试）
foreach ($n in @("config.ini", "bootcompleted.ini", "version_num.cache", "multiinstance.lock")) {
    Set-Content -LiteralPath "$pkg\.run\sysdir-5580\$n" -Value "SHOULD-NOT-TRAVEL" -Encoding ASCII
}
$linkFail = @()
foreach ($n in @("system-qemu.img", "vendor-qemu.img", "userdata.img")) {
    try { New-Item -ItemType SymbolicLink -Path "$pkg\.run\sysdir-5580\$n" -Target "$pkg\images\$n" -Force | Out-Null }
    catch { $linkFail += $n }
}
if ($linkFail.Count -gt 0) { Write-Host ("WARN: symlink 建不了：" + ($linkFail -join ",")) }
Write-Host "READY：$pkg"
