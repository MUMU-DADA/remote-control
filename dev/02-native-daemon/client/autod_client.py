#!/usr/bin/env python3
"""autod 的 Python 参考客户端。

用途：
  1. 协议文档 —— 比 C++ 版本更容易读懂字节布局
  2. 主机侧联调 —— 配合 `--mock` 模式，不需要设备就能验证协议逻辑
  3. 自动化脚本 —— 在设备上跑（需要设备上有 python3）

注意：SOCK_SEQPACKET 和 SCM_RIGHTS 都过不了 `adb forward`（TCP），
      所以连真设备时这个脚本必须在设备上运行。

用法：
    # 主机上起 mock 服务端，验证客户端逻辑
    python3 autod_client.py --mock /tmp/autod.sock info

    # 连真设备（脚本需在设备上）
    python3 autod_client.py --socket /data/local/tmp/autod.sock capture -o shot.png
"""

import argparse
import array
import mmap
import os
import socket
import struct
import sys
import zlib

MAGIC = 0x44545541  # 'AUTD'

CMD = {
    "info": 1, "capture": 2, "tap": 3, "swipe": 4,
    "touch_down": 5, "touch_move": 6, "touch_up": 7,
}
CMD_NAME = {v: k for k, v in CMD.items()}

STATUS = {
    0x0000: "ok",
    0x1001: "bad magic",
    0x1002: "unknown command",
    0x1003: "bad argument",
    0x1004: "no display found",
    0x1005: "capture failed",
    0x1006: "injection failed",
    0x1007: "internal error",
    0x1008: "unsupported",
}

# <IIII iiii Iff  —— 与 daemon/protocol.h 的 Request 严格对应
REQUEST_FMT = "<IIIIiiiiIff"
REQUEST_SIZE = struct.calcsize(REQUEST_FMT)

# <IIIIIIIIQ —— 与 Reply 严格对应。
# 注意这里有 8 个 I 而不是 7 个：C++ 侧的 `reserved` 字段不是装饰，
# 它占掉了编译器本来会插入的隐式填充。少了它就会错位 4 字节。
REPLY_FMT = "<IIIIIIIIQ"
REPLY_SIZE = struct.calcsize(REPLY_FMT)

assert REQUEST_SIZE == 44, f"Request 大小不符: {REQUEST_SIZE}"
assert REPLY_SIZE == 40, f"Reply 大小不符: {REPLY_SIZE}"

# 字段索引，避免解包时数错位置
_R_MAGIC, _R_STATUS, _R_CMD = 0, 1, 2
_R_WIDTH, _R_HEIGHT, _R_STRIDE, _R_FORMAT = 3, 4, 5, 6
_R_RESERVED, _R_SIZE = 7, 8

# android PixelFormat 常量（frameworks/native/libs/ui/include/ui/PixelFormat.h）
PIXEL_FORMAT = {
    1: "RGBA_8888",
    2: "RGBX_8888",
    3: "RGB_888",
    4: "RGB_565",
    5: "BGRA_8888",
    22: "RGBA_FP16",
    43: "RGBA_1010102",
}


def make_request(cmd, **kw):
    return struct.pack(
        REQUEST_FMT,
        MAGIC,
        CMD[cmd],
        kw.get("flags", 0),
        kw.get("pointer_id", 0),
        kw.get("x", 0),
        kw.get("y", 0),
        kw.get("x2", 0),
        kw.get("y2", 0),
        kw.get("duration_ms", 0),
        kw.get("pressure", 0.0),
        kw.get("size", 0.0),
    )


def pack_reply(cmd, status=0, width=0, height=0, stride=0, fmt=0, size=0):
    return struct.pack(
        REPLY_FMT, MAGIC, status, cmd, width, height, stride, fmt, 0, size
    )


def parse_reply(data):
    fields = struct.unpack(REPLY_FMT, data)
    return {
        "magic": fields[_R_MAGIC],
        "status": fields[_R_STATUS],
        "cmd": fields[_R_CMD],
        "width": fields[_R_WIDTH],
        "height": fields[_R_HEIGHT],
        "stride": fields[_R_STRIDE],
        "format": fields[_R_FORMAT],
        "data_size": fields[_R_SIZE],
    }


# ---------------------------------------------------------------------------
# 传输
# ---------------------------------------------------------------------------

