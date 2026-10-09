# parse-ps1.ps1 <文件> —— 用 PowerShell 自己的解析器做语法检查。
# 输出刻意用 ASCII：经 SSH 回传时中文会变乱码，判断结论就够了。
param([Parameter(Mandatory)][string]$Path)
if (-not (Test-Path -LiteralPath $Path)) { Write-Host "MISSING: $Path"; exit 2 }
$errors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$null, [ref]$errors)
if ($errors.Count -eq 0) {
    Write-Host "SYNTAX OK: 0 errors"
    exit 0
}
Write-Host ("SYNTAX ERRORS: {0}" -f $errors.Count)
foreach ($e in $errors) {
    Write-Host ("  line {0} col {1}: {2}" -f $e.Extent.StartLineNumber, $e.Extent.StartColumnNumber, $e.Message)
}
exit 1
