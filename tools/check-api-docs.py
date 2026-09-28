#!/usr/bin/env python3
"""核对 docs/api/ 里写的事实与运行中的服务是否一致。

文档最容易腐烂的地方就是**数字和清单** —— 命令有几条、端点叫什么、
错误码是多少：写的时候是对的，改了代码就忘了改文档，而没有人会去读
一份过期的接口文档。

所以把它们变成可执行的断言。改了协议就跑一遍。

用法:
    python3 tools/check-api-docs.py [http://host:8088]

退出码 0 = 全部一致；非 0 = 有文档与实际不符。
"""
import json, urllib.request, urllib.error, re, sys, os

# 默认地址可被参数或 AUTOD_BASE 覆盖 —— 设备 IP 会变，
# 写死一个只会让人以为"检查通过了"而其实连的是别的东西。
BASE = (sys.argv[1] if len(sys.argv) > 1
        else os.environ.get("AUTOD_BASE", "http://127.0.0.1:8088"))
B = BASE.rstrip("/") + "/api/v1"
DOCS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "api")
def api(p):
    try:
        with urllib.request.urlopen(B+p, timeout=20) as r: return json.load(r)
    except Exception as e: return {"__err": str(e)}

ok = bad = 0
def chk(cond, what, detail=""):
    global ok, bad
    if cond: ok += 1; print(f"  \033[1;32m✓\033[0m {what}")
    else:    bad += 1; print(f"  \033[1;31m✗\033[0m {what}  {detail}")

d = api("/describe")
cmds = d.get("commands", [])
print("\n[1] 命令数 / 版本")
chk(d.get("protocolVersion") == 7, "protocolVersion = 7", f"实际 {d.get('protocolVersion')}")
chk(len(cmds) == 33, "命令 33 条", f"实际 {len(cmds)}")
# README 里写"32 条命令"
for f in ("README.md","03-socket.md","01-http.md"):
    t = open(os.path.join(DOCS,f),encoding='utf-8').read()
    nums = set(re.findall(r'(\d+)\s*条命令', t))
    if nums:
        chk(nums == {"33"}, f"{f} 里写的命令数与实际一致", f"文档写 {nums}")

print("\n[2] 命令号表（03-socket.md）")
t = open(os.path.join(DOCS,"03-socket.md"),encoding='utf-8').read()
docmap = dict(re.findall(r'\|\s*(\d+)\s*\|\s*`(\w+)`\s*\|\s*\d+\s*\|', t))
real = {str(c["cmd"]): c["name"] for c in cmds}
mismatch = [(k, docmap.get(k), real.get(k)) for k in real if docmap.get(k) != real.get(k)]
chk(not mismatch, "32 条命令的编号与名称全部对应", str(mismatch[:3]))

print("\n[3] HTTP 端点数（README/01-http）")
t = open(os.path.join(DOCS,"01-http.md"),encoding='utf-8').read()
doc_eps = set(re.findall(r'^###\s+(?:GET|POST|WS)\s+/([a-z]+)', t, re.M))
doc_eps |= set(re.findall(r'^\|\s*`?(?:GET|POST|WS)`?\s*\|\s*`/api/v1/([a-z]+)', t, re.M))
doc_eps |= set(re.findall(r'^\|\s*`POST /([a-z]+)`', t, re.M))
live = set()
for res in ["describe","config","selftest","stats","log","logfile","logstream","shutdown",
            "restart","capture","stream","touch","longpress","drag","doubletap","key",
            "clipboard","service","running","params","power","info","tap","swipe","apps",
            "foreground","install","download","files","rotate"]:
    live.add(res)
missing = sorted(live - doc_eps)
chk(not missing, "活着的端点都写进了文档", f"缺 {missing}")

print("\n[4] capabilities 字段（README）")
t = open(os.path.join(DOCS,"README.md"),encoding='utf-8').read()
chk("describe" in t and "capabilities" in t, "README 提到 describe/capabilities")

