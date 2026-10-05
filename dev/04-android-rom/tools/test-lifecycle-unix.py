#!/usr/bin/env python3
"""Exercise Unix release/development lifecycle against local console/HTTP peers."""
import http.server
import json
import os
from pathlib import Path
import shutil
import socketserver
import subprocess
import tempfile
import tarfile
import threading
import time


project = Path(__file__).resolve().parents[1]
config_text = (project / "emulator/config.ini").read_text()
count = 0


def check(condition, label):
    global count
    assert condition, label
    count += 1
    print(f"[OK] {label}", flush=True)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


for kind in ("linux", "darwin", "emulator", "run-linux"):
    with tempfile.TemporaryDirectory(prefix=f"autosnap-{kind}-") as temp:
        root = Path(temp)
        development = kind in ("emulator", "run-linux")
        if development:
            (root / "aosp").mkdir()
            work = root / "dev/04-android-rom"
            bindir = work / "scripts"
            bindir.mkdir(parents=True)
            for name in ("common.sh", "emulator.sh", "run-linux.sh"):
                shutil.copy2(project / "scripts" / name, bindir / name)
            helper = work / "packaging/bin/linux"
            helper.mkdir(parents=True)
            shutil.copy2(project / "packaging/bin/linux/console.sh", helper / "console.sh")
            images = work / "artifacts/rom-remote_control_x64_arm64"
            config = work / "emulator/config.ini"
            common = bindir / "common.sh"
        else:
            work = root
            bindir = work / "bin"
            bindir.mkdir()
            for name in ("lib.sh", "console.sh", "start-headless.sh", "status.sh", "stop.sh"):
                shutil.copy2(project / "packaging/bin" / kind / name, bindir / name)
            images = work / "images"
            config = work / "templates/config.ini"
            common = bindir / "lib.sh"
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_text(config_text)
        (images / "system").mkdir(parents=True)
        for name in ("system-qemu.img", "vendor-qemu.img", "product-qemu.img", "ramdisk-qemu.img",
                     "kernel-ranchu", "encryptionkey.img", "userdata.img", "advancedFeatures.ini", "config.ini"):
            (images / name).write_bytes(b"BASE-IMAGE")
        for name, target in (("system.img", "system-qemu.img"), ("vendor.img", "vendor-qemu.img"),
                             ("ramdisk.img", "ramdisk-qemu.img")):
            (images / name).symlink_to(target)
        (images / "system/build.prop").write_text("ro.product.device=remote_control_x64_arm64\n")
        runtime = work / "runtime/emulator"
        runtime.mkdir(parents=True)
        emulator = runtime / "emulator"
        emulator.write_text("#!/bin/sh\nexit 1\n")
        emulator.chmod(0o755)
        backend = runtime / "qemu" / ("darwin-aarch64/qemu-system-aarch64-headless" if kind == "darwin"
                                       else "linux-x86_64/qemu-system-x86_64-headless")
        backend.parent.mkdir(parents=True)
        backend.write_text("#!/bin/sh\nexit 1\n")
        backend.chmod(0o755)
        running = root / "running"
        args_file = root / "arguments.json"
        console_token = root / "console.token"
        console_token.write_text("console-test-token\n")
        adb_log = root / "adb.log"
        adb = root / "adb"
        adb.write_text('#!/bin/sh\necho called >> "$MOCK_ADB_LOG"\nexit 99\n')
        adb.chmod(0o755)
        # Replace only process creation/enumeration, retaining the production
        # config, metadata, token, auth, redirect, HTTP, and filesystem code.
        with common.open("a") as f:
            f.write('''\n
emu_pid_for_port() { [ -f "$MOCK_RUNNING" ] && printf '424242\\n'; return 0; }
mock_launch() {
    python3 - "$@" <<'MOCKPY'
import json, os, pathlib, sys
pathlib.Path(os.environ['MOCK_ARGUMENTS']).write_text(json.dumps(sys.argv[1:]))
pathlib.Path(os.environ['MOCK_RUNNING']).touch()
MOCKPY
    printf 'Boot completed in 100 ms\\n'
}
setsid() { mock_launch "$@"; }
nohup() { mock_launch "$@"; }
''')
        mockbin = root / "mockbin"
        mockbin.mkdir()
        if kind == "darwin":
            uname = mockbin / "uname"
            uname.write_text("#!/bin/sh\ncase \"${1:-}\" in -m) echo arm64 ;; *) echo Darwin ;; esac\n")
            uname.chmod(0o755)
        state = {"redirects": set(), "commands": [], "drop": False, "reject": False, "exit_delay": 0}
        token_path = work / ".run/instances/default.token"

        class Console(socketserver.StreamRequestHandler):
            def handle(self):
                self.wfile.write(b"Android Console: Authentication required\r\nOK\r\n")
                if self.rfile.readline().decode().strip() != "auth console-test-token":
                    self.wfile.write(b"KO: authentication failed\r\n")
                    return
                self.wfile.write(b"OK\r\n")
                command = self.rfile.readline().decode().strip()
                state["commands"].append(command)
                if command == "redir list":
                    for host, guest in state["redirects"]:
                        self.wfile.write(f"tcp:{host} => {guest}\r\n".encode())
                elif command.startswith("redir add tcp:"):
                    state["redirects"].add(tuple(map(int, command.removeprefix("redir add tcp:").split(":"))))
                elif command == "kill":
                    running.unlink(missing_ok=True)
                    self.wfile.write(b"killing emulator\r\n")
                self.wfile.write(b"OK\r\n")

        class Http(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def serve(self):
                instance = work / ".run/instances/default.env"
                values = dict(line.split("=", 1) for line in instance.read_text().splitlines()
                              if "=" in line and not line.startswith("#"))
                token = token_path.read_text().strip() if token_path.exists() else ""
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                if values.get("SERVICE_AUTH") == "1" and self.headers.get("X-Remote-Control-Token") != token:
                    code, response = 401, {"error": "unauthorized"}
                elif self.command == "POST":
                    assert self.path == "/api/v1/power" and json.loads(body) == {"action": "shutdown"}
                    if state["reject"]:
                        code, response = 503, {"ok": False}
                    else:
                        if state["exit_delay"]:
                            timer = threading.Timer(state["exit_delay"], lambda: running.unlink(missing_ok=True))
                            timer.daemon = True
                            timer.start()
                        else:
                            running.unlink(missing_ok=True)
                        if state["drop"]:
                            self.close_connection = True
                            return
                        code, response = 200, {"ok": True}
                else:
                    code, response = 200, {"service": "remote-control"}
                payload = json.dumps(response).encode()
                self.send_response(code)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            do_GET = serve
            do_POST = serve

        console = Server(("127.0.0.1", 0), Console)
        if console.server_address[1] % 2:
            candidate = console.server_address[1] + 1
            console.server_close()
            while True:
                try:
                    console = Server(("127.0.0.1", candidate), Console)
                    break
                except OSError:
                    candidate += 2
        http_peer = Server(("127.0.0.1", 0), Http)
        for server in (console, http_peer):
            threading.Thread(target=server.serve_forever, daemon=True).start()
        port = console.server_address[1]
        env = dict(os.environ, AUTOSNAP_HTTP_PORT=str(http_peer.server_address[1]),
                   AUTOSNAP_CONSOLE_TOKEN_FILE=str(console_token), EMULATOR_BIN=str(emulator),
                   EMULATOR_PORT_BASE=str(port), AUTOSNAP_PORT_BASE=str(port),
                   ADB=str(adb), AUTOSNAP_ADB=str(adb), MOCK_ADB_LOG=str(adb_log),
                   MOCK_RUNNING=str(running), MOCK_ARGUMENTS=str(args_file), NET_BRIDGE_IF="",
                   PATH=str(mockbin) + os.pathsep + os.environ["PATH"])

        def run(script, *args, success=True):
            result = subprocess.run(["bash", str(bindir / script), *args], env=env,
                                    capture_output=True, text=True, timeout=40)
            if success:
                assert result.returncode == 0, f"{kind}/{script}: {result.stdout}\n{result.stderr}"
            else:
                assert result.returncode != 0, f"{kind}/{script}: unexpectedly succeeded"
            return result.stdout + result.stderr

        def start(reuse=False, test_instance=False):
            if kind == "emulator":
                return run("emulator.sh", "start", "default", "--port", str(port), "--nat",
                           *(('--test-instance',) if test_instance else ()))
            if kind == "run-linux":
                return run("run-linux.sh", "--name", "default", "--port", str(port), *(('--reuse',) if reuse else ()))
            return run("start-headless.sh", "--port", str(port), "--accel", "off", "--timeout", "5",
                       *(('--reuse',) if reuse else ()),
                       *(('--test-instance',) if test_instance else ()))

        def stop(success=True, force=False, timeout=2):
            env["AUTOSNAP_STOP_TIMEOUT"] = str(timeout)
            if kind == "emulator":
                return run("emulator.sh", "kill" if force else "stop", "default", success=success)
            if kind == "run-linux":
                # --port intentionally follows --stop to cover argument order.
                return run("run-linux.sh", "--stop", "--port", str(port), *(('--force',) if force else ()), success=success)
            return run("stop.sh", "--port", str(port), "--timeout", str(timeout), *(('--force',) if force else ()), success=success)

        try:
            output = start()
            token = token_path.read_text().strip()
            props = json.loads(args_file.read_text())
            check(len(token) == 64 and token not in output and token_path.stat().st_mode & 0o777 == 0o600,
                  f"{kind}: generated token stays private")
            check(f"qemu.rc.token={token}" in props and not any("vendor.qemu.rc." in str(p) for p in props),
                  f"{kind}: emulator receives supported qemu properties")
            check((http_peer.server_address[1], 8088) in state["redirects"], f"{kind}: console auth and NAT HTTP redirect")
            if kind in ("linux", "darwin"):
                output = run("status.sh", "--port", str(port))
            else:
                output = run("emulator.sh", "status", "default")
            check("就绪" in output and token not in output, f"{kind}: authenticated HTTP status without ADB")
            stop()
            check(not running.exists() and "kill" not in state["commands"], f"{kind}: graceful guest poweroff")
            config.write_text(config_text.replace("service.port=8088", "service.port=9090")
                              .replace("service.auth=1", "service.auth=0")
                              .replace("service.token=", "service.token=changed-template-token"))
            start(reuse=True)
            props = json.loads(args_file.read_text())
            check(token_path.read_text().strip() == token and f"qemu.rc.token={token}" in props
                  and "qemu.rc.port=8088" in props and "qemu.rc.auth=1" in props,
                  f"{kind}: reuse preserves port/auth/token despite template changes")
            if kind in ("linux", "darwin", "emulator"):
                stop()
                start(reuse=True, test_instance=True)
                props = json.loads(args_file.read_text())
                check(token_path.read_text().strip() == token and f"qemu.rc.token={token}" in props
                      and "qemu.rc.auth=1" in props,
                      f"{kind}: test-instance flag preserves authentication on reuse")
            state["drop"] = True
            state["exit_delay"] = 3.5
            shutdown_started = time.monotonic()
            stop(timeout=6)
            shutdown_elapsed = time.monotonic() - shutdown_started
            check(not running.exists() and shutdown_elapsed >= 3.4 and "kill" not in state["commands"],
                  f"{kind}: dropped shutdown response waits for delayed process exit beyond 3s "
                  f"(elapsed={shutdown_elapsed:.2f}s, running={running.exists()}, commands={state['commands']})")
            state["drop"] = False
            state["exit_delay"] = 0
            start(reuse=True)
            state["reject"] = True
            stop(success=False)
            check(running.exists() and "kill" not in state["commands"], f"{kind}: failed graceful request preserves guest")
            stop(force=True)
            state["reject"] = False
            check(not running.exists() and "kill" in state["commands"], f"{kind}: explicit force uses console kill")
            check(not adb_log.exists(), f"{kind}: lifecycle never invokes ADB")
            if kind == "emulator":
                run("emulator.sh", "clone", "default", "clone", "--port", str(port + 2))
                clone_token = work / ".run/instances/clone.token"
                clone_metadata = (work / ".run/instances/clone.env").read_text()
                check(clone_token.read_text().strip() == token and "SERVICE_PORT=8088" in clone_metadata
                      and "HTTP_PORT=" not in clone_metadata,
                      "emulator: clone carries guest service credentials without host ports")
                archive = root / "guest.tar"
                run("emulator.sh", "export", "default", "--out", str(archive))
                with tarfile.open(archive) as exported:
                    saved_token = exported.extractfile("./host-service.token").read().decode().strip()
                check(saved_token == token and archive.stat().st_mode & 0o777 == 0o600,
                      "emulator: export preserves service token in a private archive")
                run("emulator.sh", "import", str(archive), "--name", "restored", "--port", str(port + 4), "--unsafe")
                restored_metadata = (work / ".run/instances/restored.env").read_text()
                restored_token = work / ".run/instances/restored.token"
                check(restored_token.read_text().strip() == token and "SERVICE_PORT=8088" in restored_metadata
                      and not (work / ".run/host-service.token").exists(),
                      "emulator: import restores host service credentials")
                run("emulator.sh", "reset", "default", "--yes")
                check(not token_path.exists() and "SERVICE_PORT=" not in (work / ".run/instances/default.env").read_text(),
                      "emulator: reset clears persisted host service defaults and token")
                start(test_instance=True)
                props = json.loads(args_file.read_text())
                check(not token_path.exists() and "qemu.rc.auth=0" in props
                      and not any(arg.startswith("qemu.rc.token=") for arg in props),
                      "emulator: test-instance starts a new guest without a service token")
                stop()
                start(reuse=True, test_instance=True)
                props = json.loads(args_file.read_text())
                check(not token_path.exists() and "qemu.rc.auth=0" in props
                      and not any(arg.startswith("qemu.rc.token=") for arg in props),
                      "emulator: test-instance reuse preserves the guest's unauthenticated settings")
                stop()
            if kind in ("linux", "darwin", "run-linux"):
                token_path.unlink()
                if kind == "run-linux":
                    output = run("run-linux.sh", "--name", "default", "--port", str(port), "--reuse", success=False)
                else:
                    output = run("start-headless.sh", "--port", str(port), "--accel", "off", "--reuse", success=False)
                check("缺少服务令牌" in output and not running.exists(),
                      f"{kind}: missing reuse token fails before launching guest")
                if kind in ("linux", "darwin"):
                    start(test_instance=True)
                    props = json.loads(args_file.read_text())
                    check(not token_path.exists() and "qemu.rc.auth=0" in props
                          and not any(arg.startswith("qemu.rc.token=") for arg in props),
                          f"{kind}: test-instance starts a new guest without a service token")
                    stop()
                    start(reuse=True, test_instance=True)
                    props = json.loads(args_file.read_text())
                    check(not token_path.exists() and "qemu.rc.auth=0" in props
                          and not any(arg.startswith("qemu.rc.token=") for arg in props),
                          f"{kind}: test-instance reuse preserves the guest's unauthenticated settings")
                    stop()
            if kind == "run-linux":
                env["ADB"] = str(root / "missing-adb")
                output = run("run-linux.sh", "--verify", "--port", str(port), success=False)
                check("显式诊断需要 ADB" in output, "run-linux: explicit --verify requires ADB")
        finally:
            for server in (console, http_peer):
                server.shutdown()
                server.server_close()
print(f"{count} lifecycle checks passed")
