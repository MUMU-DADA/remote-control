<#
.SYNOPSIS
  在 Windows 上**后台**跑一个耗时脚本，并把输出落到日志文件。

.DESCRIPTION
  为什么需要它：fetch-images.ps1 要拉 ~3 GB，run-windows.ps1 要等开机，
  都远超一次远程调用能等的时长。而这些脚本又必须**脱离**发起它的那个
  shell 继续跑（ssh 一断就被带走）。

  实测踩过：`ssh host "start /b cmd /c \"powershell ... > log 2>&1\""`
  这种多层引号嵌套在 cmd 与 PowerShell 之间会被吃掉，日志文件压根没建出来。
  用 Start-Process + 重定向就干净得多。

用法：
  powershell -File _run-detached.ps1 -Script fetch-images.ps1 -Log fetch.log
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Script,
    [string]$Log = "",
    [string[]]$ScriptArgs = @()
)

$ErrorActionPreference = "Stop"
if (-not $Log) { $Log = ($Script -replace '\.ps1$', '') + ".log" }

$dir  = $PSScriptRoot
$path = Join-Path $dir $Script
if (-not (Test-Path $path)) { throw "找不到脚本：$path" }

$out = Join-Path $dir $Log
$err = "$out.err"
Remove-Item $out, $err -ErrorAction SilentlyContinue

$argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $path) + $ScriptArgs
$p = Start-Process -FilePath "powershell.exe" -ArgumentList $argList `
        -WorkingDirectory $dir -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $out -RedirectStandardError $err
Write-Host "已启动 PID=$($p.Id) 脚本=$Script 日志=$out"
