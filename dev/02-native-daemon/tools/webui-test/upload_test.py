#!/usr/bin/env python3
"""验证控制台的上传：进度显示 + 大文件不再被拒。"""
import os
import sys, time
from playwright.sync_api import sync_playwright

import wctest_token

# 交付镜像默认开鉴权：没令牌页面会 401，而多数检查照样"通过"。
_TOKEN = wctest_token.resolve_token()

import argparse
_ap = argparse.ArgumentParser()
_ap.add_argument("--host", default="127.0.0.1")
_ap.add_argument("--port", type=int, default=8088)
_a = _ap.parse_args()
BASE = f"http://{_a.host}:{_a.port}"
_SMALL = ("/root/AutoSnapshotAndroid/dev/05-controller-app/"
          "build/remote-control-controller.apk")

# 大文件那一档的素材以前是手搓到 /tmp/mid.apk 的，**仓库里没有** ——
# 换台机器跑就 FileNotFoundError 崩在一个 prepare 步骤上，
# 看起来像测试本身坏了。现在没有就现生成一份：这一档要验的是
# "大文件能不能走完上传链路"，素材内容无所谓（所以用不可压缩的随机字节，
# 免得被中间层压缩掉、测不到真实的传输量）。
_BIG = "/tmp/rc-upload-300m.bin"
_BIG_BYTES = 300 * 1024 * 1024


def ensure_big_fixture():
    if os.path.exists(_BIG) and os.path.getsize(_BIG) == _BIG_BYTES:
        return _BIG
    print(f"  生成大文件素材（{_BIG_BYTES // (1024*1024)} MB，一次性）…")
    with open(_BIG, "wb") as fh:
        left = _BIG_BYTES
        chunk = os.urandom(1 << 20)
        while left > 0:
            n = min(left, len(chunk))
            fh.write(chunk[:n])
            left -= n
    return _BIG


CASES = [
    ("小 APK", _SMALL),
    ("300MB", ensure_big_fixture()),
]

with sync_playwright() as p:
    b = p.chromium.launch(headless=True,
        executable_path="/root/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome",
        args=["--no-sandbox", "--disable-gpu",
              f"--unsafely-treat-insecure-origin-as-secure={BASE}"])
    results = []
    for name, path in CASES:
        pg = wctest_token.new_page(b, _TOKEN, viewport={"width": 900, "height": 1000})
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.goto(BASE + "/", wait_until="domcontentloaded", timeout=30000)
        time.sleep(2)
        pg.evaluate("() => { try { toggleCard('card-apps'); } catch (e) {} }")
        time.sleep(1)

        seen = []
        pg.set_input_files("#apkfile", path)
        # 记下网络层的结果，别只看界面文字：
        # 大文件那条用例喂的是随机数据，安装**必然**失败，
        # 但"上传有没有被接受"才是这里要验的（413 = 没接受）。
        net = []
        pg.on("response", lambda r: net.append(r.status)
              if "install" in r.url else None)
        for _ in range(90):
            time.sleep(1)
            m = pg.evaluate("() => { const e=document.getElementById('apkmsg'); return e?e.textContent:null; }")
            if m and (not seen or seen[-1] != m):
                seen.append(m)
            if m and ("上传中" not in m) and ("正在安装" not in m):
                break
        print(f"  ── {name} ──")
        for s in seen:
            print(f"     {s}")
        print(f"     HTTP: {net}")
        if 413 in net:
            print("     ✗ 被 413 拒了 —— 大文件上传仍然失败")

        # ── 判据 ──
        #
        # 这个脚本以前**只打印不判定** —— 跑完没有任何结论，看起来"跑了"
        # 其实是把判断留给了读日志的人。现在给出明确结论：
        #   1. 上传被接受（没有 413）
        #   2. 界面确实走过了"上传中"（否则可能是根本没开始传）
        #   3. 页面没有 JS 报错
        # 注意**不**要求安装成功：大文件那条喂的是随机字节，
        # 安装必然失败，而这里验的是上传链路。
        # 判据要**分用例**看，因为两条的证据形态不一样：
        #   小 APK：消息变成"✓ 已安装 …" —— 整条链路（上传+安装）走通了
        #   大文件：喂的是随机字节，安装必然失败；能证明上传真的发生了的
        #           是**带百分比的进度消息**（"上传中… 24% 72/300.0 MB"），
        #           而不是最后那句安装失败
        installed = any("已安装" in s for s in seen)
        progressed = any(("上传中" in s) and ("%" in s) for s in seen)
        accepted = 413 not in net
        big = "300" in name
        good = accepted and not errs and (progressed if big else installed)
        results.append((name, good))
        print(f"     → 上传被接受={accepted} 有进度/已安装="
              f"{progressed if big else installed} 无页面错误={not errs}")
        if errs:
            print(f"     页面错误: {errs[:1]}")
        pg.close()
    b.close()

    ok = sum(1 for _, v in results if v)
    print(f"\n  通过 {ok}/{len(results)}")
    for nm, v in results:
        if not v:
            print(f"    ✗ {nm}")
    raise SystemExit(0 if ok == len(results) else 1)
