#!/usr/bin/env python3
"""验证控制台的上传：进度显示 + 大文件不再被拒。"""
import sys, time
from playwright.sync_api import sync_playwright

import argparse
_ap = argparse.ArgumentParser()
_ap.add_argument("--host", default="127.0.0.1")
_ap.add_argument("--port", type=int, default=8088)
_a = _ap.parse_args()
BASE = f"http://{_a.host}:{_a.port}"
CASES = [
    ("小 APK", "/root/AutoSnapshotAndroid/dev/05-controller-app/build/autod-controller.apk"),
    ("300MB", "/tmp/mid.apk"),
]

with sync_playwright() as p:
    b = p.chromium.launch(headless=True,
        executable_path="/root/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome",
        args=["--no-sandbox", "--disable-gpu",
              f"--unsafely-treat-insecure-origin-as-secure={BASE}"])
    for name, path in CASES:
        pg = b.new_page(viewport={"width": 900, "height": 1000})
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.goto(BASE + "/", wait_until="domcontentloaded", timeout=30000)
        time.sleep(2)
        pg.evaluate("() => { try { toggleCard('card-apps'); } catch (e) {} }")
        time.sleep(1)

        seen = []
        pg.set_input_files("#apkfile", path)
        # 记下网络层的结果，别只看界面文字：
        # 大文件那条用例喂的是随机数据，安装**必然**失败，
        # 但"上传有没有被接受"才是这里要验的（413 = 没接受）。
        net = []
        pg.on("response", lambda r: net.append(r.status)
              if "install" in r.url else None)
        for _ in range(90):
            time.sleep(1)
            m = pg.evaluate("() => { const e=document.getElementById('apkmsg'); return e?e.textContent:null; }")
            if m and (not seen or seen[-1] != m):
                seen.append(m)
            if m and ("上传中" not in m) and ("正在安装" not in m):
                break
        print(f"  ── {name} ──")
        for s in seen:
            print(f"     {s}")
        print(f"     HTTP: {net}")
        if 413 in net:
            print("     ✗ 被 413 拒了 —— 大文件上传仍然失败")
        if errs:
            print(f"     页面错误: {errs[:1]}")
        pg.close()
    b.close()