def transact(sock, payload):
    """发一条请求，收一条应答。返回 (reply_dict, fd 或 None)。"""
    sock.send(payload)

    fds = array.array("i")
    msg, ancdata, flags, _addr = sock.recvmsg(
        REPLY_SIZE, socket.CMSG_LEN(fds.itemsize)
    )
    if len(msg) != REPLY_SIZE:
        raise RuntimeError(f"应答长度不符: {len(msg)} != {REPLY_SIZE}")

    for level, ctype, cdata in ancdata:
        if level == socket.SOL_SOCKET and ctype == socket.SCM_RIGHTS:
            fds.frombytes(cdata[: len(cdata) - (len(cdata) % fds.itemsize)])

    reply = parse_reply(msg)
    if reply["magic"] != MAGIC:
        raise RuntimeError(f"magic 不匹配: 0x{reply['magic']:x}")
    if reply["status"] != 0:
        name = STATUS.get(reply["status"], f"0x{reply['status']:x}")
        raise RuntimeError(f"服务端错误: {name}")

    return reply, (fds[0] if fds else None)


def connect(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    s.connect(path)
    return s


# ---------------------------------------------------------------------------
# PNG 编码（纯标准库，无依赖）
# ---------------------------------------------------------------------------

def write_png(path, width, height, rgba):
    def chunk(typ, data):
        return (
            struct.pack(">I", len(data))
            + typ
            + data
            + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)
        )

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        raw += rgba[y * stride : (y + 1) * stride]

    blob = b"\x89PNG\r\n\x1a\n"
    blob += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    blob += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    blob += chunk(b"IEND", b"")

    with open(path, "wb") as f:
        f.write(blob)


def to_rgba(raw, width, height, fmt):
    """把 android PixelFormat 转成紧凑 RGBA_8888。"""
    if fmt == 1:  # RGBA_8888
        return raw
    if fmt == 2:  # RGBX_8888，把 X 填成 FF
        out = bytearray(raw)
        out[3::4] = b"\xff" * (len(raw) // 4)
        return bytes(out)
    if fmt == 5:  # BGRA_8888，交换 R/B
        out = bytearray(raw)
        out[0::4], out[2::4] = raw[2::4], raw[0::4]
        return bytes(out)
    if fmt == 4:  # RGB_565
        out = bytearray(width * height * 4)
        for i in range(width * height):
            px = raw[i * 2] | (raw[i * 2 + 1] << 8)
            r = ((px >> 11) & 0x1F) * 255 // 31
            g = ((px >> 5) & 0x3F) * 255 // 63
            b = (px & 0x1F) * 255 // 31
            out[i * 4 : i * 4 + 4] = bytes((r, g, b, 255))
        return bytes(out)
    raise RuntimeError(
        f"不支持的 pixel format {fmt} ({PIXEL_FORMAT.get(fmt, '?')})，"
        "需要先加转换逻辑"
    )


# ---------------------------------------------------------------------------
# 子命令
# ---------------------------------------------------------------------------

def do_info(sock):
    reply, _ = transact(sock, make_request("info"))
    print(f"分辨率: {reply['width']} x {reply['height']}")
    return 0


def do_capture(sock, out_path, raw_mode):
    reply, fd = transact(sock, make_request("capture"))
    if fd is None:
        print("服务端没有返回帧 fd", file=sys.stderr)
        return 1

    size = reply["data_size"]
    w, h, stride, fmt = reply["width"], reply["height"], reply["stride"], reply["format"]

    if raw_mode:
        with os.fdopen(fd, "rb") as f:
            data = f.read(size)
        with open(out_path, "wb") as f:
            f.write(data)
        print(f"已写入 {out_path}（原始 {PIXEL_FORMAT.get(fmt, fmt)}，"
              f"{w}x{h} stride={stride}，{size} 字节）")
        return 0

    with mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ) as mm:
        raw = mm[:size]
    os.close(fd)

    rgba = to_rgba(raw, w, h, fmt)
    write_png(out_path, w, h, rgba)
    print(f"已写入 {out_path}（PNG，{w}x{h}）")
    return 0


def do_tap(sock, x, y, ms):
    reply, _ = transact(sock, make_request("tap", x=x, y=y, duration_ms=ms))
    print(f"已点击 ({x}, {y})")
    return 0


def do_swipe(sock, x1, y1, x2, y2, ms):
    reply, _ = transact(
        sock, make_request("swipe", x=x1, y=y1, x2=x2, y2=y2, duration_ms=ms)
    )
    print(f"已滑动 ({x1},{y1}) -> ({x2},{y2})")
    return 0


# ---------------------------------------------------------------------------
# Mock 服务端 —— 主机侧验证协议用，不需要设备
# ---------------------------------------------------------------------------

