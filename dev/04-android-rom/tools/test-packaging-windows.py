#!/usr/bin/env python3
"""Run packaged PowerShell lifecycle commands against isolated console/HTTP peers."""
import http.server
import io
import json
import os
from pathlib import Path
import shutil
import socketserver
import subprocess
import tarfile
import tempfile
import threading
import time


project = Path(__file__).resolve().parents[1]
repo = project.parents[1]
pwsh = shutil.which("pwsh") or str(repo / ".tmp/pwsh/pwsh")
if not Path(pwsh).is_file():
    raise SystemExit("pwsh unavailable; install the host test runtime first")


with tempfile.TemporaryDirectory(prefix="autosnap-windows-") as temp:
    root = Path(temp)
    bindir = root / "bin"
    shutil.copytree(project / "packaging/bin/windows", bindir)
    images = root / "images"
    (images / "system").mkdir(parents=True)
    for name in ("system-qemu.img", "vendor-qemu.img", "product-qemu.img",
                 "ramdisk-qemu.img", "kernel-ranchu", "encryptionkey.img",
                 "userdata.img", "advancedFeatures.ini", "config.ini"):
        (images / name).write_bytes(b"BASE-IMAGE")
    (images / "system/build.prop").write_text("ro.product.device=remote_control_x64_arm64\n")
    (root / "templates").mkdir()
    config = root / "templates/config.ini"
    initial_config = (project / "emulator/config.ini").read_text()
    config.write_text(initial_config)
    runtime = root / "runtime/emulator/qemu/windows-x86_64"
    runtime.mkdir(parents=True)
    (runtime / "qemu-system-x86_64.exe").touch()
    (root / "runtime/emulator/emulator.exe").touch()
    running = root / "running"
    console_token = root / "console.token"
    console_token.write_text("console-test-token\n")
    arguments = root / "arguments.json"
    # Windows process APIs do not exist on Unix; keep actual PS lifecycle,
    # config, file operations, authentication, and network code under test.
    mock_process_functions = r'''
function Get-EmuProcess {
    param([int]$P)
    if (Test-Path -LiteralPath $env:MOCK_RUNNING) {
        return @([pscustomobject]@{ ProcessId = 424242 })
    }
    return @()
}
function Start-Process {
    param($FilePath, $ArgumentList, [switch]$PassThru, $WindowStyle,
          $RedirectStandardOutput, $RedirectStandardError)
    Set-Content -LiteralPath $env:MOCK_RUNNING -Value alive
    $ArgumentList | ConvertTo-Json | Set-Content -LiteralPath $env:MOCK_ARGUMENTS
    # 默认写一行 boot 标记；测试可用 MOCK_STDOUT 换成"真实但不友好"的日志
    $stdout = if ($env:MOCK_STDOUT) { $env:MOCK_STDOUT } else { 'Boot completed in 100 ms' }
    Set-Content -LiteralPath $RedirectStandardOutput -Value $stdout
    Set-Content -LiteralPath $RedirectStandardError -Value ''
    return [pscustomobject]@{ Id = 424242; HasExited = $false }
}
function Stop-Process {
    param($Id, [switch]$Force, $ErrorAction)
    Remove-Item -LiteralPath $env:MOCK_RUNNING -Force -ErrorAction SilentlyContinue
}
'''
    with (bindir / "common.ps1").open("a") as f:
        f.write(mock_process_functions)

    legacy = root / "legacy/windows"
    legacy.mkdir(parents=True)
    shutil.copytree(images, legacy / "images")
    shutil.copytree(bindir, root / "legacy/packaging/bin/windows")
    (root / "legacy/emulator").mkdir()
    (root / "legacy/emulator/config.ini").write_text(initial_config)
    legacy_source = (project / "windows/emulator.ps1").read_text()
    lifecycle_marker = "# ---------------------------------------------------------------------------\n# 入口\n"
    legacy_source = legacy_source.replace(lifecycle_marker, mock_process_functions + "\n" + lifecycle_marker)
    (legacy / "emulator.ps1").write_text(legacy_source)
    shutil.copy(project / "windows/run-windows.ps1", legacy / "run-windows.ps1")
    for image_name in ("initrd", "system.img", "vendor.img", "ramdisk.img"):
        (legacy / "images" / image_name).write_bytes(b"BASE-IMAGE")
    (legacy / "sdk/emulator").mkdir(parents=True)
    (legacy / "sdk/emulator/emulator.exe").touch()

    class State:
        redirects = set()
        commands = []
        reject_shutdown = False
        drop_shutdown_reply = False
        shutdown_delay = 0
        calls = []

    state = State()

    class Console(socketserver.StreamRequestHandler):
        def handle(self):
            self.wfile.write(b"Android Console: Authentication required\r\nOK\r\n")
            auth = self.rfile.readline().decode().strip()
            if auth != "auth console-test-token":
                self.wfile.write(b"KO: authentication failed\r\n")
                return
            self.wfile.write(b"OK\r\n")
            command = self.rfile.readline().decode().strip()
            state.commands.append(command)
            if command == "redir list":
                for host, guest in state.redirects:
                    self.wfile.write(f"tcp:{host} => {guest}\r\n".encode())
            elif command.startswith("redir add tcp:"):
                host, guest = map(int, command.removeprefix("redir add tcp:").split(":"))
                state.redirects.add((host, guest))
            elif command == "kill":
                running.unlink(missing_ok=True)
                self.wfile.write(b"killing emulator\r\n")
            self.wfile.write(b"OK\r\n")

    instance_path = root / ".run/instances/default.env"
    token_path_for_http = root / ".run/instances/default.token"

    class Http(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def serve(self):
            instance = instance_path
            values = dict(line.split("=", 1) for line in instance.read_text(encoding="utf-8-sig").splitlines()
                          if "=" in line and not line.startswith("#"))
            token_file = token_path_for_http
            token = token_file.read_text().strip() if token_file.exists() else ""
            supplied = self.headers.get("X-Remote-Control-Token", "")
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            state.calls.append((self.command, self.path, body))
            if values.get("SERVICE_AUTH") == "1" and supplied != token:
                code, response = 401, {"error": "unauthorized"}
            elif self.command == "POST":
                if state.reject_shutdown:
                    code, response = 503, {"ok": False}
                elif self.path == "/api/v1/power" and json.loads(body) == {"action": "shutdown"}:
                    if state.shutdown_delay:
                        threading.Timer(state.shutdown_delay, lambda: running.unlink(missing_ok=True)).start()
                    else:
                        running.unlink(missing_ok=True)
                    if state.drop_shutdown_reply:
                        self.close_connection = True
                        return
                    code, response = 200, {"ok": True, "action": "shutdown"}
                else:
                    code, response = 400, {"ok": False}
            else:
                code, response = 200, {"service": "remote-control"}
            payload = json.dumps(response).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        do_GET = serve
        do_POST = serve

    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True

    console = Server(("127.0.0.1", 0), Console)
    # Linux's automatic bind(0) allocation commonly yields only odd ports;
    # emulator console ports must be even, so explicitly bind the neighbour.
    if console.server_address[1] % 2:
        candidate = console.server_address[1] + 1
        console.server_close()
        while True:
            try:
                console = Server(("127.0.0.1", candidate), Console)
                break
            except OSError:
                candidate += 2
    http = Server(("127.0.0.1", 0), Http)
    for server in (console, http):
        threading.Thread(target=server.serve_forever, daemon=True).start()
    port = console.server_address[1]
    # 隔离模拟器主目录：被测脚本会在缺失时自动创建它（缺了它模拟器会报 error: 3 并无限重试）
    emu_home = root / "emu-home"
    env = dict(os.environ, DOTNET_SYSTEM_GLOBALIZATION_INVARIANT="1",
               AUTOSNAP_HTTP_PORT=str(http.server_address[1]),
               AUTOSNAP_CONSOLE_TOKEN_FILE=str(console_token),
               ANDROID_EMULATOR_HOME=str(emu_home),
               MOCK_RUNNING=str(running), MOCK_ARGUMENTS=str(arguments))
    count = 0

    def check(condition, label):
        global count
        assert condition, label
        count += 1
        print(f"[OK] {label}")

    def run(script, *args, success=True, directory=bindir, cwd=None):
        result = subprocess.run([pwsh, "-NoProfile", "-File", str(directory / script), *args],
                                env=env, capture_output=True, text=True, timeout=30, cwd=cwd)
        if success:
            assert result.returncode == 0, f"{script}: {result.stdout}\n{result.stderr}"
        else:
            assert result.returncode != 0, f"{script}: unexpectedly succeeded"
        return result.stdout + result.stderr

    try:
        output = run("start-headless.ps1", "-Port", str(port), "-NoAccel", "-TimeoutSec", "5")
        token_path = root / ".run/instances/default.token"
        token = token_path.read_text().strip()
        props = json.loads(arguments.read_text())
        check(len(token) == 64 and int(token, 16) > 0, "first start generates a host token")
        check(token not in output and "console-test-token" not in output, "startup does not print tokens")
        check(token_path.stat().st_mode & 0o777 == 0o600, "token file is private")
        check(f"qemu.rc.token={token}" in props and not any("vendor.qemu.rc." in str(p) for p in props),
              "emulator receives qemu properties without rejected vendor prefix")
        check((http.server_address[1], 8088) in state.redirects, "console auth and HTTP redirection work")
        output = run("status.ps1", "-Port", str(port))
        check("服务已就绪" in output and token not in output, "status checks authenticated HTTP readiness")
        run("stop.ps1", "-Port", str(port), "-TimeoutSec", "2")
        check(not running.exists() and "kill" not in state.commands, "normal stop requests guest shutdown without console kill")
        config.write_text(initial_config.replace("service.port=8088", "service.port=9090")
                          .replace("service.auth=1", "service.auth=0")
                          .replace("service.token=", "service.token=changed-template-token"))
        run("start-headless.ps1", "-Reuse", "-Port", str(port), "-NoAccel", "-TimeoutSec", "5")
        props = json.loads(arguments.read_text())
        check(token_path.read_text().strip() == token and f"qemu.rc.token={token}" in props,
              "reuse preserves the guest token despite template changes")
        check("qemu.rc.port=8088" in props and "qemu.rc.auth=1" in props,
              "reuse preserves guest port and auth metadata")
        state.reject_shutdown = True
        run("stop.ps1", "-Port", str(port), "-TimeoutSec", "1", success=False)
        check(running.exists() and "kill" not in state.commands, "failed normal stop leaves emulator running")
        run("stop.ps1", "-Port", str(port), "-TimeoutSec", "1", "-Force")
        check(not running.exists() and "kill" in state.commands, "explicit Force permits console kill")
        sysdir = root / f".run/sysdir-{port}"
        (sysdir / "userdata-qemu.img.qcow2").write_text("instance data")
        (sysdir / "snapshots").mkdir()
        (sysdir / "snapshots/state").write_text("state")
        run("reset.ps1", "-Port", str(port), "-Yes")
        check(not (sysdir / "userdata-qemu.img.qcow2").exists() and not (sysdir / "snapshots").exists(),
              "reset removes instance data and snapshots")
        check((images / "system-qemu.img").read_bytes() == b"BASE-IMAGE" and
              (sysdir / "system-qemu.img").exists(), "reset preserves shared base images")
        check(not token_path.exists(), "reset removes stale token and service metadata")
        config.write_text(initial_config)
        state.reject_shutdown = False
        output = run("start-headless.ps1", "-TestInstance", "-Port", str(port), "-NoAccel", "-TimeoutSec", "5")
        props = json.loads(arguments.read_text())
        check("qemu.rc.auth=0" in props and not any(str(p).startswith("qemu.rc.token=") for p in props)
              and not token_path.exists(), "TestInstance boots with auth disabled and no token")
        state.drop_shutdown_reply = True
        state.shutdown_delay = 3.5
        run("stop.ps1", "-Port", str(port), "-TimeoutSec", "6")
        check(not running.exists(), "stop waits full timeout for delayed exit when HTTP reply is lost")
        check(all(path != "/api/v1/shutdown" for _, path, _ in state.calls), "lifecycle never shuts down only the daemon")
        state.drop_shutdown_reply = False
        state.shutdown_delay = 0

        # --- 回归：2026-10-05 在 Windows 真机上踩到的三个坑 ---
        # 1) 日志里没有 boot time 行时不能崩：旧版写 (...).Line，没有匹配时管道给的是 $null，
        #    StrictMode 2.0 下访问它的属性会抛「在此对象上找不到属性"Line"」直接终止脚本。
        #    顺便验证 -accel off 的 "may not work" 警告不会被误判成致命错误。
        env["MOCK_STDOUT"] = ("INFO | Android emulator (fake)\n"
                              "WARNING | x86_64 emulation may not work without hardware acceleration!\n"
                              "INFO | 这行故意不含 boot 标记")
        output = run("start-headless.ps1", "-Reuse", "-Port", str(port), "-NoAccel", "-TimeoutSec", "5")
        check("找不到属性" not in output and "开机完成" in output,
              "start survives a log without a boot-time line")
        # 2) 模拟器主目录必须被自动建出来（缺了它模拟器只会无限重试、把日志刷爆）
        check(emu_home.is_dir(), "start creates the emulator home directory when it is missing")
        run("stop.ps1", "-Port", str(port), "-TimeoutSec", "2")

        # 3) 致命且会无限重试的错误必须快速失败 + 中文诊断，而不是一路等到超时
        env["MOCK_STDOUT"] = ("INFO | Android emulator (fake)\n"
                              "ERROR   | Unexpected error while creating: "
                              r"C:\Users\PC\.android\emu-last-feature-flags.protobuf.lock (error: 3)" "\n")
        started = time.monotonic()
        output = run("start-headless.ps1", "-Reuse", "-Port", str(port), "-NoAccel",
                     "-TimeoutSec", "300", success=False)
        elapsed = time.monotonic() - started
        check("模拟器创建文件失败" in output and "ERROR_PATH_NOT_FOUND" in output,
              "fatal emulator log produces a readable diagnosis")
        check(elapsed < 60, f"fatal log fails fast instead of waiting for the 300s timeout ({elapsed:.1f}s)")
        check(not running.exists(), "fatal path stops the stuck emulator instead of leaving it spinning")
        output = run("status.ps1", "-Port", str(port))
        check("模拟器主目录" in output, "status reports the emulator home directory")
        env.pop("MOCK_STDOUT", None)

        manager_run = root / ".run"
        manager_instances = manager_run / "instances"
        run("emulator.ps1", "create", "manager-source", "-Port", "5680")
        manager_start = bindir / "start-headless.ps1"
        saved_manager_start = manager_start.read_bytes()
        manager_start_args = root / "manager-start-args.json"
        env["MOCK_MANAGER_START_ARGS"] = str(manager_start_args)
        manager_start.write_text(r'''[CmdletBinding()]
param(
    [string]$Name = "", [int]$Port = 0, [int]$TimeoutSec = 300,
    [string]$Gpu = "", [int]$Memory = 0, [int]$Cores = 0,
    [switch]$Reuse, [switch]$WipeData, [switch]$NoWait, [switch]$Gui,
    [switch]$TestInstance, [switch]$NoAccel
)
$PSBoundParameters | ConvertTo-Json -Compress | Set-Content -LiteralPath $env:MOCK_MANAGER_START_ARGS
''')
        try:
            output = run("emulator.ps1", "start", "manager-source", "-Port", "5682", success=False)
            check("manager-source" in output and
                  "5680，不是 5682" in output and not manager_start_args.exists(),
                  "manager rejects a mismatched name/port before starting")
            output = run("emulator.ps1", "status", "-Port", "5680")
            check("实例 manager-source（端口 5680" in output,
                  "manager status preserves the name resolved from its port")
            (manager_instances / "restart-probe.env").write_text("PORT=5684\n")
            run("emulator.ps1", "restart", "restart-probe", "-TimeoutSec", "17", "-NoWait")
            start_args = json.loads(manager_start_args.read_text())
            check(start_args.get("TimeoutSec") == 17,
                  "manager restart forwards an explicit timeout to startup")
        finally:
            manager_start.write_bytes(saved_manager_start)
            env.pop("MOCK_MANAGER_START_ARGS", None)

        duplicate = manager_instances / "duplicate.env"
        duplicate.write_text("PORT=5680\n")
        for command in (
            ("start", "manager-source"),
            ("stop", "manager-source"),
            ("restart", "manager-source"),
            ("reset", "manager-source", "-Yes"),
            ("delete", "manager-source", "-Yes"),
        ):
            output = run("emulator.ps1", *command, success=False)
            check("没有唯一" in output or "重复登记" in output,
                  f"manager {command[0]} refuses a duplicate port owner")
        check((manager_run / "sysdir-5680/system-qemu.img").exists(),
              "duplicate-owner guards preserve the instance working directory")
        duplicate.unlink()

        external_token = root / "external-token"
        external_token.write_text("secret-token\n")
        source_token = manager_instances / "manager-source.token"
        source_token.symlink_to(external_token)
        output = run("emulator.ps1", "clone", "manager-source", "token-link-copy", "-Port", "5696",
                     success=False)
        check("令牌是 reparse point" in output and
              not (manager_instances / "token-link-copy.env").exists(),
              "manager clone rejects a reparse-point service token")
        source_token.unlink()

        source_sys = manager_run / "sysdir-5690"
        source_sys.symlink_to(manager_run / "sysdir-5680", target_is_directory=True)
        (manager_instances / "sys-link.env").write_text("PORT=5690\n")
        output = run("emulator.ps1", "clone", "sys-link", "sys-link-copy", "-Port", "5692",
                     success=False)
        check("工作目录是 reparse point" in output and
              not (manager_instances / "sys-link-copy.env").exists(),
              "manager clone rejects a reparse-point system directory root")
        source_sys.unlink()
        (manager_run / "sysdir-5690").mkdir()
        (manager_run / "datadir-5690").symlink_to(root / "external-data", target_is_directory=True)
        (root / "external-data").mkdir()
        output = run("emulator.ps1", "clone", "sys-link", "data-link-copy", "-Port", "5694",
                     success=False)
        check("数据目录是 reparse point" in output and
              not (manager_instances / "data-link-copy.env").exists(),
              "manager clone rejects a reparse-point data directory root")

        output = run("emulator.ps1", "create", "below-minimum", "-Port", "2", success=False)
        check("5554..65534" in output, "Windows manager rejects console ports below 5554")

        inspect_work = root / "inspect work"
        inspect_work.mkdir()
        archive = root / "instance archive.tar"
        manifest = b'{"name":"archive-fixture","port":5580}\n'
        payload_name = "inspect-payload-marker.txt"
        with tarfile.open(archive, "w") as bundle:
            manifest_info = tarfile.TarInfo("INSTANCE-MANIFEST.json")
            manifest_info.size = len(manifest)
            bundle.addfile(manifest_info, io.BytesIO(manifest))
            payload = b"must not be extracted"
            payload_info = tarfile.TarInfo(payload_name)
            payload_info.size = len(payload)
            bundle.addfile(payload_info, io.BytesIO(payload))
        output = run("emulator.ps1", "inspect", str(archive), cwd=inspect_work)
        check('"name":"archive-fixture"' in output and payload_name in output,
              "manager inspect reads the manifest and archive listing")
        check(not (inspect_work / payload_name).exists(),
              "manager inspect does not extract archive entries")
        missing_manifest_archive = root / "without manifest.tar"
        with tarfile.open(missing_manifest_archive, "w") as bundle:
            payload_info = tarfile.TarInfo("payload.txt")
            payload_info.size = 1
            bundle.addfile(payload_info, io.BytesIO(b"x"))
        output = run("emulator.ps1", "inspect", str(missing_manifest_archive),
                     success=False, cwd=inspect_work)
        check("没有 INSTANCE-MANIFEST.json" in output,
              "manager inspect reports archives without a manifest")

        instance_path = legacy / ".run/instances/default.env"
        token_path_for_http = legacy / ".run/instances/default.token"
        output = run("emulator.ps1", "start", "default", "-Port", str(port), directory=legacy)
        legacy_token = token_path_for_http.read_text().strip()
        check(legacy_token not in output and "qemu.rc.auth=1" in json.loads(arguments.read_text()),
              "development emulator starts through authenticated console and HTTP without ADB")
        output = run("emulator.ps1", "status", "default", directory=legacy)
        check("服务已就绪" in output, "development emulator status uses authenticated HTTP")
        run("emulator.ps1", "stop", "default", directory=legacy)
        check(not running.exists(), "development emulator stop requests guest poweroff without ADB")
        run("emulator.ps1", "clone", "default", "clone", directory=legacy)
        check((legacy / ".run/instances/clone.token").read_text().strip() == legacy_token,
              "development clone keeps token matching cloned guest configuration")
        run("emulator.ps1", "reset", "default", "-Yes", directory=legacy)
        check(not token_path_for_http.exists(), "development reset discards stale guest token")
        run("emulator.ps1", "delete", "clone", "-Yes", directory=legacy)
        check(not (legacy / ".run/instances/clone.token").exists(), "development delete removes host token")
        wrapper_data = legacy / "data"
        instance_path = wrapper_data / f".instances/instances/run-{port}.env"
        token_path_for_http = wrapper_data / f".instances/instances/run-{port}.token"
        output = run("run-windows.ps1", "-Port", str(port), "-Headless", directory=legacy)
        check(running.exists() and "-Verify" in output, "run-windows normal start defers ADB verification")
        run("run-windows.ps1", "-Port", str(port), "-Stop", directory=legacy)
        check(not running.exists(), "run-windows normal stop uses shared HTTP shutdown")
        (wrapper_data / "guest/userdata-qemu.img.qcow2").write_text("wipe me")
        output = run("run-windows.ps1", "-Port", str(port), "-Headless", "-WipeData", directory=legacy)
        check(not (wrapper_data / "guest/userdata-qemu.img.qcow2").exists() and instance_path.exists()
              and token_path_for_http.exists(), "wrapper WipeData clears guest data while preserving instance registration")
        run("run-windows.ps1", "-Port", str(port), "-Stop", directory=legacy)
        print(f"PASS: {count} Windows lifecycle checks")
    finally:
        for server in (console, http):
            server.shutdown()
            server.server_close()
