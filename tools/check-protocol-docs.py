#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""协议命令清单对照：`protocol.h` 里有的 token，文档里必须写得到。

为什么要有它
============

"有哪些命令"这件事写在**三个地方**：

    真源      dev/02-native-daemon/daemon/protocol.h   enum Token（40 个）+ kErr*（13 个）
    分发      daemon/rest_api.cpp                      名字 → Cmd:: 的映射
    文档      docs/api/*.md + README                  客户端作者读的就是这份

**错误码**是这条链子的另一半（2026-10-09 扩进来的）：客户端靠 `0x1009` 这样的码
分支处理。这里查两件事 —— 每个码文档里要有，而且**必须真的会被返回**：
定义了却从不返回的码，会让客户端为一件不会发生的事写分支。

2026-10-09 逐个对照过：REST 层覆盖 39/40（缺的那个是 `Token`，**socket-only**，
v11 的既定设计），文档里 40 个**一个不缺**。三份目前是同步的。

但它们是各写一份。哪天往 enum 里加一个命令而忘了写文档，表现是
**客户端作者永远不知道这个命令存在** —— 没有任何东西会报错。
所以把它变成会报错的东西。

松紧程度（说清楚，免得被当成强保证）
====================================

这个检查只确认"文档里**出现过**这个名字"（`CamelCase` 或 `snake_case` 任一种）。
它**不**验证文档是否正确描述了语义、参数、返回 —— 那是 `check-api-docs.py`
的活（它拿**运行中的服务**比对，需要一台起着的实例）。

**它守的是"漏了整整一个命令"，不是"文档写得对不对"。**

比不了什么
==========

`rest_api.cpp` 那侧不用比：它引用的 `Cmd::X` 写错了**编译器就会挡**。
工具只查"enum 有、文档没有"这个方向。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROTOCOL = ROOT / "dev" / "02-native-daemon" / "daemon" / "protocol.h"
DOC_GLOBS = ["docs/api/*.md"]
DOC_FILES = ["README.md", "dev/02-native-daemon/README.md"]

# 刻意不写进 API 文档的（要有理由）。
# 今天为空 —— `Token` 虽然 socket-only，但 docs/api/03-socket.md 里写了。谁要豁免谁写进来。
ALLOW = {}


def snake(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


def main() -> int:
    if not PROTOCOL.is_file():
        print(f"[x] 找不到 {PROTOCOL.relative_to(ROOT)}")
        return 2
    src = PROTOCOL.read_text(encoding="utf-8")
    tokens = re.findall(r"^\s*([A-Z][A-Za-z0-9]*)\s*=\s*(\d+),", src, re.M)
    if len(tokens) < 10:
        print(f"[x] 只从 protocol.h 抽出 {len(tokens)} 个 token —— 枚举的写法变了？")
        return 2

    docs = []
    for pattern in DOC_GLOBS:
        docs += sorted(ROOT.glob(pattern))
    docs += [ROOT / f for f in DOC_FILES]
    docs = [d for d in docs if d.is_file()]
    if not docs:
        print("[x] 一个文档都没找到 —— 路径不对？")
        return 2
    text = "\n".join(d.read_text(encoding="utf-8") for d in docs)
    low = text.lower()

    missing = []      # 文档里找不到的（命令 token 和错误码共用）
    dead = []         # 定义了但 daemon 从不返回的错误码
    for name, _ in tokens:
        if name in ALLOW:
            continue
        if name.lower() in low or snake(name) in low:
            continue
        missing.append((name, snake(name)))

    # ── 错误码：同一条链子的另一半 ──────────────────────────────────────
    # 客户端要靠码来分支处理（`0x1009` = 找不到）。加了码却忘了写文档 →
    # 客户端不知道怎么处理；写了却**从不返回** → 客户端为一件不会发生的事写分支。
    # 两件都报。
    errs = re.findall(r"(kErr[A-Za-z]+)\s*=\s*(0x[0-9A-Fa-f]+)", src)
    src_all = "\n".join(
        f.read_text(encoding="utf-8", errors="replace")
        for f in Path(__file__).resolve().parent.parent.joinpath(
            "dev", "02-native-daemon", "daemon").rglob("*")
        if f.suffix in (".cpp", ".h"))
    for name, val in errs:
        if val.lower() not in low and str(int(val, 16)) not in text:
            missing.append((name, val))
        uses = len(re.findall(r"\b" + name + r"\b", src_all)) - 1   # 减去定义那一行
        if uses <= 0:
            dead.append((name, val))

    print(f"协议清单对照：protocol.h 里 {len(tokens)} 个 token + {len(errs)} 个错误码，"
          f"对照 {len(docs)} 个文档")
    if not missing and not dead:
        print(f"  ✓ 每个 token（{len(tokens)} 个）和错误码（{len(errs)} 个）都在文档里，"
              f"且每个错误码都真的会被返回")
        return 0
    print(f"  ✗ {len(missing)} 个条目在文档里找不到 —— 客户端作者不会知道它们存在：")
    for name, s in missing:
        hint = f"（文档里找的是 `{name}` 或 `{s}`）" if not s.startswith("0x") else f"（找的是 `{s}` 或十进制 {int(s, 16)}）"
        print(f"      {name:<18}{hint}")
    if dead:
        print(f"  ✗ {len(dead)} 个错误码**定义了却从不返回** —— 客户端会为不会发生的事写分支：")
        for name, val in dead:
            print(f"      {val}  {name}")
    print()
    print("  缺文档的：去 docs/api/ 把它写上；确实不该公开的（比如 socket-only 的 Token），")
    print("            写进本文件的 ALLOW 并说明理由。")
    print("  从不返回的：要么是死码（删掉），要么是某条分支漏了返回它（补上）。")
    print("            客户端会为一件不会发生的事写分支 —— 那和缺文档一样是坑。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
