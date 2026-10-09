# =============================================================================
# Windows 侧 CLI 的行为/安全回归（**必须在 Windows 上跑**）
#
# 为什么单有一个：仓库里 `test-windows-emulator.sh` 是在 **Linux 上用 pwsh**
# 跑的，覆盖的是文件与生命周期逻辑。但下面这几件事**只能在 Windows 上验**，
# 因为确认提示要真去启动 `powershell.exe` 读一行输入：
#
#   1. `-Force` 用在不适用的动词上必须被拒（每个动词有独立的允许参数表）
#   2. 未知动词必须被拒
#   3. 非交互环境下 `delete` 不带 `-Yes` → **限时快速失败**，消息里要写真实秒数
#   4. **喂一行 `yes` 进去 → 只提示一次就成功**
#      ← 这一条是 2026-10-09 那个 bug 的回归：补丁把 `$ans = Read-LineWithTimeout`
#        塞了两遍，于是确认会弹两次 —— 第一次吃掉 yes，第二次读到 EOF 说"已取消"，
#        **破坏性操作永远走不下去**。非交互下第一次就 die 了，看不出来；
#        只有真喂一行输入才暴露。所以它必须留在这里。
#   5. `-Yes` 照常执行
#
# 全程在**临时 run 目录**里对着一个假实例做，绝不碰真实例。
#
# 用法（在 Windows 上）：
#     powershell -NoProfile -ExecutionPolicy Bypass -File verify-windows-cli.ps1
#     powershell ... -File verify-windows-cli.ps1 -Manager ..\packaging\bin\windows\emulator.ps1
# =============================================================================
param(
    [string]$Manager = ""
)

# 注意用 Continue：原生命令（cmd / powershell 子进程）把进度写到 stderr，
# 在 Stop 之下会变成终止性错误把脚本打断。这个坑本项目踩过好几次。
$ErrorActionPreference = "Continue"
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$pass = 0; $fail = 0
function Ok($m)   { Write-Host ("  [OK]   " + $m) -ForegroundColor Green; $script:pass++ }
function Bad($m)  { Write-Host ("  [FAIL] " + $m) -ForegroundColor Red;   $script:fail++ }
function Info($m) { Write-Host ("         " + $m) -ForegroundColor DarkGray }

# ── 定位被测的管理器 ────────────────────────────────────────────────────
if (-not $Manager) {
    # ⚠️ **默认测交付版**（`packaging/bin/windows/emulator.ps1`）—— 那是用户真正跑的。
    #    2026-10-09 实测：两条血统**不一样**：
    #      · 交付版的 `-Force` 是**按动词查表**的（`-Force` 用在 status 上会被拒）；
    #      · 仓库侧的 `windows/emulator.ps1` **没有那张表**，`-Force` 到处都被默默接受。
    #    先测本地那份会得到"没被拒"的结论，看起来像缺陷，其实是选错了对象。
    #    要测仓库侧那份就显式 `-Manager .\emulator.ps1`。
    $cands = @(
        (Join-Path $PSScriptRoot "..\packaging\bin\windows\emulator.ps1"),
        (Join-Path $PSScriptRoot "emulator.ps1")
    )
    foreach ($c in $cands) { if (Test-Path -LiteralPath $c) { $Manager = (Resolve-Path $c).Path; break } }
}
if (-not $Manager -or -not (Test-Path -LiteralPath $Manager)) {
    Write-Host "[x] 找不到 emulator.ps1（用 -Manager 指定）" -ForegroundColor Red
    Write-Host "    这是在 Windows 上跑的脚本；先把它和 emulator.ps1 放在一起。" -ForegroundColor Red
    exit 2
}
Info ("被测管理器：" + $Manager)

