# =============================================================================
# 实例归档：export / import 的**共享实现**（Windows 侧）。
#
# 与 Linux 侧 `packaging/bin/linux/archive.sh` **同一份契约**：
# 归档格式两边一致，所以**跨平台互认** —— Linux 导出的能在 Windows 恢复，反之亦然。
# 改格式要**两边一起改**，并且要跑 tests/verify-export-import 的对应检查。
#
# 归档格式（契约）
# ----------------
#   ./INSTANCE-MANIFEST.json    清单：format / dataFile / instance / 镜像列表 …
#   ./host-service.env          服务属性（端口/鉴权/…）
#   ./host-service.token        服务令牌
#   ./sysdir/…  ./datadir/…     实例状态
#
#   format 1：数据镜像**原样搬**（backing + 覆盖层）。恢复时**自动压平迁移**
#             （读穿链写出单张 raw、删掉覆盖层）。历史原因见 docs/12 §7：
#             不迁移的话 guest 读不到数据（文件逐字节忠实，起来之后 /data 却空）。
#   format 2：数据镜像压平成**一张自包含** qcow2（`dataFile`），恢复时转回 raw。
#             **已验证：数据完好。**
#
# 接口约定（与 Linux 侧刻意保持一致）
# -----------------------------------
# 只依赖：调用方 dot-source 的 `common.ps1`（Write-Log / Write-Warn / Die /
# Get-SysDir / Get-DataDir 等），以及**显式传进来的路径** —— 不去猜调用方的命名。
# 可选钩子：调用方定义 `Add-ArchiveExtraStage`，本文件会在打包前调它摆服务元数据。
#
# 与 Linux 侧的一处**实现差异**（不是语义差异）：
#   Windows 自带的是 **bsdtar**，没有 GNU tar 的 `--transform`，但有自己的
#   `-s` 替换语法；稀疏由 `--sparse` 处理，两边都支持。
#   空间检查用的是 `Get-AllocatedFileBytes`（storage.ps1）—— **实占**而不是表观，
#   理由和 Linux 侧那段注释一样：表观是 64G，拿它比剩余空间会要求 64G 空闲。
# =============================================================================

# ⚠️ 这几个 `$script:` 变量**必须先初始化**：管理器开了 Set-StrictMode，
#    读一个没赋过值的变量会直接抛错（不是当 $null）。2026-10-09 实测踩到。
$script:ArchiveListing = @()
$script:ArchiveListingFor = ""
$script:ArchiveMf = ""
$script:ArchiveFmt = 1
$script:ArchiveNeedMb = 0
$script:ArchiveImages = @()
$script:ArchiveExportName = ""
$script:ArchiveProduct = "unknown"
$script:ArchiveRomFp = "unknown"

$ArchiveManifestName = "INSTANCE-MANIFEST.json"
$ArchiveDataFlat     = "userdata-flat.qcow2"
$ArchDataImg         = "userdata-qemu.img"
$ArchDataOvl         = "userdata-qemu.img.qcow2"

# 不进归档的东西：运行期/路径相关/锁/配置（config.ini 由调用方按本机真源重写）
function Test-ArchiveExcluded {
    param([string]$Name)
    switch -Regex ($Name) {
        '^(hardware-qemu\.ini|hardware-qemu\.ini\.lock|multiinstance\.lock|emu-launch-params\.txt|bootcompleted\.ini|version_num\.cache|config\.ini)$' { return $true }
        '\.qcow2\.lock$' { return $true }
        default { return $false }
    }
}

function Get-ArchiveTar {
    # 系统自带（Win10 1803+ 的 System32）。找不到就明确报，别装作能干活。
    foreach ($n in @("tar.exe", "tar")) {
        $m = @(Get-Command $n -CommandType Application -ErrorAction SilentlyContinue)
        if ($m.Count -gt 0) { return $m[0].Source }
    }
    return $null
}

