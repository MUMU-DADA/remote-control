<#
.SYNOPSIS
  Unified command line for managing the Windows release package emulator.

.EXAMPLE
  .\bin\emulator.ps1 start
  .\bin\emulator.ps1 create test -Port 5582
  .\bin\emulator.ps1 clone default test2 -Port 5584
  .\bin\emulator.ps1 delete test2 -Yes
#>
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")

function Show-Usage {
    Write-Host @"
用法：.\bin\emulator.ps1 <命令> [实例名] [选项]

命令：
  create [名称]       创建实例（默认 default）
  start [名称]        启动；已有实例默认保留数据
  stop [名称]         请求 Android 正常关机
  kill [名称]         强制关机，可能丢失未保存数据
  restart [名称]      正常关机后保留数据重启
  status [名称]       查看实例和 ROM 状态
  list                查看全部实例
  verify [名称]       验收 ROM、服务和 ARM64 应用支持
  inspect <归档>      只读查看实例归档清单与内容
  reset [名称]        清空数据分区和快照，保留实例
  clone <源> <新名称> 复制实例状态与已安装应用
  delete [名称]       停止并删除实例
  help                显示本页（也可用 -?）

常用选项：
  -Port N             指定偶数 console 端口（默认从 5580 自动分配）
  -Gpu MODE           覆盖 GPU 模式
  -Memory MB          覆盖内存；-Cores N 覆盖 CPU 核数
  -NoWait -Gui -Reuse -WipeData -TestInstance -NoAccel
  -TimeoutSec N       停止超时秒数（默认 60）
  -Force              允许停止失败后强制断电
  -Yes                跳过 reset/delete 的确认

名称省略时使用 default。破坏性命令默认要求输入 yes。
示例：.\bin\emulator.ps1 start test -Port 5584 -NoWait
"@
}

function Assert-InstanceName {
    param([string]$Value, [string]$Label = "实例名")
    if ([string]::IsNullOrWhiteSpace($Value) -or $Value -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -or $Value -in @(".", "..")) {
        Die "$Label 只能由字母、数字、点、下划线和连字符组成，且不能以点开头：'$Value'"
    }
}

function Assert-ConsolePort {
    param([int]$Value)
    if (($Value % 2) -ne 0) { Die "端口必须是偶数：$Value（console 口，ADB 口为 port+1）" }
    if ($Value -lt 5554 -or $Value -gt 65534) { Die "端口必须在 5554..65534 范围内：$Value" }
}

