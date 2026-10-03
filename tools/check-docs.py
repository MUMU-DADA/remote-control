#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文档体检：链接、索引覆盖、编号连续、标题重复。

    python3 tools/check-docs.py            # 只报告
    python3 tools/check-docs.py --strict   # 有问题就非 0 退出（给 CI/提交前用）

查六件事：
  [1] 相对链接是否指得到（含锚点语法；不校验锚点是否存在，只校验文件）
  [2] 索引覆盖：docs/ 下每个子目录、每个编号文档，是否被某个索引提到
  [3] 编号连续性：01,02,03… 有没有断号/重号
  [4] 标题重复：不同文档用了完全相同的 H1（往往是复制粘贴留下的）
  [5] 孤儿文档：没有任何其它文档链接到它，且不在索引里
  [6] 明显的陈旧引用：指向已改名的路径（这类最坑，读的人会去找一个不存在的东西）
  [7] 文档里让你运行的脚本路径是否还在（"照着做 → no such file" 是最坑的一种陈旧）
  [8] markdown 表格行是否被换行截断（本次整理时我自己犯过一次，而当时所有检查都是绿的）

排除：vendor/（第三方自带）、aosp/（AOSP 源码树）、.run/ .tmp/（构建产物）、
     .git/、以及 node_modules 之类。
