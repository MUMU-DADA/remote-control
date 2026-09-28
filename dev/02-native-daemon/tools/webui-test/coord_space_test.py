#!/usr/bin/env python3
"""坐标空间一致性：控制台发出的坐标，服务端应当按**触控范围**解释。

这个测试是有来历的 —— 它抓出过一个真 bug：

    服务端把 x/y 直接当 uinput 的 ABS 值写下去（inject_uinput.cpp
    里没有缩放），坐标空间是注入器的 ABS 范围；
    而控制台 toScreen() 却按**降采样后的图尺寸**换算。
    maxWidth=360 时，点画面正中央发出去的是 (180,320)，
    在 720x1280 的屏幕上那是左上角四分之一处。

判据不是"界面看起来对不对"，而是**直接读 uinput 收到了什么**：

    adb shell getevent -l /dev/input/eventN

用法:
    ./coord_space_test.py --host 192.168.0.110 --port 8088 --max-width 360
"""
import argparse
import os
import re
import subprocess
import sys
import time
import urllib.request
import json

from playwright.sync_api import sync_playwright

CHROME = os.environ.get(
    "CHROME", "/root/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome")


def sh(serial, cmd):
    r = subprocess.run(["adb", "-s", serial, "shell", cmd],
                       capture_output=True, text=True, timeout=40)
    return r.stdout.replace("\r", "")


def find_touch_device(serial):
    """找 autod 建的那个虚拟触控屏（名字叫 autod-touch）。"""
    out = sh(serial, "getevent -pl 2>/dev/null")
    dev = name = None
    for line in out.splitlines():
        m = re.match(r"\s*add device \d+:\s+(\S+)", line)
        if m:
            dev = m.group(1)
        if "name:" in line and "autod-touch" in line:
            name = dev
            break
    return name


def abs_range(serial, dev):
    out = sh(serial, f"getevent -pl {dev} 2>/dev/null")
    xs = re.search(r"ABS_MT_POSITION_X\s*:\s*value \d+, min \d+, max (\d+)", out)
    ys = re.search(r"ABS_MT_POSITION_Y\s*:\s*value \d+, min \d+, max (\d+)", out)
    if not xs or not ys:
        return None
    return int(xs.group(1)) + 1, int(ys.group(1)) + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--serial", default="emulator-5580")
    ap.add_argument("--max-width", type=int, default=360)
    a = ap.parse_args()

    origin = f"http://{a.host}:{a.port}"
    dev = find_touch_device(a.serial)
    if dev is None:
        print("  ✗ 找不到 autod-touch 设备", file=sys.stderr)
        return 1
    rng = abs_range(a.serial, dev)
    print(f"  触控设备 {dev}  ABS 范围 {rng[0]}x{rng[1]}")

    with urllib.request.urlopen(origin + "/api/v1/info", timeout=8) as r:
        info = json.load(r)
    tW, tH = info["touchWidth"], info["touchHeight"]
    print(f"  /info 报的触控空间 {tW}x{tH}")
    if (tW, tH) != rng:
        print(f"  ✗ /info 报的触控空间和 ABS 范围不一致 —— "
              f"客户端拿到的坐标空间是错的", file=sys.stderr)
        return 1
    print("  ✓ /info 的触控空间 == ABS 范围")

    sh(a.serial, "am start -a android.intent.action.MAIN "
                 "-c android.intent.category.HOME >/dev/null 2>&1")
    time.sleep(2)

    # ⚠️ 顺序很重要：浏览器启动 + 加载页面要好几秒，
    #    先开 getevent 的话 12 秒窗口会被它吃光，抓不到点击。
    #    所以先在浏览器里准备好、算出点击位置，再开抓取。
    with sync_playwright() as p:
        b = p.chromium.launch(headless=True, executable_path=CHROME,
            args=["--no-sandbox", "--disable-gpu",
                  f"--unsafely-treat-insecure-origin-as-secure={origin}"])
        pg = b.new_page(viewport={"width": 900, "height": 1000})
        pg.goto(f"{origin}/?format=jpeg&fps=5&maxWidth={a.max_width}",
                wait_until="domcontentloaded", timeout=30000)
        time.sleep(4)
        g = pg.evaluate("""() => {
            const r = cvs.getBoundingClientRect();
            return {left:r.left, top:r.top, w:r.width, h:r.height, sw:sw, sh:sh};
        }""")
        px = g["left"] + g["w"] / 2
        py = g["top"] + g["h"] / 2
        sent = pg.evaluate("(a)=>toScreen({clientX:a[0],clientY:a[1]})", [px, py])
        print(f"  图 {g['sw']}x{g['sh']}，点画布正中央 → 控制台发出 "
              f"({sent['x']},{sent['y']})")
        # ⚠️ 先把触点"停"到角落去。
        #
        #    内核的 evdev 会对 MT 轴做去重：`input_handle_abs_event()`
        #    发现新值跟该 slot 记录的值相同时直接 INPUT_IGNORE_EVENT，
        #    压根不会送到设备节点。所以如果上一次触摸刚好也在正中央，
        #    这次点击**一个 POSITION 事件都不会有**，测试会误判成失败。
        #    （这不是 bug：InputReader 用自己缓存的槽位位置，点击是准的。）
        import urllib.request as _u
        _req = _u.Request(origin + "/api/v1/tap",
                          data=b'{"x":5,"y":5}',
                          headers={"Content-Type": "application/json"})
        try:
            _u.urlopen(_req, timeout=8).read()
        except Exception:
            pass
        time.sleep(1)

        # 现在才开抓取，然后立刻点
        ev = subprocess.Popen(
            ["adb", "-s", a.serial, "shell", f"timeout 10 getevent -l {dev}"],
            stdout=subprocess.PIPE, text=True)
        time.sleep(1.5)
        pg.mouse.click(px, py)
        time.sleep(2)
        b.close()

    out = ev.communicate(timeout=20)[0] or ""
    xs = [int(v, 16) for v in re.findall(r"ABS_MT_POSITION_X\s+([0-9a-f]+)", out)]
    ys = [int(v, 16) for v in re.findall(r"ABS_MT_POSITION_Y\s+([0-9a-f]+)", out)]
    print(f"  uinput 实际收到 X={xs[:3]} Y={ys[:3]}")

    if not xs or not ys:
        print("  ✗ 没抓到注入事件", file=sys.stderr)
        return 1

    # 正中央 → 两边都该是范围的一半（容差 2px）
    ex, ey = tW // 2, tH // 2
    ok = abs(xs[0] - ex) <= 2 and abs(ys[0] - ey) <= 2
    print(f"  {'✓' if ok else '✗'} 期望 ({ex},{ey})，实际 ({xs[0]},{ys[0]})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
