# ---------------------------------------------------------------------------
# 模拟器 console / guest HTTP 管理
#
# Windows 侧不依赖 adb：console TCP 是模拟器本身提供的管理接口，HTTP
# 重定向后由 remote-control daemon 提供就绪与优雅关机。令牌只从文件读，
# 不在日志或状态输出中打印。
# ---------------------------------------------------------------------------
function Get-ConsoleTokenFile {
    if ($env:AUTOSNAP_CONSOLE_TOKEN_FILE) { return $env:AUTOSNAP_CONSOLE_TOKEN_FILE }
    if ($env:USERPROFILE) { return (Join-Path $env:USERPROFILE ".emulator_console_auth_token") }
    if ($env:HOME) { return (Join-Path $env:HOME ".emulator_console_auth_token") }
    return ""
}

function Get-ConsoleToken {
    $f = Get-ConsoleTokenFile
    if (-not $f -or -not (Test-Path -LiteralPath $f)) { return "" }
    return ((Get-Content -LiteralPath $f -Raw -ErrorAction SilentlyContinue) -replace "\s", "").Trim()
}

function Invoke-ConsoleCommand {
    param([int]$Port, [string]$Command, [int]$TimeoutMs = 3000)
    if ($Port -le 0 -or [string]::IsNullOrWhiteSpace($Command)) { return $null }
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $client.ReceiveTimeout = $TimeoutMs
        $client.SendTimeout = $TimeoutMs
        $connecting = $client.BeginConnect("127.0.0.1", $Port, $null, $null)
        try {
            if (-not $connecting.AsyncWaitHandle.WaitOne($TimeoutMs)) { throw "模拟器 console 连接超时" }
            $client.EndConnect($connecting)
        } finally { $connecting.AsyncWaitHandle.Close() }
        $stream = $client.GetStream()
        $stream.ReadTimeout = $TimeoutMs
        $stream.WriteTimeout = $TimeoutMs
        $reader = New-Object System.IO.StreamReader($stream, [Text.Encoding]::ASCII, $false, 4096, $true)
        $writer = New-Object System.IO.StreamWriter($stream, [Text.Encoding]::ASCII, 4096, $true)
        $writer.NewLine = "`n"
        $writer.AutoFlush = $true

        $lines = New-Object System.Collections.Generic.List[string]
        $authRequired = $false
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line) { throw "模拟器 console 在问候前关闭了连接" }
            $line = $line.TrimEnd("`r")
            [void]$lines.Add($line)
            if ($line -match "Authentication required") { $authRequired = $true }
            if ($line -eq "OK" -or $line -like "KO:*") { break }
            if ($lines.Count -gt 128) { throw "模拟器 console 问候响应过长" }
        }
        if ($lines[$lines.Count - 1] -like "KO:*" -and -not $authRequired) { throw "模拟器 console 拒绝连接" }
        if ($authRequired) {
            $token = Get-ConsoleToken
            if ([string]::IsNullOrWhiteSpace($token)) { throw "模拟器 console 要求认证，但找不到 console token 文件" }
            $writer.WriteLine("auth $token")
            while ($true) {
                $line = $reader.ReadLine()
                if ($null -eq $line) { throw "模拟器 console 认证时关闭了连接" }
                $line = $line.TrimEnd("`r")
                [void]$lines.Add($line)
                if ($line -eq "OK" -or $line -like "KO:*") { break }
                if ($lines.Count -gt 128) { throw "模拟器 console 认证响应过长" }
            }
            if ($lines[$lines.Count - 1] -like "KO:*") { throw "模拟器 console 认证失败" }
        }
        $writer.WriteLine($Command)
        $reply = New-Object System.Collections.Generic.List[string]
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line) { break }
            $line = $line.TrimEnd("`r")
            [void]$reply.Add($line)
            if ($line -eq "OK" -or $line -like "KO:*") { break }
            if ($reply.Count -gt 512) { throw "模拟器 console 响应过长" }
        }
        if ($reply.Count -eq 0) { throw "模拟器 console 没有返回命令结果" }
        if ($reply[$reply.Count - 1] -like "KO:*") { throw "模拟器 console 拒绝命令" }
        if ($reply[$reply.Count - 1] -ne "OK" -and
            -not ($Command -eq "kill" -and ($reply -join "`n") -match "killing emulator")) {
            throw "模拟器 console 返回了不完整的命令结果"
        }
        return ($reply -join "`n")
    } finally {
        if ($client) { $client.Dispose() }
    }
}

