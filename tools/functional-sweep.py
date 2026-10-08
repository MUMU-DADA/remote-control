#!/usr/bin/env python3
"""functional-sweep.py —— remote-control 全功能体检

设计原则：**不只看接口返回 ok，要看设备真的动了没有**。
这一点是被坑出来的：
  · 电源重启接口一直回 ok:true，而设备纹丝不动（SELinux 挡了 exec sh）
  · 画面流接口一切正常，而抓帧线程一帧都没成功（SELinux 挡了 HAL fd）
两次都是"接口说成功、设备没反应"。所以每项检查尽量带**独立证据**：
重启看 uptime 归零、截图看像素是否真的在变、按键看焦点是否换了窗口。

用法:
    python3 tools/functional-sweep.py                      # 默认 127.0.0.1:8088
    python3 tools/functional-sweep.py host:port

关于「已知平台限制」：有少数项在平台上确实做不到。这类项若一律算失败，
脚本就恒退出 1，
而一个永远红的灯等于没有灯 —— 当不了回归闸门。所以它们单独记账（⊘），
不计入失败。但**不是永久豁免**：一旦它意外通过了，脚本会打印提示要求复核，
避免限制解除后标注还赖着不走。

⚠️ 这条机制要慎用 —— 实测栽过：原来"写剪贴板"被标成平台限制，理由是
"Android 10+ 只允许前台应用写"。真相是那次请求没带 op，缺省值 get 让它
变成了一次**读**，压根没写。**一个错误的豁免比一条失败的检查更危险**：
它把真问题锁进了"已知、不用管"。标注之前先确认那个"限制"本身是真的。
"""
import base64
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

