<#
.SYNOPSIS
  从 Linux 构建机把自编 ROM 的镜像拉到 Windows，并校验完整性。

.DESCRIPTION
  默认从**打包目录**取（dev/04-android-rom/artifacts/rom-remote_control_x64_arm64/）——它由
  scripts/package-rom.sh 生成，文件集固定、带 SHA256SUMS 与 MANIFEST.txt。
  也可以 -RemoteDir 指到构建产物目录（out/target/product/remote_control_x64_arm64）。

  同名同大小默认跳过，可反复执行。
#>
[CmdletBinding()]
param(
    [string]$Remote    = "root@192.168.0.108",
    [string]$RemoteDir = "/root/AutoSnapshotAndroid/dev/04-android-rom/artifacts/rom-remote_control_x64_arm64",
    [string]$Dest      = "",
    [switch]$Force,
    [switch]$SkipVerify
)

# ⚠️ **不要**把 `$PSScriptRoot` 写在 param 的默认值里。
#    带 `[CmdletBinding()]` 时参数绑定发生在 `$PSScriptRoot` 赋值**之前**，
#    默认值会算成 `\images` 这种丢掉了盘符和目录的路径，而且**不报错**。
#    2026-10-09 实测（同一台机器，只改这一处）：
#        param([string]$X = "$PSScriptRoot\images")            → [\images]   ✗
#        [CmdletBinding()] param([string]$X = "$PSScriptRoot\images") → [\images] ✗
#    于是镜像明明就在 `windows\images\` 里，预检却逐条报"缺 xxx.img"。
#    正确写法：默认值留空，绑定之后再补：
if (-not $Dest) { $Dest = Join-Path $PSScriptRoot "images" }

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# 打包目录里已有 system\build.prop；若指向构建产物目录，则 system\ 下文件更多，一并带上
# ⚠️ 必需的是 **-qemu 家族**：
#   system/vendor/product-qemu.img = 带 GPT 分区表的包装版（模拟器靠它做 /dev/block/by-name/*）
#   ramdisk-qemu.img = (cat ramdisk.img vendor_ramdisk.img) 合并版（first-stage 的 fstab 在里面）
# 只拉裸 ext4 的 *.img，模拟器会以 "partition(s) not found in /sys" / "failed to find device
# default fstab" 卡死并无限重启（本地实测，见 docs/02-build-traps.md 坑 8）。
$required = @("system-qemu.img", "vendor-qemu.img", "product-qemu.img", "ramdisk-qemu.img",
              "kernel-ranchu", "encryptionkey.img", "userdata.img",
              "advancedFeatures.ini", "config.ini")
$optional = @("system_ext-qemu.img", "initrd", "system.img", "vendor.img", "product.img",
              "ramdisk.img", "build.prop", "source.properties",
              "dtb.img", "vbmeta.img", "SHA256SUMS", "MANIFEST.txt")

# 跑一条远端命令，把 stdout 当字符串返回。
#
# ⚠️ **不要**写成 `(ssh $Remote "...") -join ""` —— 在 Windows 上那条路会**挂住**。
#    实测（Windows 10 + OpenSSH for Windows + PowerShell 5.1）：
#        ssh host echo hi                   → 0.33 秒返回
#        $r = (ssh host echo hi) -join ""   → **永远不返回**
#        $r = (cmd /c echo hi) -join ""     → 正常（所以不是"捕获"本身的问题）
#    `ssh -n`（不读 stdin）也照样挂。换成 Start-Process + 重定向到文件就正常。
#
#    这个坑特别隐蔽：脚本打印完第一行"==> 远端：…"之后再也不动，
#    看起来像"网络慢"或"在拉大文件"，实际上是卡在第一个 stat 上。
function Invoke-Remote([string]$cmdline) {
    $tmp = Join-Path $env:TEMP ("rc-remote-" + [guid]::NewGuid().ToString("N") + ".txt")
    $err = "$tmp.err"
    try {
        Start-Process -FilePath "ssh" -ArgumentList @($Remote, $cmdline) `
            -NoNewWindow -Wait -RedirectStandardOutput $tmp `
            -RedirectStandardError $err | Out-Null
        if (Test-Path $tmp) {
            $s = Get-Content $tmp -Raw -ErrorAction SilentlyContinue
            if ($null -ne $s) { return $s }
        }
        return ""
    } finally {
        Remove-Item $tmp, $err -ErrorAction SilentlyContinue
    }
}

function Remote-Size([string]$rel) {
    return (Invoke-Remote "stat -c %s '$RemoteDir/$rel' 2>/dev/null").Trim()
}

Write-Host "==> 远端：$Remote`:$RemoteDir" -ForegroundColor Cyan
$todo = @(); $missingRequired = @()
foreach ($f in ($required + $optional)) {
    $local = Join-Path $Dest $f
    $rsize = (Remote-Size $f).Trim()
    if (-not $rsize) {
        if ($required -contains $f) { $missingRequired += $f }
        continue
    }
    if ((Test-Path $local) -and -not $Force -and ([int64]$rsize -eq (Get-Item $local).Length)) {
        Write-Host ("    skip {0} ({1:N0} bytes)" -f $f, (Get-Item $local).Length)
        continue
    }
    # ⚠️ 必须带 **$Remote`:** 前缀。原来只写 "$RemoteDir/$f"，于是 scp 拿到的是
    #    "/root/.../system-qemu.img" 这种**本地**路径 —— 每个文件报一次
    #    "The system cannot find the path specified."，然后整体退出码 1。
    #    也就是说这个脚本在任何机器上都拉不到东西（2026-10-08 在 Windows 上实测）。
    $todo += "$Remote`:$RemoteDir/$f"
}
if ($missingRequired.Count -gt 0) {
    throw ("远端缺必需镜像：{0}（先跑构建与打包： scripts/build-rom.sh; scripts/package-rom.sh）" -f ($missingRequired -join ', '))
}
if ($todo.Count -gt 0) {
    Write-Host ("==> 拉取 {0} 个文件（可能几 GB，耐心等）" -f $todo.Count) -ForegroundColor Cyan
    & scp @($todo + $Dest)
    if ($LASTEXITCODE -ne 0) { throw "scp 失败（$LASTEXITCODE）" }
}

