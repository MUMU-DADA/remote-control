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

def native_width():
    """当前显示宽度 —— "不降采样"时画面就该是这个宽。"""
    with urllib.request.urlopen(BASE + "/api/v1/info", timeout=8) as r:
        return json.load(r)["primaryWidth"]


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
        # FrameHub 的抓帧宽度 = **所有订阅者里最大的 maxWidth**（0 视作原始）。
        # 所以断言必须按这个算，不能假定"只有我一个客户端" ——
        # 第一版就是那么写的，控制台另开一个页面时它会假失败。
        subs = s.get("subscriberList") or []
        c["expectW"] = 0 if any(x["maxWidth"] == 0 for x in subs) \
                            else max([x["maxWidth"] for x in subs], default=0)
        c["nsubs"] = len(subs)
        return c

    checks = []
    def chk(name, cond, extra=""):
        checks.append(bool(cond))
        print(f"  {'✓' if cond else '✗'} {name} {extra}")

    a = state()
    print(f"  改之前: 画布 {a['sw']}x{a['sh']}  抓帧宽 {a['srvW']}")
    # 初始是 URL 里指定的 maxWidth=720（不是原生宽度）——
    # 测的是"改 maxWidth 能不能生效"，得从一个明确的值出发。
    chk("初始按 URL 的 maxWidth=720", a["sw"] == 720, f"实际 {a['sw']}")

    # ── 中途改到 360 ──
    pg.evaluate("()=>{window.__log=[]; setMaxWidth(360);}")
    time.sleep(3)
    bb = state()
    print("       消息:", pg.evaluate("()=>window.__log.slice(-3)"))
    print(f"  改到 360: 画布 {bb['sw']}x{bb['sh']}  抓帧宽 {bb['srvW']}  流还在={bb['ready']}")
    chk("画面宽度变成 360", bb["sw"] == 360, f"实际 {bb['sw']}")
    chk("服务端抓帧宽度 = 订阅者里的最大值",
        bb["srvW"] == bb["expectW"],
        f"实际 {bb['srvW']}，按 {bb['nsubs']} 个订阅者算应为 {bb['expectW']}")
    chk("连接没断", bb["ready"] is True)

    # ── 再改到 480 ──
    pg.evaluate("()=>{window.__log=[]; setMaxWidth(480);}")
    time.sleep(3)
    c = state()
    print("       消息:", pg.evaluate("()=>window.__log.slice(-3)"))
    print(f"  改到 480: 画布 {c['sw']}x{c['sh']}  抓帧宽 {c['srvW']}")
    chk("再改到 480 也生效", c["sw"] == 480 and c["srvW"] == c["expectW"],
        f"画布 {c['sw']}，抓帧宽 {c['srvW']}（应为 {c['expectW']}）")

    # ── 改回原始 ──
    pg.evaluate("()=>setMaxWidth(0)")
    time.sleep(3)
    d = state()
    print(f"  改回原始: 画布 {d['sw']}x{d['sh']}  抓帧宽 {d['srvW']}")
    # ⚠️ 不写死 720 —— 设备分辨率会变（这台从竖屏 720x1280 改成了
    #    横屏 1280x720）。"改回原始"的期望值是**显示宽度**，从 /info 取。
    native = native_width()
    chk("改回原始（不降采样）", d["sw"] == native and d["srvW"] == d["expectW"],
        f"画布 {d['sw']}（期望 {native}），抓帧宽 {d['srvW']}")

    chk("页面无 JS 报错", not errs, str(errs[:1]))
    b.close()

print(f"\n  通过 {sum(checks)}/{len(checks)}")