function Test-EmulatorPortRunning { param([int]$Port) return (@(Get-EmuProcess $Port).Count -gt 0) }
function Test-ConsoleReady {
    param([int]$Port)
    try { [void](Invoke-ConsoleCommand $Port "avd status" 2000); return $true } catch { return $false }
}
function Wait-ConsoleReady {
    param([int]$Port, [int]$TimeoutSec = 60)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-ConsoleReady $Port) { return $true }
        if (-not (Test-EmulatorPortRunning $Port)) { return $false }
        Start-Sleep -Seconds 2
    }
    return $false
}
function Add-ConsoleRedirect {
    param([int]$Port, [int]$HostPort, [int]$GuestPort)
    try {
        $existing = Invoke-ConsoleCommand $Port "redir list" 3000
        if ($existing -match "tcp\s*:\s*$HostPort\s*=>\s*$GuestPort(?:\s|$)") { return $true }
        [void](Invoke-ConsoleCommand $Port "redir add tcp:${HostPort}:${GuestPort}" 3000)
        return $true
    } catch { return $false }
}
function Request-ConsoleKill {
    param([int]$Port)
    try { [void](Invoke-ConsoleCommand $Port "kill" 3000); return $true } catch { return $false }
}

function Get-ServiceTokenFile { param([string]$N) Join-Path $script:InstancesDir "$N.token" }
function Get-ServiceToken {
    param([string]$N)
    $f = Get-ServiceTokenFile $N
    if (-not (Test-Path -LiteralPath $f)) { return "" }
    return ((Get-Content -LiteralPath $f -Raw -ErrorAction SilentlyContinue) -replace "\s", "").Trim()
}
function Protect-ServiceTokenFile {
    param([string]$File)
    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
        $acl = Get-Acl -LiteralPath $File
        $acl.SetOwner($sid)
        $acl.SetAccessRuleProtection($true, $false)
        foreach ($existing in @($acl.Access)) { [void]$acl.RemoveAccessRule($existing) }
        $rule = New-Object Security.AccessControl.FileSystemAccessRule($sid, "FullControl", "Allow")
        [void]$acl.SetAccessRule($rule)
        Set-Acl -LiteralPath $File -AclObject $acl
    } else {
        & chmod 600 -- $File
        if ($LASTEXITCODE -ne 0) { throw "无法保护服务访问令牌文件" }
    }
}
function New-ServiceToken {
    param([string]$N, [switch]$TestInstance)
    if ($TestInstance) { return "" }
    $auth = Get-ConfigValue "service.auth" "1"
    if ($auth -ne "1") { return "" }
    $configured = Get-ConfigValue "service.token" ""
    $token = $configured
    if ([string]::IsNullOrWhiteSpace($token)) { $token = Get-ServiceToken $N }
    if ([string]::IsNullOrWhiteSpace($token)) {
        $bytes = New-Object byte[] 32
        $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
        try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
        $token = ([BitConverter]::ToString($bytes) -replace "-", "").ToLowerInvariant()
    }
    New-Item -ItemType Directory -Force -Path $script:InstancesDir | Out-Null
    $file = Get-ServiceTokenFile $N
    Set-Content -LiteralPath $file -Encoding ASCII -Value $token
    Protect-ServiceTokenFile $file
    return $token
}

function Get-InstanceValue {
    param([string]$N, [string]$Key)
    $f = Get-InstanceFile $N
    if (-not (Test-Path -LiteralPath $f)) { return "" }
    $hit = Select-String -Path $f -Pattern ("^" + [regex]::Escape($Key) + "=(.*)$") -ErrorAction SilentlyContinue |
           Select-Object -Last 1
    if ($hit) { return $hit.Matches[0].Groups[1].Value }
    return ""
}
function Set-InstanceService {
    param([string]$N, [int]$HostPort, [int]$GuestPort, [string]$Enabled, [string]$Auth,
          [string]$Bind = "0.0.0.0", [string]$AdbEnabled = "1")
    $f = Get-InstanceFile $N
    $keep = @()
    if (Test-Path -LiteralPath $f) {
        $keep = @(Get-Content -LiteralPath $f | Where-Object { $_ -notmatch '^(HTTP_PORT|SERVICE_PORT|SERVICE_ENABLED|SERVICE_AUTH|SERVICE_BIND|SERVICE_ADB)=' })
    }
    $keep += @("HTTP_PORT=$HostPort", "SERVICE_PORT=$GuestPort", "SERVICE_ENABLED=$Enabled", "SERVICE_AUTH=$Auth",
               "SERVICE_BIND=$Bind", "SERVICE_ADB=$AdbEnabled")
    Set-Content -LiteralPath $f -Encoding UTF8 -Value $keep
}

