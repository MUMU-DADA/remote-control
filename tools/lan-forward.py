#!/usr/bin/env python3
# =============================================================================
# lan-forward.py —— 把模拟器的端口暴露到局域网网卡
#
# 为什么需要它：
#   Android 模拟器（和 adb forward）都只监听 127.0.0.1，局域网上够不着。
#   这个转发器监听 0.0.0.0:<端口>，把连接转给 127.0.0.1:<目标端口>。
#
# 为什么不用真桥接（-net-tap + 网桥）：
#   真桥接要把 ens33 加进网桥并迁移 IP，中间必然有一个"没有 IP"的窗口 ——
#   远程 SSH 会当场断开，配错了只能到机器跟前救。
#   端口转发能达到同样的访问效果，且不动网络配置、随时可撤。
#
# 用法:
#   python3 lan-forward.py            # 前台跑
#   systemctl start remote-control-lan-forward # 作为服务跑（见 install-service.sh）
# =============================================================================

import socket
import selectors
import sys
import threading

# (监听端口, 目标端口, 说明)
#
# ⚠️ 目标端口不能和监听端口相同 —— adb forward 与模拟器都监听 127.0.0.1
#    的 8088/5555，而 0.0.0.0:8088 与 127.0.0.1:8088 在 Linux 上**会冲突**
#    （通配地址和具体地址重叠，SO_REUSEADDR 也救不了）。
#    所以先把 adb forward 挪到 18088，转发的目标就是它。
# HTTP 不再需要转发：remote-control 现在自己读 /sdcard/remote-control.conf 里的 bind/port，
# 可以直接绑 0.0.0.0。多一层转发反而让"改端口"变成一个改完就失联的操作 ——
# 上位机改了 remote-control 的端口，转发器还指着旧端口，两边对不上。
FORWARDS = [
    (15555, 5555, "模拟器的 adb 端口（adb connect 接受任意端口）"),
]

LISTEN_ADDR = "0.0.0.0"
TARGET_ADDR = "127.0.0.1"
BUF = 65536


def pump(src, dst, sel):
    """把一个方向的数据搬过去；对端关闭就注销两个 fd。"""
    try:
        data = src.recv(BUF)
    except (BlockingIOError, InterruptedError):
        return
    except OSError:
        data = b""
    if not data:
        # 一端关了，两个方向都收掉，让连接结束
        for s in (src, dst):
            try:
                sel.unregister(s)
            except (KeyError, ValueError):
                pass
            try:
                s.close()
            except OSError:
                pass
        return
    try:
        dst.sendall(data)
    except OSError:
        for s in (src, dst):
            try:
                sel.unregister(s)
            except (KeyError, ValueError):
                pass
            try:
                s.close()
            except OSError:
                pass


def serve_one(listen_port, target_port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((LISTEN_ADDR, listen_port))
    srv.listen(64)
    print(f"[lan-forward] {LISTEN_ADDR}:{listen_port} → "
          f"{TARGET_ADDR}:{target_port}", flush=True)

    while True:
        try:
            conn, peer = srv.accept()
        except OSError as e:
            print(f"[lan-forward] accept 失败: {e}", file=sys.stderr, flush=True)
            continue

        # 每个连接一个线程。局域网里连接数很少，
        # 用 selectors 做单线程事件循环反而更难读。
        threading.Thread(target=handle, args=(conn, peer, target_port),
                         daemon=True).start()


def handle(conn, peer, target_port):
    try:
        upstream = socket.create_connection((TARGET_ADDR, target_port), timeout=5)
    except OSError as e:
        # 目标没起来（比如模拟器没跑）—— 关掉连接即可，
        # 不要把整个转发器拖垮
        print(f"[lan-forward] 连不上 {TARGET_ADDR}:{target_port}: {e}",
              file=sys.stderr, flush=True)
        conn.close()
        return

    conn.setblocking(False)
    upstream.setblocking(False)

    sel = selectors.DefaultSelector()
    sel.register(conn, selectors.EVENT_READ, "c2u")
    sel.register(upstream, selectors.EVENT_READ, "u2c")

    try:
        while True:
            events = sel.select(timeout=300)   # 5 分钟没有任何数据就收掉
            if not events:
                break
            for key, _ in events:
                if key.data == "c2u":
                    pump(conn, upstream, sel)
                else:
                    pump(upstream, conn, sel)
            # 两个 fd 都被注销了就结束
            if not sel.get_map():
                break
    except OSError:
        pass
    finally:
        try:
            sel.close()
        except Exception:
            pass
        for s in (conn, upstream):
            try:
                s.close()
            except OSError:
                pass


def main():
    threads = []
    for listen_port, target_port, desc in FORWARDS:
        t = threading.Thread(target=serve_one, args=(listen_port, target_port),
                             daemon=True)
        t.start()
        threads.append(t)
        print(f"[lan-forward] 已启动：{desc}", flush=True)

    # 主线程只等着 —— 子线程是 daemon，主线程退出就全没了
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        print("\n[lan-forward] 退出", flush=True)


if __name__ == "__main__":
    main()