function Get-ArchiveQemuImg {
    # 与 Linux 侧同布局：**运行时目录**下的 `emulator\qemu-img.exe`
    # （模拟器二进制也在那儿：`$script:Emulator` = runtime\emulator\emulator.exe）。
    #
    # ⚠️ **不要用 `Get-EmulatorHome`** —— 它返回的是**用户配置目录**
    #    （`%USERPROFILE%\.android`），不是"模拟器装在哪"。
    #    2026-10-09 读出来才发现：用它找 qemu-img 会去 `C:\Users\<你>\.android` 找，
    #    当然找不到。当时还没跑到数据路径，是**读代码**发现的。
    if ($env:AUTOSNAP_QEMU_IMG) { return $env:AUTOSNAP_QEMU_IMG }
    # ⚠️ `$script:Runtime` 只有**交付版**的 common.ps1 会定义；仓库侧的
    #    windows\emulator.ps1 有自己的目录变量。管理器开了 Set-StrictMode，
    #    直接读一个没定义的 $script: 变量会**抛错**而不是当 $null ——
    #    所以这里先问"有没有这个变量"。（与 Linux 侧 `${QEMU_IMG_BIN:-}` 同理。）
    $runtime = $null
    if (Get-Variable -Name Runtime -Scope Script -ErrorAction SilentlyContinue) { $runtime = $script:Runtime }
    if (-not $runtime -and $env:AUTOSNAP_RUNTIME) { $runtime = $env:AUTOSNAP_RUNTIME }
    if (-not $runtime -and $env:AUTOSNAP_EMULATOR) {
        $runtime = Split-Path -Parent (Split-Path -Parent $env:AUTOSNAP_EMULATOR)
    }
    if (-not $runtime) { return $null }
    foreach ($p in @((Join-Path $runtime "emulator\qemu-img.exe"),
                     (Join-Path $runtime "emulator\qemu-img"))) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

function Get-ArchiveListing {
    # 归档成员名单（缓存）。**必须先用它判断成员在不在** ——
    # 直接对不存在的成员跑 `tar -xOf` 会往 stderr 写一行，
    # 而 PowerShell 会把原生命令的 stderr 当**终止错误**，把整个导入打断。
    # 2026-10-09 在真 Windows 上实测踩到（导出的实例没有令牌文件时）。
    param([string]$Archive)
    if ($script:ArchiveListingFor -eq $Archive -and $script:ArchiveListing) { return $script:ArchiveListing }
    $tar = Get-ArchiveTar
    if (-not $tar) { Die "需要系统 tar.exe" }
    $script:ArchiveListing = @(& $tar -tf $Archive 2>$null | ForEach-Object { ([string]$_).Trim() })
    $script:ArchiveListingFor = $Archive
    return $script:ArchiveListing
}

function Get-ArchiveMember {
    # 读出归档里某个成员的内容（不落盘）。成员不在就返回空 —— 不报错、不打断。
    param([string]$Archive, [string]$Member)
    $tar = Get-ArchiveTar
    if (-not $tar) { Die "需要系统 tar.exe" }
    $listing = Get-ArchiveListing -Archive $Archive
    foreach ($candidate in @("./$Member", $Member)) {
        if ($listing -notcontains $candidate) { continue }
        $lines = @(& $tar -xOf $Archive $candidate 2>$null)
        if ($LASTEXITCODE -eq 0 -and $lines.Count -gt 0) { return ($lines -join "`n") }
    }
    return ""
}

function Get-ArchiveField {
    # 从清单正文里取一个字符串字段（清单是我们自己写的，形状稳定）
    param([string]$Manifest, [string]$Field)
    $m = [regex]::Match($Manifest, '"' + [regex]::Escape($Field) + '"\s*:\s*"([^"]*)"')
    if ($m.Success) { return $m.Groups[1].Value }
    return ""
}

function Get-ArchiveNumberField {
    param([string]$Manifest, [string]$Field)
    $m = [regex]::Match($Manifest, '"' + [regex]::Escape($Field) + '"\s*:\s*(\d+)')
    if ($m.Success) { return [int64]$m.Groups[1].Value }
    return 0
}

function Get-DirAllocatedBytes {
    # 目录的**实占**合计（稀疏空洞不算）—— 空间检查与 dirBytes 都用它
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return [int64]0 }
    $sum = [int64]0
    foreach ($f in Get-ChildItem -LiteralPath $Path -File -Recurse -Force -ErrorAction SilentlyContinue) {
        $alloc = Get-AllocatedFileBytes -Path $f.FullName
        if ($alloc -gt 0) { $sum += $alloc } else { $sum += [int64]$f.Length }
    }
    return $sum
}