BASE = (sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:8088")
if not BASE.startswith("http"):
    BASE = "http://" + BASE

ADB = None
for cand in ("/root/AutoSnapshotAndroid/aosp/out/host/linux-x86/bin/adb", "adb"):
    try:
        subprocess.run([cand, "version"], capture_output=True, timeout=10)
        ADB = cand
        break
    except Exception:
        continue
SERIAL = "emulator-5580"

passed, failed, known = [], [], []


def check(name, ok, detail="", known_limitation=None):
    """known_limitation：这条检查「失败是预期内的」时，写上原因。

    为什么要有这个：体检脚本的价值在于**能当闸门用**（退出码 0/1）。
    但平台上确实存在做不到的事，如果一律算失败，脚本就恒退出 1 ——
    没人会看一个永远红的灯，闸门也就没了意义。

    所以这类项单独记账（⊘），不计入失败。但不能变成"永久豁免"：
    **一旦它意外通过了，就打印提示要求复核** —— 否则限制解除了、
    标注还赖在那里，等于把闸门悄悄关掉。
    """
    if not ok and known_limitation:
        known.append(name)
        print(f"\033[1;33m  ⊘\033[0m {name}  —— 已知平台限制：{known_limitation}")
        return ok
    if ok and known_limitation:
        print(f"\033[1;33m  ! \033[0m {name}"
              f"  —— 这条标注为「已知限制」的检查**通过了**，"
              f"限制可能已解除，请复核并去掉标注")
    (passed if ok else failed).append(name)
    mark = "\033[1;32m  ✓\033[0m" if ok else "\033[1;31m  ✗\033[0m"
    print(f"{mark} {name}" + (f"  —— {detail}" if detail else ""))
    return ok


def req(path, method="GET", body=None, raw=False, timeout=25):
    url = BASE + path
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    r = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            payload = resp.read()
            return (resp.status, payload) if raw else (resp.status, json.loads(payload))
    except urllib.error.HTTPError as e:
        payload = e.read()
        try:
            return e.code, (payload if raw else json.loads(payload))
        except Exception:
            return e.code, (payload if raw else {})
    except Exception as e:
        return 0, (b"" if raw else {"error": str(e)})


def sh(cmd, timeout=25):
    try:
        r = subprocess.run([ADB, "-s", SERIAL, "shell", cmd],
                           capture_output=True, timeout=timeout)
        return r.stdout.decode(errors="replace").replace("\r", "").strip()
    except Exception:
        return ""


def uptime():
    out = sh("cut -d. -f1 /proc/uptime")
    return int(out) if out.isdigit() else -1


def jpeg_size(payload):
    """从 JPEG 字节流里取宽高（SOFn 段）。不依赖任何图像库。"""
    i = 2
    while i + 9 < len(payload):
        if payload[i] != 0xFF:
            i += 1
            continue
        m = payload[i + 1]
        if m in (0xC0, 0xC1, 0xC2, 0xC3):
            h = (payload[i + 5] << 8) | payload[i + 6]
            w = (payload[i + 7] << 8) | payload[i + 8]
            return w, h
        if m in (0xD8, 0xD9) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        seg = (payload[i + 2] << 8) | payload[i + 3]
        i += 2 + seg
    return 0, 0


def frame_entropy(payload):
    """字节多样性 —— 全黑图熵极低，真实画面高。用来识别"黑屏但接口正常"。"""
    if not payload:
        return 0
    from collections import Counter
    c = Counter(payload[:20000])
    return len(c)


# ─────────────────────────────────────────────────────────────────────────────
print(f"\n\033[1m=== remote-control 全功能体检 ===\033[0m  {BASE}\n")

print("\033[1;34m[1] 元信息\033[0m")
st, d = req("/api/v1/describe")
check("GET /describe", st == 200 and d.get("service") == "remote-control",
      f"service={d.get('service')} 协议 v{d.get('protocolVersion')} 命令 {len(d.get('commands', []))} 条")
# ⚠️ 这里原来写死 "命令数 = 33"，协议涨到 39 之后它一直是红的 ——
#    而"文档里的命令数与实际是否一致"是 check-api-docs.py 的职责
#    （它拿 docs/api/03-socket.md 对服务端，改了文档不改进代码会红）。
#    体检脚本再写死一个数字，只会多一处要同步维护、且一定会烂的地方。
#    这里改成查**自洽性**：编号不重复、名字不空。
_cmds = d.get("commands", [])
_nums = [c.get("cmd") for c in _cmds]
check("命令表自洽（编号不重复、名字非空）",
      len(_nums) == len(set(_nums)) and all(c.get("name") for c in _cmds),
      f"{len(_cmds)} 条命令")
check("buildId 存在", bool(d.get("buildId")), str(d.get("buildId"))[:16])

st, info = req("/api/v1/info")
check("GET /info", st == 200 and info.get("ok") is True,
      f"{info.get('primaryWidth')}x{info.get('primaryHeight')} 触控 {info.get('touchWidth')}x{info.get('touchHeight')}")
check("显示尺寸与触控范围一致",
      info.get("primaryWidth") == info.get("touchWidth") and
      info.get("primaryHeight") == info.get("touchHeight"))

# ── 截图 ──
print("\n\033[1;34m[2] 截图\033[0m")
st, png = req("/api/v1/capture", raw=True, timeout=30)
png_ok = st == 200 and png[:8] == b"\x89PNG\r\n\x1a\n"
check("GET /capture 返回 PNG", png_ok, f"{len(png)} 字节")
if png_ok:
    check("不是纯色黑图（熵判据）", frame_entropy(png) > 200,
          f"字节多样性 {frame_entropy(png)}")

st, jpg = req("/api/v1/capture?format=jpeg", raw=True, timeout=30)
if st == 200 and jpg[:2] == b"\xff\xd8":
    w, h = jpeg_size(jpg)
    check("JPEG 截图尺寸正确", w == info.get("primaryWidth") and h == info.get("primaryHeight"),
          f"{w}x{h}，{len(jpg)} 字节")
else:
    check("JPEG 截图", False, f"HTTP {st}")

# ── 区域抓帧（源头裁剪）──
#
# 判据：返回图的**尺寸**必须等于请求的区域，而不是整屏 ——
# 只验"HTTP 200"的话，一个把 x/y/w/h 全忽略掉的实现照样能绿。
PW, PH = info.get("primaryWidth", 0), info.get("primaryHeight", 0)
if PW > 200 and PH > 200:
    cx, cy, cw, ch = PW // 4, PH // 4, PW // 2, PH // 2
    st, region = req(f"/api/v1/capture?format=jpeg&x={cx}&y={cy}&w={cw}&h={ch}",
                     raw=True, timeout=30)
    if st == 200 and region[:2] == b"\xff\xd8":
        rw_, rh_ = jpeg_size(region)
        check("区域抓帧尺寸 = 请求的区域（不是整屏）",
              rw_ == cw and rh_ == ch,
              f"请求 {cw}x{ch}，得到 {rw_}x{rh_}（整屏是 {PW}x{PH}）")
        check("区域抓帧确实更小", len(region) < len(jpg),
              f"区域 {len(region)} 字节 vs 整屏 {len(jpg)} 字节")
    else:
        check("区域抓帧", False, f"HTTP {st}")

    # 少给参数必须被拒 —— 默认整屏比猜一个安全
    st, _ = req(f"/api/v1/capture?x={cx}&y={cy}&w={cw}", raw=True, timeout=20)
    check("只给三个区域参数 → 400", st == 400, f"HTTP {st}")

    # 越界必须明确报错，不能静默钳到边界
    st, _ = req(f"/api/v1/capture?x={PW - 10}&y=0&w=100&h=100",
                raw=True, timeout=20)
    check("越界的裁剪区域 → 400", st == 400, f"HTTP {st}")

    # WebP method：单次截图的默认应当偏"小体积"，而不是流那档
    st, webp_default = req("/api/v1/capture?format=webp&quality=90", raw=True, timeout=30)
    st2, webp_m2 = req("/api/v1/capture?format=webp&quality=90&method=2",
                       raw=True, timeout=30)
    st3, webp_m6 = req("/api/v1/capture?format=webp&quality=90&method=6",
                       raw=True, timeout=30)
    if st == 200 and st2 == 200 and st3 == 200:
        check("WebP 单次截图默认 = 方法 6（体积优先）",
              len(webp_default) == len(webp_m6),
              f"默认 {len(webp_default)} 字节，method=6 {len(webp_m6)} 字节")
        check("默认比画面流那档（method=2）更小",
              len(webp_default) < len(webp_m2),
              f"{len(webp_default)} vs {len(webp_m2)} 字节")
    else:
        check("WebP method 参数", False, f"HTTP {st}/{st2}/{st3}")

    st, _ = req("/api/v1/capture?format=webp&method=9", raw=True, timeout=20)
    check("method 越界 → 400", st == 400, f"HTTP {st}")

    # 源头降采样
    st, small = req(f"/api/v1/capture?format=jpeg&width={PW // 4}",
                    raw=True, timeout=30)
    if st == 200 and small[:2] == b"\xff\xd8":
        # ⚠️ 这两个变量**不能**叫 sh —— 上面有个同名的辅助函数
        #    sh(命令)，用它跑 adb。第一版写成 `sw, sh = ...` 把函数
        #    覆盖成了 int，于是后面 [5] 节调 sh(...) 直接
        #    "'int' object is not callable" —— 报错位置离真正的原因
        #    隔了一百多行。
        dw_, dh_ = jpeg_size(small)
        check("width 参数在源头降采样", dw_ == PW // 4,
              f"要 {PW // 4} 宽，得到 {dw_}x{dh_}")
    else:
        check("width 降采样", False, f"HTTP {st}")

# ── 触控 ──
print("\n\033[1;34m[3] 触控\033[0m")
# ⚠️ 触控是**分开的接口**（/tap、/swipe…），不是 /touch + action。
st, d = req("/api/v1/tap", "POST", {"x": 640, "y": 360})
check("点击屏幕中心", st == 200 and d.get("ok"), str(d)[:70])
st, d = req("/api/v1/swipe", "POST", {"x1": 200, "y1": 500, "x2": 800, "y2": 500, "ms": 300})
check("滑动（x1/y1/x2/y2）", st == 200 and d.get("ok"), str(d)[:70])

# ⚠️ /swipe 以前**另写了一遍**，只认 x1/y1 且不支持 pressure/curve；
#    而 /drag 认 x/y 也认 x1/y1、还支持压力曲线。同一个概念两套行为。
#    现在统一走一套，所以这两条都该成立。
st, d = req("/api/v1/swipe", "POST", {"x": 200, "y": 500, "x2": 800, "y2": 500, "ms": 300})
check("滑动也接受 x/y 写法（与 /drag 一致）", st == 200 and d.get("ok"), str(d)[:70])

st, d = req("/api/v1/swipe", "POST",
            {"x1": 200, "y1": 500, "x2": 800, "y2": 500, "ms": 300,
             "pressure": 0.2, "pressure2": 0.9, "curve": "updown"})
check("滑动带压力曲线（updown）", st == 200 and d.get("ok"), str(d)[:70])

st, d = req("/api/v1/swipe", "POST",
            {"x1": 200, "y1": 500, "x2": 800, "y2": 500, "ms": 300,
             "curve": "nosuchcurve"})
check("未知曲线 → 400", st == 400, f"HTTP {st}；{str(d)[:60]}")

st, d = req("/api/v1/swipe", "POST",
            {"x1": 200, "y1": 500, "x2": 800, "y2": 500, "ms": 300,
             "pressure": 1.5})
check("pressure 超出 0..1 → 400", st == 400, f"HTTP {st}；{str(d)[:60]}")
st, d = req("/api/v1/longpress", "POST", {"x": 640, "y": 360, "ms": 800})
check("长按", st == 200 and d.get("ok"), str(d)[:70])
st, d = req("/api/v1/doubletap", "POST", {"x": 640, "y": 360})
check("双击", st == 200 and d.get("ok"), str(d)[:70])
st, d = req("/api/v1/drag", "POST", {"x1": 300, "y1": 300, "x2": 700, "y2": 600, "ms": 600})
check("拖拽", st == 200 and d.get("ok"), str(d)[:70])

# ── 按键：不能只看接口，要看焦点真的变了 ──
print("\n\033[1;34m[4] 按键注入\033[0m")
before = sh("dumpsys window 2>/dev/null | grep -m1 mCurrentFocus")
st, d = req("/api/v1/key", "POST", {"key": "home"})
check("注入 HOME 键", st == 200 and d.get("ok"), str(d)[:60])
time.sleep(1.5)
after = sh("dumpsys window 2>/dev/null | grep -m1 mCurrentFocus")
check("HOME 生效（焦点窗口变化或已是桌面）",
      after != before or "launcher" in after.lower() or "Launcher" in after,
      (after or "(取不到)")[:70])
st, d = req("/api/v1/key", "POST", {"key": "back"})
check("注入 BACK 键", st == 200 and d.get("ok"), str(d)[:60])
st, d = req("/api/v1/key", "POST", {"key": "appswitch"})
check("注入 RECENTS 键", st == 200 and d.get("ok"), str(d)[:60])

# ── 应用管理：pm / am 是 shell 脚本，这条曾经整类失效 ──
print("\n\033[1;34m[5] 应用管理（pm/am 是 shell 脚本，曾经整类失效）\033[0m")
st, d = req("/api/v1/apps")
apps = d.get("apps") or d.get("packages") or []
check("列出应用", st == 200 and isinstance(apps, list) and
      d.get("count") == len(apps),
      f"{len(apps)} 个（干净 ROM 允许为 0）")
st, d = req("/api/v1/foreground")
fg = d.get("package") or d.get("foreground") or d.get("pkg") or ""
check("取前台应用", st == 200 and bool(fg), str(fg)[:60])
st, d = req("/api/v1/apps?system=1")
apps_all = d.get("apps") or d.get("packages") or []
check("列出应用（含系统）", st == 200 and len(apps_all) > 0, f"{len(apps_all)} 个")

TEST_PKG = "com.android.settings"
st, d = req(f"/api/v1/apps/{TEST_PKG}")
check(f"查询 {TEST_PKG} 信息", st == 200 and d.get("ok", True) is not False, str(d)[:70])

st, d = req(f"/api/v1/apps/{TEST_PKG}/launch", "POST", {})
launch_ok = st == 200 and d.get("ok") is not False
check("启动 Settings（am start）", launch_ok, str(d)[:70])
if launch_ok:
    time.sleep(2.5)
    fg2 = sh("dumpsys window 2>/dev/null | grep -m1 mCurrentFocus")
    check("Settings 真的起来了", "settings" in fg2.lower(), (fg2 or "(取不到)")[:70])
    st, d = req(f"/api/v1/apps/{TEST_PKG}/kill", "POST", {})
    check("结束 Settings（am force-stop）", st == 200 and d.get("ok") is not False, str(d)[:60])

# ── 应用生命周期：装 / 卸 / 清数据 / 启停 / 权限（v8）──
#
# ⚠️ 这一节是**补出来**的，原因值得写下来：原来的体检完全没有覆盖 install，
#    于是「APK 安装在这套 ROM 上根本装不上」这件事活了很久没被发现 ——
#    暂存目录 /data/misc/remote-control 的 SELinux 标签是
#    remote_control_data_file，而策略没给 system_server 读它的权限，
#    `pm install` 必然失败。接口确实如实回了 500，但**没有任何一条检查去看它**。
#
#    所以这里判据一律取设备侧证据（`pm list packages` / `dumpsys package`），
#    不信接口返回 —— 这正是本脚本开头那条原则。
print("\n\033[1;34m[5b] 应用生命周期（装/卸/清数据/启停/权限）\033[0m")

PROBE = "org.autosnap.arm64probe"


def upload(path, blob, timeout=180):
    """二进制上传（APK）。req() 会把 body 当 JSON 编码，这里不能复用它。"""
    r = urllib.request.Request(BASE + path, data=blob,
                               headers={"Content-Type": "application/octet-stream"},
                               method="POST")
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read() or b"{}")
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw or b"{}")
        except Exception:
            return e.code, {"error": raw[:200].decode(errors="replace")}
    except Exception as e:
        return 0, {"error": str(e)}


