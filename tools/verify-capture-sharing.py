#!/usr/bin/env python3
"""验证"多客户端共享同一份抓帧"这条承诺 —— 真的量，不是读代码觉得合理。

承诺写在这些地方：
  · docs/05-design-notes.md「三个客户端每个都跑满 30 fps，而抓帧次数几乎没变」
  · docs/03-reference.md「一个抓帧线程按所有订阅者的最高需求定时抓」
  · rest_api.cpp「多个客户端会等到同一帧 —— 这正是共享的意义」

为什么要专门有个工具：这两条是**性能承诺**，回归了不会报错、也不会让谁的功能
坏掉，只会悄悄多烧 CPU（三个客户端就抓三次）。功能测试发现不了。

判据取**服务端 /params.capture.frames 的增长**，不是客户端收了多少 ——
后者在"各自抓帧"和"共享抓帧"两种情况下是一样的，区分不出来。

用法:
    RC_TOKEN=<令牌> python3 tools/verify-capture-sharing.py [host:port]

令牌按 --token / RC_TOKEN / 设备上的配置文件顺序取（同 functional-sweep）。
退出码 0 = 两条承诺都成立。
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.request

BOUNDARY = b"--remote-control-frame"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target", nargs="?", default="127.0.0.1:18088",
                    help="host:port（默认 127.0.0.1:18088）")
    ap.add_argument("--token", default=os.environ.get("RC_TOKEN", ""))
    ap.add_argument("--secs", type=float, default=8.0)
    ap.add_argument("--serial", default=os.environ.get("RC_SERIAL", "emulator-5580"))
    a = ap.parse_args()

    token = a.token
    if not token:                      # 交付镜像默认开鉴权，拿不到令牌就别装作测过了
        try:
            out = subprocess.run(
                ["adb", "-s", a.serial, "shell",
                 "grep -m1 '^token=' /data/misc/remote-control/remote-control.conf"],
                capture_output=True, text=True, timeout=15).stdout.strip()
            if out.startswith("token="):
                token = out.split("=", 1)[1]
        except Exception:
            pass
    if not token:
        print("✗ 没拿到访问令牌（--token= / RC_TOKEN= / 设备上的配置文件都不行）；"
              "不带令牌跑出来的数只会是 401", file=sys.stderr)
        return 2

    base = a.target
    host, port = base.rsplit(":", 1)

    def params():
        req = urllib.request.Request(
            f"http://{base}/api/v1/params",
            headers={"Authorization": "Bearer " + token})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.load(r)

    def hold(fps, secs, ready, stop):
        s = socket.create_connection((host, int(port)), timeout=15)
        s.settimeout(10)
        s.sendall((f"GET /api/v1/stream?format=jpeg&fps={fps}&skipUnchanged=0 HTTP/1.1\r\n"
                   f"Host: {host}\r\nAuthorization: Bearer {token}\r\n"
                   f"Connection: close\r\n\r\n").encode())
        frames, tail = 0, b""
        ready.set()
        t0 = time.monotonic()
        while time.monotonic() - t0 < secs and not stop.is_set():
            try:
                chunk = s.recv(1 << 20)
            except Exception:
                break
            if not chunk:
                break
            data = tail + chunk
            frames += data.count(BOUNDARY)
            tail = data[-32:]
        s.close()
        return frames

    def measure(fps_list):
        before = params()["capture"]["frames"]
        stop = threading.Event()
        results, threads, rdy = [], [], [threading.Event() for _ in fps_list]
        for i, f in enumerate(fps_list):
            t = threading.Thread(
                target=lambda i=i, f=f: results.append(hold(f, a.secs, rdy[i], stop)))
            t.start()
            threads.append(t)
        for e in rdy:
            e.wait(5)
        time.sleep(0.3)
        for t in threads:
            t.join(timeout=a.secs + 10)
        cap = params()["capture"]["frames"] - before
        return cap, sum(results)

    print(f"目标 {base}   每场 {a.secs:.0f} 秒   判据：服务端 capture.frames 的增长\n")
    print(f"{'场景':<26}{'抓帧次数':>10}{'客户端收到':>12}{'抓帧节奏':>12}")
    print("-" * 62)

    cap1, got1 = measure([30])
    print(f"{'1 路 @30fps':<26}{cap1:>10}{got1:>12}{cap1/a.secs:>11.1f}f")
    cap3, got3 = measure([30, 30, 30])
    print(f"{'3 路 @30fps（同一需求）':<26}{cap3:>10}{got3:>12}{cap3/a.secs:>11.1f}f")
    capm, gotm = measure([10, 30, 60])
    print(f"{'3 路 @10/30/60fps':<26}{capm:>10}{gotm:>12}{capm/a.secs:>11.1f}f")

    print()
    ok = True

    def chk(name, cond, detail=""):
        nonlocal ok
        print(f"  {'✓' if cond else '✗'} {name}   {detail}")
        if not cond:
            ok = False

    chk("三路同需求时抓帧没有变成三倍", cap3 < cap1 * 1.8,
        f"1 路 {cap1} → 3 路 {cap3}（各自抓帧应约 {cap1 * 3}）")
    chk("多路不同需求时按最高 fps 抓帧", capm / a.secs > 45,
        f"抓帧节奏 {capm / a.secs:.1f} fps（最高需求是 60）")
    chk("三路都真的收到了帧", got3 > cap3, f"合计收到 {got3} 帧 / 抓帧 {cap3} 次")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