print("\n[5] 错误码表（05-errors.md）")
t = open(os.path.join(DOCS,"05-errors.md"),encoding='utf-8').read()
# 实测几个码
def code_of(path, method="GET", body=None):
    data = json.dumps(body).encode() if body else None
    h = {"Content-Type":"application/json"} if data else {}
    req = urllib.request.Request(B+path, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(req, timeout=20) as r: return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        try:
            body = e.read().decode()
            return e.code, json.loads(body) if body.strip() else {}
        except Exception: return e.code, {}
st, j = code_of("/nonexistent")
chk(j.get("status") == 4098 and st == 400, "kErrBadCmd = 4098 → HTTP 400（未知路由）",
    f"实际 status={j.get('status')} http={st}")
st, j = code_of("/apps/com.nope.nope")
chk(j.get("status") == 4105 and st == 404, "kErrNotFound = 4105 → HTTP 404",
    f"实际 status={j.get('status')} http={st}")
st, j = code_of("/key", "POST", {"key":"nonsense"})
chk(j.get("status") == 4099 and st == 400, "kErrBadArg = 4099 → HTTP 400",
    f"实际 status={j.get('status')} http={st}")
st, j = code_of("/capture?format=bogus")
chk(st == 400, "未知格式 → HTTP 400", f"实际 {st}")

print("\n[6] /params 的抓帧节奏（01-http.md）")
pm = api("/params")
cap = pm.get("capture", {})
t = open(os.path.join(DOCS, "01-http.md"), encoding="utf-8").read()
for f in ("activeFps", "subscribers", "frames", "lastCaptureMs",
          "captureWidth", "served", "misses", "running", "subscriberList",
          "changeGen", "unchanged"):
    chk(f in cap, f"capture.{f} 存在", f"实际字段 {sorted(cap)}")
    chk(f in t, f"01-http.md 里写了 capture.{f}")
chk(isinstance(cap.get("subscriberList"), list),
    "capture.subscriberList 是**数组**（不是被转义的字符串）",
    f"实际类型 {type(cap.get('subscriberList')).__name__}")

print("\n[7] /params 的 quality 范围（01-http.md）")
ql = pm.get("quality", {})
for f, want in (("png", (1, 9)), ("jpeg", (1, 100)),
                ("webp", (1, 100)), ("h264", (1, 100))):
    v = ql.get(f, {})
    chk((v.get("min"), v.get("max")) == want,
        f"quality.{f} 范围 = {want[0]}-{want[1]}",
        f"实际 {v.get('min')}-{v.get('max')}")
    chk(f in t, f"01-http.md 里写了 quality.{f}")

print("\n[8] 屏幕方向 /rotate（01-http.md）")
# /rotate 是 POST（有副作用），不能用上面的 api() —— 那个是 GET
def api_post(path, body):
    req = urllib.request.Request(B + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r)
    except Exception as e:
        return {"__err": str(e)}

rot = api_post("/rotate", {"to": "status"})
chk(rot.get("ok") is True, "Rotate 可用（POST /rotate）")
for f in ("requested", "applied", "method", "rotation", "actualRotation",
          "width", "height", "note"):
    chk(f in rot, f"rotate.{f} 存在", f"实际字段 {sorted(rot)}")
chk(rot.get("method") in ("user-rotation", "wm-size", "none"),
    "rotate.method 是三选一", f"实际 {rot.get('method')!r}")
t01 = open(os.path.join(DOCS, "01-http.md"), encoding="utf-8").read()
for f in ("applied", "actualRotation", "wm-size"):
    chk(f in t01, f"01-http.md 里写了 rotate 的 {f}")
chk("/rotate" in t01, "01-http.md 里有 /rotate")

print("\n[9] 触控坐标空间 == 注入器 ABS 范围")
info = api("/info")
chk(info.get("touchWidth", 0) > 0 and info.get("touchHeight", 0) > 0,
    "info.touchWidth/Height 非零",
    f"实际 {info.get('touchWidth')}x{info.get('touchHeight')}")
chk("touchWidth" in t01 and "ABS" in t01,
    "01-http.md 里说明了触控空间是 ABS 范围")

print("\n[10] 编码器逐格式上报（01-http.md）")
be = pm.get("codecs", {}).get("backend", "")
chk("jpeg=" in be and "webp=" in be and "png=" in be,
    "codecs.backend 逐格式上报", f"实际 {be!r}")

print("\n[11] 文档内部的链接都能解析")
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
import glob
allmd = []
for pat in ("README.md", "docs/**/*.md", "dev/*/README.md",
            "dev/02-native-daemon/**/*.md"):
    allmd += glob.glob(os.path.join(ROOT, pat), recursive=True)
allmd = sorted(set(f for f in allmd if os.path.isfile(f)
                   and "/vendor/" not in f and "04-x64-android" not in f))
dead = []
for f in allmd:
    base = os.path.dirname(f)
    txt = open(f, encoding="utf-8").read()
    for m in re.finditer(r'\[[^\]]*\]\(([^)#]+?)(#[^)]*)?\)', txt):
        t = m.group(1).strip()
        if not t or t.startswith(("http://", "https://", "mailto:")):
            continue
        if " " in t or "&" in t:      # 代码片段里的误报
            continue
        if not os.path.exists(os.path.normpath(os.path.join(base, t))):
            dead.append(f"{os.path.relpath(f, ROOT)} → {t}")
# ⚠️ 反引号里的路径也要查 —— 上一轮就是这里漏了：
#    `docs/08-official-implementations.md` 早就并走了，但它是写在
#    反引号里而不是 markdown 链接里，只看 []() 的检查扫不到。
for f in allmd:
    base = os.path.dirname(f)
    txt = open(f, encoding="utf-8").read()
    for m in re.finditer(r'`([\w./-]*docs/[\w./-]+\.md)`(?!\]\()', txt):
        t = m.group(1)
        # `docs/xxx.md` 这种写法一律按**仓库根**解析；
        # 带 ../ 的按文件所在目录解析。
        # 两种解析都试：先按文件所在目录，再按仓库根。
        # 文档里既写 `../../docs/x.md` 也写 `dev/04-x64-android/...`，
        # 只认一种会误报。
        cands = [os.path.normpath(os.path.join(base, t)),
                 os.path.normpath(os.path.join(ROOT, t))]
        if not any(os.path.exists(c) for c in cands):
            dead.append(f"{os.path.relpath(f, ROOT)} → `{t}`")
chk(not dead, f"{len(allmd)} 份文档没有断链（含反引号路径）", str(dead[:4]))

print("\n[12] 文档里的事实与运行中的服务一致")
# 这几条最容易腐烂：改了协议忘了改文档。全部文档一起查，
# 而不是只查 docs/api/ —— 上次就是 docs/02 的版本表停在了 v6。
t_all = ""
for f in allmd:
    t_all += open(f, encoding="utf-8").read()
pv = d.get("protocolVersion")
ncmd = len(cmds)
import collections
# ⚠️ 只揪「当前协议 vN」这种**断言**，不碰 `since` 字段里的历史版本号
#    （"v6 引入" 永远是对的，不该报错）。
claims = {int(x) for x in
          re.findall(r'(?:当前)?协议\s*\*{0,2}v(\d+)', t_all)}
chk(claims <= {pv}, f"文档里没有过期的「当前协议 vN」断言",
    f"出现 v{sorted(claims)}，实际 v{pv}")
chk(f"**v{pv}**" in t_all or f"协议 **v{pv}**" in t_all or f"协议 v{pv}" in t_all,
    f"文档里说明了当前协议是 v{pv}")

# 命令数：任何 "N 条命令" 都必须等于实际
nums = {int(x) for x in re.findall(r'(\d+)\s*条命令', t_all)}
chk(nums <= {ncmd}, f"文档里的命令数都是 {ncmd}", f"出现 {sorted(nums)}")

# 删掉的文档不该再被提到
chk("SUMMARY.md" not in t_all, "没有文档还在引用已删除的 SUMMARY.md")

print("\n[13] README 里自报的检查数没有过期")
# ⚠️ 这条是自指的：README 写「文档一致性 N 项」，N 必须等于实际跑出来的数。
#    不守的话它一定会烂 —— 每加一条断言，README 就错一次。
# ⚠️ 要**加上本节自己的两条断言**，否则算出来永远是改之前的数
#    （第一次就是这么错的：报 64 == 64，最后打印 66）。
_total = ok + bad + 2
_rm = open(os.path.join(ROOT, "README.md"), encoding="utf-8").read()
_m = re.search(r'文档一致性\s*\*\*(\d+)\s*项\*\*', _rm)
chk(_m is not None, "README 里写了文档一致性的检查数")
if _m:
    chk(int(_m.group(1)) == _total,
        f"README 报的 {_m.group(1)} 项 == 实际的 {_total} 项",
        "改了断言就要同步改 README")

print(f"\n{'='*44}\n  通过 {ok} 项，失败 {bad} 项\n")
sys.exit(1 if bad else 0)