_apk = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..",
    "dev", "04-android-rom", "artifacts", "arm64-probe.apk"))

if not os.path.exists(_apk):
    check("安装用例的探针 APK 存在", False, f"缺 {_apk}")
else:
    # 先清干净：上一轮的残留会让「安装成功」变成假绿
    sh(f"pm uninstall {PROBE}")
    st, d = upload("/api/v1/install", open(_apk, "rb").read())
    listed = PROBE in sh("pm list packages")
    check("安装 APK（判据：设备上真的出现了该包）", st == 200 and listed,
          f"HTTP {st}；pm list 命中={listed}；{str(d)[:80]}")

    if listed:
        st, d = req(f"/api/v1/apps/{PROBE}/clear", "POST", {})
        check("清除应用数据", st == 200 and d.get("ok") is not False, str(d)[:70])

        st, d = req(f"/api/v1/apps/{PROBE}/enabled", "POST", {"enabled": False})
        disabled = PROBE in sh(f"pm list packages -d | grep -F {PROBE}")
        check("停用应用（判据：pm list -d 里出现）", st == 200 and disabled,
              f"停用列表命中={disabled}；{str(d)[:60]}")

        st, d = req(f"/api/v1/apps/{PROBE}/enabled", "POST", {"enabled": True})
        back = PROBE not in sh(f"pm list packages -d | grep -F {PROBE}")
        check("重新启用应用（判据：已不在停用列表）", st == 200 and back,
              f"已恢复={back}；{str(d)[:60]}")

        # 权限：探针 APK 没申请权限，用系统里确实申请了的那条来验；
        # 判据是 `dumpsys package` 里的 granted= 真的翻转。
        PERM_PKG = "com.android.contacts"
        PERM = "android.permission.READ_CONTACTS"

        def _granted():
            out = sh(f"dumpsys package {PERM_PKG} | grep -F '{PERM}:'")
            return "granted=true" in out

        st, d = req(f"/api/v1/apps/{PERM_PKG}/permission", "POST",
                    {"permission": PERM, "grant": True})
        g1 = _granted()
        st2, d2 = req(f"/api/v1/apps/{PERM_PKG}/permission", "POST",
                      {"permission": PERM, "grant": False})
        g0 = _granted()
        check("授予/撤销运行时权限（判据：dumpsys 里 granted 真的翻转）",
              st == 200 and st2 == 200 and g1 and not g0,
              f"grant→{g1}，revoke→{g0}")

        # 不存在的包必须 404 —— 而不是一句误导性的「系统应用通常卸不掉」
        st, d = req("/api/v1/apps/com.nope.nope/uninstall", "POST", {})
        check("卸载不存在的包 → 404", st == 404 and d.get("status") == 4105,
              f"HTTP {st} status={d.get('status')}")

        st, d = req(f"/api/v1/apps/{PROBE}/uninstall", "POST", {})
        gone = PROBE not in sh("pm list packages")
        check("卸载 APK（判据：设备上真的没了）", st == 200 and gone,
              f"HTTP {st}；已消失={gone}")