# ── 沙箱：临时 run 目录 + 一个假实例 ─────────────────────────────────────
$sandbox = Join-Path ([System.IO.Path]::GetTempPath()) ("autosnap-cli-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
$instDir = Join-Path $sandbox "instances"
New-Item -ItemType Directory -Force -Path $instDir | Out-Null
$instFile = Join-Path $instDir "clitest.env"
function New-Fake { "PORT=5999" | Out-File -LiteralPath $instFile -Encoding utf8 -Force }
New-Fake
$env:AUTOSNAP_RUN_DIR = $sandbox
Info ("沙箱：" + $sandbox)

function Run-Mgr {
    # ⚠️ 参数名不能叫 $Args —— 那是 PowerShell 的自动变量，赋值会报错。
    #    （本项目在 $pid 上踩过同一个坑，见 windows/README.md。）
    param([string[]]$MgrArgs, [string]$Stdin = "")
    # 统一走 cmd：需要喂一行输入时用 `echo yes|`，不需要时用 `< NUL`
    $q = '"' + $Manager + '"'
    $line = "powershell -NoProfile -ExecutionPolicy Bypass -File $q " + ($MgrArgs -join " ")
    if ($Stdin) { $line = "echo " + $Stdin + "| " + $line } else { $line = $line + " < NUL" }
    $out = & cmd /c ($line + " 2>&1")
    return ($out | Out-String)
}

function Remove-Instance { Remove-Item -LiteralPath $instFile -Force -ErrorAction SilentlyContinue }

Write-Host ""
Write-Host "── 1. 参数校验：-Force 只能用在特定动词上"

$o = Run-Mgr @("status", "clitest", "-Force")
if ($o -match "不适用于") { Ok "-Force 用在 status 上被拒" } else { Bad "-Force 用在 status 上没被拒"; Info ($o.Trim() -split "`n" | Select-Object -Last 1) }

$o = Run-Mgr @("nosuchverb")
if ($o -match "未知|不认识|unknown") { Ok "未知动词被拒" } else { Bad "未知动词没被拒"; Info ($o.Trim() -split "`n" | Select-Object -Last 1) }

Write-Host ""
Write-Host "── 2. 非交互：不带 -Yes 必须**限时快速失败**，且消息里写真实秒数"

# ⚠️ 这里**有两种合法结果**，第一版我只认了其中一种，差点报成缺陷：
#    · 子进程立刻拿到 EOF（`< NUL` 常见）→ 空读 → `Die "已取消"`，**不经过超时**；
#    · 子进程一直阻塞到窗口结束 → `Die "等了 N 秒没人输入…"`。
#    两者都对。真正要守住的是三件事：
#      ① 有界（不能挂死）；② 不删实例；③ **秒数若出现，必须等于设定的值**
#         —— ③ 就是 2026-10-09 那个"改了默认值、消息还写死 20"的回归。
New-Fake
$env:AUTOSNAP_CONFIRM_TIMEOUT = "3"
$t0 = Get-Date
$o = Run-Mgr @("delete", "clitest")
$sec = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)

if ($sec -lt 20) { Ok ("快速失败（$sec 秒）") } else { Bad "没有限时失败，耗时 $sec 秒（可能挂死了）" }

$m = [regex]::Match($o, "等了\s*(\d+)\s*秒")
if ($m.Success) {
    if ($m.Groups[1].Value -eq "3") { Ok ("消息里的秒数是设定值 3（不是写死的）") }
    else { Bad ("消息里写的是 $($m.Groups[1].Value) 秒，但设定的是 3 —— 秒数被写死了") }
} else {
    Ok "没走到超时分支（子进程直接 EOF），消息为'已取消' —— 合法"
}
if (Test-Path -LiteralPath $instFile) { Ok "失败后实例**没**被删（保护生效）" } else { Bad "非交互下竟然把实例删了" }
Remove-Item Env:AUTOSNAP_CONFIRM_TIMEOUT -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "── 3. 喂一行 yes：必须**只提示一次**就成功（2026-10-09 那个 bug 的回归）"

New-Fake
$o = Run-Mgr -MgrArgs @("delete", "clitest") -Stdin "yes"
$gone = -not (Test-Path -LiteralPath $instFile)
$cancelled = $o -match "已取消"
if ($gone -and -not $cancelled) {
    Ok "喂一行 yes 就删掉了 —— 确认只提示了一次"
} else {
    Bad "喂了 yes 却没删掉（确认被提示了不止一次？）"
    Info ("实例还在：" + (Test-Path -LiteralPath $instFile) + "，输出里有'已取消'：" + $cancelled)
    ($o.Trim() -split "`n") | Select-Object -Last 3 | ForEach-Object { Info $_ }
}

Write-Host ""
Write-Host "── 4. -Yes 照常执行"

New-Fake
$o = Run-Mgr @("delete", "clitest", "-Yes")
if (-not (Test-Path -LiteralPath $instFile)) { Ok "-Yes 跳过确认并执行" } else { Bad "-Yes 没能执行删除"; Info (($o.Trim() -split "`n") | Select-Object -Last 1) }

# ── 收尾 ────────────────────────────────────────────────────────────────
Remove-Item -LiteralPath $sandbox -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ""
Write-Host ("  通过 $pass，失败 $fail")
if ($fail -gt 0) { exit 1 }
exit 0