function Test-ReparsePoint {
    param([System.IO.FileSystemInfo]$Item)
    return (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
}

# Remove links as links and recurse only through real directories. This matters
# because Build-SysDir uses junctions for shared, read-only image directories.
function Remove-TreeEntry {
    param([string]$Path)
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if ($null -eq $item) { return }
    if (Test-ReparsePoint $item) {
        if ($item.PSIsContainer) { [System.IO.Directory]::Delete($item.FullName, $false) }
        else { [System.IO.File]::Delete($item.FullName) }
        return
    }
    if ($item.PSIsContainer) {
        foreach ($child in @(Get-ChildItem -LiteralPath $item.FullName -Force)) {
            Remove-TreeEntry $child.FullName
        }
        [System.IO.Directory]::Delete($item.FullName, $false)
    } else {
        [System.IO.File]::Delete($item.FullName)
    }
}

function Copy-TreeWithoutReparsePoints {
    param([string]$Source, [string]$Destination)
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($child in @(Get-ChildItem -LiteralPath $Source -Force)) {
        if (Test-ReparsePoint $child) { continue }
        $target = Join-Path $Destination $child.Name
        if ($child.PSIsContainer) {
            Copy-TreeWithoutReparsePoints $child.FullName $target
        } else {
            Copy-Item -LiteralPath $child.FullName -Destination $target -Force -ErrorAction Stop
        }
    }
}

function Confirm-DestructiveAction {
    param([string]$Action, [string]$Name, [switch]$Yes)
    if ($Yes) { return }
    Write-Warn "$Action '$Name' 会删除实例数据（已装应用、应用数据、/sdcard 与快照）"
    if ((Read-Host "输入 yes 继续") -ne "yes") { Die "已取消" }
}

function Invoke-InspectArchive {
    param([string]$ArchivePath)
    if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) {
        Die "归档不存在或不是文件：$ArchivePath"
    }
    $archive = (Resolve-Path -LiteralPath $ArchivePath).Path
    $tarMatches = @(Get-Command "tar.exe" -CommandType Application -ErrorAction SilentlyContinue)
    if ($tarMatches.Count -eq 0) { $tarMatches = @(Get-Command "tar" -CommandType Application -ErrorAction SilentlyContinue) }
    $tar = if ($tarMatches.Count -gt 0) { $tarMatches[0] } else { $null }
    if (-not $tar) { Die "inspect 需要系统 tar.exe" }

    $manifestLines = @(& $tar.Source -xOf $archive "./INSTANCE-MANIFEST.json" 2>$null)
    $tarExitCode = $LASTEXITCODE
    $manifest = $manifestLines -join "`n"
    if ($tarExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($manifest)) {
        $manifestLines = @(& $tar.Source -xOf $archive "INSTANCE-MANIFEST.json" 2>$null)
        $tarExitCode = $LASTEXITCODE
        $manifest = $manifestLines -join "`n"
    }
    if ($tarExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($manifest)) {
        Die "归档中没有 INSTANCE-MANIFEST.json，或归档无法读取：$archive"
    }

    $entries = @(& $tar.Source -tf $archive 2>$null)
    if ($LASTEXITCODE -ne 0) { Die "无法读取归档内容：$archive" }
    Write-Log "实例归档：$archive"
    foreach ($line in ($manifest -split "`r?`n")) { Write-Host "    $line" }
    Write-Log "归档内容（前 12 项）"
    foreach ($entry in @($entries | Select-Object -First 12)) { Write-Host "    $entry" }
}

function Assert-UniqueInstanceOwner {
    param([string]$Name)
    $port = Get-InstancePort $Name
    if ($port -le 0) { Die "实例 '$Name' 没登记过有效端口" }
    $owners = @(Get-InstanceNamesForPort $port)
    if ($owners.Count -ne 1 -or $owners[0] -ne $Name) {
        Die "实例 '$Name' 的端口 $port 没有唯一登记；拒绝修改实例数据"
    }
    return $port
}

function Stop-ForMutation {
    param([string]$Name, [int]$TimeoutSec, [switch]$Force)
    $port = Assert-UniqueInstanceOwner $Name
    if (Test-InstanceRunning $port) {
        Write-Log "先停止实例 '$Name'"
        if (-not (Stop-Instance $port $TimeoutSec -Force:$Force)) {
            Die "实例未停止，未修改数据；检查服务后重试，或显式使用 -Force"
        }
    }
}

function Set-CloneRegistration {
    param([string]$SourceName, [string]$TargetName, [int]$Port)
    $sourceFile = Get-InstanceFile $SourceName
    $targetFile = Get-InstanceFile $TargetName
    $lines = @("PORT=$Port", "HTTP_PORT=$(18088 + [int](($Port - $script:PortBase) / 2))")
    foreach ($line in @(Get-Content -LiteralPath $sourceFile)) {
        if ($line -match '^(PORT|HTTP_PORT)=') { continue }
        $lines += $line
    }
    Set-Content -LiteralPath $targetFile -Encoding UTF8 -Value $lines

    $sourceToken = Get-ServiceTokenFile $SourceName
    if (Test-Path -LiteralPath $sourceToken) {
        $sourceTokenItem = Get-Item -LiteralPath $sourceToken -Force
        if (Test-ReparsePoint $sourceTokenItem) {
            throw "源实例令牌是 reparse point，拒绝复制：$sourceToken"
        }
        $targetToken = Get-ServiceTokenFile $TargetName
        Copy-Item -LiteralPath $sourceToken -Destination $targetToken -Force -ErrorAction Stop
        Protect-ServiceTokenFile $targetToken
    } elseif ((Get-InstanceValue $SourceName "SERVICE_AUTH") -eq "1") {
        throw "源实例启用了服务鉴权，但令牌文件不存在：$sourceToken"
    }
}

