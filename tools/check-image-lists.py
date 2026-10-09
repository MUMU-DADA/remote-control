#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""镜像清单对照：**生产端产出什么**，**消费端认为必需什么**，必须对得上。

为什么要有它
============

"哪些文件构成一份 ROM"这件事，在仓库里写在**三个地方**：

    生产端   scripts/package-rom.sh          REQUIRED / OPTIONAL
    消费端   windows/fetch-images.ps1        $required / $optional
    消费端   macos/fetch-images.sh           REQUIRED / OPTIONAL

2026-10-09 对照过一遍，**目前是一致的**。但它们是各写一份 ——
哪天生产端加了一个文件而某个消费端没跟上，表现是**那个平台悄悄少了镜像**：
下载脚本"成功"、校验"通过"，直到模拟器起不来才发现。这类分叉不会自己报错。

所以：**不要求它们一样**（它们本来就该一样），要求的是**对得上** ——
对不上就报，并指出是哪一端多了/少了。

比什么
======

比**集合**，不比顺序。抽的时候要先去 shell 的续行符（`\\` 换行）——
第一版没处理，结果抽出来一个孤零零的 `\\`，差点当成真差异报出去。

比不了什么
==========

子目录里的文件（`system/build.prop` 等）两端都是**按 `SHA256SUMS` 清单补齐**的
（见 `windows/fetch-images.ps1` 里那段注释），那份清单是构建时生成的，
不在这里比。本工具只管**顶层那份硬编码列表**。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
X64 = ROOT / "dev" / "04-android-rom"

PRODUCER = "scripts/package-rom.sh"
CONSUMERS = [
    ("windows/fetch-images.ps1", "ps"),
    ("macos/fetch-images.sh", "sh"),
]

# 这几项由打包脚本自己生成，不属于"镜像"，两端都不列，比较时忽略
IGNORE = {"SHA256SUMS", "MANIFEST.txt", "initrd", "source.properties"}


def _norm(items):
    return {i for i in items if i and i != "\\" and i not in IGNORE}


def sh_var(path: Path, name: str):
    """取 `NAME="a b c"` 或 `NAME='a b c'`，先去续行符。"""
    t = path.read_text(encoding="utf-8").replace("\\\n", " ")
    m = re.search(name + r'="([^"]*)"', t) or re.search(name + r"='([^']*)'", t)
    return _norm(m.group(1).split()) if m else None


def get(path: Path, name: str, kind: str):
    if kind == "ps":
        t = path.read_text(encoding="utf-8").replace("`\n", " ")
        m = re.search(r"\$" + name + r"\s*=\s*@\(([^)]*)\)", t, re.S)
        return _norm(re.findall(r'"([^"]+)"', m.group(1))) if m else None
    return sh_var(path, name)


def main() -> int:
    problems = []
    prod_req = sh_var(X64 / PRODUCER, "REQUIRED")
    prod_opt = sh_var(X64 / PRODUCER, "OPTIONAL")
    if prod_req is None:
        print(f"[x] 从 {PRODUCER} 抽不到 REQUIRED —— 变量名改了？")
        return 2
    print(f"镜  像清单对照：生产端 {PRODUCER}")
    print(f"  REQUIRED {len(prod_req)} 个：{' '.join(sorted(prod_req))}")
    print(f"  OPTIONAL {len(prod_opt or [])} 个：{' '.join(sorted(prod_opt or []))}")

    for rel, kind in CONSUMERS:
        path = X64 / rel
        if not path.is_file():
            problems.append(f"找不到 {rel}")
            continue
        req = get(path, "required" if kind == "ps" else "REQUIRED", kind)
        opt = get(path, "optional" if kind == "ps" else "OPTIONAL", kind)
        print(f"\n  消费端 {rel}")
        if req is None:
            problems.append(f"{rel}: 抽不到必需清单 —— 变量名改了？")
            continue
        print(f"    必需 {len(req)} 个")
        for label, a, b in (("必需", prod_req, req), ("可选", prod_opt or set(), opt or set())):
            only_prod, only_cons = a - b, b - a
            if only_prod:
                problems.append(f"{rel}: 生产端有而这里**没有**的{label}文件 {sorted(only_prod)}")
            if only_cons:
                problems.append(f"{rel}: 这里有而生产端**不产出**的{label}文件 {sorted(only_cons)}")
            if not only_prod and not only_cons:
                print(f"    ✓ {label}清单与生产端一致")

    if problems:
        print()
        print("  ✗ 对不上：")
        for p in problems:
            print(f"      {p}")
        print()
        print("  这种分叉的表现是**某个平台悄悄少了镜像**：下载脚本'成功'、校验'通过'，")
        print("  直到模拟器起不来才发现。去把两边对齐；确实是刻意的差异就写进本文件的 IGNORE")
        print("  并说明理由。")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
