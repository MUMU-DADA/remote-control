#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""连续重复的赋值语句：同一个左值连着赋两次，几乎必然是 bug。

为什么要有它
============

2026-10-09 那个"确认提示弹两次"的 bug 就是这么来的：

    $ans = Read-LineWithTimeout
    $ans = Read-LineWithTimeout      ← 重复

我当时用「把 A 块替换成 B 行」的方式打补丁，而 **B 行本来就在**，
于是变成了两行。后果是交互确认会弹两次：第一次吃掉用户输入的 yes，
第二次读到 EOF → `Die "已取消"` —— 破坏性操作**永远走不下去**。

它躲过了所有测试，因为：
  · 非交互环境下**第一次**就 die 了，走不到第二次；
  · `test-windows-emulator.sh` 走 `-Yes`，压根不进确认分支。

**只有"真的喂一行输入"才暴露。** 所以不能靠测试兜，得靠静态检查兜。

只查什么
========

只查**赋值**，而且只查**紧挨着的两行左值和右值都完全相同**的情况：

    $x = 1
    $x = 1            ← 报（两行一模一样）

    $x = f "a"
    $x = f "b"        ← **不报**（左值同、右值不同 —— 重复读取是常见且正当的写法）

    echo hi
    echo hi           ← **不报**（重复命令可能是故意的，比如重试/占位）

这样误报率极低。第一版的经验（`check-verify-honesty.py`）：**吵闹的规则等于没有规则。**

识别三种语言里最常见的赋值写法：
    sh    : VAR=...           / VAR=$(...)
    ps1   : $var = ...
    py    : var = ...         （含 self.x = ...）
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 只看这些后缀；跳过大目录
EXTS = (".sh", ".ps1", ".py")
SKIP_DIRS = {"aosp", "vendor", ".git", ".tmp", ".run", "node_modules", "artifacts"}

# 白名单：(相对路径, 行号) -> 理由。行号漂了就重新确认一次，这是刻意的。
ALLOW = {}

PATTERNS = [
    # PowerShell: $x = ...   （排除 $x += / $x -= 等复合赋值）
    ("ps", re.compile(r"^\s*(\$[A-Za-z_][\w:.]*)\s*=(?!=)\s*(.*)$")),
    # sh: VAR=... / VAR=$(...)
    ("sh", re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)=(?!=)\s*(.*)$")),
    # python: x = ... / self.x = ...
    ("py", re.compile(r"^\s*((?:self\.)?[A-Za-z_][\w.]*)\s*=(?!=)\s*(.*)$")),
]


def classify(path: Path, line: str):
    """返回 (左值, 右值) 或 None。按后缀选语言，免得把 `x = 1` 这种
    在别的语言里也长得一样的东西误判 —— 宁可漏。"""
    suffix = path.suffix.lower()
    if suffix == ".ps1":
        kinds = ("ps",)
    elif suffix == ".sh":
        kinds = ("sh",)
    elif suffix == ".py":
        kinds = ("py",)
    else:
        return None
    for kind, pat in PATTERNS:
        if kind not in kinds:
            continue
        m = pat.match(line)
        if m:
            return m.group(1), m.group(2).strip()
    return None


def main() -> int:
    # 用 os.walk + **原地剪枝**，而不是 rglob：rglob 会走进 aosp/（几万个文件）
    # 和 .run/、artifacts/ 才在结果里过滤掉，慢到跑不完。
    # 这个检查要进 runner，必须是秒级的。
    import os
    files = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if fn.endswith(EXTS):
                files.append(Path(dirpath) / fn)
    files.sort()

    problems, checked = [], 0
    for f in files:
        try:
            lines = f.read_text(encoding="utf-8").splitlines()
        except (UnicodeDecodeError, OSError):
            continue
        prev = None          # (行号, 左值, 右值)
        for i, line in enumerate(lines, 1):
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                prev = None
                continue
            got = classify(f, line)
            if got is None:
                prev = None
                continue
            checked += 1
            # ⚠️ **左值和右值都要相同**才算。
            #    只比左值会一大堆误报（第一版就是这样，10 处里大半是假的）：
            #      · `$saved = Get-InstanceValue $N "SERVICE_ENABLED"` 连着四次
            #        —— 读的是四个不同的键，完全正常；
            #      · canvas 里 `g.fillStyle = '#f00'; …` 再 `= '#00f'; …`
            #        —— 换颜色，天经地义。
            #    真正那个 bug 的签名是**两行一模一样**：
            #      `$ans = Read-LineWithTimeout` / `$ans = Read-LineWithTimeout`
            if prev and prev[1] == got[0] and prev[2] == got[1]:
                rel = str(f.relative_to(ROOT))
                if (rel, i) not in ALLOW:
                    problems.append((rel, prev[0], i, got[0], got[1][:60]))
                prev = (i, got[0], got[1])
            else:
                prev = (i, got[0], got[1])

    print(f"连续重复赋值体检：{len(files)} 个脚本，{checked} 条赋值")
    if not problems:
        print("  ✓ 没有同一个左值连着赋两次的情况")
        return 0

    print(f"  ✗ {len(problems)} 处：")
    for rel, l1, l2, lhs, rhs in problems:
        print(f"      {rel}:{l1} 和 :{l2}   都是 `{lhs} = …`")
        print(f"        第二处右边：{rhs}")
    print()
    print("  这几乎总是打补丁时「替换成了本来就在的那一行」造成的。")
    print("  确认是故意的（极罕见）就写进本文件的 ALLOW 并说明理由。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