function Invoke-Create {
    param([string]$Name, [int]$Port)
    if (Test-Instance $Name) { Die "实例 '$Name' 已存在（换个名字，或先 delete）" }
    if ($Port -le 0) { $Port = New-FreePort }
    Assert-ConsolePort $Port
    if (Test-PortTaken $Port) { Die "端口 $Port 已被其他实例占用" }
    $sysdir = Get-SysDir $Port
    $datadir = Get-DataDir $Port
    if ((Test-Path -LiteralPath $sysdir) -or (Test-Path -LiteralPath $datadir)) {
        Die "端口 $Port 的实例目录已存在但没有登记：$sysdir / $datadir；请先检查残留数据"
    }

    Register-Instance $Name $Port
    try {
        Build-SysDir $Port
        New-Item -ItemType Directory -Force -Path $datadir | Out-Null
    } catch {
        Remove-TreeEntry $sysdir
        Remove-TreeEntry $datadir
        Remove-Item -LiteralPath (Get-InstanceFile $Name) -Force -ErrorAction SilentlyContinue
        throw
    }

    $width = Get-ConfigValue "hw.lcd.width" "1280"
    $height = Get-ConfigValue "hw.lcd.height" "720"
    Write-Ok "已创建实例 '$Name'"
    Write-Host "    端口     $Port（adb -s $(Get-Serial $Port)）"
    Write-Host "    工作目录 $($script:RunDir)\sysdir-$Port"
    Write-Host ("    显示     {0}x{1} @{2}dpi" -f $width, $height, (Get-ConfigValue "hw.lcd.density" "320"))
    Write-Host ("    内存/核  {0} MB / {1} 核" -f (Get-ConfigValue "hw.ramSize" "6144"), (Get-ConfigValue "hw.cpu.ncore" "4"))
    Write-Host "    下一步   .\bin\emulator.ps1 start $Name"
}

function Invoke-Delete {
    param([string]$Name, [int]$TimeoutSec, [switch]$Force, [switch]$Yes)
    if (-not (Test-Instance $Name)) { Die "没有叫 '$Name' 的实例（运行 list 查看实例）" }
    Stop-ForMutation $Name $TimeoutSec -Force:$Force
    Confirm-DestructiveAction "删除实例" $Name -Yes:$Yes
    $port = Get-InstancePort $Name
    foreach ($path in @((Get-SysDir $port), (Get-DataDir $port), (Get-LogFile $port), (Get-ErrFile $port),
                        (Get-ServiceTokenFile $Name), (Get-InstanceFile $Name))) {
        Remove-TreeEntry $path
    }
    Write-Ok "已删除实例 '$Name'"
}

