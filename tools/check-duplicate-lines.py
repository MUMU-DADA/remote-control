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


# ── 规则二：环境相关的绝对路径只许有一份 ─────────────────────────────
#
# 2026-10-09 实测：`webui-test/` 下 **10 个脚本各写一份** Chromium 路径，而且
# 两种写法混着 —— 4 个用 `os.environ.get("CHROME", "<默认>")`（可覆盖，对的做法），
# 7 个直接把全路径塞进 `launch(executable_path=...)`。
#
# 这正是"同一句话抄到 N 处、只改了一处"：playwright 换了 Chromium 版本之后，
# 改 4 个、忘 7 个 —— 表现是**一部分脚本能跑、一部分起不来**，
# 比"全都起不来"更难查，因为它看起来像已经修好了。
#
# 为什么不写成通用的"同目录内重复的绝对路径"：试过了，全仓命中 4 处，
# 全是正当的（`/api/v1/describe` 这类 API 字符串、AOSP 的 `/aosp/out/soong_ui`）。
# 要压成能用的规则就得加白名单，而白名单会随着新测试不断变长。
# 所以只写这一条**确定性**的：作用域明确、零误报、且正的这次踩的坑。
PATH_RULES = [
    # (目录, 允许持有该字面量的文件, 禁令里的字面量, 说明)
    ("dev/02-native-daemon/tools/webui-test", {"wctest_chrome.py"}, "ms-playwright",
     "Chromium 路径只能由 wctest_chrome.py 持有，别的脚本一律 `from wctest_chrome import CHROME`"),
]


def check_path_rules(problems) -> int:
    checked = 0
    for rel_dir, allowed, needle, why in PATH_RULES:
        d = ROOT / rel_dir
        if not d.is_dir():
            continue
        for f in sorted(list(d.glob("*.py")) + list(d.glob("*.sh")) + list(d.glob("*.ps1"))):
            checked += 1
            if f.name in allowed:
                continue
            if needle in f.read_text(encoding="utf-8", errors="replace"):
                problems.append(
                    f"{f.relative_to(ROOT)} 里出现了 `{needle}` —— {why}")
    return checked


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
                    # ⚠️ 统一用**字符串**（原来这里是 5 元组，加第二条规则时
                    #    两处都往同一个列表里塞，报告循环按元组解包 → ValueError。
                    #    是反向测试逮到的：规则命中了，却在**打印之前崩掉**。）
                    problems.append(
                        f"{rel}:{prev[0]} 和 :{i} 都是 `{got[0]} = …`\n"
                        f"        第二处右边：{got[1][:60]}")
                prev = (i, got[0], got[1])
            else:
                prev = (i, got[0], got[1])

    n_path = check_path_rules(problems)
    print(f"连续重复赋值体检：{len(files)} 个脚本，{checked} 条赋值"
          f"；另有 {n_path} 个脚本受「环境相关路径只许一份」规则约束")
    if not problems:
        print("  ✓ 没有同一个左值连着赋两次的情况，环境相关的路径也只有一份")
        return 0

    print(f"  ✗ {len(problems)} 处：")
    for item in problems:
        print(f"      {item}")
    print()
    print("  重复赋值：几乎总是打补丁时「替换成了本来就在的那一行」造成的。")
    print("  重复路径：把它收进一个共享模块，别的文件引用它。")
    print("  两者都确实有理由时，写进本文件的 ALLOW / PATH_RULES 并说明。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
