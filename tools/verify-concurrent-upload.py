#!/usr/bin/env python3
"""两个上传同时打同一个目标名 —— 结果必须是**其中一个的完整内容**。

文档承诺（docs/api/01-http.md）：
    服务会串行化自身上传并在发布前检查目标；绕过此服务、直接写共享目录的
    其他进程不参与这把锁。

如果串行化没生效，两个 spool 同时往同一个目标发布，落盘的就是**两个文件交错的
产物** —— 大小可能对、哈希一定不对。这种损坏不会报错，只会让人以为"上传成功"。

所以判据是**设备侧 sha256 必须等于 A 或 B 之一**，不是接口返回 ok。

用法: RC_TOKEN=xxx python3 tools/verify-concurrent-upload.py <host:port> [轮数]
"""
import concurrent.futures as cf
import hashlib
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

SIZE = 6 << 20          # >4MiB：必然走 spool，要测的就是那条路


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:18088"
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    token = os.environ.get("RC_TOKEN", "")
    serial = os.environ.get("RC_SERIAL", "emulator-5580")
    if not token:
        print("✗ 需要令牌：RC_TOKEN=<令牌>", file=sys.stderr)
        return 2

    def sh(cmd):
        return subprocess.run(["adb", "-s", serial, "shell", cmd],
                              capture_output=True, text=True, timeout=40).stdout.strip()

    def post(path, body):
        req = urllib.request.Request(
            f"http://{base}{path}", data=body, method="POST",
            headers={"Authorization": "Bearer " + token,
                     "Content-Type": "application/octet-stream"})
        try:
            with urllib.request.urlopen(req, timeout=90) as r:
                return r.status, ""
        except urllib.error.HTTPError as e:
            return e.code, e.read()[:100].decode(errors="replace")
        except Exception as e:
            return 0, str(e)[:100]

    a = bytes((i * 7 + 11) & 0xFF for i in range(SIZE))
    b = bytes((i * 13 + 29) & 0xFF for i in range(SIZE))
    sha_a = hashlib.sha256(a).hexdigest()
    sha_b = hashlib.sha256(b).hexdigest()
    print(f"A = {sha_a[:16]}…    B = {sha_b[:16]}…    各 {SIZE} 字节")
    print(f"每轮把 A、B **同时** POST 到同一个名字，共 {rounds} 轮\n")

    bad = 0
    for i in range(1, rounds + 1):
        name = f"conc-{i}.bin"
        path = f"/api/v1/files/upload?path=%2Fsdcard%2FDownload&name={name}"
        with cf.ThreadPoolExecutor(max_workers=2) as ex:
            fa, fb = ex.submit(post, path, a), ex.submit(post, path, b)
            ra, rb = fa.result(), fb.result()
        got = sh(f"sha256sum /sdcard/Download/{name} 2>/dev/null | cut -d' ' -f1").strip()
        if got == sha_a:
            verdict = "A（完整）"
        elif got == sha_b:
            verdict = "B（完整）"
        else:
            verdict = f"**都不是！交错产物** got={got[:16]}…"
            bad += 1
        print(f"  第 {i} 轮  HTTP {ra[0]}/{rb[0]}   落盘 = {verdict}")
        sh(f"rm -f /sdcard/Download/{name}")

    print()
    if bad:
        print(f"✗ {bad}/{rounds} 轮落盘内容既不是 A 也不是 B —— 串行化没生效")
        return 1
    print(f"✓ {rounds}/{rounds} 轮落盘都是某一个的**完整**内容 —— 串行化生效")

    # 反面：**不同**名字的两个上传不该被串行化掉（那会变成“并发上传互相排队”）。
    # 只测“同名字不坏”是不够的 —— 一个把所有上传都串起来的实现同样能通过上面那条。
    print("\n不同名字的两个上传（应当都能成功）：")
    with cf.ThreadPoolExecutor(max_workers=2) as ex:
        f1 = ex.submit(post, "/api/v1/files/upload?path=%2Fsdcard%2FDownload&name=par-a.bin", a)
        f2 = ex.submit(post, "/api/v1/files/upload?path=%2Fsdcard%2FDownload&name=par-b.bin", b)
        r1, r2 = f1.result(), f2.result()
    s1 = sh("sha256sum /sdcard/Download/par-a.bin 2>/dev/null | cut -d' ' -f1").strip()
    s2 = sh("sha256sum /sdcard/Download/par-b.bin 2>/dev/null | cut -d' ' -f1").strip()
    sh("rm -f /sdcard/Download/par-a.bin /sdcard/Download/par-b.bin")
    ok_par = (r1[0] in (200, 201) and r2[0] in (200, 201)
              and s1 == sha_a and s2 == sha_b)
    print(f"  HTTP {r1[0]}/{r2[0]}   a={'OK' if s1 == sha_a else 'BAD'} "
          f"b={'OK' if s2 == sha_b else 'BAD'}")
    if not ok_par:
        print("✗ 不同名字的并发上传没能各自成功 —— 串行化过头了")
        return 1
    print("✓ 不同名字互不阻塞，两份内容都对")
    return 0


if __name__ == "__main__":
    sys.exit(main())
