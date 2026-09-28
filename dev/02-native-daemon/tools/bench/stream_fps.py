#!/usr/bin/env python3
"""测真实到达客户端的帧率，并对照服务端自报的抓帧节奏。

    ./stream_fps.py                          # 720p / jpeg / 30fps / 8s
    ./stream_fps.py --fps 60 --secs 6
    ./stream_fps.py --format webp --quality 60
    ./stream_fps.py --compare                # jpeg/webp/png 各跑一遍

⚠️ 两个"帧率"不是一回事，别读混：

    投递帧率  客户端真收到多少帧 —— 受 skipUnchanged 影响，画面不动就没帧
    抓帧节奏  服务端 /params.capture.frames 的增长速度 —— 只跟订阅者的 fps 有关

模拟器桌面是静态的，默认 skipUnchanged=1 时投递帧率会低到 1 fps 上下，
那**不是**性能问题。所以这里默认带 skipUnchanged=0 再测投递能力。
"""

import argparse
import json
import socket
import sys
import time
import urllib.request

BOUNDARY = "autodframe"


def http_get(url: str, timeout: float = 5.0) -> bytes:
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def params(base: str) -> dict:
    try:
        return json.loads(http_get(base + "/api/v1/params"))
    except Exception:
        return {}


def measure_once(host: str, port: int, path: str, secs: float):
    """返回 (帧数, 收到字节数, 首帧延迟秒)。

    计数只在新到达的数据里做，并把 marker 长度-1 的尾巴留到下一轮 ——
    否则 marker 跨 recv 边界会漏计；而如果整段 buf 反复 count，
    留在手里的那个 marker 又会被重复计入。
    """
    s = socket.create_connection((host, port), timeout=10)
    s.settimeout(10)
    s.sendall(
        f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\n"
        f"Connection: close\r\n\r\n".encode()
    )

    marker = ("--" + BOUNDARY).encode()
    keep = max(len(marker) - 1, 0)
    tail = b""
    frames = 0
    total = 0
    t0 = time.monotonic()
    first = None
    deadline = t0 + secs

    try:
        while time.monotonic() < deadline:
            try:
                chunk = s.recv(1 << 20)
            except socket.timeout:
                break
            if not chunk:
                break
            total += len(chunk)
            data = tail + chunk
            n = data.count(marker)
            if n:
                if first is None:
                    first = time.monotonic() - t0
                frames += n
            tail = data[-keep:] if keep else b""
    finally:
        s.close()

    return frames, total, first


def run(base, host, port, fps, fmt, quality, max_width, secs,
        skip_unchanged, label=""):
    before = params(base).get("capture", {})
    path = f"/api/v1/stream?fps={fps}&format={fmt}"
    if quality is not None:
        path += f"&quality={quality}"
    if max_width is not None:
        path += f"&maxWidth={max_width}"
    if skip_unchanged is not None:
        path += f"&skipUnchanged={1 if skip_unchanged else 0}"

    frames, total, first = measure_once(host, port, path, secs)
    after = params(base).get("capture", {})

    delivered = frames / secs
    per_frame = (total / frames) if frames else 0
    cap_delta = after.get("frames", 0) - before.get("frames", 0)
    miss_delta = after.get("misses", 0) - before.get("misses", 0)

    tag = f"{label:<5}" if label else ""
    print(
        f"  {tag} {fmt:<5} q={str(quality if quality is not None else '-'):<3} "
        f"maxW={str(max_width if max_width is not None else '-'):<5} "
        f"→ 投递 {delivered:5.1f} fps / 目标 {fps}"
        f" | {per_frame/1024:6.1f} KiB/帧"
        f" | 抓帧 {after.get('lastCaptureMs', 0):5.1f} ms"
        f" | 抓帧节奏 {cap_delta/secs:5.1f} fps"
        f" | 丢 {miss_delta:3d}"
        f" | 首帧 {(first*1000 if first else 0):5.0f} ms"
    )
    return delivered


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=18088)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--format", default="jpeg")
    ap.add_argument("--quality", type=int, default=None)
    ap.add_argument("--max-width", type=int, default=None)
    ap.add_argument("--secs", type=float, default=8.0)
    ap.add_argument("--skip-unchanged", dest="skip", action="store_true",
                    default=False,
                    help="按服务端默认行为测（画面不动就不出帧）")
    ap.add_argument("--compare", action="store_true",
                    help="jpeg / webp / png 各跑一遍")
    a = ap.parse_args()

    base = f"http://{a.host}:{a.port}"
    p = params(base)
    if not p:
        print("服务不可达", file=sys.stderr)
        return 1

    cap = p.get("capture", {})
    print(f"服务端 capture: {json.dumps(cap, ensure_ascii=False)}")
    print(f"测量 {a.secs:.0f}s / 目标 {a.fps} fps / "
          f"skipUnchanged={1 if a.skip else 0}\n")

    args = (base, a.host, a.port, a.fps, None, a.quality, a.max_width,
            a.secs, a.skip)
    if a.compare:
        for fmt in ("jpeg", "webp", "png"):
            run(*args[:4], fmt, *args[5:], label=fmt)
    else:
        run(*args[:4], a.format, *args[5:])

    print("\n  退出后:", json.dumps(params(base).get("capture", {}),
                                    ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
