#!/usr/bin/env python3
"""铺满模式：保持宽高比撑到显示区里**能完整放下的最大值**，不溢出、不裁切。

两种模式的区别
--------------
适应（默认）：保持比例，**只缩不放** —— 画面比显示区小就按原始像素显示。
铺满：        保持比例，**总是撑到能放下的最大值**（画面小的时候会放大）。

            尺度 = min(显示区宽 / 画面宽, 显示区高 / 画面高)

所以"铺满"和"适应"只在**画面比显示区小**时才不同 —— 测试必须造出这种
场景（用 maxWidth 把流降小），否则两种模式算出同一个尺寸，测了等于没测。

为什么渲染交给 CSS，坐标却要自己算
----------------------------------
铺满用 `object-fit:contain`：**缩放由浏览器做**，最坏情况只是留黑边。
之前有一版是用 JS 把尺寸算好写到元素上（--fw/--fh），失败模式正好是
用户报的"比例拉伸"：JS 没跑到时 width 退回 auto，再被基类的
max-width:100% + max-height:100% **分别**钳一下，比例就没了。

代价是元素盒子 = 整个显示区，黑边那块也归 canvas 收事件。所以：
  · toScreen() 走 contentRect() 算真正画着画面的矩形
  · 点在矩形外的一律当没点（inContent 守卫），否则会被钳成误触

渲染（CSS）和换算（JS）是两套实现，**必须验它们一致** ——
[1] 那个像素探针就是干这个的：画已知图案，截图回来量黑边的位置和厚度。

用法:
    ./fillmode_test.py --host 192.168.0.110 --port 8088
"""
import argparse
import base64
import json
import os
import sys
import time
import urllib.request

from playwright.sync_api import sync_playwright

CHROME = os.environ.get(
    "CHROME", "/root/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome")

checks = []


def chk(name, cond, extra=""):
    checks.append(bool(cond))
    print(f"  {'✓' if cond else '✗'} {name}" + (f"  {extra}" if extra else ""))


# ── 探针：<canvas> 上的 object-fit:contain 到底怎么摆？ ──
#
# 源图 200x100（上半红、下半蓝），盒子 100x300。
#   contain 的比例 = min(100/200, 300/100) = 0.5 → 内容 100x50
#   垂直居中 → 内容占 y 125..175（上 25 行红、下 25 行蓝），其余是黑边
#
# 数像素就能反推出浏览器实际用的缩放比和居中位置，
# 再和 contentRect() 的公式对一下 —— 对得上才敢用它换算坐标。
PROBE_HTML = """<!doctype html><meta charset=utf-8>
<style>
  body { margin:0; background:#0f0; }
  #box { width:100px; height:300px; background:#000;
         display:flex; align-items:center; justify-content:center; }
  #pc  { display:block; width:100%; height:100%; object-fit:contain; }
</style>
<div id=box><canvas id=pc width=200 height=100></canvas></div>
<script>
  const g = document.getElementById('pc').getContext('2d');
  g.fillStyle = '#f00'; g.fillRect(0, 0, 200, 50);
  g.fillStyle = '#00f'; g.fillRect(0, 50, 200, 50);
</script>"""


def probe_contain(ctx):
    pg = ctx.new_page()
    pg.set_content(PROBE_HTML)
    time.sleep(0.3)
    shot = pg.screenshot(clip={"x": 0, "y": 0, "width": 100, "height": 300})
    r = pg.evaluate("""async (b64) => {
        const img = new Image();
        img.src = 'data:image/png;base64,' + b64;
        await img.decode();
        const cv = document.createElement('canvas');
        cv.width = img.width; cv.height = img.height;
        const g = cv.getContext('2d'); g.drawImage(img, 0, 0);
        const d = g.getImageData(0, 0, cv.width, cv.height).data;
        // 每行判一次颜色，得出红/蓝/黑的起止行
        const rows = [];
        for (let y = 0; y < cv.height; y++) {
            const i = (y * cv.width + 50) * 4;
            const r = d[i], b = d[i + 2];
            rows.push(r > 200 && b < 60 ? 'R' : (b > 200 && r < 60 ? 'B' : '-'));
        }
        const span = (ch) => {
            const first = rows.indexOf(ch);
            const last = rows.lastIndexOf(ch);
            return first < 0 ? null : {first, last, n: rows.filter(c => c === ch).length};
        };
        return {h: cv.height, red: span('R'), blue: span('B'),
                fit: getComputedStyle(document.getElementById('pc')).objectFit};
    }""", base64.b64encode(shot).decode())
    pg.close()
    return r


BOX_JS = """() => {
    const el = cvs.parentElement, sr = el.getBoundingClientRect();
    return {left: sr.left + el.clientLeft, top: sr.top + el.clientTop,
            width: el.clientWidth, height: el.clientHeight};
}"""

