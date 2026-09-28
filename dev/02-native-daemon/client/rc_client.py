#!/usr/bin/env python3
"""remote-control 的 Python 参考客户端。

用途：
  1. 协议文档 —— 比 C++ 版本更容易读懂字节布局
  2. 主机侧联调 —— 配合 `--mock` 模式，不需要设备就能验证协议逻辑
  3. 自动化脚本 —— 在设备上跑（需要设备上有 python3）

注意：SOCK_SEQPACKET 和 SCM_RIGHTS 都过不了 `adb forward`（TCP），
      所以连真设备时这个脚本必须在设备上运行。

用法：
    # 主机上起 mock 服务端，验证客户端逻辑
    python3 rc_client.py --mock /tmp/remote-control.sock info

    # 连真设备（脚本需在设备上）
    python3 rc_client.py --socket /data/local/tmp/remote-control.sock capture -o shot.png
"""

import argparse
import json
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
    # v2：应用与文件管理
    "list_apps": 10, "app_info": 11, "launch_app": 12, "kill_app": 13,
    "foreground_app": 14, "install_app": 15, "download": 16, "file_op": 17,
}

# v2 命令的 flags 位（与 protocol.h 的 Flags 对应）
FLAG_INCLUDE_SYSTEM = 1 << 3
FLAG_WITH_METADATA  = 1 << 4
FLAG_REPLACE        = 1 << 5
FLAG_RECURSIVE      = 1 << 6
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
    0x1009: "not found",
    0x100a: "permission denied",
    0x100b: "timeout",
    0x100c: "bad payload",
    0x100d: "io error",
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


def pack_payload(*parts):
    """把参数拼成 NUL 分隔的 payload。

    协议约定：v2 命令的请求 payload 是一串 NUL 分隔的 UTF-8 字符串。
    字符串本身不含 NUL，所以不需要转义，也不会有歧义。
    """
    return b"\0".join(p.encode("utf-8") if isinstance(p, str) else p
                       for p in parts if p is not None)


