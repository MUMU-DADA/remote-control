#!/usr/bin/env python3
"""端到端：网页控制台的「文字输入」是否真的把整串文字发给了 /api/v1/text。

为什么单独测这一层：接口本身由 functional-sweep 的 [5c] 节按**设备侧证据**
验（用 uiautomator 把输入框里的字读回来），这里验的是另一半 ——
网页上敲进去的字有没有原样发出去、模式有没有选对。
两件事都容易在改动里被顺手弄坏，而且症状都是"点了没反应"。

用法：
    python3 text_test.py [--host 127.0.0.1] [--port 8088]
"""
import argparse
import json
import time
import urllib.request

from playwright.sync_api import sync_playwright

import wctest_token
from wctest_chrome import CHROME

# 交付镜像默认开鉴权：没令牌页面会 401，而多数检查照样"通过"。
_TOKEN = wctest_token.resolve_token()

_ap = argparse.ArgumentParser()
_ap.add_argument("--host", default="127.0.0.1")
_ap.add_argument("--port", type=int, default=8088)
_a = _ap.parse_args()
BASE = f"http://{_a.host}:{_a.port}"

CJK = "你好，世界！Hello ✅"


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


d = api("/api/v1/describe")
has_text = any(c.get("name") == "Text" for c in d.get("commands", []))
chk("服务端有 Text 命令", has_text)

with sync_playwright() as p:
    b = p.chromium.launch(
        headless=True,
        executable_path=CHROME,
        args=["--no-sandbox", "--disable-gpu",
              f"--unsafely-treat-insecure-origin-as-secure={BASE}"])
    pg = wctest_token.new_page(b, _TOKEN, viewport={"width": 900, "height": 1000})
    errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))

    sent = []
    pg.on("request", lambda r: sent.append(json.loads(r.post_data))
          if r.url.endswith("/api/v1/text") and r.post_data else None)

    pg.goto(f"{BASE}/?format=jpeg&fps=1&maxWidth=0&skipUnchanged=1",
            wait_until="domcontentloaded", timeout=30000)
    # ⚠️ Playwright 同步 API 只在进入它自己的调用时才派发事件；
    #    在 Python 里 time.sleep 不会泵事件循环（mouse_test.py 踩过这个坑）
    pg.wait_for_timeout(4000)

    def act(fn, timeout=20.0):
        """执行 fn()，等**新增**的一条 /text 请求，返回它。"""
        before = len(sent)
        fn()
        deadline = time.time() + timeout
        while time.time() < deadline:
            pg.wait_for_timeout(50)
            new = sent[before:]
            if new:
                return new
        return []

    # ⚠️ 控制卡片在**右侧面板**里，而面板在横屏流下默认是收起的
    #    （见 webui.cpp 的 setPanel：横屏让宽度给画面，竖屏才默认展开）。
    #    不先展开就会拿到一个 0x0 的元素 —— Playwright 报 "element is not
    #    visible"，看起来像"页面上没有这个框"。
    #
    #    这个坑第一次没暴露：当时测的是 NDK 那个抓帧后端，它一帧都抓不到，
    #    于是页面从没收到 size 消息、也就没触发"横屏收起面板"的逻辑，
    #    元素恰好是可见的 —— **测试因为错误的原因通过了**。
    #    换到能正常出帧的 ROM 上，立刻就红了。
    if pg.evaluate("()=>document.body.classList.contains('nopanel')"):
        pg.click("#panelsw")
        pg.wait_for_timeout(400)
    chk("右侧面板已展开", not pg.evaluate(
        "()=>document.body.classList.contains('nopanel')"), "面板没能展开")

    box = pg.locator("#textin").bounding_box()
    chk("文字输入框可见", bool(box) and box["width"] > 10,
        "页面上没有 #textin" if not box else f"{box['width']:.0f}x{box['height']:.0f}")

    # ── 粘贴（默认模式，支持中文）──
    pg.fill("#textin", CJK)
    got = act(lambda: pg.click("text=粘贴"))
    chk("点「粘贴」→ 发出 /text 请求", len(got) >= 1, f"收到 {len(got)} 条")
    if got:
        chk("文字原样发出（含中文与 emoji）", got[0].get("text") == CJK,
            f"发出的是 {got[0].get('text')!r}")
        chk("模式 = paste", got[0].get("mode") == "paste",
            str(got[0].get("mode")))

    # ── 逐字键入 ──
    pg.fill("#textin", "Ab1!")
    got = act(lambda: pg.click("text=逐字键入"))
    chk("点「逐字键入」→ 发出 /text 请求", len(got) >= 1, f"收到 {len(got)} 条")
    if got:
        chk("模式 = type", got[0].get("mode") == "type", str(got[0].get("mode")))

    # ── 回车 = 粘贴 ──
    pg.fill("#textin", "enter-key")
    got = act(lambda: pg.press("#textin", "Enter"))
    chk("在输入框里回车 → 发出 /text 请求", len(got) >= 1, f"收到 {len(got)} 条")
    if got:
        chk("回车走 paste 模式", got[0].get("mode") == "paste",
            str(got[0].get("mode")))

    # ── 空内容不该发请求（省一次无意义的往返）──
    pg.fill("#textin", "")
    got = act(lambda: pg.click("text=粘贴"), timeout=3.0)
    chk("空内容不发请求", len(got) == 0, f"却发了 {len(got)} 条")

    chk("过程中没有 JS 报错", not errs, "; ".join(errs[:2]))
    b.close()

print()
ok = sum(1 for c in checks if c)
print(f"通过 {ok}/{len(checks)}")
raise SystemExit(0 if ok == len(checks) else 1)