def run_mock(path, width, height):
    if os.path.exists(path):
        os.unlink(path)

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    srv.bind(path)
    srv.listen(4)
    print(f"[mock] 监听 {path}，模拟 {width}x{height} 显示", file=sys.stderr)

    try:
        while True:
            conn, _ = srv.accept()
            with conn:
                while True:
                    try:
                        data = conn.recv(REQUEST_SIZE)
                    except ConnectionResetError:
                        break
                    if not data:
                        break
                    if len(data) != REQUEST_SIZE:
                        print(f"[mock] 请求长度不符: {len(data)}", file=sys.stderr)
                        break

                    (magic, cmd, flags, pid, x, y, x2, y2,
                     ms, pressure, size) = struct.unpack(REQUEST_FMT, data)
                    name = CMD_NAME.get(cmd, f"cmd={cmd}")

                    if magic != MAGIC:
                        print(f"[mock] magic 不匹配 0x{magic:x}", file=sys.stderr)
                        break

                    if cmd == CMD["info"]:
                        conn.send(pack_reply(cmd, width=width, height=height,
                                             stride=width, fmt=1))

                    elif cmd == CMD["capture"]:
                        # 生成一张彩色渐变测试图
                        fd = os.memfd_create("mock-frame")
                        os.ftruncate(fd, width * height * 4)
                        with mmap.mmap(fd, width * height * 4,
                                       mmap.MAP_SHARED, mmap.PROT_WRITE) as mm:
                            buf = bytearray(width * height * 4)
                            for row in range(height):
                                base = row * width * 4
                                for col in range(width):
                                    o = base + col * 4
                                    buf[o] = col * 255 // max(width - 1, 1)
                                    buf[o + 1] = row * 255 // max(height - 1, 1)
                                    buf[o + 2] = 128
                                    buf[o + 3] = 255
                            mm[:] = bytes(buf)
                        reply = pack_reply(cmd, width=width, height=height,
                                           stride=width, fmt=1,
                                           size=width * height * 4)
                        conn.sendmsg(
                            [reply],
                            [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                              array.array("i", [fd]))],
                        )
                        os.close(fd)

                    elif cmd in (CMD["tap"], CMD["swipe"],
                                 CMD["touch_down"], CMD["touch_move"],
                                 CMD["touch_up"]):
                        if cmd == CMD["swipe"]:
                            print(f"[mock] {name}: ({x},{y}) -> ({x2},{y2}) "
                                  f"{ms}ms", file=sys.stderr)
                        else:
                            print(f"[mock] {name}: ({x},{y}) {ms}ms",
                                  file=sys.stderr)
                        conn.send(pack_reply(cmd))

                    else:
                        print(f"[mock] 未知命令 {cmd}", file=sys.stderr)
                        conn.send(pack_reply(cmd, status=0x1002))
    except KeyboardInterrupt:
        print("\n[mock] 退出", file=sys.stderr)
    finally:
        srv.close()
        if os.path.exists(path):
            os.unlink(path)
    return 0


# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="autod 参考客户端",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--socket", help="autod 的 Unix socket 路径")
    parser.add_argument("--mock", metavar="PATH",
                        help="以 mock 服务端模式运行并监听该路径")
    parser.add_argument("--mock-size", default="1080x1920",
                        help="mock 的模拟分辨率（默认 1080x1920）")

    sub = parser.add_subparsers(dest="command")
    sub.add_parser("info", help="查询显示参数")

    p_cap = sub.add_parser("capture", help="截图")
    p_cap.add_argument("-o", "--output", default="shot.png")
    p_cap.add_argument("--raw", action="store_true",
                       help="输出原始像素而不是 PNG")

    p_tap = sub.add_parser("tap", help="单击")
    p_tap.add_argument("x", type=int)
    p_tap.add_argument("y", type=int)
    p_tap.add_argument("--ms", type=int, default=50)

    p_sw = sub.add_parser("swipe", help="滑动")
    p_sw.add_argument("x1", type=int)
    p_sw.add_argument("y1", type=int)
    p_sw.add_argument("x2", type=int)
    p_sw.add_argument("y2", type=int)
    p_sw.add_argument("--ms", type=int, default=300)

    args = parser.parse_args()

    if args.mock:
        w, h = (int(v) for v in args.mock_size.lower().split("x"))
        return run_mock(args.mock, w, h)

    if not args.socket:
        parser.error("需要 --socket 或 --mock")
    if not args.command:
        parser.error("需要指定子命令")

    sock = connect(args.socket)
    try:
        if args.command == "info":
            return do_info(sock)
        if args.command == "capture":
            return do_capture(sock, args.output, args.raw)
        if args.command == "tap":
            return do_tap(sock, args.x, args.y, args.ms)
        if args.command == "swipe":
            return do_swipe(sock, args.x1, args.y1, args.x2, args.y2, args.ms)
        parser.error(f"未知子命令 {args.command}")
    except RuntimeError as e:
        print(f"错误: {e}", file=sys.stderr)
        return 1
    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