# ---------------------------------------------------------------------------
# Export-InstanceArchive -Name <名> -SysDir <目录> -DataDir <目录> -Out <归档> [-Force]
#
# 调用方在调用前设置（写进清单）：
#   $script:ArchiveProduct   产品名
#   $script:ArchiveRomFp     ROM 指纹
#   $script:ArchiveImages    镜像文件名数组
# 并可定义 Add-ArchiveExtraStage -Stage <暂存目录> 来摆服务属性/令牌。
# ---------------------------------------------------------------------------
function Export-InstanceArchive {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$SysDir,
        [Parameter(Mandatory)][string]$DataDir,
        [Parameter(Mandatory)][string]$Out,
        [switch]$Force
    )
    if (-not (Test-Path -LiteralPath $SysDir -PathType Container)) { Die "工作目录不存在：$SysDir" }
    if (Test-Path -LiteralPath $Out) {
        if (-not $Force) { Die "已存在：$Out（加 --force 覆盖）" }
    }
    $tar = Get-ArchiveTar
    if (-not $tar) { Die "export 需要系统 tar.exe（Win10 1803+ 自带）" }

    # 空间检查：归档 ≈ 实占（稀疏空洞不写）
    $need = (Get-DirAllocatedBytes -Path $SysDir) + (Get-DirAllocatedBytes -Path $DataDir)
    $needMb = [int64]($need / 1MB)
    $outDir = Split-Path -Parent $Out
    if (-not (Test-Path -LiteralPath $outDir)) { Die "目标目录不存在：$outDir" }
    $drive = (Get-Item -LiteralPath $outDir).PSDrive
    $freeMb = [int64]($drive.Free / 1MB)
    if ($freeMb -le ($needMb + 512)) {
        Die "空间不够：归档预计 ${needMb}MB，目标盘只剩 ${freeMb}MB"
    }

    $stage = Join-Path $outDir (".archive-stage-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path (Join-Path $stage "sysdir") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $stage "datadir") -Force | Out-Null

    $stateFiles = 0
    foreach ($f in Get-ChildItem -LiteralPath $SysDir -File -Force -ErrorAction SilentlyContinue) {
        $b = $f.Name
        # ⚠️ **符号链接不要导** —— 那些是指向 ROM 镜像的（system/vendor/userdata…）。
        #    Linux 侧同样跳过；不跳的话真 ROM 上归档会白胖 3G，而恢复时
        #    本来就是"按清单里的名字重链本地那份"。2026-10-09 实测抓到：
        #    第一版把它们当普通文件打进去了（哑镜像时看不出来，真镜像就是 3G）。
        if ($f.LinkType) { continue }
        if (Test-ArchiveExcluded -Name $b) { continue }
        if ($b -eq $ArchDataImg -or $b -eq $ArchDataOvl) { continue }   # 走压平路线
        Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $stage "sysdir\$b") -Force
        $stateFiles++
    }
    if (Test-Path -LiteralPath $DataDir -PathType Container) {
        foreach ($f in Get-ChildItem -LiteralPath $DataDir -File -Force -ErrorAction SilentlyContinue) {
            Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $stage "datadir\$($f.Name)") -Force
        }
    }

    # 压平数据镜像：以覆盖层为入口（读穿 backing 链）；没有覆盖层就从 backing 压
    $qi = Get-ArchiveQemuImg
    $src = $null
    if (Test-Path -LiteralPath (Join-Path $SysDir $ArchDataOvl)) { $src = Join-Path $SysDir $ArchDataOvl }
    elseif (Test-Path -LiteralPath (Join-Path $SysDir $ArchDataImg)) { $src = Join-Path $SysDir $ArchDataImg }
    $haveData = 0
    if ($src) {
        if (-not $qi) { Remove-Item -LiteralPath $stage -Recurse -Force; Die "找不到 qemu-img：$($script:Runtime)\emulator\qemu-img.exe（包里的运行时没带？）" }
        Write-Log "压平数据镜像（$ArchiveDataFlat）…"
        & $qi convert -O qcow2 -c $src (Join-Path $stage "sysdir\$ArchiveDataFlat") 2>$null
        if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $stage -Recurse -Force; Die "qemu-img convert 失败：$src" }
        $haveData = 1
    } else {
        # 没有数据镜像不是错：从没启动过的实例、最小实例都这样。
        Write-Warn "这个实例没有数据镜像（$ArchDataImg／$ArchDataOvl 都不在）—— 归档里不会有数据"
    }

    # 可选钩子：调用方摆服务属性/令牌（各家来源不同）。不定义也合法。
    if (Get-Command Add-ArchiveExtraStage -ErrorAction SilentlyContinue) {
        Add-ArchiveExtraStage -Stage $stage
    }

    $bytes = (Get-DirAllocatedBytes -Path $stage)
    $ts = (Get-Date).ToString("yyyy-MM-ddTHH:mm:sszzz")
    $imagesJson = ""
    if ($script:ArchiveImages) {
        $imagesJson = (@($script:ArchiveImages) | ForEach-Object { '"' + $_ + '"' }) -join ","
    }
    $manifest = @()
    $manifest += "{"
    $manifest += "  `"format`": 2,"
    if ($haveData -eq 1) { $manifest += "  `"dataFile`": `"$ArchiveDataFlat`"," }
    $manifest += "  `"instance`": `"$Name`","
    $manifest += "  `"exportedAt`": `"$ts`","
    $manifest += "  `"product`": `"$($script:ArchiveProduct)`","
    $manifest += "  `"romFingerprint`": `"$($script:ArchiveRomFp)`","
    $manifest += "  `"imageFiles`": [$imagesJson],"
    $manifest += "  `"stateFiles`": $stateFiles,"
    $manifest += "  `"dirBytes`": $bytes"
    $manifest += "}"
    Set-Content -LiteralPath (Join-Path $stage $ArchiveManifestName) -Value ($manifest -join "`n") -Encoding UTF8

    Write-Log "打包（状态实占约 $([int64]($need / 1GB))G；稀疏空洞不写）"
    Push-Location $stage
    try {
        # ⚠️ **不要传 `--sparse` / `--numeric-owner`** —— Windows 自带的是 bsdtar，
        #    它不认这些 GNU tar 的选项，会直接报 "Option --sparse is not supported"
        #    然后打包失败。2026-10-09 在真 Windows 上实测踩到（本机没有 PowerShell，
        #    写完只验了语法，是端到端那一步把它抓出来的）。
        #    bsdtar 默认就会**识别并稀疏地存**稀疏文件，本来也不需要那个开关。
        & $tar -cf $Out .
        $tarOk = ($LASTEXITCODE -eq 0)
    } finally { Pop-Location }
    if (-not $tarOk) { Remove-Item -LiteralPath $stage -Recurse -Force; Die "tar 失败" }
    Remove-Item -LiteralPath $stage -Recurse -Force

    # 读回确认 —— tar 说成功不等于归档能用
    $listed = @(& $tar -tf $Out 2>$null)
    if ($listed.Count -eq 0) { Die "归档读不回来（tar -tf 是空的）：$Out" }
    if (-not ($listed -match [regex]::Escape($ArchiveManifestName))) { Die "归档里没有 $ArchiveManifestName" }

    $script:ArchiveLast = $Out
    $script:ArchiveLastEntries = $listed.Count
    $script:ArchiveLastBytes = $bytes
    $script:ArchiveLastHasData = $haveData
}

