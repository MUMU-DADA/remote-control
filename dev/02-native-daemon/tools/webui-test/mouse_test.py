#!/usr/bin/env python3
"""端到端：网页控制台里的**滚轮**与**右键**是否真的转成了鼠标接口调用。

为什么单独测这一层：鼠标的接口本身由 test_mouse（真实 uinput）和
functional-sweep 覆盖，但"网页上滚一下会不会真的发出去"是另一回事 ——
画布上的手势默认全部当触控，滚轮/右键是**例外**，最容易被后续改动顺手吞掉，
而且症状是"页面上滚了半天没反应"，看起来像设备坏了。

用法：
    python3 mouse_test.py [--host 127.0.0.1] [--port 8088]
"""
import argparse
import json
import time
import urllib.request

from playwright.sync_api import sync_playwright

_ap = argparse.ArgumentParser()
_ap.add_argument("--host", default="127.0.0.1")
_ap.add_argument("--port", type=int, default=8088)
_a = _ap.parse_args()
BASE = f"http://{_a.host}:{_a.port}"


def api(path, body=None):
    req = urllib.request.Request(
        BASE + path,
        data=json.dumps(body).encode() if body else None,
        headers={"Content-Type": "application/json"} if body else {})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r)


checks = []


def chk(name, cond, extra=""):
    checks.append(bool(cond))
    print(f"  {'✓' if cond else '✗'} {name} {extra}")


# 先确认服务端认识这个命令，否则后面看到的请求都是白搭
d = api("/api/v1/describe")
mouse_cmds = [c for c in d.get("commands", []) if c.get("name") == "Mouse"]
chk("服务端有 Mouse 命令", len(mouse_cmds) == 1,
    f"since={mouse_cmds[0]['since'] if mouse_cmds else '-'}")
if mouse_cmds:
    chk("Mouse 报告为可用", mouse_cmds[0].get("available") is not False,
        str(mouse_cmds[0].get("reason", "")))

with sync_playwright() as p:
    b = p.chromium.launch(
        headless=True,
        executable_path="/root/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome",
        args=["--no-sandbox", "--disable-gpu",
              f"--unsafely-treat-insecure-origin-as-secure={BASE}"])
    pg = b.new_page(viewport={"width": 900, "height": 1000})
    errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))

    # 记录发往 /mouse 的请求体
    sent = []
    pg.on("request", lambda r: sent.append(json.loads(r.post_data))
          if r.url.endswith("/api/v1/mouse") and r.post_data else None)

    def act(fn, action, timeout=20.0):
        """执行 fn()，然后等**新增**的一条指定 action 请求，返回它。

        ⚠️ 第一版是「clear 列表 → 睡 1 秒 → 找请求」，结果整体错位一格：
        clear 之后上一步的请求才到，于是每步读到的是**上一步**的值，
        看起来像符号算反了。改成按"新增条数"等待，就不依赖时序了。
        """
        before = len(sent)
        fn()
        deadline = time.time() + timeout
        while time.time() < deadline:
            # ⚠️ 必须用 pg.wait_for_timeout 而不是 time.sleep：
            #    Playwright 的**同步** API 只在进入它自己的调用时才派发事件，
            #    在 Python 里干等（time.sleep）不会泵事件循环 —— 于是
            #    page.on("request") 的回调一直不跑，看起来像"页面没发请求"，
            #    而下一步又突然收到上一步的请求（整体错位一格）。
            #    第一版就是这么误报的：连符号都被判成反的。
            pg.wait_for_timeout(50)
            new = [s for s in sent[before:] if s.get("action") == action]
            if new:
                return new
        return []

    # 画面流开到最低档：这个测试验的是**输入事件**，不需要流畅画面，
    # 而高帧率会把页面主线程占满、把事件处理推迟好几秒（实测踩到）。
    pg.goto(f"{BASE}/?format=jpeg&fps=1&maxWidth=0&skipUnchanged=1",
            wait_until="domcontentloaded", timeout=30000)
    pg.wait_for_timeout(4000)   # 同样要泵事件，见 act() 里的说明

    box = pg.locator("#screen").bounding_box()
    chk("画布可见", bool(box) and box["width"] > 10 and box["height"] > 10,
        f"{box['width']:.0f}x{box['height']:.0f}" if box else "无")
    cx = box["x"] + box["width"] / 2
    cy = box["y"] + box["height"] / 2
    pg.mouse.move(cx, cy)
    pg.wait_for_timeout(300)

    # ── 滚轮向上 ──
    up = act(lambda: pg.mouse.wheel(0, -300), "wheel")   # deltaY 为负 = 向上滚
    chk("向上滚 → 发出 wheel 请求", len(up) >= 1, f"收到 {len(up)} 条")
    if up:
        # deltaY=-300 → 3 格，且**符号为正**（接口约定：正 = 滚轮向上）
        chk("向上滚 → delta 为正且换算成格", up[0].get("delta") == 3,
            f"delta={up[0].get('delta')}（期望 3）")
        chk("默认走垂直轴", up[0].get("axis") == "v", str(up[0].get("axis")))

    # ── 滚轮向下 ──
    down = act(lambda: pg.mouse.wheel(0, 250), "wheel")  # deltaY 为正 = 向下滚
    chk("向下滚 → 发出 wheel 请求", len(down) >= 1, f"收到 {len(down)} 条")
    if down:
        chk("向下滚 → delta 为负", down[0].get("delta") == -2,
            f"delta={down[0].get('delta')}（期望 -2）")

    # ── 小幅滚动也要有反应（不足一格按一格算）──
    tiny = act(lambda: pg.mouse.wheel(0, 10), "wheel")   # 10px，远不足 100px
    chk("轻微滚动也会发出请求（不足一格按一格）",
        len(tiny) >= 1 and tiny[0].get("delta") == -1,
        f"{tiny[0].get('delta') if tiny else '无请求'}")

    # ── 右键 ──
    rc = act(lambda: pg.mouse.click(cx, cy, button="right"), "click")
    chk("右键 → 发出 click 请求", len(rc) >= 1, f"收到 {len(rc)} 条")
    if rc:
        chk("右键 → button=right", rc[0].get("button") == "right",
            str(rc[0].get("button")))

    chk("过程中没有 JS 报错", not errs, "; ".join(errs[:2]))

    b.close()

print()
ok = sum(1 for c in checks if c)
print(f"通过 {ok}/{len(checks)}")
raise SystemExit(0 if ok == len(checks) else 1)