"""
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SKIP_DIRS = {".git", "aosp", "vendor", ".run", ".tmp", ".dsh-tools",
             "node_modules", "lost+found", "out", ".repo"}
# 已被改名/移除、不该再被引用的路径。
# 第三项是**新的名字**：同一行里如果已经出现了新名字，说明这行是在讲"改名这件事"本身
# （例如文档里的历史记录），不算陈旧引用 —— 不加这条就会天天误报。
STALE_PATTERNS = [
    ("packaging/bin/macos/", "已改名为 packaging/bin/darwin/", "packaging/bin/darwin/"),
]

# 渲染进发布包的模板：里面的相对链接是**包内路径**，要按包布局校验，不能按仓库。
# 包布局由 scripts/release.sh 的 stage_release() 决定。
PACKAGE_TEMPLATES = {
    "dev/04-android-rom/packaging/START-HERE.md",
}
PACKAGE_LAYOUT_PREFIXES = ("bin/", "images/", "runtime/", "templates/", "tools/")
PACKAGE_LAYOUT_FILES = {"START-HERE.md", "RELEASE.json", "SHA256SUMS"}

# 文档里出现的这些路径是**发布包内**的（或客户端的），不在本仓库里，不该报缺失。
PKG_ONLY_PREFIXES = ("./bin/", "bin/", "./runtime/", "runtime/", "./images/",
                     "/data/local/tmp/", "/system/", "/sdcard/", "/vendor/", "/dev/")

# 形如： ./scripts/x.sh   ../scripts/x.sh   scripts/x.sh   tools/x   bash scripts/x.sh
SCRIPT_REF_RE = re.compile(
    r"(?:\.\./)*(?:scripts|tools|dev/[0-9a-z-]+/scripts|dev/[0-9a-z-]+/tools)"
    r"/[A-Za-z0-9_.-]+\.(?:sh|py|ps1)")

LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
H1_RE = re.compile(r"^#\s+(.+?)\s*$", re.M)

problems = defaultdict(list)
stats = {"files": 0, "links": 0, "broken": 0}


def walk_md():
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in sorted(filenames):
            if fn.endswith(".md"):
                yield os.path.join(dirpath, fn)


def rel(p):
    return os.path.relpath(p, ROOT).replace(os.sep, "/")


files = list(walk_md())
stats["files"] = len(files)

# --------------------------------------------------------------------------
# [1] 链接
# --------------------------------------------------------------------------
inbound = defaultdict(set)      # 被指向的文件 → 指向它的文件集合
for f in files:
    try:
        text = open(f, encoding="utf-8").read()
    except UnicodeDecodeError:
        problems["读不了（非 utf-8）"].append(rel(f))
        continue
    for m in LINK_RE.finditer(text):
        target = m.group(1)
        if target.startswith(("http://", "https://", "mailto:", "#", "tel:")):
            continue
        stats["links"] += 1
        path_part = target.split("#", 1)[0]
        if not path_part:
            continue
        # 模板文件：链接是**包内路径**，按发布包布局校验
        if rel(f) in PACKAGE_TEMPLATES:
            if path_part.startswith(PACKAGE_LAYOUT_PREFIXES) or path_part in PACKAGE_LAYOUT_FILES:
                stats["links"] -= 1          # 不计入仓库链接统计
                continue
            problems["模板里的链接既不在包布局里、也不是仓库路径"].append(
                "%s → %s" % (rel(f), target))
            continue
        # 按文件所在目录解析（Markdown 的语义）
        cand = os.path.normpath(os.path.join(os.path.dirname(f), path_part))
        # 也试一下按仓库根解析 —— 有些文档习惯写仓库根相对路径
        cand_root = os.path.normpath(os.path.join(ROOT, path_part))
        if os.path.exists(cand):
            if os.path.isfile(cand):
                inbound[cand].add(f)
        elif os.path.exists(cand_root):
            if os.path.isfile(cand_root):
                inbound[cand_root].add(f)
            problems["链接按仓库根才解析得到（建议写相对当前文件的路径）"].append(
                "%s → %s" % (rel(f), target))
        else:
            stats["broken"] += 1
            problems["链接指不到"].append("%s:%d → %s" % (
                rel(f), text[:m.start()].count("\n") + 1, target))
        # [6] 陈旧引用（链接目标这一侧）
        for bad, why, newname in STALE_PATTERNS:
            if bad in target:
                problems["陈旧引用（%s）" % why].append("%s → %s" % (rel(f), target))

# [6] 正文里的陈旧路径（含代码块与行内代码）
for f in files:
    text = open(f, encoding="utf-8").read()
    for bad, why, newname in STALE_PATTERNS:
        for line_no, line in enumerate(text.splitlines(), 1):
            if bad in line and newname not in line:
                problems["陈旧引用（正文，%s）" % why].append(
                    "%s:%d %s" % (rel(f), line_no, line.strip()[:100]))

# --------------------------------------------------------------------------
# [2] 索引覆盖：每个 .md（除索引自身与顶层约定文件）都应被某个索引提到
# --------------------------------------------------------------------------
INDEX_NAMES = {"README.md", "index.md", "PLAN.md", "START-HERE.md"}


def covered_by_index(f):
    """该文件是否被同目录或上层的索引链接到。"""
    p = os.path.dirname(f)
    while p.startswith(ROOT):
        for name in INDEX_NAMES:
            idx = os.path.join(p, name)
            if os.path.isfile(idx) and f in inbound:
                if idx in inbound[f]:
                    return True
        if p == ROOT:
            break
        p = os.path.dirname(p)
    # 被任何文档链接也算"可达"
    return bool(inbound.get(f))


for f in files:
    if os.path.basename(f) in INDEX_NAMES:
        continue
    if "/vendor/" in rel(f) or rel(f).startswith("dev/02-native-daemon/daemon/vendor/"):
        continue
    if not covered_by_index(f):
        problems["孤儿文档（没有任何文档链接到它）"].append(rel(f))

# --------------------------------------------------------------------------
# [3] 编号连续性（针对 00-xx-name.md 这种命名）
# --------------------------------------------------------------------------
NUM_RE = re.compile(r"^(\d{2})-(.+)$")
by_dir = defaultdict(list)
for f in files:
    b = os.path.basename(f)
    m = NUM_RE.match(b)
    if m:
        by_dir[os.path.dirname(f)].append((int(m.group(1)), b))

for d, items in sorted(by_dir.items()):
    items.sort()
    nums = [n for n, _ in items]
    dup = {n for n in nums if nums.count(n) > 1}
    if dup:
        problems["编号重复"].append("%s: %s" % (rel(d), sorted(dup)))
    gaps = [n for n in range(nums[0], nums[-1] + 1) if n not in nums]
    if gaps:
        # 把连续区间收成 `16–98`；还是太长就只列头几个。
        # （反向测试时插了个 99 号文件，原来的写法一口气列出 83 个数字 ——
        #   报得没错，但没人看得下去的消息等于没报。）
        runs, start, prev = [], gaps[0], gaps[0]
        for n in gaps[1:]:
            if n == prev + 1:
                prev = n
                continue
            runs.append((start, prev)); start = prev = n
        runs.append((start, prev))
        parts = ["%02d" % a if a == b else "%02d–%02d" % (a, b) for a, b in runs]
        shown = "、".join(parts[:6]) + ("…" if len(parts) > 6 else "")
        problems["编号断号"].append("%s: 缺 %s（共 %d 个号；现有 %02d..%02d）" % (
            rel(d), shown, len(gaps), nums[0], nums[-1]))

# --------------------------------------------------------------------------
# [4] 标题重复
# --------------------------------------------------------------------------
titles = defaultdict(list)
for f in files:
    if "/vendor/" in rel(f):
        continue
    text = open(f, encoding="utf-8").read()
    m = H1_RE.search(text)
    if m:
        titles[m.group(1)].append(rel(f))
for t, fs in sorted(titles.items()):
    if len(fs) > 1:
        problems["H1 完全相同（可能是复制粘贴留下的）"].append(
            "%r ← %s" % (t, ", ".join(fs)))

# --------------------------------------------------------------------------
# [7] 文档里让你运行的脚本路径是否还在
# --------------------------------------------------------------------------
CODE_BLOCK_RE = re.compile(r"```.*?```", re.S)
for f in files:
    text = open(f, encoding="utf-8").read()
    # (引用, 基准目录) —— 基准目录来自块内的 `cd <dir>`
    refs = []
    for block in CODE_BLOCK_RE.findall(text):
        cwd = None                      # 块内的当前目录（被 `cd` 改）
        for line in block.splitlines():
            stripped = line.strip()
            m_cd = re.match(r"^cd\s+(\S+)\s*$", stripped)
            if m_cd:
                arg = m_cd.group(1)
                # 只跟相对路径；`cd $VAR` / `cd -` 之类不跟（跟了反而会误判）
                if not arg.startswith(("/", "$", "-")) and " " not in arg:
                    cwd = arg if cwd is None else os.path.normpath(os.path.join(cwd, arg))
                continue
            for r in SCRIPT_REF_RE.findall(line):
                refs.append((r, cwd))
    # 行内代码也算（`bash tools/xxx.sh`）
    # 按人的读法逐级向上找：文档目录 → 各级祖先 → 仓库根。
    # 本项目里 docs/ 与 scripts/ 是兄弟目录，文档写 `scripts/x.sh` 指的是**模块根**下的那个。
    # 只试"文档目录"和"仓库根"会把这类全误报（第一次跑 34 条里绝大多数是这么来的）。
    def resolvable(doc, ref, cwd=None):
        d = os.path.dirname(doc)
        # 块内 `cd` 过的话，先按那个目录试（这是最贴近读者理解的基准）
        if cwd:
            if os.path.exists(os.path.normpath(os.path.join(d, cwd, ref))):
                return True
            if os.path.exists(os.path.normpath(os.path.join(ROOT, cwd, ref))):
                return True
        while True:
            if os.path.exists(os.path.normpath(os.path.join(d, ref))):
                return True
            if os.path.normpath(d) == os.path.normpath(ROOT):
                break
            parent = os.path.dirname(d)
            if parent == d:
                break
            d = parent
        return os.path.exists(os.path.normpath(os.path.join(ROOT, ref)))

    seen = set()
    for r, cwd in refs:
        if r.startswith(PKG_ONLY_PREFIXES) or (r, cwd) in seen:
            continue
        seen.add((r, cwd))
        if resolvable(f, r, cwd):
            continue
        problems["文档里的脚本路径不存在（照着做会 no such file）"].append(
            "%s → %s%s" % (rel(f), r, ("（块内 cd %s 之后仍找不到）" % cwd) if cwd else ""))

# --------------------------------------------------------------------------
# [8] markdown 表格行是否被换行截断
#
# 本次整理时我自己犯过一次：往表格单元格里塞了个换行，那行被劈成两半、表格当场断掉，
# 而所有自动检查都是绿的（链接没断、索引没漏、编号没乱）。
# 规则：以 `|` 开头的行必须也以 `|` 结尾（允许行尾空格）—— 截断的行几乎必然违反它。
# --------------------------------------------------------------------------
for f in files:
    if "/vendor/" in rel(f):
        continue
    for line_no, line in enumerate(open(f, encoding="utf-8").read().splitlines(), 1):
        st = line.strip()
        if not st.startswith("|"):
            continue
        if not st.endswith("|"):
            problems["表格行被换行截断（不以 | 收尾）"].append(
                "%s:%d %s" % (rel(f), line_no, st[:70]))

# --------------------------------------------------------------------------
# 报告
# --------------------------------------------------------------------------
print("文档体检：%d 个 .md，%d 条相对链接，其中 %d 条指不到" % (
    stats["files"], stats["links"], stats["broken"]))
print()

total = 0
for kind in sorted(problems):
    items = problems[kind]
    total += len(items)
    print("── %s（%d）" % (kind, len(items)))
    for it in items[:40]:
        print("   %s" % it)
    if len(items) > 40:
        print("   … 还有 %d 条" % (len(items) - 40))
    print()

if total == 0:
    print("✅ 没有问题")
else:
    print("共 %d 个问题" % total)

if "--strict" in sys.argv and total:
    sys.exit(1)
