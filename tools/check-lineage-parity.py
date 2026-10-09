#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""两条血统的动词表对照：**仓库侧** 与 **交付版** 的管理器必须只差已知的那几处。

为什么要有它
============

`release.sh` 里交付包的 `bin/` 是这么来的：

    cp -a "$PACKAGING_DIR/bin/$(target_platform "$plat")/." "$root/bin/"

也就是说 `packaging/bin/*/emulator.*` 是**用户真正拿到的**管理器，而
`scripts/emulator.sh` / `windows/emulator.ps1` 是**开发侧**的另一套。
两份是**手工维护的两套实现**（918 行 vs 451 行，内容完全不同）。

2026-10-09 这一轮会话里，"两条血统不一样"这个意外撞见了 **四** 次：

  · `-Force` 的作用域校验：交付版按动词查表，仓库侧没有；
  · `export` / `import`：只有仓库侧的 Linux 管理器有；
  · `inspect`：交付版 Windows 有，仓库侧 Windows 没有；
  · `Read-LineWithTimeout`：两份各写一份。

每次都要现场查一遍才知道哪个是对的。所以把它变成可查的：
**差异必须恰好等于下面记录在案的那几处，多出来一处就报错。**

不是要求两边完全一样 —— 它们**本来就不该一样**（交付版是精简过的，
不带"未通过验证"的 `import`）。要求的是：**不一样的地方必须是写下来的、
有理由的，而不是悄悄长出来的。**

只比"动词集合"。参数作用域、实现细节比不了 —— 那部分靠人读，不假装机器能查。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
X64 = ROOT / "dev" / "04-android-rom"

# 成对比较：(仓库侧, 交付版)
PAIRS = [
    ("scripts/emulator.sh", "packaging/bin/linux/emulator.sh"),
    ("windows/emulator.ps1", "packaging/bin/windows/emulator.ps1"),
]

# 已知且**有理由**的差异：{对: (只在仓库侧有的, 只在交付版有的)}
EXPECTED = {
    ("scripts/emulator.sh", "packaging/bin/linux/emulator.sh"): (
        {"export", "import"},        # 交付版不带：import 未通过验证，见 docs/12 §7
        {"verify"},                  # 交付包自带验收；开发侧走 run-linux.sh --verify
    ),
    ("windows/emulator.ps1", "packaging/bin/windows/emulator.ps1"): (
        set(),                       # 仓库侧没有独有的动词
        {"inspect", "verify"},       # inspect 读归档（归档由 Linux 侧 export 产生）；verify 同上
    ),
}


def sh_verbs(path: Path):
    t = path.read_text(encoding="utf-8")
    out = set()
    for m in re.finditer(r"^\s{4}([a-z|]+)\)", t, re.M):
        out |= set(m.group(1).split("|"))
    return {v for v in out if len(v) > 2}


def ps_verbs(path: Path):
    t = path.read_text(encoding="utf-8")
    out = set(re.findall(r'^\s{4}"([a-z]+)"\s*=\s*@', t, re.M))     # 交付版：允许参数表
    out |= set(re.findall(r'^\s{4}"([a-z]+)"\s*\{', t, re.M))       # 仓库侧：switch 分支
    out |= set(re.findall(r'\$command -eq "([a-z]+)"', t))
    return out


def verbs(path: Path):
    return ps_verbs(path) if path.suffix.lower() == ".ps1" else sh_verbs(path)


def main() -> int:
    problems = []
    print(f"血统对照：{len(PAIRS)} 对管理器")
    for dev_rel, pkg_rel in PAIRS:
        dev, pkg = X64 / dev_rel, X64 / pkg_rel
        for p in (dev, pkg):
            if not p.is_file():
                problems.append(f"找不到 {p.relative_to(ROOT)}")
        if problems:
            continue
        dv, pv = verbs(dev), verbs(pkg)
        only_dev, only_pkg = dv - pv, pv - dv
        exp_dev, exp_pkg = EXPECTED.get((dev_rel, pkg_rel), (set(), set()))
        print(f"  {dev_rel}")
        print(f"    vs {pkg_rel}")
        print(f"    仓库侧 {len(dv)} 个 / 交付版 {len(pv)} 个")

        extra_dev, extra_pkg = only_dev - exp_dev, only_pkg - exp_pkg
        missing_dev, missing_pkg = exp_dev - only_dev, exp_pkg - only_pkg
        if extra_dev:
            problems.append(f"{dev_rel}: 仓库侧多出未记录的动词 {sorted(extra_dev)}")
        if extra_pkg:
            problems.append(f"{pkg_rel}: 交付版多出未记录的动词 {sorted(extra_pkg)}")
        if missing_dev or missing_pkg:
            problems.append(
                "记录在案的差异已经不存在了（要么改回去了，要么记录过期）："
                f" 仓库侧独有 {sorted(missing_dev)}、交付版独有 {sorted(missing_pkg)}")
        if not (extra_dev or extra_pkg or missing_dev or missing_pkg):
            print(f"    ✓ 差异恰好是记录在案的那几处："
                  f"仓库侧独有 {sorted(only_dev) or '（无）'} / 交付版独有 {sorted(only_pkg) or '（无）'}")

    if problems:
        print()
        print("  ✗ 对不上：")
        for p in problems:
            print(f"      {p}")
        print()
        print("  两条血统是**手工维护的两套实现**，差异会悄悄长出来。")
        print("  确实是有意的差异，就写进本文件的 EXPECTED 并说明理由；")
        print("  不是有意的，就去把它补齐。")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
