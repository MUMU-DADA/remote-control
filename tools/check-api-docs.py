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
chk(d.get("protocolVersion") == 6, "protocolVersion = 6", f"实际 {d.get('protocolVersion')}")
chk(len(cmds) == 32, "命令 32 条", f"实际 {len(cmds)}")
# README 里写"32 条命令"
for f in ("README.md","03-socket.md","01-http.md"):
    t = open(os.path.join(DOCS,f),encoding='utf-8').read()
    nums = set(re.findall(r'(\d+)\s*条命令', t))
    if nums:
        chk(nums == {"32"}, f"{f} 里写的命令数与实际一致", f"文档写 {nums}")

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
            "foreground","install","download","files"]:
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

print("\n[8] 编码器逐格式上报（01-http.md）")
be = pm.get("codecs", {}).get("backend", "")
chk("jpeg=" in be and "webp=" in be and "png=" in be,
    "codecs.backend 逐格式上报", f"实际 {be!r}")

print(f"\n{'='*44}\n  通过 {ok} 项，失败 {bad} 项\n")
sys.exit(1 if bad else 0)