function Invoke-Clone {
    param([string]$SourceName, [string]$TargetName, [int]$Port, [int]$TimeoutSec, [switch]$Force)
    if (-not (Test-Instance $SourceName)) { Die "没有叫 '$SourceName' 的源实例（运行 list 查看实例）" }
    if (Test-Instance $TargetName) { Die "实例 '$TargetName' 已存在" }
    $sourcePort = Assert-UniqueInstanceOwner $SourceName
    $sourceSys = Get-SysDir $sourcePort
    $sourceData = Get-DataDir $sourcePort
    $sourceSysItem = Get-Item -LiteralPath $sourceSys -Force -ErrorAction SilentlyContinue
    if ($null -eq $sourceSysItem) { Die "源实例工作目录不存在：$sourceSys" }
    if (Test-ReparsePoint $sourceSysItem) { Die "源实例工作目录是 reparse point，拒绝复制：$sourceSys" }
    $sourceDataItem = Get-Item -LiteralPath $sourceData -Force -ErrorAction SilentlyContinue
    if ($sourceDataItem -and (Test-ReparsePoint $sourceDataItem)) {
        Die "源实例数据目录是 reparse point，拒绝复制：$sourceData"
    }
    Stop-ForMutation $SourceName $TimeoutSec -Force:$Force

    if ($Port -le 0) { $Port = New-FreePort }
    Assert-ConsolePort $Port
    if (Test-PortTaken $Port) { Die "端口 $Port 已被其他实例占用" }
    $targetSys = Get-SysDir $Port
    $targetData = Get-DataDir $Port
    if ((Test-Path -LiteralPath $targetSys) -or (Test-Path -LiteralPath $targetData) -or
        (Test-Path -LiteralPath (Get-InstanceFile $TargetName)) -or
        (Test-Path -LiteralPath (Get-ServiceTokenFile $TargetName))) {
        Die "目标实例路径已存在但没有登记：$targetSys / $targetData；请先检查残留数据"
    }

    $stale = @("hardware-qemu.ini", "hardware-qemu.ini.lock", "multiinstance.lock",
               "emu-launch-params.txt", "bootcompleted.ini", "version_num.cache",
               "userdata-qemu.img.qcow2.lock", "cache.img.qcow2.lock", "snapshots")
    $images = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($item in @(Get-ChildItem -LiteralPath $script:Images -Force)) { [void]$images.Add($item.Name) }

    try {
        Build-SysDir $Port
        Write-Log "复制实例状态（镜像 junction 与硬链接保持共享，不跟随复制）"
        foreach ($item in @(Get-ChildItem -LiteralPath $sourceSys -Force)) {
            if ($script:NoLink -contains $item.Name -or $stale -contains $item.Name -or $images.Contains($item.Name)) { continue }
            if (Test-ReparsePoint $item) { continue }
            $destination = Join-Path $targetSys $item.Name
            if ($item.PSIsContainer) { Copy-TreeWithoutReparsePoints $item.FullName $destination }
            else { Copy-Item -LiteralPath $item.FullName -Destination $destination -Force -ErrorAction Stop }
        }
        if (Test-Path -LiteralPath (Join-Path $sourceSys "snapshots")) {
            Write-Warn "源实例快照没有复制（快照绑定了旧硬件配置与路径）"
        }
        New-Item -ItemType Directory -Force -Path $targetData | Out-Null
        if (Test-Path -LiteralPath $sourceData) { Copy-TreeWithoutReparsePoints $sourceData $targetData }
        Set-CloneRegistration $SourceName $TargetName $Port
    } catch {
        $cloneError = $_
        foreach ($path in @($targetSys, $targetData, (Get-ServiceTokenFile $TargetName), (Get-InstanceFile $TargetName))) {
            Remove-TreeEntry $path
        }
        throw $cloneError
    }

    Write-Ok "已复制：'$SourceName' → '$TargetName'（端口 $Port）"
    Write-Host "    下一步   .\bin\emulator.ps1 start $TargetName"
}

$rawArgs = @($args)
if ($rawArgs.Count -eq 0) { Show-Usage; exit 0 }
$command = ([string]$rawArgs[0]).ToLowerInvariant()
$rawArgs = @($rawArgs | Select-Object -Skip 1)
if ($command -in @("help", "-h", "--help", "-?")) { Show-Usage; exit 0 }
if ($command -eq "ls") { $command = "list" }
$commands = @("create", "start", "stop", "kill", "restart", "status", "list", "verify", "inspect", "reset", "clone", "delete")
if ($commands -notcontains $command) { Die "未知命令：$command（运行 .\bin\emulator.ps1 help）" }