MEASURE_JS = """(pts) => {
    const c = cvs, el = c.parentElement;
    const cr = c.getBoundingClientRect();
    const sr = el.getBoundingClientRect();
    const inside = pts.map(p => inContent({clientX: p[0], clientY: p[1]}));
    return {
        iw: c.width, ih: c.height,
        sw: sw, sh: sh, touchW: touchW, touchH: touchH,
        elem: {left: cr.left, top: cr.top, width: cr.width, height: cr.height},
        box: {left: sr.left + el.clientLeft, top: sr.top + el.clientTop,
              width: el.clientWidth, height: el.clientHeight},
        content: contentRect(),
        fill: document.body.classList.contains('fillmode'),
        objectFit: getComputedStyle(c).objectFit,
        sent: pts.map(p => toScreen({clientX: p[0], clientY: p[1]})),
        inside: inside
    };
}"""


def contain_size(bw, bh, iw, ih):
    s = min(bw / iw, bh / ih)
    return iw * s, ih * s


def contain_rect(box, iw, ih):
    cw, ch = contain_size(box["width"], box["height"], iw, ih)
    return {"left": box["left"] + (box["width"] - cw) / 2,
            "top": box["top"] + (box["height"] - ch) / 2,
            "width": cw, "height": ch}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8088)
    a = ap.parse_args()
    origin = f"http://{a.host}:{a.port}"

    with urllib.request.urlopen(origin + "/api/v1/info", timeout=8) as r:
        info = json.load(r)
    tW, tH = info["touchWidth"], info["touchHeight"]
    print(f"  触控空间 {tW}x{tH}")

    with sync_playwright() as p:
        b = p.chromium.launch(headless=True, executable_path=CHROME,
            args=["--no-sandbox", "--disable-gpu",
                  f"--unsafely-treat-insecure-origin-as-secure={origin}"])

        print("\n\033[1;34m[1] 探针：object-fit:contain 的实际摆放\033[0m")
        pr = probe_contain(b.new_context(viewport={"width": 300, "height": 400}))
        chk("object-fit 计算值 == contain", pr["fit"] == "contain",
            f"实际 {pr['fit']!r}")
        # 源图 200x100 塞进 100x300 盒子：内容 100x50，垂直居中 → y 125..175
        # 上半红 → 红 125..149（25 行），下半蓝 → 蓝 150..174（25 行）
        ok_red = pr["red"] and pr["red"]["n"] == 25 and abs(pr["red"]["first"] - 125) <= 1
        ok_blue = pr["blue"] and pr["blue"]["n"] == 25 and abs(pr["blue"]["first"] - 150) <= 1
        chk("红带位置/厚度 == contain 公式算的（缩放比 0.5、垂直居中）",
            ok_red, f"实际 {pr['red']}")
        chk("蓝带位置/厚度 == contain 公式算的", ok_blue, f"实际 {pr['blue']}")
        chk("上下确实是黑边（说明是 letterbox 而不是拉伸）",
            pr["red"]["first"] > 0 and pr["blue"]["last"] < pr["h"] - 1,
            f"红起 {pr['red']['first']}，蓝止 {pr['blue']['last']}，盒高 {pr['h']}")

        # ── 场景 A：原尺寸流塞进比它小的显示区 ──
        pg = b.new_page(viewport={"width": 900, "height": 1000})
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.goto(f"{origin}/?format=jpeg&fps=10&maxWidth=0&skipUnchanged=0",
                wait_until="domcontentloaded", timeout=30000)
        time.sleep(4)

        for label, want_fill in (("[2] 铺满 · 显示区比画面小", True),
                                 ("[3] 适应 · 显示区比画面小", False)):
            print(f"\n\033[1;34m{label}\033[0m")
            pg.evaluate("(on) => setFill(on, false)", want_fill)
            time.sleep(0.6)
            box = pg.evaluate(BOX_JS)
            pts = [[box["left"] + box["width"] * fx, box["top"] + box["height"] * fy]
                   for fy in (0.5, 0.25, 0.75) for fx in (0.5, 0.25, 0.75)]
            m = pg.evaluate(MEASURE_JS, pts)
            iw, ih = m["iw"], m["ih"]
            exp = contain_rect(box, iw, ih)
            print(f"    画面 {iw}x{ih}（比例 {iw/ih:.3f}）  显示区 "
                  f"{box['width']}x{box['height']}  元素盒子 "
                  f"{m['elem']['width']:.0f}x{m['elem']['height']:.0f}"
                  f"  内容 {exp['width']:.0f}x{exp['height']:.0f}")
            chk(f"body.fillmode == {want_fill}", m["fill"] == want_fill)
            chk("内容保持原始宽高比（没有被拉伸）",
                abs(exp["width"] / exp["height"] - iw / ih) < 0.005)
            chk("内容不溢出显示区",
                exp["width"] <= box["width"] + 1 and
                exp["height"] <= box["height"] + 1)
            if want_fill:
                chk("用的是 object-fit:contain", m["objectFit"] == "contain",
                    f"实际 {m['objectFit']!r}")
                chk("撑到了能放下的最大值（至少贴满一对边）",
                    abs(exp["width"] - box["width"]) <= 1.5 or
                    abs(exp["height"] - box["height"]) <= 1.5,
                    f"内容 {exp['width']:.1f}x{exp['height']:.1f} "
                    f"区 {box['width']}x{box['height']}")
                chk("页面里 contentRect() 和独立算的一致",
                    abs(m["content"]["width"] - exp["width"]) <= 1 and
                    abs(m["content"]["height"] - exp["height"]) <= 1 and
                    abs(m["content"]["left"] - exp["left"]) <= 1 and
                    abs(m["content"]["top"] - exp["top"]) <= 1,
                    f"页面 {m['content']['width']:.1f}x{m['content']['height']:.1f}"
                    f" @ {m['content']['left']:.1f},{m['content']['top']:.1f}"
                    f" vs {exp['width']:.1f}x{exp['height']:.1f}"
                    f" @ {exp['left']:.1f},{exp['top']:.1f}")
                # 黑边上的点必须被判成"不在画面里"
                corners = [[box["left"] + 2, box["top"] + 2],
                           [box["left"] + box["width"] - 2, box["top"] + box["height"] - 2]]
                mc = pg.evaluate(MEASURE_JS, corners)
                if exp["height"] < box["height"] - 4:
                    chk("点在上下黑边上 → 判定为不在画面里（不会误触）",
                        not mc["inside"][0] and not mc["inside"][1],
                        f"inside={mc['inside']}")
                elif exp["width"] < box["width"] - 4:
                    chk("点在左右黑边上 → 判定为不在画面里（不会误触）",
                        not mc["inside"][0] and not mc["inside"][1],
                        f"inside={mc['inside']}")
            else:
                chk("适应模式下元素盒子就是画面（没有黑边在盒子里）",
                    abs(m["elem"]["width"] / m["elem"]["height"] - iw / ih) < 0.01,
                    f"盒子比例 {m['elem']['width']/m['elem']['height']:.3f}")

            # ── 坐标：用内容矩形换算 ──
            bad = []
            for (px, py), got in zip(pts, m["sent"]):
                x = round((px - exp["left"]) / exp["width"] * tW)
                y = round((py - exp["top"]) / exp["height"] * tH)
                x = max(0, min(tW - 1, x)); y = max(0, min(tH - 1, y))
                if abs(got["x"] - x) > 2 or abs(got["y"] - y) > 2:
                    bad.append(f"({px:.0f},{py:.0f}) 得到 ({got['x']},{got['y']}) "
                               f"期望 ({x},{y})")
            chk(f"toScreen() 与内容矩形线性对应（{len(pts)} 个点）",
                not bad, "；".join(bad[:2]))
            # 中心点在两种算法下都一样 —— 说明为什么必须测偏心点
            if want_fill and exp["height"] < box["height"] - 4:
                cx = exp["left"] + exp["width"] / 2
                cy = exp["top"] + exp["height"] / 2
                boxx = round((cx - m["elem"]["left"]) / m["elem"]["width"] * tW)
                contx = round((cx - exp["left"]) / exp["width"] * tW)
                chk("中心点两种算法重合（所以只测中心查不出问题）", boxx == contx)

        print("\n\033[1;34m[4] 切换与持久化\033[0m")
        pg.evaluate("() => setFill(true, true)")
        time.sleep(0.3)
        chk("setFill(true) 之后 fillmode 生效",
            pg.evaluate("() => document.body.classList.contains('fillmode')"))
        chk("选择记进了 localStorage",
            pg.evaluate("() => localStorage.getItem('autod.fill')") == "1")
        pg.reload(wait_until="domcontentloaded")
        time.sleep(3)
        chk("刷新后仍然是铺满",
            pg.evaluate("() => document.body.classList.contains('fillmode')"))
        chk("按钮文字反映当前状态",
            pg.evaluate("() => $('fillsw').textContent").strip().startswith("铺满"))

        print("\n\033[1;34m[5] 面板收放 / 改窗口后内容矩形要跟着变\033[0m")
        before = pg.evaluate(BOX_JS)
        pg.click("#panelsw")
        time.sleep(0.6)
        after = pg.evaluate(BOX_JS)
        chk("切换面板后显示区尺寸确实变了",
            abs(after["width"] - before["width"]) > 10,
            f"{before['width']} → {after['width']}")
        m5 = pg.evaluate(MEASURE_JS, [[0, 0]])
        e5 = contain_rect(after, m5["iw"], m5["ih"])
        chk("面板收起后内容矩形跟着重算",
            abs(m5["content"]["width"] - e5["width"]) <= 1 and
            abs(m5["content"]["height"] - e5["height"]) <= 1,
            f"页面 {m5['content']['width']:.0f}x{m5['content']['height']:.0f} "
            f"期望 {e5['width']:.0f}x{e5['height']:.0f}")

        pg.set_viewport_size({"width": 1600, "height": 500})
        time.sleep(0.8)
        box6 = pg.evaluate(BOX_JS)
        m6 = pg.evaluate(MEASURE_JS, [[0, 0]])
        e6 = contain_rect(box6, m6["iw"], m6["ih"])
        chk("改成很扁的窗口后内容依然按 contain 算",
            abs(m6["content"]["width"] - e6["width"]) <= 1 and
            abs(m6["content"]["height"] - e6["height"]) <= 1,
            f"页面 {m6['content']['width']:.0f}x{m6['content']['height']:.0f} "
            f"期望 {e6['width']:.0f}x{e6['height']:.0f}")
        chk("很扁的窗口下画面不溢出、不变形",
            e6["height"] <= box6["height"] + 1 and
            abs(e6["width"] / e6["height"] - m6["iw"] / m6["ih"]) < 0.005)
        chk("全程无 JS 报错", not errs, str(errs[:1]))
        pg.close()

        # ── 场景 B：小流放进大显示区（两种模式**真正不同**的场景）──
        pg2 = b.new_page(viewport={"width": 900, "height": 1000})
        errs2 = []
        pg2.on("pageerror", lambda e: errs2.append(str(e)))
        pg2.goto(f"{origin}/?format=jpeg&fps=10&maxWidth=360&skipUnchanged=0",
                 wait_until="domcontentloaded", timeout=30000)
        time.sleep(4)

        def measure_case(fill):
            pg2.evaluate("(on) => setFill(on, false)", fill)
            time.sleep(0.6)
            box = pg2.evaluate(BOX_JS)
            m = pg2.evaluate(MEASURE_JS, [[0, 0]])
            return box, m, contain_rect(box, m["iw"], m["ih"])

        print("\n\033[1;34m[6] 铺满 · 小流（画面比显示区小 → 要放大）\033[0m")
        bx, mf, ef = measure_case(True)
        print(f"    画面 {mf['iw']}x{mf['ih']}  显示区 {bx['width']}x{bx['height']}"
              f"  内容 {ef['width']:.0f}x{ef['height']:.0f}")
        chk("画面比显示区小，铺满**放大了**它", ef["width"] > mf["iw"] + 1,
            f"内容宽 {ef['width']:.0f} > 画面宽 {mf['iw']}")
        chk("放大后依然完整可见（不裁切、不溢出）",
            ef["width"] <= bx["width"] + 1 and ef["height"] <= bx["height"] + 1 and
            abs(ef["width"] / ef["height"] - mf["iw"] / mf["ih"]) < 0.005)
        chk("放大到了能放下的最大值",
            abs(ef["width"] - bx["width"]) <= 1.5 or
            abs(ef["height"] - bx["height"]) <= 1.5)

        print("\n\033[1;34m[7] 适应 · 小流（1:1，不放大）\033[0m")
        bx2, ma, ea = measure_case(False)
        # ⚠️ 适应模式下"画面"就是**元素盒子本身**（360x202），
        #    不是 contain(显示区) —— 后者是铺满才成立的算法。
        #    这里一开始写错了，断言就假红了。
        ea = ma["elem"]
        print(f"    元素盒子 {ea['width']:.0f}x{ea['height']:.0f}"
              f"  显示区 {bx2['width']}x{bx2['height']}")
        chk("适应模式**不放大**（按原始像素 1:1 显示）",
            abs(ma["elem"]["width"] - ma["iw"]) <= 1.5,
            f"盒子宽 {ma['elem']['width']:.0f} vs 画面宽 {ma['iw']}")
        chk("两种模式在这个场景下结果确实不同（否则测了等于没测）",
            ef["width"] > ea["width"] * 1.5,
            f"铺满 {ef['width']:.0f} vs 适应 {ea['width']:.0f}")
        chk("场景 B 无 JS 报错", not errs2, str(errs2[:1]))
        pg2.close()
        b.close()

    print(f"\n  通过 {sum(checks)}/{len(checks)}")
    return 0 if all(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