function Initialize-ServiceInstance {
    param([string]$N, [int]$Port, [switch]$Reuse, [switch]$TestInstance)
    $guestPort = Get-ConfigValue "service.port" "8088"
    $enabled = Get-ConfigValue "service.enabled" "1"
    $auth = if ($TestInstance) { "0" } else { Get-ConfigValue "service.auth" "1" }
    $bind = Get-ConfigValue "service.bind" "0.0.0.0"
    $adbEnabled = Get-ConfigValue "service.adb_enabled" "1"
    if ($Reuse) {
        $saved = Get-InstanceValue $N "SERVICE_PORT"; if ($saved) { $guestPort = $saved }
        $saved = Get-InstanceValue $N "SERVICE_ENABLED"; if ($saved) { $enabled = $saved }
        $saved = Get-InstanceValue $N "SERVICE_AUTH"; if ($saved) { $auth = $saved }
        $saved = Get-InstanceValue $N "SERVICE_BIND"; if ($saved) { $bind = $saved }
        $saved = Get-InstanceValue $N "SERVICE_ADB"; if ($saved) { $adbEnabled = $saved }
        $token = if ($auth -eq "1") { Get-ServiceToken $N } else { "" }
        if ($auth -eq "1" -and -not $token) { throw "复用实例缺少服务令牌文件：$(Get-ServiceTokenFile $N)" }
    } else {
        $token = New-ServiceToken $N -TestInstance:$TestInstance
    }
    if ($guestPort -notmatch '^[0-9]+$' -or [int]$guestPort -lt 1 -or [int]$guestPort -gt 65535) {
        throw "service.port 必须是 1-65535 的整数"
    }
    if ($enabled -notmatch '^[01]$' -or $auth -notmatch '^[01]$' -or $adbEnabled -notmatch '^[01]$') {
        throw "service.enabled/auth/adb_enabled 只能是 0 或 1"
    }
    if ($bind -notmatch '^[A-Za-z0-9.:_-]+$' -or $token -notmatch '^[A-Za-z0-9_-]{0,80}$') {
        throw "service.bind/token 格式不受支持"
    }
    $httpPort = Get-ServiceHttpPort $Port
    if ($env:AUTOSNAP_HTTP_PORT) {
        if ($env:AUTOSNAP_HTTP_PORT -notmatch '^[0-9]+$') { throw "HTTP 端口必须是 1-65535 的整数" }
        $httpPort = [int]$env:AUTOSNAP_HTTP_PORT
    }
    if ($httpPort -lt 1 -or $httpPort -gt 65535) { throw "HTTP 端口必须是 1-65535 的整数" }
    Set-InstanceService $N $httpPort ([int]$guestPort) $enabled $auth $bind $adbEnabled
    $properties = @("-prop", "qemu.rc.enabled=$enabled", "-prop", "qemu.rc.bind=$bind",
                    "-prop", "qemu.rc.port=$guestPort", "-prop", "qemu.rc.auth=$auth",
                    "-prop", "qemu.rc.adb=$adbEnabled")
    if ($token) { $properties += @("-prop", "qemu.rc.token=$token") }
    return [pscustomobject]@{ HttpPort = $httpPort; GuestPort = [int]$guestPort; Properties = $properties }
}
function Get-InstanceNameForPort {
    param([int]$Port)
    foreach ($n in Get-InstanceNames) { if ((Get-InstancePort $n) -eq $Port) { return $n } }
    return ""
}
function Get-ServiceHttpPort {
    param([int]$Port)
    $name = Get-InstanceNameForPort $Port
    $saved = if ($name) { Get-InstanceValue $name "HTTP_PORT" } else { "" }
    if ($saved -match '^[0-9]+$') { return [int]$saved }
    if ($env:AUTOSNAP_HTTP_PORT -match '^[0-9]+$') { return [int]$env:AUTOSNAP_HTTP_PORT }
    return 18088 + [int](($Port - $script:PortBase) / 2)
}
function Invoke-ServiceHttp {
    param([int]$Port, [string]$Path, [string]$Token = "", [string]$Body = "", [int]$TimeoutSec = 3)
    $httpPort = Get-ServiceHttpPort $Port
    $uri = "http://127.0.0.1:$httpPort$Path"
    $request = [System.Net.HttpWebRequest]::Create($uri)
    $request.Proxy = $null
    $request.Timeout = $TimeoutSec * 1000
    $request.ReadWriteTimeout = $TimeoutSec * 1000
    if ($Token) { $request.Headers["X-Remote-Control-Token"] = $Token }
    if ($Body) {
        $request.Method = "POST"
        $request.ContentType = "application/json"
        $bytes = [Text.Encoding]::UTF8.GetBytes($Body)
        $request.ContentLength = $bytes.Length
        $requestStream = $request.GetRequestStream()
        try { $requestStream.Write($bytes, 0, $bytes.Length) } finally { $requestStream.Dispose() }
    } else {
        $request.Method = "GET"
    }
    $response = $request.GetResponse()
    try {
        $reader = New-Object System.IO.StreamReader($response.GetResponseStream())
        try { $json = $reader.ReadToEnd() } finally { $reader.Dispose() }
        return ($json | ConvertFrom-Json)
    } finally { $response.Dispose() }
}
function Test-ServiceReady {
    param([int]$Port)
    $name = Get-InstanceNameForPort $Port
    $token = if ($name) { Get-ServiceToken $name } else { "" }
    try {
        $r = Invoke-ServiceHttp $Port "/api/v1" $token "" 3
        return ($null -ne $r -and $r.PSObject.Properties["service"] -and $r.service -eq "remote-control")
    } catch { return $false }
}
function Wait-ServiceReady {
    param([int]$Port, [int]$TimeoutSec = 300)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-ServiceReady $Port) { return $true }
        if (-not (Test-EmulatorPortRunning $Port)) { return $false }
        Start-Sleep -Seconds 2
    }
    return $false
}
function Request-ServiceShutdown {
    param([int]$Port)
    $name = Get-InstanceNameForPort $Port
    $token = if ($name) { Get-ServiceToken $name } else { "" }
    try {
        $r = Invoke-ServiceHttp $Port "/api/v1/power" $token '{"action":"shutdown"}' 8
        return ($null -ne $r -and $r.PSObject.Properties["ok"] -and $r.ok -eq $true)
    } catch { return $false }
}