$valueAliases = @{
    "name" = "name"; "n" = "name"; "port" = "port"; "gpu" = "gpu"
    "memory" = "memory"; "cores" = "cores"; "timeout" = "timeoutsec"; "timeoutsec" = "timeoutsec"
}
$flagAliases = @{
    "yes" = "yes"; "y" = "yes"; "force" = "force"; "f" = "force"
    "reuse" = "reuse"; "wipedata" = "wipedata"; "wipe-data" = "wipedata"
    "nowait" = "nowait"; "no-wait" = "nowait"; "gui" = "gui"
    "testinstance" = "testinstance"; "test-instance" = "testinstance"
    "noaccel" = "noaccel"; "no-accel" = "noaccel"
}
$values = @{}
$flags = @{}
$positionals = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt $rawArgs.Count; $i++) {
    $token = [string]$rawArgs[$i]
    $optionMatch = [regex]::Match($token, '^--?([A-Za-z][A-Za-z0-9-]*)(?:[=:](.*))?$')
    if ($optionMatch.Success) {
        $option = $optionMatch.Groups[1].Value.ToLowerInvariant()
        $hasInlineValue = $optionMatch.Groups[2].Success
        $inlineValue = $optionMatch.Groups[2].Value
        if ($valueAliases.ContainsKey($option)) {
            $key = $valueAliases[$option]
            if ($hasInlineValue) { $value = $inlineValue }
            else {
                if ($i + 1 -ge $rawArgs.Count) { Die "-$option 后需要参数值" }
                $next = [string]$rawArgs[$i + 1]
                if ($next -match '^--?[A-Za-z][A-Za-z0-9-]*(?:[=:].*)?$') { Die "-$option 后缺少参数值（遇到选项 $next）" }
                $value = $next
                $i++
            }
            if ([string]::IsNullOrWhiteSpace($value)) { Die "-$option 的参数值不能为空" }
            if ($values.ContainsKey($key)) { Die "-$option 不能重复指定" }
            $values[$key] = $value
        } elseif ($flagAliases.ContainsKey($option)) {
            if ($hasInlineValue) { Die "-$option 不接受参数值" }
            $flags[$flagAliases[$option]] = $true
        } else {
            Die "未知选项：$token（运行 .\bin\emulator.ps1 help）"
        }
    } else {
        [void]$positionals.Add($token)
    }
}

$allowed = @{
    "create" = @("name", "port")
    "start" = @("name", "port", "gpu", "memory", "cores", "timeoutsec", "reuse", "wipedata", "nowait", "gui", "testinstance", "noaccel")
    "stop" = @("name", "port", "timeoutsec", "force")
    "kill" = @("name", "port", "timeoutsec", "force")
    "restart" = @("name", "port", "gpu", "memory", "cores", "timeoutsec", "force", "reuse", "wipedata", "nowait", "gui", "testinstance", "noaccel")
    "status" = @("name", "port")
    "list" = @()
    "verify" = @("name", "port")
    "inspect" = @()
    "reset" = @("name", "port", "timeoutsec", "force", "yes")
    "clone" = @("port", "timeoutsec", "force")
    "delete" = @("name", "timeoutsec", "force", "yes")
}
foreach ($key in $values.Keys) { if ($allowed[$command] -notcontains $key) { Die "-$key 不适用于 $command 命令" } }
foreach ($key in $flags.Keys) { if ($allowed[$command] -notcontains $key) { Die "-$key 不适用于 $command 命令" } }

if ($command -eq "inspect") {
    if ($positionals.Count -ne 1) { Die "用法：.\bin\emulator.ps1 inspect <归档路径>" }
    Invoke-InspectArchive ([string]$positionals[0])
    exit 0
}

