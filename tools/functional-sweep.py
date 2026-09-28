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
"""
import base64
import json
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

passed, failed = [], []


def check(name, ok, detail=""):
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
check("命令数 = 33", len(d.get("commands", [])) == 33, f"实际 {len(d.get('commands', []))}")
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

# ── 触控 ──
print("\n\033[1;34m[3] 触控\033[0m")
# ⚠️ 触控是**分开的接口**（/tap、/swipe…），不是 /touch + action。
st, d = req("/api/v1/tap", "POST", {"x": 640, "y": 360})
check("点击屏幕中心", st == 200 and d.get("ok"), str(d)[:70])
st, d = req("/api/v1/swipe", "POST", {"x1": 200, "y1": 500, "x2": 800, "y2": 500, "ms": 300})
check("滑动", st == 200 and d.get("ok"), str(d)[:70])
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
check("列出应用", st == 200 and len(apps) > 0, f"{len(apps)} 个")
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

# ── 文件 ──
print("\n\033[1;34m[6] 文件管理\033[0m")
st, d = req("/api/v1/files?path=/sdcard")
check("列出 /sdcard", st == 200 and d.get("ok", True) is not False,
      f"{len(d.get('entries', []))} 项")

# ── 剪贴板 ──
print("\n\033[1;34m[7] 剪贴板\033[0m")
mark = f"rc-sweep-{int(time.time())}"
st, d = req("/api/v1/clipboard", "POST", {"text": mark})
set_ok = st == 200 and d.get("ok") is not False
check("写入剪贴板", set_ok, str(d)[:60])
if set_ok:
    st, d = req("/api/v1/clipboard")
    check("读回剪贴板且内容一致", d.get("text") == mark, f"读到 {str(d.get('text'))[:40]}")

# ── 旋转 ──
print("\n\033[1;34m[8] 屏幕方向\033[0m")
st, d = req("/api/v1/rotate", "POST", {"rotation": 1})
rot_ok = st == 200 and d.get("ok") is not False
check("旋转到 90°", rot_ok, str(d)[:70])
time.sleep(2)
st, info2 = req("/api/v1/info")
check("旋转后显示尺寸反过来",
      info2.get("primaryWidth") == info.get("primaryHeight"),
      f"{info.get('primaryWidth')}x{info.get('primaryHeight')} → {info2.get('primaryWidth')}x{info2.get('primaryHeight')}")
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
if not (st == 200 and d.get("ok")):
    check("重启被受理", False, "接口如实报了失败（这正是修复后的正确行为）")
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
print(f"  通过 {len(passed)} 项，失败 {len(failed)} 项")
if failed:
    print("\n\033[1;31m失败项：\033[0m")
    for f in failed:
        print(f"  · {f}")
sys.exit(1 if failed else 0)
