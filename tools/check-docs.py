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
    "dev/04-x64-android/packaging/START-HERE.md",
}
PACKAGE_LAYOUT_PREFIXES = ("bin/", "images/", "runtime/", "templates/", "tools/")
PACKAGE_LAYOUT_FILES = {"START-HERE.md", "RELEASE.json", "SHA256SUMS"}

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
        problems["编号断号"].append("%s: 缺 %s（现有 %s..%s）" % (
            rel(d), gaps, nums[0], nums[-1]))

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