if ($command -eq "clone") {
    if ($values.ContainsKey("name")) { Die "clone 使用两个位置参数：clone <源实例> <新实例>" }
    if ($positionals.Count -ne 2) { Die "用法：.\bin\emulator.ps1 clone <源实例> <新实例> [-Port N]" }
    $name = [string]$positionals[0]
    $targetName = [string]$positionals[1]
} else {
    if ($positionals.Count -gt 1) { Die "$command 最多接受一个位置实例名" }
    if ($values.ContainsKey("name") -and $positionals.Count -gt 0) { Die "位置实例名与 -Name 不能同时使用" }
    $name = $script:DefaultName
    if ($positionals.Count -eq 1) { $name = [string]$positionals[0] }
    if ($values.ContainsKey("name")) { $name = [string]$values["name"] }
}
$nameSpecified = ($positionals.Count -gt 0) -or $values.ContainsKey("name")
Assert-InstanceName $name
if ($command -eq "clone") { Assert-InstanceName $targetName "目标实例名" }

$port = 0
if ($values.ContainsKey("port")) {
    if ($values["port"] -notmatch '^\d+$') { Die "-Port 必须是整数：$($values['port'])" }
    try { $port = [int]$values["port"] } catch { Die "-Port 超出整数范围：$($values['port'])" }
    Assert-ConsolePort $port
}
if (-not $nameSpecified -and $port -gt 0 -and
    $command -in @("start", "stop", "kill", "restart", "status", "verify", "reset")) {
    $owner = Get-InstanceNameForPort $port
    if ($owner) { $name = $owner }
}
if ($nameSpecified -and $port -gt 0 -and
    $command -in @("start", "stop", "kill", "restart", "status", "verify", "reset")) {
    $registeredPort = Get-InstancePort $name
    if ($registeredPort -gt 0 -and $registeredPort -ne $port) {
        Die "实例 '$name' 登记的端口是 $registeredPort，不是 $port"
    }
    $owners = @(Get-InstanceNamesForPort $port)
    if ($owners.Count -gt 0 -and ($owners.Count -ne 1 -or $owners[0] -ne $name)) {
        $ownerText = $owners -join ", "
        Die "端口 $port 属于实例 '$ownerText'，不是 '$name'"
    }
}
if ((Test-Instance $name) -and $command -in @("start", "stop", "kill", "restart", "reset", "delete")) {
    [void](Assert-UniqueInstanceOwner $name)
}
$timeoutSec = 60
if ($values.ContainsKey("timeoutsec")) {
    if ($values["timeoutsec"] -notmatch '^\d+$') {
        Die "-TimeoutSec 必须是 1..3600 的整数"
    }
    try { $timeoutSec = [int]$values["timeoutsec"] } catch { Die "-TimeoutSec 必须是 1..3600 的整数" }
    if ($timeoutSec -lt 1 -or $timeoutSec -gt 3600) { Die "-TimeoutSec 必须是 1..3600 的整数" }
}
foreach ($key in @("memory", "cores")) {
    if ($values.ContainsKey($key)) {
        if ($values[$key] -notmatch '^\d+$') { Die "-$key 必须是正整数：$($values[$key])" }
        try { $number = [int]$values[$key] } catch { Die "-$key 超出整数范围：$($values[$key])" }
        if ($number -lt 1 -or $number -gt 65536) { Die "-$key 超出允许范围：$($values[$key])" }
    }
}

New-Item -ItemType Directory -Force -Path $script:RunDir, $script:InstancesDir | Out-Null
if ($command -eq "list") {
    if ($positionals.Count -gt 0) { Die "list 不接受位置参数" }
    & (Join-Path $PSScriptRoot "status.ps1")
    exit 0
}
if ($command -eq "create") { Invoke-Create $name $port; exit 0 }
if ($command -eq "delete") { Invoke-Delete $name $timeoutSec -Force:$flags["force"] -Yes:$flags["yes"]; exit 0 }
if ($command -eq "clone") { Invoke-Clone $name $targetName $port $timeoutSec -Force:$flags["force"]; exit 0 }

