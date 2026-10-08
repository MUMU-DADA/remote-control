#!/usr/bin/env python3
"""网页控制台的端到端验收：画质拖动条 + 抓帧节奏显示。

跑的是**真浏览器**（Chrome for Testing + WebCodecs），不是 HTML 字符串匹配 ——
「元素在页面里」和「拖一下真的改到了服务端」是两回事。

    xvfb-run -a ./webui_test.py --host 192.168.0.110 --port 8088

需要 Chrome for Testing。路径可用 CHROME 环境变量覆盖。
"""

import argparse
import os
import sys
import time

from playwright.sync_api import sync_playwright

import wctest_token

# 交付镜像默认开鉴权：没令牌页面会 401，而多数检查照样"通过"。
_TOKEN = wctest_token.resolve_token()

CHROME = os.environ.get(
    "CHROME", "/root/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome"
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--shot", default="", help="截图输出路径（可选）")
    a = ap.parse_args()

    origin = f"http://{a.host}:{a.port}"
    url = f"{origin}/?format=jpeg&fps=10"

    checks = []
    errors = []

    with sync_playwright() as p:
        b = p.chromium.launch(
            headless=True,
            executable_path=CHROME,
            args=[
                "--no-sandbox",
                "--disable-gpu",
                # ⚠️ WebCodecs 要求安全上下文，而 http://<局域网IP> 不算。
                #    不加这个开关 VideoDecoder 就是 undefined。
                f"--unsafely-treat-insecure-origin-as-secure={origin}",
            ],
        )
        pg = wctest_token.new_page(b, _TOKEN, viewport={"width": 900, "height": 1000})
        # 页面里的 JS 报错必须让测试失败 —— 否则"拖动条坏了"可能只是个
        # 静默异常，界面上看起来一切正常
        pg.on("pageerror", lambda e: errors.append(str(e)))
        pg.goto(url, wait_until="domcontentloaded", timeout=30000)
        time.sleep(3)

        def slider():
            return pg.evaluate("""() => {
                const el = document.getElementById('qslider');
                const lab = document.getElementById('qval');
                return el ? {min: el.min, max: el.max, value: el.value,
                             label: lab ? lab.textContent : '?'} : null;
            }""")

        def check(name, cond):
            checks.append((name, bool(cond)))
            print(f"  {'✓' if cond else '✗'} {name}")

        # ── 1. 拖动条存在，且范围来自服务端（jpeg = 1-100）──
        s = slider()
        if s is None:
            print("  ✗ 找不到 #qslider —— 网页没渲染出来", file=sys.stderr)
            return 1
        print(f"    初始(jpeg): min={s['min']} max={s['max']} value={s['value']}"
              f" label={s['label']}")
        check("拖动条范围跟随 jpeg（1-100）", s["min"] == "1" and s["max"] == "100")

        # ── 2. 拖动能改到页面的 quality ──
        pg.evaluate("""() => {
            const el = document.getElementById('qslider');
            el.value = 42; el.dispatchEvent(new Event('input'));
        }""")
        time.sleep(0.6)
        q = pg.evaluate("() => (typeof quality !== 'undefined') ? quality : null")
        check("拖到 42 后页面 quality == 42", q == 42)

        # ── 3. 换格式 → 范围跟着换（png = 1-9）且值被钳进去 ──
        pg.evaluate("() => setCodec('png', 1)")
        time.sleep(0.8)
        s = slider()
        print(f"    切 PNG    : min={s['min']} max={s['max']} value={s['value']}")
        check("换 PNG 后范围变 1-9", s["min"] == "1" and s["max"] == "9")
        check("换 PNG 后旧的值被钳进新范围",
              int(s["value"]) <= 9)

        # ── 4. 切回去能恢复 ──
        pg.evaluate("() => setCodec('jpeg', 75)")
        time.sleep(0.8)
        s = slider()
        check("切回 jpeg 后范围回到 1-100",
              s["min"] == "1" and s["max"] == "100")

        # ── 5. 抓帧节奏（对应目标 1）──
        #
        # 页面自己拉流，所以这一行应当报出**服务端的**抓帧节奏，
        # 而不是"空闲"。等一拍让 2 秒轮询至少跑过一次。
        time.sleep(3)
        cad = pg.evaluate("""() => {
            const el = document.getElementById('cadence');
            return el ? el.textContent : null;
        }""")
        print(f"    抓帧行    : {cad!r}")
        check("#cadence 元素存在", cad is not None)
        # ⚠️ 这里原来断言的是文本里有"服务端"三个字 —— 而这三个字**只出现在
        #    #cadence 上面那行 HTML 注释里**，渲染出来的串里从来没有过：
        #        抓帧  上限 10fps · 1 个订阅 · 16ms · 探测 100ms · 宽 720
        #    所以这条**任何情况下都不可能通过**，跟令牌、跟服务端都没关系。
        #    改成断言真正要看的东西：这一行报出了活的节奏（帧率上限 + 周期），
        #    而不是"空闲"那句占位话。
        check("抓帧行报出了服务端节奏（不是空闲）",
              cad is not None and "空闲" not in cad
              and "上限" in cad and "fps" in cad)
        check("抓帧行带上了本页需求的对照或订阅数",
              cad is not None and ("订阅" in cad))

        if a.shot:
            pg.screenshot(path=a.shot,
                          clip={"x": 0, "y": 0, "width": 900, "height": 700})
        b.close()

    print()
    for e in errors:
        print(f"  [页面 JS 错误] {e}", file=sys.stderr)
    # ── 无令牌时：认证条必须告诉用户**去哪拿**令牌 ──
    #
    # 交付镜像的令牌是首启**随机生成**的，走到这一步的人手上没有。
    # 以前那一条只写"此服务需要访问令牌 / 粘贴令牌"—— 等于让人干瞪眼，
    # 而文档里其实早写了位置（docs/api/04-config.md「令牌从哪来」）。
    # 这里钉住"界面上必须给出真实位置"，免得以后被"简化"回一句话。
    with sync_playwright() as p2:
        b2 = p2.chromium.launch(headless=True, executable_path=CHROME,
                                args=["--no-sandbox", "--disable-gpu"])
        ctx2 = b2.new_context(viewport={"width": 900, "height": 1000})
        pg2 = ctx2.new_page()
        pg2.goto(origin + "/", wait_until="domcontentloaded", timeout=30000)
        time.sleep(1.5)
        bar = pg2.evaluate("""() => {
            const el = document.getElementById('authbar');
            if (!el) return null;
            return {visible: getComputedStyle(el).display !== 'none',
                    text: el.textContent.replace(/\s+/g, ' ').trim()};
        }""")
        check("无令牌时认证条可见", bar is not None and bar["visible"])
        check("认证条说了令牌在哪（不是只说'需要令牌'）",
              bar is not None and "remote-control.conf" in bar["text"])
        print("    认证条:", (bar or {}).get("text", "?")[:120])
        b2.close()

    check("页面没有 JS 报错", not errors)

    passed = sum(1 for _, ok in checks if ok)
    print(f"\n  通过 {passed}/{len(checks)}")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
