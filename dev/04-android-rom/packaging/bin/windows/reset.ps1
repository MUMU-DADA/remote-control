<#
.SYNOPSIS
  重置实例的数据与快照；保留实例登记和包内基础镜像。
.EXAMPLE
  .\bin\reset.ps1 -Name default -Yes
  .\bin\reset.ps1 -Port 5584 -Yes
.NOTES
  先经 HTTP 请求 Android 正常关机，只有显式 -Force 才允许强制断电。
#>
[CmdletBinding()]
param(
    [string]$Name = "",
    [int]$Port = 0,
    [int]$TimeoutSec = 60,
    [switch]$Yes,
    [switch]$Force
)

. (Join-Path $PSScriptRoot "common.ps1")
if ($Port -le 0) {
    if ([string]::IsNullOrWhiteSpace($Name)) { $Name = $script:DefaultName }
    $Port = Get-InstancePort $Name
}
if ($Port -le 0) { Die "实例 '$Name' 没登记过端口" }
$owners = @(Get-InstanceNamesForPort $Port)
if ($owners.Count -ne 1) { Die "端口 $Port 没有唯一的已登记实例；拒绝清理未登记或重复登记的数据" }
$owner = $owners[0]
if ([string]::IsNullOrWhiteSpace($Name)) { $Name = $owner }
if ($owner -ne $Name) { Die "端口 $Port 属于实例 '$owner'，不是 '$Name'" }
if (-not $Yes) {
    Write-Warn "重置 '$owner' 会清空已装应用、应用数据、sdcard 与快照"
    if ((Read-Host "输入 yes 继续") -ne "yes") { Die "已取消" }
}
if (-not (Stop-Instance $Port $TimeoutSec -Force:$Force)) {
    Die "实例还在运行；未删除数据。检查服务或显式 -Force 后重试"
}

$sysdir = Get-SysDir $Port
$datadir = Get-DataDir $Port
$state = @("userdata-qemu.img", "userdata-qemu.img.qcow2", "userdata-qemu.img.qcow2.lock",
           "cache.img", "cache.img.qcow2", "cache.img.qcow2.lock", "encryptionkey.img.qcow2",
           "bootcompleted.ini", "hardware-qemu.ini", "hardware-qemu.ini.lock", "multiinstance.lock",
           "emu-launch-params.txt", "version_num.cache", "read-snapshot.txt")
foreach ($f in $state) {
    Remove-Item -LiteralPath (Join-Path $sysdir $f) -Force -ErrorAction SilentlyContinue
}
foreach ($d in @("build.avd", "snapshots", "tmpAdbCmds")) {
    $path = Join-Path $sysdir $d
    $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        [IO.Directory]::Delete($item.FullName, $false)
    } elseif ($item) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}
$dataItem = Get-Item -LiteralPath $datadir -Force -ErrorAction SilentlyContinue
if ($dataItem -and ($dataItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    [IO.Directory]::Delete($dataItem.FullName, $false)
} elseif ($dataItem) {
    Remove-Item -LiteralPath $datadir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $datadir | Out-Null
Remove-Item -LiteralPath (Get-ServiceTokenFile $owner) -Force -ErrorAction SilentlyContinue
Register-Instance $owner $Port
Write-Ok "已重置 '$owner'（下次启动是全新机器）"