$namedArgs = @{ Name = $name }
if ($port -gt 0) { $namedArgs["Port"] = $port }
switch ($command) {
    "start" {
        foreach ($key in @("gpu", "memory", "cores", "timeoutsec")) {
            if ($values.ContainsKey($key)) {
                $paramName = switch ($key) { "timeoutsec" { "TimeoutSec" } default { (Get-Culture).TextInfo.ToTitleCase($key) } }
                $namedArgs[$paramName] = $values[$key]
            }
        }
        foreach ($key in @("reuse", "wipedata", "nowait", "gui", "testinstance", "noaccel")) {
            if ($flags[$key]) {
                $paramName = switch ($key) {
                    "wipedata" { "WipeData" }; "nowait" { "NoWait" }; "testinstance" { "TestInstance" }
                    "noaccel" { "NoAccel" }; default { (Get-Culture).TextInfo.ToTitleCase($key) }
                }
                $namedArgs[$paramName] = $true
            }
        }
        if ((Test-Instance $name) -and -not $flags["reuse"] -and -not $flags["wipedata"]) { $namedArgs["Reuse"] = $true }
        & (Join-Path $PSScriptRoot "start-headless.ps1") @namedArgs
    }
    "stop" {
        $namedArgs["TimeoutSec"] = $timeoutSec
        if ($flags["force"]) { $namedArgs["Force"] = $true }
        & (Join-Path $PSScriptRoot "stop.ps1") @namedArgs
    }
    "kill" {
        Write-Warn "强制停止可能丢失未保存的数据。"
        $namedArgs["TimeoutSec"] = $timeoutSec
        $namedArgs["Force"] = $true
        & (Join-Path $PSScriptRoot "stop.ps1") @namedArgs
    }
    "restart" {
        $stopArgs = @{ Name = $name; TimeoutSec = $timeoutSec }
        if ($port -gt 0) { $stopArgs["Port"] = $port }
        # Restart must leave the guest stopped before reusing its writable disks.
        # Stop-Instance still requests graceful shutdown first, then escalates.
        $stopArgs["Force"] = $true
        & (Join-Path $PSScriptRoot "stop.ps1") @stopArgs
        $startArgs = @{ Name = $name }
        if ($port -gt 0) { $startArgs["Port"] = $port }
        if ($values.ContainsKey("timeoutsec")) { $startArgs["TimeoutSec"] = $timeoutSec }
        if (-not $flags["reuse"] -and -not $flags["wipedata"]) { $startArgs["Reuse"] = $true }
        foreach ($key in @("gpu", "memory", "cores")) {
            if ($values.ContainsKey($key)) {
                $paramName = (Get-Culture).TextInfo.ToTitleCase($key)
                $startArgs[$paramName] = $values[$key]
            }
        }
        foreach ($key in @("wipedata", "nowait", "gui", "testinstance", "noaccel")) {
            if ($flags[$key]) {
                $paramName = switch ($key) {
                    "wipedata" { "WipeData" }; "nowait" { "NoWait" }; "testinstance" { "TestInstance" }
                    "noaccel" { "NoAccel" }; default { (Get-Culture).TextInfo.ToTitleCase($key) }
                }
                $startArgs[$paramName] = $true
            }
        }
        & (Join-Path $PSScriptRoot "start-headless.ps1") @startArgs
    }
    "status" {
        & (Join-Path $PSScriptRoot "status.ps1") @namedArgs
    }
    "verify" { & (Join-Path $PSScriptRoot "verify.ps1") @namedArgs }
    "reset" {
        $resetArgs = @{ TimeoutSec = $timeoutSec }
        if ($port -gt 0 -and -not $nameSpecified) { $resetArgs["Port"] = $port }
        else { $resetArgs["Name"] = $name; if ($port -gt 0) { $resetArgs["Port"] = $port } }
        if ($flags["force"]) { $resetArgs["Force"] = $true }
        if ($flags["yes"]) { $resetArgs["Yes"] = $true }
        & (Join-Path $PSScriptRoot "reset.ps1") @resetArgs
    }
}
