#!/usr/bin/env python3
"""仓库脚本体检：PowerShell 脚本必须带 UTF-8 BOM。

**为什么这条不是洁癖** —— 实测踩过，而且是整条 Windows 路线都跑不起来的那种：

  Windows PowerShell 5.1（Win10 自带的那版）读 .ps1 时，
  **没有 BOM 就按系统 ANSI 代码页解码**，不看内容是不是合法 UTF-8。
  于是文件里的中文注释与中文字符串被按 GBK 重新解释，
  解码出来的字节序列会撞坏语法结构（引号、括号），报出来的是：

      preflight.ps1:38 字符: 1
      + }
      + ~
      表达式或语句中包含意外的标记“}”

  报错位置指向一个**完全正确**的大括号，因为真正的损坏发生在
  一百多行之前的中文注释里。这类错误极难从报错本身推回去。

  加上 BOM（EF BB BF）之后 PS 5.1 会正确按 UTF-8 解码，PS 7 也认。
  代价是三个字节。

用法：
    python3 tools/check-scripts.py          # 有问题时退出码 1
"""
import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BOM = b"\xef\xbb\xbf"

EXTS = {".ps1": "Windows PowerShell 5.1 需要 BOM 才能正确按 UTF-8 解码"}


# 不进入的目录。
#
# ⚠️ 用 os.walk 的 dirs[:] 剪枝，而不是 rglob + 事后过滤：
#    **rglob 的匹配发生在遍历之后** —— 它照样会把 aosp/（完整 AOSP 源码树，
#    几万个文件）走一遍，脚本要跑好几分钟。第一版就是这么写的，直接超时，
#    看起来像"这个检查很慢"，其实是写法不对。
SKIP_DIRS = {".git", ".cache", "aosp", "artifacts", "out", "build",
             "node_modules", "__pycache__", "payload",
             # 这几类是跑测试留下的临时目录，不是仓库内容 ——
             # 不排的话它们会贡献几百条噪声，把真问题淹掉。
             ".tmp", "images", ".run"}


def find(ext):
    out = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if name.endswith(ext):
                out.append(pathlib.Path(dirpath) / name)
    return sorted(out)


def main():
    problems = []
    checked = 0

    for ext, why in EXTS.items():
        for p in find(ext):
            checked += 1
            raw = p.read_bytes()
            rel = p.relative_to(ROOT)

            try:
                raw.decode("utf-8")
            except UnicodeDecodeError as e:
                problems.append(f"{rel}: 不是合法 UTF-8（{e}）")
                continue

            if not raw.startswith(BOM):
                problems.append(f"{rel}: 缺 UTF-8 BOM —— {why}")

    # 顺带看一眼 .sh：它们不需要 BOM（反而会破坏 shebang），
    # 但必须是合法 UTF-8，否则 Linux 侧读中文注释会出乱码。
    for p in find(".sh"):
        checked += 1
        try:
            p.read_bytes().decode("utf-8")
        except UnicodeDecodeError as e:
            problems.append(f"{p.relative_to(ROOT)}: 不是合法 UTF-8（{e}）")

    print(f"脚本体检：{checked} 个文件（{', '.join(EXTS)} + .sh）")
    if problems:
        print(f"\n发现 {len(problems)} 个问题：")
        for x in problems:
            print(f"  ✗ {x}")
        return 1
    print("  ✓ PowerShell 脚本都带 BOM，且全部是合法 UTF-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
