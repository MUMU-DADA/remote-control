#!/usr/bin/env python3
"""控制台文件浏览的端到端验收。"""
import time, sys
from playwright.sync_api import sync_playwright

import argparse
_ap = argparse.ArgumentParser()
_ap.add_argument("--host", default="127.0.0.1")
_ap.add_argument("--port", type=int, default=8088)
_a = _ap.parse_args()
BASE = f"http://{_a.host}:{_a.port}"
checks = []
def chk(name, cond, extra=""):
    checks.append(bool(cond))
    print(f"  {'✓' if cond else '✗'} {name} {extra}")

with sync_playwright() as p:
    b = p.chromium.launch(headless=True,
        executable_path="/root/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome",
        args=["--no-sandbox", "--disable-gpu",
              f"--unsafely-treat-insecure-origin-as-secure={BASE}"])
    pg = b.new_page(viewport={"width": 900, "height": 1400})
    errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto(BASE + "/", wait_until="domcontentloaded", timeout=30000)
    time.sleep(3)

    chk("文件卡片存在", pg.evaluate("()=>!!document.getElementById('card-files')"))
    # 卡片若默认收起，展开
    pg.evaluate("()=>{const c=document.getElementById('card-files');"
                " if(c && c.classList.contains('collapsed')) toggleCard('card-files');}")
    time.sleep(1)

    chk("启动时问到了存储根",
        pg.evaluate("()=>typeof fsRoot==='string' && fsRoot.length>0"),
        pg.evaluate("()=>fsRoot"))

    pg.evaluate("()=>fsGoto('/')")   # 相对存储根
    time.sleep(2)
    txt = pg.evaluate("()=>document.getElementById('fsPath').textContent")
    listing = pg.evaluate("()=>document.getElementById('fsList').textContent")
    print(f"    路径栏: {txt}")
    print(f"    列表: {listing[:90]}")
    chk("跳到根目录后列出来了", "DCIM" in listing or "Documents" in listing)

    # 进子目录
    pg.evaluate("()=>fsGoto('/sdcard/Documents')")
    time.sleep(2)
    l2 = pg.evaluate("()=>document.getElementById('fsList').textContent")
    print(f"    Documents 里: {l2[:80]}")
    chk("能进子目录并列出文件", "a.txt" in l2)

    # 新建目录
    pg.evaluate("()=>fsPost({op:'mkdir',path:'/sdcard/Documents/fromui'},'建好了')")
    time.sleep(2)
    l3 = pg.evaluate("()=>document.getElementById('fsList').textContent")
    chk("能从界面新建目录", "fromui" in l3, l3[:60])

    # 删除
    pg.evaluate("()=>fsPost({op:'delete',path:'/sdcard/Documents/fromui',recursive:true},'删了')")
    time.sleep(2)
    l4 = pg.evaluate("()=>document.getElementById('fsList').textContent")
    chk("能从界面删除", "fromui" not in l4)

    # 越界要被拒且报出来
    pg.evaluate("()=>fsGoto('/data')")
    time.sleep(2)
    m = pg.evaluate("()=>document.getElementById('fsMsg').textContent")
    print(f"    越界提示: {m[:70]}")
    chk("越界路径被拒且提示清楚", "✗" in m and "允许范围" in m)

    chk("页面无 JS 报错", not errs, str(errs[:1]))
    pg.screenshot(path="/tmp/fs_ui.png", clip={"x":600,"y":200,"width":300,"height":500})
    b.close()

print(f"\n  通过 {sum(checks)}/{len(checks)}")
sys.exit(0 if all(checks) else 1)