# ── 子目录里的文件：**按 SHA256SUMS 清单补齐** ──
#
# 原来这里是硬编码的"补 system\build.prop"一条，而清单里还有
# `vendor/build.prop`（模拟器同样要读它）—— 漏了它，校验阶段就报
# "缺文件：vendor\build.prop"。**这正是 SHA256SUMS 校验存在的意义**：
# 清单是权威，手工维护的下载列表一定会落后于它。
#
# 改成读清单：凡带子目录的条目，本地没有就拉。以后打包多带什么文件，
# 这里不用再改。
if (Test-Path "$Dest\SHA256SUMS") {
    $subTodo = @()
    foreach ($line in Get-Content "$Dest\SHA256SUMS") {
        if ($line -notmatch '^\s*[0-9a-fA-F]{64}\s+\*?\./(.+)$') { continue }
        $rel = $Matches[1] -replace '/', '\'
        if ($rel -notmatch '\\') { continue }        # 顶层文件已由上面两个清单管
        if (Test-Path (Join-Path $Dest $rel)) { continue }
        $subTodo += $rel
    }
    foreach ($rel in $subTodo) {
        $relRemote = $rel -replace '\\', '/'
        $localPath = Join-Path $Dest $rel
        New-Item -ItemType Directory -Force -Path (Split-Path $localPath) | Out-Null
        Write-Host "==> 补 $rel（按 SHA256SUMS 清单）" -ForegroundColor Cyan
        & scp "$Remote`:$RemoteDir/$relRemote" $localPath
        if ($LASTEXITCODE -ne 0) { throw "scp 失败（$rel）" }
    }
}

# initrd：模拟器 -initrd 指向它
if ((-not (Test-Path "$Dest\initrd")) -and (Test-Path "$Dest\ramdisk.img")) {
    Copy-Item "$Dest\ramdisk.img" "$Dest\initrd" -Force
    Write-Host "==> 已由 ramdisk.img 生成 initrd"
}

# 自建 arm64 探针 APK（Linux 侧 tools/build-probe-apk.sh 产出）
$probeRemote = "/root/AutoSnapshotAndroid/dev/04-android-rom/artifacts/arm64-probe.apk"
$probeLocal  = Join-Path $Dest "arm64-probe.apk"
# ⚠️ 原来这里把 Remote-Size 调了两遍、还内联了一次 ssh 捕获 ——
#    同一个值问两遍既慢又容易两次得到不同结果，内联那次还会挂住。
$probeInDir = (Remote-Size "arm64-probe.apk")
$probeAlt   = (Invoke-Remote "stat -c %s '$probeRemote' 2>/dev/null").Trim()
# ⚠️ 比的是**字节数**不是"有没有这个字符串"：PowerShell 里 "0" 是真值，
#    远端那个 0 字节的占位文件会被当成"有"，于是拉回来一个空包。
if (([int]$probeInDir -gt 0) -or ([int]$probeAlt -gt 0)) {
    $src = if ($probeInDir) { "$RemoteDir/arm64-probe.apk" } else { $probeRemote }
    if ((-not (Test-Path $probeLocal)) -or $Force) {
        Write-Host "==> 拉取自建 arm64 探针 APK"
        & scp "$Remote`:$src" "$Dest"
    }
}

# ---------------------------------------------------------------- 校验
if (-not $SkipVerify -and (Test-Path "$Dest\SHA256SUMS")) {
    Write-Host "==> 按 SHA256SUMS 校验" -ForegroundColor Cyan
    $bad = 0; $checked = 0
    foreach ($line in Get-Content "$Dest\SHA256SUMS") {
        if ($line -notmatch '^([0-9a-f]{64})\s+\*?\./(.+)$') { continue }
        $want = $Matches[1]; $rel = $Matches[2] -replace '/', '\'
        $p = Join-Path $Dest $rel
        if (-not (Test-Path $p)) { Write-Warning "缺文件：$rel"; $bad++; continue }
        $got = (Get-FileHash $p -Algorithm SHA256).Hash.ToLower()
        $checked++
        if ($got -ne $want) { Write-Host ("    [!!] {0} 校验不符" -f $rel) -ForegroundColor Red; $bad++ }
    }
    if ($bad -eq 0) { Write-Host ("==> 校验通过 ✓（{0} 个文件）" -f $checked) -ForegroundColor Green }
    else { throw ("校验失败：{0} 个文件有问题" -f $bad) }
}

Write-Host "==> 完成。清单：" -ForegroundColor Green
Get-ChildItem $Dest -File | Sort-Object Name |
    Format-Table Name, @{N='MB';E={[math]::Round($_.Length/1MB,1)}} -AutoSize

$missing = $required | Where-Object { -not (Test-Path (Join-Path $Dest $_)) }
if ($missing) { Write-Warning ("缺少：{0}" -f ($missing -join ', ')) }
else { Write-Host "==> 必需文件齐全 ✓" -ForegroundColor Green }