# ---------------------------------------------------------------------------
# Test-ArchivePreflight -Archive <归档>
# 只读检查（格式闸 + 空间），**必须在注册实例之前调** —— 否则被拒的导入会留下半个实例。
# 结果放在 $script:ArchiveMf / ArchiveFmt / ArchiveNeedMb。
# ---------------------------------------------------------------------------
function Test-ArchivePreflight {
    param([Parameter(Mandatory)][string]$Archive, [switch]$Unsafe)
    if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) { Die "没有这个归档：$Archive" }
    if ((Get-Item -LiteralPath $Archive).Length -eq 0) { Die "归档是空文件：$Archive" }

    $mf = Get-ArchiveMember -Archive $Archive -Member $ArchiveManifestName
    if ([string]::IsNullOrWhiteSpace($mf)) { Die "归档里没有 $ArchiveManifestName —— 不是本工具导出的？" }
    $fmt = Get-ArchiveNumberField -Manifest $mf -Field "format"
    if ($fmt -lt 1) { $fmt = 1 }        # 没有 format 字段 = 老归档 = 当作 1
    if ($fmt -lt 2) {
        Write-Log "格式 1 的老归档：恢复时会自动压平（backing+覆盖层 → 单张 raw）"
    }
    $script:ArchiveMf = $mf
    $script:ArchiveFmt = $fmt
    $script:ArchiveNeedMb = [int64]((Get-ArchiveNumberField -Manifest $mf -Field "dirBytes") / 1MB)
}