def transact_v2(sock, cmd, *parts, flags=0, fd=None):
    """发一条 v2 命令，收 JSON 应答。

    应答的 JSON 通过 memfd + SCM_RIGHTS 回来（和截图同一个通道），
    这里读出来解析。fd 参数用于 InstallApp：把 APK 用 memfd 送过去。
    """
    req = make_request(cmd, flags=flags)
    payload = pack_payload(*parts)

    msg_bytes = req + payload
    if fd is not None:
        sent = sock.sendmsg([msg_bytes],
                            [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                              array.array("i", [fd]))])
    else:
        sent = sock.send(msg_bytes)

    # ⚠️ SOCK_SEQPACKET 上不能用 sendall：
    #    sendall 在短写时会循环调用 send，那就变成**多条消息**了，
    #    而服务端按"一条消息 = 一个请求"解析，会直接错位。
    #    SEQPACKET 的 send 要么整条发出去，要么报 EMSGSIZE，校验长度即可。
    if sent != len(msg_bytes):
        raise RuntimeError(f"请求未完整发送: {sent} != {len(msg_bytes)}")

    fds = array.array("i")
    msg, ancdata, _flags, _addr = sock.recvmsg(
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

    # v2 的应答体是 JSON，即使失败也带在 fd 里（里面有可读的原因），
    # 所以先读内容再判断状态 —— 这样报错信息是服务端给的原文，
    # 而不是"internal error"这种没用的东西。
    doc = None
    if fds:
        try:
            data = os.pread(fds[0], reply["data_size"], 0)
            doc = json.loads(data.decode("utf-8"))
        finally:
            os.close(fds[0])

    if reply["status"] != 0:
        reason = (doc or {}).get("error") if isinstance(doc, dict) else None
        name = STATUS.get(reply["status"], f"0x{reply['status']:x}")
        raise RuntimeError(reason or name)

    return doc if doc is not None else {}


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



# ---------------------------------------------------------------------------
# v2：应用与文件管理
# ---------------------------------------------------------------------------

def _print_json(doc, compact=False):
    if compact:
        print(json.dumps(doc, ensure_ascii=False))
    else:
        print(json.dumps(doc, ensure_ascii=False, indent=2))


def do_list_apps(sock, include_system=False, with_metadata=False,
                 names_only=False):
    flags = 0
    if include_system: flags |= FLAG_INCLUDE_SYSTEM
    if with_metadata:  flags |= FLAG_WITH_METADATA
    doc = transact_v2(sock, "list_apps", flags=flags)
    if names_only:
        for a in doc.get("apps", []):
            print(a["package"])
        return 0
    _print_json(doc)
    print(f"\n共 {doc.get('count', 0)} 个应用"
          f"（{'含系统' if include_system else '仅第三方'}）", file=sys.stderr)
    return 0


def do_app_info(sock, package):
    _print_json(transact_v2(sock, "app_info", package))
    return 0


def do_launch_app(sock, package, activity=None):
    _print_json(transact_v2(sock, "launch_app", package, activity))
    return 0


def do_kill_app(sock, package):
    _print_json(transact_v2(sock, "kill_app", package))
    return 0


def do_foreground(sock):
    _print_json(transact_v2(sock, "foreground_app"))
    return 0


def do_install(sock, apk_path, replace=True):
    """把 APK 通过 memfd 送过去。

    为什么用 fd 而不是把字节塞进 payload：SEQPACKET 单条消息约 208KB 上限，
    APK 动辄几十 MB。fd 通道没有大小限制。
    """
    size = os.path.getsize(apk_path)
    if size == 0:
        print("错误: APK 是空文件", file=sys.stderr)
        return 1

    fd = os.memfd_create("apk") if hasattr(os, "memfd_create") else None
    if fd is None:
        # 老 Python 没有 os.memfd_create，退化到临时文件
        import tempfile
        tf = tempfile.TemporaryFile()
        fd = tf.fileno()
    with open(apk_path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            os.write(fd, chunk)
    os.lseek(fd, 0, os.SEEK_SET)

    flags = FLAG_REPLACE if replace else 0
    try:
        doc = transact_v2(sock, "install_app", flags=flags, fd=fd)
    finally:
        os.close(fd)
    _print_json(doc)
    return 0


def do_download(sock, url, filename=None, subdir=None):
    _print_json(transact_v2(sock, "download", url, filename, subdir))
    return 0


def do_file_op(sock, op, path=None, arg2=None, recursive=False):
    flags = FLAG_RECURSIVE if recursive else 0
    doc = transact_v2(sock, "file_op", op, path, arg2, flags=flags)

    # list 的表格化输出比 JSON 好读
    if op == "list" and "entries" in doc:
        print(f"{doc.get('dir', '.')}  ({doc.get('count', 0)} 项)")
        for e in doc["entries"]:
            kind = "d" if e.get("dir") else "-"
            size = "" if e.get("dir") else f"{e.get('size', 0):>12,}"
            print(f"  {kind} {size}  {e['name']}")
        return 0
    _print_json(doc)
    return 0


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
        description="remote-control 参考客户端",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--socket", help="remote-control 的 Unix socket 路径")
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

    # ── v2：应用与文件管理 ──
    p_la = sub.add_parser("list-apps", help="列出应用")
    p_la.add_argument("--system", action="store_true", help="含系统应用")
    p_la.add_argument("--meta", action="store_true", help="附带版本/路径等元数据")
    p_la.add_argument("--names", action="store_true", help="只输出包名，便于管道")

    p_ai = sub.add_parser("app-info", help="应用详情（含权限与组件清单）")
    p_ai.add_argument("package")

    p_ln = sub.add_parser("launch", help="启动应用")
    p_ln.add_argument("package")
    p_ln.add_argument("activity", nargs="?", default=None)

    p_kl = sub.add_parser("kill", help="强制停止应用")
    p_kl.add_argument("package")

    sub.add_parser("foreground", help="查询当前前台应用")

    p_in = sub.add_parser("install", help="安装 APK（内容经 memfd 传输）")
    p_in.add_argument("apk")
    p_in.add_argument("--no-replace", action="store_true",
                      help="不覆盖已安装的同名应用")

    p_dl = sub.add_parser("download", help="下载文件到下载目录")
    p_dl.add_argument("url")
    p_dl.add_argument("filename", nargs="?", default=None)
    p_dl.add_argument("--subdir", default=None, help="下载目录下的子目录")

    p_fl = sub.add_parser("ls", help="列出下载目录")
    p_fl.add_argument("path", nargs="?", default="")
    p_fs = sub.add_parser("stat", help="查看文件信息")
    p_fs.add_argument("path")
    p_fm = sub.add_parser("mkdir", help="创建目录")
    p_fm.add_argument("path")
    p_fm.add_argument("-p", "--parents", action="store_true")
    p_fr = sub.add_parser("rm", help="删除文件或目录")
    p_fr.add_argument("path")
    p_fr.add_argument("-r", "--recursive", action="store_true")
    p_fv = sub.add_parser("mv", help="重命名/移动")
    p_fv.add_argument("src")
    p_fv.add_argument("dst")

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

        if args.command == "list-apps":
            return do_list_apps(sock, args.system, args.meta, args.names)
        if args.command == "app-info":
            return do_app_info(sock, args.package)
        if args.command == "launch":
            return do_launch_app(sock, args.package, args.activity)
        if args.command == "kill":
            return do_kill_app(sock, args.package)
        if args.command == "foreground":
            return do_foreground(sock)
        if args.command == "install":
            return do_install(sock, args.apk, not args.no_replace)
        if args.command == "download":
            return do_download(sock, args.url, args.filename, args.subdir)
        if args.command == "ls":
            return do_file_op(sock, "list", args.path)
        if args.command == "stat":
            return do_file_op(sock, "stat", args.path)
        if args.command == "mkdir":
            return do_file_op(sock, "mkdir", args.path, recursive=args.parents)
        if args.command == "rm":
            return do_file_op(sock, "delete", args.path, recursive=args.recursive)
        if args.command == "mv":
            return do_file_op(sock, "rename", args.src, args.dst)

        parser.error(f"未知子命令 {args.command}")
    except RuntimeError as e:
        print(f"错误: {e}", file=sys.stderr)
        return 1
    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
