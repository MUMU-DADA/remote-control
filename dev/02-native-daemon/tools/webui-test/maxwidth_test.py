#!/usr/bin/env python3
"""中途切分辨率：不重连，改 maxWidth，看画面尺寸是否跟着变。

验三件事：
  1. 客户端画布尺寸变了（服务端 size 消息 + createImageBitmap 都会驱动）
  2. 服务端 /params 的 captureWidth 跟着变（源头抓帧尺寸）
  3. 连接**没有**断（用同一个 WS，靠帧计数判断）
"""
import time, json, urllib.request
from playwright.sync_api import sync_playwright

import argparse
_ap = argparse.ArgumentParser()
_ap.add_argument("--host", default="127.0.0.1")
_ap.add_argument("--port", type=int, default=8088)
_a = _ap.parse_args()
BASE = f"http://{_a.host}:{_a.port}"

def params():
    with urllib.request.urlopen(BASE + "/api/v1/params", timeout=8) as r:
        return json.load(r)["capture"]

with sync_playwright() as p:
    b = p.chromium.launch(headless=True,
        executable_path="/root/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome",
        args=["--no-sandbox", "--disable-gpu",
              f"--unsafely-treat-insecure-origin-as-secure={BASE}"])
    pg = b.new_page(viewport={"width": 900, "height": 1000})
    errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto(f"{BASE}/?format=jpeg&fps=10&maxWidth=720&skipUnchanged=0",
            wait_until="domcontentloaded", timeout=30000)
    time.sleep(4)

    # 记录所有文本消息，便于定位
    pg.evaluate("""() => {
        window.__log = [];
        const orig = streamWs.onmessage;
        streamWs.onmessage = function(ev) {
            if (typeof ev.data === 'string') window.__log.push(ev.data);
            return orig.call(this, ev);
        };
    }""")

    def state():
        c = pg.evaluate("()=>({sw:sw, sh:sh, cw:cvs.width, ch:cvs.height, ready:streamReady})")
        s = params()
        c["srvW"] = s["captureWidth"]
        return c

    checks = []
    def chk(name, cond, extra=""):
        checks.append(bool(cond))
        print(f"  {'✓' if cond else '✗'} {name} {extra}")

    a = state()
    print(f"  改之前: 画布 {a['sw']}x{a['sh']}  抓帧宽 {a['srvW']}")
    chk("初始 720", a["sw"] == 720 and a["srvW"] == 720)

    # ── 中途改到 360 ──
    pg.evaluate("()=>{window.__log=[]; setMaxWidth(360);}")
    time.sleep(3)
    bb = state()
    print("       消息:", pg.evaluate("()=>window.__log.slice(-3)"))
    print(f"  改到 360: 画布 {bb['sw']}x{bb['sh']}  抓帧宽 {bb['srvW']}  流还在={bb['ready']}")
    chk("画面宽度变成 360", bb["sw"] == 360, f"实际 {bb['sw']}")
    chk("服务端抓帧宽度也变成 360", bb["srvW"] == 360, f"实际 {bb['srvW']}")
    chk("连接没断", bb["ready"] is True)

    # ── 再改到 480 ──
    pg.evaluate("()=>{window.__log=[]; setMaxWidth(480);}")
    time.sleep(3)
    c = state()
    print("       消息:", pg.evaluate("()=>window.__log.slice(-3)"))
    print(f"  改到 480: 画布 {c['sw']}x{c['sh']}  抓帧宽 {c['srvW']}")
    chk("再改到 480 也生效", c["sw"] == 480 and c["srvW"] == 480)

    # ── 改回原始 ──
    pg.evaluate("()=>setMaxWidth(0)")
    time.sleep(3)
    d = state()
    print(f"  改回原始: 画布 {d['sw']}x{d['sh']}  抓帧宽 {d['srvW']}")
    chk("改回原始（不降采样）", d["sw"] == 720 and d["srvW"] == 0)

    chk("页面无 JS 报错", not errs, str(errs[:1]))
    b.close()

print(f"\n  通过 {sum(checks)}/{len(checks)}")