# ---------------------------------------------------------------------------
# Import-InstanceArchive -Archive <归档> -SysDir <目录> -DataDir <目录> -Name <名> [-Unsafe]
#
# 调用方负责：名字不重名、端口已选好并注册、服务属性/令牌的落盘、config.ini 重写。
# 这里只做：读清单 → 格式闸 → 解包 → 还原数据 → 清掉运行期文件。
# ---------------------------------------------------------------------------
function Import-InstanceArchive {
    param(
        [Parameter(Mandatory)][string]$Archive,
        [Parameter(Mandatory)][string]$SysDir,
        [Parameter(Mandatory)][string]$DataDir,
        [Parameter(Mandatory)][string]$Name,
        [switch]$Unsafe
    )
    Test-ArchivePreflight -Archive $Archive
    $mf = $script:ArchiveMf; $fmt = $script:ArchiveFmt; $needMb = $script:ArchiveNeedMb
    $tar = Get-ArchiveTar
    if (-not $tar) { Die "import 需要系统 tar.exe（Win10 1803+ 自带）" }

    $parent = Split-Path -Parent $SysDir
    $drive = (Get-Item -LiteralPath $parent).PSDrive
    $freeMb = [int64]($drive.Free / 1MB)
    if ($freeMb -le ($needMb + 512)) { Die "空间不够：需要约 ${needMb}MB，只剩 ${freeMb}MB" }

    New-Item -ItemType Directory -Path $SysDir -Force | Out-Null
    New-Item -ItemType Directory -Path $DataDir -Force | Out-Null

    $port = ($SysDir -split '-')[-1]
    Write-Log "解包 → sysdir-$port（约 $([int64]($needMb / 1024))G）"
    # ⚠️ Windows 自带的 bsdtar **不支持 `-s` 替换语法**（也不支持 GNU tar 的
    #    `--transform`），实测报 "-s is not supported by this version of bsdtar"。
    #    所以老老实实解到父目录下的临时目录，再 **Move-Item 改名就位** ——
    #    同卷改名是瞬时的，不会把 64GiB 的稀疏镜像复制一遍。
    #    另外：原生命令往 stderr 写一行会被 PowerShell 当**终止错误**，
    #    所以这里把所有流都吃掉（`*>&1 | Out-Null`），成败只认 $LASTEXITCODE。
    $tmp = Join-Path $parent (".import-tmp-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    & $tar -xf $Archive -C $tmp *>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue; Die "tar 解包失败：$Archive" }

    if (Test-Path -LiteralPath (Join-Path $tmp "sysdir")) {
        if (Test-Path -LiteralPath $SysDir) { Remove-Item -LiteralPath $SysDir -Recurse -Force }
        Move-Item -LiteralPath (Join-Path $tmp "sysdir") -Destination $SysDir -Force
    }
    if (Test-Path -LiteralPath (Join-Path $tmp "datadir")) {
        if (Test-Path -LiteralPath $DataDir) { Remove-Item -LiteralPath $DataDir -Recurse -Force }
        Move-Item -LiteralPath (Join-Path $tmp "datadir") -Destination $DataDir -Force
    }
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue

    Remove-Item -LiteralPath (Join-Path $SysDir $ArchiveManifestName) -Force -ErrorAction SilentlyContinue

    # ── 格式 2：把压平镜像转回 raw 当 backing
    $dfile = Get-ArchiveField -Manifest $mf -Field "dataFile"
    if ([string]::IsNullOrWhiteSpace($dfile)) { $dfile = $ArchiveDataFlat }
    $hasDfile = ($mf -match '"dataFile"')
    $qi = Get-ArchiveQemuImg
    if ($fmt -ge 2 -and $hasDfile) {
        $flat = Join-Path $SysDir $dfile
        if (-not (Test-Path -LiteralPath $flat)) { Die "归档是格式 2，但里面没有 $dfile —— 归档不完整？" }
        if (-not $qi) { Die "找不到 qemu-img：$($script:Runtime)\emulator\qemu-img.exe（包里的运行时没带？）" }
        Write-Log "还原数据镜像（$dfile → $ArchDataImg）…"
        Remove-Item -LiteralPath (Join-Path $SysDir $ArchDataImg) -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $SysDir $ArchDataOvl) -Force -ErrorAction SilentlyContinue
        & $qi convert -O raw $flat (Join-Path $SysDir $ArchDataImg) 2>$null
        if ($LASTEXITCODE -ne 0) { Die "qemu-img convert 失败：$flat" }
        Remove-Item -LiteralPath $flat -Force
    }

    # ── 格式 1：归档里是"原样搬的一对文件"。就地迁移成单张 raw（实测：不迁移读不到数据）
    $ovl = Join-Path $SysDir $ArchDataOvl
    if ($fmt -lt 2 -and (Test-Path -LiteralPath $ovl)) {
        if (-not $qi) { Die "找不到 qemu-img：$($script:Runtime)\emulator\qemu-img.exe（包里的运行时没带？）" }
        Write-Log "格式 1：压平 backing+覆盖层（老归档这样才读得出数据）…"
        $flatTmp = Join-Path $SysDir (".flat-raw-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
        & $qi convert -O raw $ovl $flatTmp 2>$null
        if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $flatTmp -Force -ErrorAction SilentlyContinue; Die "qemu-img convert 失败：$ovl" }
        Move-Item -LiteralPath $flatTmp -Destination (Join-Path $SysDir $ArchDataImg) -Force
        Remove-Item -LiteralPath $ovl -Force
    }

    # 清掉运行期/路径相关的东西（config.ini 由调用方按本机真源重写）
    foreach ($n in @("hardware-qemu.ini", "hardware-qemu.ini.lock", "multiinstance.lock",
                     "emu-launch-params.txt", "bootcompleted.ini", "version_num.cache")) {
        Remove-Item -LiteralPath (Join-Path $SysDir $n) -Force -ErrorAction SilentlyContinue
    }
    Get-ChildItem -LiteralPath $SysDir -Filter "*.qcow2.lock" -Force -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue

    $img = Join-Path $SysDir $ArchDataImg
    if (-not (Test-Path -LiteralPath $img) -or (Get-Item -LiteralPath $img).Length -eq 0) {
        if ($fmt -ge 2 -and $hasDfile) { Die "格式 2 的归档恢复后没有 $ArchDataImg —— 不该发生" }
        Write-Warn "归档里没有数据镜像（导出时实例就没有）"
    }

    $script:ArchiveImages = @()
    $im = [regex]::Match($mf, '"imageFiles"\s*:\s*\[([^\]]*)\]')
    if ($im.Success) {
        $script:ArchiveImages = @($im.Groups[1].Value -split ',' |
            ForEach-Object { $_.Trim().Trim('"') } | Where-Object { $_ })
    }
}