function Stop-Instance {
    param([int]$Port, [int]$TimeoutSec = 60, [switch]$Force)
    if (-not (Test-EmulatorPortRunning $Port)) { return $true }
    Write-Log "请 guest 通过 remote-control 正常关机"
    $requested = Request-ServiceShutdown $Port
    if (-not $requested) {
        # PowerManager may start shutdown before the delayed HTTP reply leaves
        # the guest.  An observed process exit is the authoritative result;
        # allow the caller's full timeout before declaring the request failed.
        $deadline = (Get-Date).AddSeconds([Math]::Max(0, $TimeoutSec))
        while ((Get-Date) -lt $deadline) {
            if (-not (Test-EmulatorPortRunning $Port)) { return $true }
            Start-Sleep -Seconds 1
        }
        if (-not (Test-EmulatorPortRunning $Port)) { return $true }
        Write-Warn "关机请求未被服务接受；检查服务就绪状态和访问令牌"
        if (-not $Force) { return $false }
    } else {
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while ((Get-Date) -lt $deadline) {
            if (-not (Test-EmulatorPortRunning $Port)) { return $true }
            Start-Sleep -Seconds 2
        }
        if (-not (Test-EmulatorPortRunning $Port)) { return $true }
        Write-Warn "等了 ${TimeoutSec} 秒，模拟器进程仍未退出"
        if (-not $Force) { return $false }
    }
    Write-Warn "已指定 -Force，通过 console 强制结束模拟器"
    [void](Request-ConsoleKill $Port)
    Start-Sleep -Seconds 2
    if (Test-EmulatorPortRunning $Port) {
        foreach ($p in @(Get-EmuProcess $Port)) {
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        }
        Start-Sleep -Seconds 2
    }
    return (-not (Test-EmulatorPortRunning $Port))
}

