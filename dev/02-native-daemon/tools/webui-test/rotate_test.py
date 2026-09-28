#!/usr/bin/env python3
"""端到端：旋转设备 → 流尺寸 / 画布 / 触控坐标 是否都跟着走。

这是三个改动交汇的地方：
  · Rotate 改设备方向（这台设备走 wm-size 退路）
  · size 消息在尺寸变化时重发 → 画布跟着改
  · 触控坐标按**触控范围**算 → 转屏后依然点得准
"""
import time, json, urllib.request
from playwright.sync_api import sync_playwright

import argparse
_ap = argparse.ArgumentParser()
_ap.add_argument("--host", default="127.0.0.1")
_ap.add_argument("--port", type=int, default=8088)
_a = _ap.parse_args()
BASE = f"http://{_a.host}:{_a.port}"

def api(path, body=None):
    req = urllib.request.Request(BASE + path,
        data=json.dumps(body).encode() if body else None,
        headers={"Content-Type": "application/json"} if body else {})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r)

checks = []
def chk(name, cond, extra=""):
    checks.append(bool(cond))
    print(f"  {'✓' if cond else '✗'} {name} {extra}")

def rotate(to):
    return api("/api/v1/rotate", {"to": to})

with sync_playwright() as p:
    b = p.chromium.launch(headless=True,
        executable_path="/root/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome",
        args=["--no-sandbox","--disable-gpu",
              f"--unsafely-treat-insecure-origin-as-secure={BASE}"])
    pg = b.new_page(viewport={"width":900,"height":1000})
    errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto(f"{BASE}/?format=jpeg&fps=10&maxWidth=0&skipUnchanged=0",
            wait_until="domcontentloaded", timeout=30000)
    time.sleep(4)

    def state():
        return pg.evaluate("()=>({sw:sw,sh:sh,tw:touchW,th:touchH})")

    s0 = state()
    print(f"  竖屏: 画布 {s0['sw']}x{s0['sh']}  触控空间 {s0['tw']}x{s0['th']}")
    chk("竖屏初始 720x1280", s0["sw"] == 720 and s0["sh"] == 1280)
    chk("触控坐标空间 = 720x1280", s0["tw"] == 720 and s0["th"] == 1280)

    # ── 转横屏 ──
    r = rotate("90")
    print(f"  旋转 90°: applied={r['applied']} method={r['method']} → {r['width']}x{r['height']}")
    time.sleep(4)
    s1 = state()
    print(f"  横屏: 画布 {s1['sw']}x{s1['sh']}  触控空间 {s1['tw']}x{s1['th']}")
    chk("服务端显示尺寸变横屏", r["width"] > r["height"], f"{r['width']}x{r['height']}")
    chk("客户端画布跟着变横屏（size 消息重发生效）",
        s1["sw"] > s1["sh"], f"{s1['sw']}x{s1['sh']}")
    # ⚠️ 行为在某一轮改过：以前坐标范围是启动时定死的、转屏不变；
    #    现在**跟着显示走** —— 因为固定不变会让客户端按屏幕像素发的坐标
    #    超出范围被内核钳住，落点全错（实测过）。页面必须在旋转后重取。
    chk("触控坐标空间跟着变成横屏",
        s1["tw"] == 1280 and s1["th"] == 720,
        f"实际 {s1['tw']}x{s1['th']}（期望 1280x720）")

    # ── 转回竖屏 ──
    r2 = rotate("0")
    time.sleep(4)
    s2 = state()
    print(f"  转回: 画布 {s2['sw']}x{s2['sh']}")
    chk("转回竖屏后画布也回来", s2["sw"] == 720 and s2["sh"] == 1280,
        f"{s2['sw']}x{s2['sh']}")

    chk("页面无 JS 报错", not errs, str(errs[:1]))
    b.close()

print(f"\n  通过 {sum(checks)}/{len(checks)}")
