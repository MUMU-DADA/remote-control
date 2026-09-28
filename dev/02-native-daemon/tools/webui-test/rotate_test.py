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

    # ⚠️ 不写死起始方向。
    #
    #    这段原来断言"竖屏初始 720x1280" —— 那是在**竖屏皮肤**的模拟器上写的。
    #    后来模拟器改成 1280x720 横屏启动，前两条断言当场就假了
    #    （和 maxwidth_test 当初写死 720 是同一个毛病）。
    #    面板原生是横是竖由皮肤决定，测试只该断言**相对变化**。
    s0 = state()
    start_landscape = s0["sw"] > s0["sh"]
    print(f"  初始: 画布 {s0['sw']}x{s0['sh']}（{'横' if start_landscape else '竖'}屏）"
          f"  触控空间 {s0['tw']}x{s0['th']}")
    chk("初始画布和触控空间一致",
        s0["tw"] == s0["sw"] and s0["th"] == s0["sh"],
        f"画布 {s0['sw']}x{s0['sh']} vs 触控 {s0['tw']}x{s0['th']}")

    # ── 转到另一个方向 ──
    target = "portrait" if start_landscape else "landscape"
    r = rotate(target)
    print(f"  转到 {target}: applied={r['applied']} method={r['method']}"
          f" → {r['width']}x{r['height']}")
    time.sleep(4)
    s1 = state()
    print(f"  转后: 画布 {s1['sw']}x{s1['sh']}  触控空间 {s1['tw']}x{s1['th']}")
    chk("服务端显示尺寸真的换了方向",
        (r["width"] > r["height"]) != start_landscape,
        f"{r['width']}x{r['height']}")
    chk("客户端画布跟着换（size 消息重发生效）",
        (s1["sw"] > s1["sh"]) != start_landscape, f"{s1['sw']}x{s1['sh']}")
    # ⚠️ 行为在某一轮改过：以前坐标范围是启动时定死的、转屏不变；
    #    现在**跟着显示走** —— 因为固定不变会让客户端按屏幕像素发的坐标
    #    超出范围被内核钳住，落点全错（实测过）。页面必须在旋转后重取。
    chk("触控坐标空间跟着换，且与画布一致",
        s1["tw"] == s1["sw"] and s1["th"] == s1["sh"],
        f"画布 {s1['sw']}x{s1['sh']} vs 触控 {s1['tw']}x{s1['th']}")

    # ── 转回来 ──
    back = "landscape" if start_landscape else "portrait"
    r2 = rotate(back)
    time.sleep(4)
    s2 = state()
    print(f"  转回 {back}: 画布 {s2['sw']}x{s2['sh']}  触控空间 {s2['tw']}x{s2['th']}")
    chk("转回原方向后画布也回来",
        s2["sw"] == s0["sw"] and s2["sh"] == s0["sh"],
        f"{s2['sw']}x{s2['sh']}（期望 {s0['sw']}x{s0['sh']}）")
    chk("转回后触控空间依然跟着画布",
        s2["tw"] == s2["sw"] and s2["th"] == s2["sh"],
        f"画布 {s2['sw']}x{s2['sh']} vs 触控 {s2['tw']}x{s2['th']}")

    chk("页面无 JS 报错", not errs, str(errs[:1]))
    b.close()

print(f"\n  通过 {sum(checks)}/{len(checks)}")
