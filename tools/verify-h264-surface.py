#!/usr/bin/env python3
"""A7/A8 验证：H.264 走虚拟显示输入时，流是不是真的有画面。

为什么不能只看"收到多少字节"：**黑屏也是一串合法字节**。
虚拟显示这条路最容易出的错就是"建起来了、但 layer stack 没镜像对，
于是编码器一直在编一块黑屏" —— 那种情况下帧率、码率、字节数全都正常。
所以这里把收到的 H.264 解出图来，再和设备上直接抓的 JPEG 比。

用法: python3 tools/verify-h264-surface.py [--port 18099] [--seconds 8]
"""
import argparse
import base64
import os
import socket as sk
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

BASE = "http://127.0.0.1:18099"

# 访问令牌。交付镜像默认开鉴权 —— **握手那处最容易漏**：
# 它是自己拼的原始 HTTP 请求，不走 urllib，所以 header 得自己加。
# （这个坑在 functional-sweep 里踩过一次，这里是同一个。）
TOKEN = os.environ.get("RC_TOKEN", "")


def ws_h264_frames(port, seconds, path="/api/v1/stream?format=h264&fps=10"):
    """收 seconds 秒的 WebSocket 二进制帧，返回拼起来的载荷。"""
    s = sk.create_connection(("127.0.0.1", port), timeout=seconds + 4)
    key = base64.b64encode(b"0123456789abcdef").decode()
    auth = f"Authorization: Bearer {TOKEN}\r\n" if TOKEN else ""
    s.sendall((f"GET {path} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"{auth}"
               f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    head, data = buf.split(b"\r\n\r\n", 1)
    if b"101" not in head.split(b"\r\n")[0]:
        raise RuntimeError("WebSocket 没升级成功: " + head.split(b"\r\n")[0].decode(errors="replace"))

    s.settimeout(1.0)
    t0 = time.time()
    payloads = []
    while time.time() - t0 < seconds:
        try:
            c = s.recv(1 << 16)
        except sk.timeout:
            continue
        except Exception:
            break
        if not c:
            break
        data += c
        # 尽量多解析几帧出来
        while True:
            if len(data) < 2:
                break
            b1, b2 = data[0], data[1]
            ln = b2 & 0x7F
            off = 2
            if ln == 126:
                if len(data) < 4:
                    break
                ln = struct.unpack(">H", data[2:4])[0]
                off = 4
            elif ln == 127:
                if len(data) < 10:
                    break
                ln = struct.unpack(">Q", data[2:10])[0]
                off = 10
            if len(data) < off + ln:
                break
            if (b1 & 0x0F) == 2:
                payloads.append(data[off:off + ln])
            data = data[off + ln:]
    s.close()
    return payloads


def main():
    global BASE, TOKEN      # 命令行可覆盖（必须在函数里任何使用之前声明）
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18099)
    ap.add_argument("--token", default="", help="访问令牌（默认取 RC_TOKEN）")
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--adb-serial", default="emulator-5580")
    ap.add_argument("--base", default=BASE,
                    help="HTTP 基地址（要和 --port 指同一个服务）")
    args = ap.parse_args()
    BASE = args.base.rstrip("/")
    TOKEN = args.token or TOKEN

    # 制造**持续**的画面变化。
    #
    # 只按一下 Home 是不够的：静止画面上虚拟显示也不产生新缓冲，
    # 编码器自然没输出 —— 那样量到的是"屏幕有多静"而不是"这条路通不通"。
    # 第一次就是这么量的，6 秒只收到 2 帧，看着像失败其实不是。
    stop = threading.Event()

    def animate():
        i = 0
        while not stop.is_set():
            y = 400 + (i % 40) * 12
            subprocess.run(["adb", "-s", args.adb_serial, "shell", "input", "swipe",
                            "360", str(y), "360", str(y + 120), "120"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            i += 1

    th = threading.Thread(target=animate, daemon=True)
    th.start()
    try:
        frames = ws_h264_frames(args.port, args.seconds)
    finally:
        stop.set()
        th.join(timeout=3)
    total = sum(len(f) for f in frames)
    print(f"收到 {len(frames)} 个二进制帧，共 {total} 字节")
    if not frames:
        print("✗ 一帧都没收到")
        return 1

    # 关键：解出图，证明不是黑屏
    if not _has("ffmpeg"):
        print("! 没有 ffmpeg，只能验到'收到了字节'这层 —— 黑屏也是合法字节，"
              "这一步不算通过")
        return 2

    with tempfile.TemporaryDirectory() as d:
        h264 = os.path.join(d, "s.h264")
        png = os.path.join(d, "s.png")
        with open(h264, "wb") as fh:
            fh.write(b"".join(frames))
        r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", h264,
                            "-frames:v", "1", png],
                           capture_output=True)
        if r.returncode != 0 or not os.path.exists(png):
            print("✗ ffmpeg 解不出画面:", r.stderr.decode(errors="replace")[:200])
            return 1
        print("✓ 解出第一帧 →", png)

        # 与设备上直接抓的 JPEG 比"是不是全黑"
        jpg = os.path.join(d, "cap.jpg")
        _req = urllib.request.Request(
            f"{BASE}/api/v1/capture",
            headers={"Authorization": "Bearer " + TOKEN} if TOKEN else {})
        with urllib.request.urlopen(_req, timeout=10) as resp:
            open(jpg, "wb").write(resp.read())
        for name, path in (("流里的帧", png), ("设备抓帧", jpg)):
            r = subprocess.run(
                ["ffmpeg", "-y", "-loglevel", "error", "-i", path,
                 "-vf", "signalstats,metadata=print:file=-", "-f", "null", "-"],
                capture_output=True)
            out = r.stdout.decode(errors="replace")
            yavg = [l for l in out.splitlines() if "YAVG" in l]
            print(f"  {name} 亮度均值: {yavg[0].split()[-1] if yavg else '?'}")
    return 0


def _has(name):
    return subprocess.run(["which", name], capture_output=True).returncode == 0


if __name__ == "__main__":
    sys.exit(main())