print("\n\033[1;34m[5c] 整串文本输入（含中文）\033[0m")
# 判据同样是设备侧证据：用 uiautomator 把输入框里的文字读回来，
# 而不是信接口那句 ok:true。曾经最典型的坑就是"接口说成功、字符没进去"。


def uiautomator_field_text():
    """读回当前界面上那个搜索框的文字（取不到就返回空串）。"""
    sh("uiautomator dump /sdcard/sweep_text.xml")
    out = sh("cat /sdcard/sweep_text.xml 2>/dev/null "
             "| tr '>' '\\n' | grep -F 'search_src_text'")
    m = re.search(r'text="([^"]*)"', out)
    return m.group(1) if m else ""


def clear_focused_field(n=80):
    """清空当前输入框：先跳到末尾，再一路退格。"""
    sh("input keyevent 123")            # KEYCODE_MOVE_END
    for _ in range(n // 10):
        sh("input keyevent " + " ".join(["67"] * 10))   # KEYCODE_DEL ×10
    time.sleep(0.5)


# 需要一个真的输入框。Settings 的搜索页最省事，且每个模拟器镜像都有。
sh("am start -a com.android.settings.action.SETTINGS_SEARCH")
time.sleep(3)
clear_focused_field()

CJK = "你好，世界！Hello123"
st, d = req("/api/v1/text", "POST", {"text": CJK})
readback = uiautomator_field_text()
check("粘贴中文（判据：输入框里读回来的就是那串字）",
      st == 200 and readback == CJK,
      f"HTTP {st}；期望 {CJK!r}，读回 {readback!r}")

# 剪贴板必须还回去 —— 不还的话一次远程输入就把用户复制的密码顶掉了。
#
# ⚠️ 必须先放一个**已知值**再测。原来是直接读剪贴板看有没有我们的内容，
#    但刚开机时剪贴板本来就是空的：那种情况下"恢复成空"做不到（没法
#    把剪贴板清空），服务端会如实回 clipboardRestored=false —— 于是这条
#    检查在空剪贴板的设备上**必然误报**。实测就是这样红的。
SENTINEL = "sweep-sentinel-请勿改动"
req("/api/v1/clipboard", "POST", {"op": "set", "text": SENTINEL})
st, d = req("/api/v1/clipboard?op=get")
check("测试前剪贴板已被设为哨兵值",
      d.get("text") == SENTINEL, f"现在是 {d.get('text', '')[:40]!r}")

clear_focused_field()
st, d = req("/api/v1/text", "POST", {"text": CJK})
readback = uiautomator_field_text()
check("再次粘贴中文仍然正确", st == 200 and readback == CJK,
      f"读回 {readback!r}")

st, d = req("/api/v1/clipboard?op=get")
clip = d.get("text", "")
check("文本输入后剪贴板被恢复成原值（哨兵还在）",
      clip == SENTINEL, f"剪贴板现在是 {clip[:40]!r}")

# type 模式：逐字符按键（走 uinput 键盘），顺便验大小写
clear_focused_field()
st, d = req("/api/v1/text", "POST", {"text": "Ab1!", "mode": "type"})
readback = uiautomator_field_text()
check("逐字符键入 ASCII（判据：输入框里读回，且大小写正确）",
      st == 200 and readback == "Ab1!",
      f"HTTP {st}；读回 {readback!r}（大写 A 能验出 Shift 有没有按对）")

# type 模式遇到中文必须**明确拒绝**，而且要指出是第几个字符 ——
# 打到一半才发现打不出来，设备上会留半截文本，比直接拒绝难收拾得多
st, d = req("/api/v1/text", "POST", {"text": "abc中文", "mode": "type"})
err = d.get("error", "")
check("type 模式遇中文 → 501 且指出第几个字符",
      st == 501 and "第 4 个字符" in err,
      f"HTTP {st}；{err[:80]}")

# 非法 UTF-8 直接拒，不"尽力而为"地塞进去。
# ⚠️ 必须发**原始字节**：走 json.dumps 的话 Python 会把它转成 \uXXXX 转义，
#    那测的是"JSON 转义对不对"，而不是"非法 UTF-8 会不会被拦下"。
_r = urllib.request.Request(
    BASE + "/api/v1/text", data=b'{"text":"\xff\xfe"}',
    headers={"Content-Type": "application/json"}, method="POST")
try:
    with urllib.request.urlopen(_r, timeout=20) as _resp:
        _st, _body = _resp.status, _resp.read()
except urllib.error.HTTPError as _e:
    _st, _body = _e.code, _e.read()
check("非法 UTF-8 → 400（不静默塞乱码）", _st == 400,
      f"HTTP {_st}；{_body[:70].decode(errors='replace')}")

# ── 文件 ──
print("\n\033[1;34m[6] 文件管理\033[0m")
st, d = req("/api/v1/files?path=/sdcard")
check("列出 /sdcard", st == 200 and d.get("ok", True) is not False,
      f"{len(d.get('entries', []))} 项")

# ── 剪贴板 ──
print("\n\033[1;34m[7] 剪贴板\033[0m")
mark = f"rc-sweep-{int(time.time())}"
st, d = req("/api/v1/clipboard", "POST", {"op": "set", "text": mark})
set_ok = st == 200 and d.get("ok") is not False
check("写入剪贴板", set_ok, str(d)[:60])
if set_ok:
    st, d = req("/api/v1/clipboard?op=get")
    # ⚠️ 这条曾经带着一条"已知平台限制：Android 10+ 后台写剪贴板被静默丢弃"。
    #    那是**误判**：当时的请求是 POST {"text": ...} 而没给 op，而 op 缺省是
    #    "get" —— 它根本没在写，返回的 ok 是"读成功"。实测（本 ROM，连做三次
    #    往返，含中文）写读完全正常。现在请求里显式给 op，判据才立得住。
    check("读回剪贴板且内容一致", d.get("text") == mark, f"读到 {str(d.get('text'))[:40]}")

# ── 旋转 ──
print("\n\033[1;34m[8] 屏幕方向\033[0m")
# NOTE: body is {"to": "<v>"}, values 0/90/180/270/portrait/landscape.
# {"rotation":1} is treated as a STATUS query - returns applied:true but
# changes nothing. Same "API says ok, device did not move" trap as before.
st, d = req("/api/v1/rotate", "POST", {"to": "90"})
rot_ok = st == 200 and d.get("ok") is not False
check("rotate to 90", rot_ok, str(d)[:90])
if rot_ok:
    time.sleep(2.5)
    st, info_rot = req("/api/v1/info")
    flipped = (info_rot.get("primaryWidth") == info.get("primaryHeight") and
               info_rot.get("primaryHeight") == info.get("primaryWidth"))
    # ⚠️ 设备感知：模拟器用的是**固定横屏皮肤**（1280x720 面板），物理上转不了，
    #    wm size 覆盖只改应用可见区域、抓帧方向不变。服务会在 note 里说明。
    #    这种情况下"尺寸没翻"是**正确行为**，不是 bug —— 接口如实报告了能力限制。
    note = str(d.get("note", ""))
    unsupported = ("不支持" in note) or ("皮肤" in note)
    check("旋转生效（或服务明确报告该设备不支持）",
          flipped or unsupported,
          f"{info.get('primaryWidth')}x{info.get('primaryHeight')} -> "
          f"{info_rot.get('primaryWidth')}x{info_rot.get('primaryHeight')}"
          + ("（设备不支持旋转，服务已如实说明）" if unsupported and not flipped else ""))
    # ⚠️ 「尺寸真的翻了」必须用**转完那一刻**的读数判，也就是上面刚读的 info_rot。
    #
    #    这里以前是等 `{"to":"0"}` 转回去之后才重读 info2 来判的 —— 那等于在问
    #    "转过去、再转回来之后，尺寸还翻着吗"，于是**只有"本次运行开始时恰好是横屏"
    #    才碰巧通过**（起始横屏 → 转 90 不动 → 转回 0 变竖屏 → 恰好满足那个式子）。
    #    起始竖屏时必然误报。是判据写错了，不是被测对象错了。
    check("旋转后显示尺寸反过来",
          info_rot.get("primaryWidth") == info.get("primaryHeight") and
          info_rot.get("primaryHeight") == info.get("primaryWidth"),
          f"{info.get('primaryWidth')}x{info.get('primaryHeight')} → "
          f"{info_rot.get('primaryWidth')}x{info_rot.get('primaryHeight')}",
          # 面板固定横屏的设备上，wm size 覆盖只改应用可见区域、不改抓帧方向，
          # 所以"没翻"是设备限制 —— 而且服务已在 note 里如实说明了。
          known_limitation=("面板固定横屏：wm size 覆盖不改变抓帧方向，"
                            "服务已在 note 里如实说明"
                            if unsupported and not flipped else None))
    # 转回默认方向，别把设备留在改过的状态
    req("/api/v1/rotate", "POST", {"to": "0"})
    time.sleep(2.5)
req("/api/v1/rotate", "POST", {"rotation": 0})
time.sleep(2)

# ── 画面流：不能只看接口，要看真的收到帧 ──
print("\n\033[1;34m[9] 画面流（帧数要看服务端统计，不是接口返回）\033[0m")
st, before_stats = req("/api/v1/params")


def stream_frames(seconds=6):
    import socket as sk
    import struct
    try:
        s = sk.create_connection((BASE.split("//")[1].split(":")[0],
                                  int(BASE.rsplit(":", 1)[1])), timeout=seconds + 4)
    except Exception as e:
        return -1, str(e)
    key = base64.b64encode(b"0123456789abcdef").decode()
    s.sendall((f"GET /api/v1/stream HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    data = buf.split(b"\r\n\r\n", 1)[1]
    s.settimeout(seconds)
    t0 = time.time()
    try:
        while time.time() - t0 < seconds:
            c = s.recv(1 << 16)
            if not c:
                break
            data += c
    except Exception:
        pass
    i = n = 0
    while i + 2 <= len(data):
        b1, b2 = data[i], data[i + 1]
        ln = b2 & 0x7F
        i += 2
        if ln == 126:
            ln = struct.unpack(">H", data[i:i + 2])[0]
            i += 2
        elif ln == 127:
            ln = struct.unpack(">Q", data[i:i + 8])[0]
            i += 8
        if (b1 & 0x0F) == 2:
            n += 1
        i += ln
    return n, ""


# 先制造变化，再收帧 —— 静止画面 skipUnchanged 本来就不发
subprocess.Popen([ADB, "-s", SERIAL, "shell", "input keyevent 3"],
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
frames, err = stream_frames(6)
check("WebSocket 收到二进制帧", frames > 0, f"{frames} 帧 {err}".strip())

st, after_stats = req("/api/v1/params")
cap = (after_stats or {}).get("capture", {})
check("服务端抓帧计数在涨",
      cap.get("frames", 0) > 0,
      f"frames={cap.get('frames')} lastCaptureMs={cap.get('lastCaptureMs')}")

# ── 服务自身 ──
print("\n\033[1;34m[10] 服务自身\033[0m")
st, d = req("/api/v1/service")
check("查询服务开关", st == 200 and "serving" in d, f"serving={d.get('serving')}")
st, d = req("/api/v1/config")
check("读取配置", st == 200, str(d)[:60])
st, d = req("/api/v1/log?lines=5")
check("读取日志", st == 200, f"{len(str(d))} 字节")

# ── 电源：这一项必须看 uptime，不能信接口返回 ──
print("\n\033[1;34m[11] 电源（重启）—— 判据是 uptime 归零，不是接口返回 ok\033[0m")
up_before = uptime()
st, d = req("/api/v1/power", "POST", {"action": "reboot"}, timeout=30)
print(f"    接口返回: {json.dumps(d, ensure_ascii=False)[:110]}")
accepted = (st == 200 and d.get("ok")) or st == 0   # st==0：设备已经重启，连接被断掉是正常的
if not accepted:
    check("重启被受理", False,
          f"接口明确报失败：{str(d.get('error',''))[:80]}")
else:
    check("重启被受理", True, f"重启前 uptime={up_before}s")
    print("    等设备重启并回到可服务状态（最多 3 分钟）…")
    back = -1
    for _ in range(60):
        time.sleep(3)
        u = uptime()
        if 0 <= u < up_before:
            back = u
            break
    if back >= 0:
        check("设备真的重启了（uptime 归零）", True, f"新 uptime={back}s")
        # ⚠️ 设备重启会把 **adb forward 一起清掉**（adbd 重起）。
        #    不重建的话这里必然 HTTP 0 —— 而那是测试脚手架的毛病，不是服务没恢复。
        #    实测踩过：脚本报"重启后服务自动恢复"失败，人工一查服务 running、HTTP 200。
        host, port = BASE.split("//")[1].split(":")
        if host in ("127.0.0.1", "localhost") and ADB:
            try:
                subprocess.run([ADB, "-s", SERIAL, "forward", "--remove-all"],
                               capture_output=True, timeout=15)
                subprocess.run([ADB, "-s", SERIAL, "forward",
                                f"tcp:{port}", "tcp:8088"],
                               capture_output=True, timeout=15)
            except Exception:
                pass
        st2 = 0
        for _ in range(20):
            st2, d2 = req("/api/v1/info", timeout=6)
            if st2 == 200:
                break
            time.sleep(3)
        check("重启后服务自动恢复", st2 == 200, f"HTTP {st2}")
    else:
        check("设备真的重启了（uptime 归零）", False,
              f"等了 3 分钟 uptime 仍 >= {up_before}，设备没重启")

# ─────────────────────────────────────────────────────────────────────────────
print(f"\n\033[1m=== 结果 ===\033[0m")
print(f"  通过 {len(passed)} 项，已知限制 {len(known)} 项，失败 {len(failed)} 项")
if known:
    print("\n\033[1;33m已知平台限制（不计入失败）：\033[0m")
    for k in known:
        print(f"  · {k}")
if failed:
    print("\n\033[1;31m失败项：\033[0m")
    for f in failed:
        print(f"  · {f}")
sys.exit(1 if failed else 0)
