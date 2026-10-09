#!/usr/bin/env python3
"""
verify-*.sh 的"诚实性"体检：**前提不成立时，不许照样下结论。**

为什么要有这个工具
==================

2026-10-09 拆 `dev/04-android-rom/tools/` 下那 5 个 `verify-*.sh` 时，
5 个里有 **4 个**会在前提不成立时输出一个**看起来很有意义**的结论：

  脚本                          前提崩掉时它会说
  ----------------------------  ------------------------------------------------
  verify-clone-independent.sh   "✗ 两台机器共用了同一份 /data"（最吓人的那个）
  verify-data-persistence.sh    "结论：数据没持久住 —— 复现了未解之谜"
  verify-partition-resize.sh    "结论：镜像没被改小（项目注释的说法成立）" ← 反而
                                 **确认**了它本该检验的假设
  verify-export-import.sh       在验一个产品**已经主动禁掉**的功能

共同形状是同一个：**setup 命令的失败被吞掉了**，于是后面拿到的空值/错误文本
被当成数据，走进了某个结论分支。

规则
====

在 verify-*.sh 里，**会改变状态的命令**（create / start / stop / clone /
reset / import / export）**必须是"被检查过的"**。算作被检查的有四种：

  1. 同一行有 `|| die` / `|| true` / `&&` / 在 `if` 条件里
  2. 它是某个包装函数的**参数**（`chk_out … $EMU export …` —— 包装函数自己就在比对结果）
  3. 上一行是跨行的 `if …; then` / `while …` 条件
  4. **紧接着两行内**有对结果变量的判空（`[ -n "$PORT" ] || die …`）——
     create 失败最典型的后果就是端口取空，挡住它就够了

不算：`delete`（在这些脚本里基本都是清残留，失败不影响结论）、
只读的 `list`/`status`/`inspect`、以及注释和**引号字符串里**出现的文字
（第一版就是被 `echo "… emulator.sh stop …"` 这种骗了）。

⚠️ 第一版写得太宽，26 处里报了 14 处、其中约 10 处是误报。
**吵闹的规则等于没有规则** —— 所以上面这四条是收紧出来的，宁可漏也不误报。

白名单
======

确实要吞掉退出码的地方写进 `ALLOW`，并且必须带一行说明理由 ——
这个仓库的惯例是"豁免要写清为什么"，不是悄悄放过。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "dev" / "04-android-rom" / "tools"

MUTATING = ("create", "start", "stop", "clone", "reset", "import", "export")

ALLOW = {
    # (文件名, 行号): 理由。行号会漂，漂了就重新确认一次 —— 这是刻意的。
    ("verify-partition-resize.sh", 0): "占位：见下面 ALLOW_TEXT",
}

# 用"内容特征"而不是行号来豁免，行号一改就失效太脆
ALLOW_TEXT = [
    # 第二次启动失败**本身就是结论的一部分**，不能 die（脚本里写清了）
    ("verify-partition-resize.sh", r"\$EMU start t1 >/dev/null 2>&1$",
     "这一次'起不来'是结论的一部分，脚本下面按 rc 分开说明"),
    # 探索型脚本：只打印原始读数，不下自动结论
    ("verify-kill-is-hard-poweroff.sh", r"timeout 180 \$EMU start t3",
     "探索型脚本，只打印 A/B/C 原始读数，不下结论"),
    ("verify-kill-is-hard-poweroff.sh", r"\$EMU stop t3 >/dev/null 2>&1",
     "同上：exploratory，失败只让读数难看，不会伪造结论"),
]

CALL = re.compile(
    r"(?:\$EMU|\$\{EMU\}|emulator\.sh|EMU=)\s+(?:" + "|".join(MUTATING) + r")\b"
)
# 包装函数：调用是它的参数，它在内部比对结果
WRAPPER = re.compile(r"\b(chk_out|chk|assert_\w+)\b")
GUARDED = re.compile(r"\|\||&&|\bthen\b|^\s*if\b|^\s*while\b")
STRINGS = re.compile(r'"[^"]*"|\'[^\']*\'')
PORT_GUARD = re.compile(
    r'\[ -n "\$(PORT|NEWPORT|\w*PORT\w*)" \]'
    r'|grep -q'   # 对实例状态做断言也算（… list | awk … | grep -q 已停止）
)


def strip_strings(line: str) -> str:
    """把引号里的内容抹掉 —— 免得 `echo "… emulator.sh stop …"` 被当成调用。"""
    return STRINGS.sub('""', line)


def main() -> int:
    if not TOOLS.is_dir():
        print(f"[x] 找不到 {TOOLS}")
        return 2
    files = sorted(TOOLS.glob("verify-*.sh"))
    if not files:
        print("[x] 一个 verify-*.sh 都没找到 —— 路径不对？")
        return 2

    problems, checked, allowed = [], 0, 0
    for f in files:
        raw = f.read_text(encoding="utf-8").splitlines()
        for i, line in enumerate(raw, 1):
            code = strip_strings(line)
            if line.strip().startswith("#") or not CALL.search(code):
                continue
            checked += 1
            if GUARDED.search(code) or WRAPPER.search(code):
                continue
            # 跨行 if / while 条件
            prev = strip_strings(raw[i - 2]) if i >= 2 else ""
            if re.search(r"\b(if|while)\b.*$", prev) and not prev.rstrip().endswith(";"):
                continue
            if prev.rstrip().endswith("; then"):
                continue
            # 紧接着两行内有对结果变量的判空
            # ⚠️ 这里必须在**原文**上找，不能找 strip_strings 之后的 ——
            #    剥离会把 `[ -n "$PORT" ]` 变成 `[ -n "" ]`，正好把要找的东西抹掉。
            #    （第一版就是这么漏掉两处的。）
            # 往后看几行"代码"——**注释不算距离**。
            # （踩过一次：自己加的 4 行解释性注释把真正的判空挤出了窗口，
            #   工具于是报了一个其实已经被守住的调用。）
            after = [x for x in raw[i:i + 6] if not x.strip().startswith("#")]
            nxt = " ".join(after[:3])
            if PORT_GUARD.search(nxt):
                continue
            if any(f.name == n and re.search(pat, line) for n, pat, _ in ALLOW_TEXT):
                allowed += 1
                continue
            if (f.name, i) in ALLOW:
                allowed += 1
                continue
            problems.append((f.name, i, line.strip()))

    print(f"verify 诚实性体检：{len(files)} 个脚本，{checked} 处状态变更调用，"
          f"{allowed} 处豁免")
    if not problems:
        print("  ✓ 每一处都有人管它的结果（或明确豁免）")
        return 0
    print(f"  ✗ {len(problems)} 处丢弃了退出码 —— 前提崩了会被当成结论：")
    for name, ln, text in problems:
        print(f"      {name}:{ln}")
        print(f"        {text[:110]}")
    print()
    print('  修法：`… start t2 >/dev/null 2>&1 || die "起不来 t2 —— 前提不成立，不下结论"`')
    print("  确实要吞掉退出码的，写进本文件的 ALLOW_TEXT 并说明理由。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
