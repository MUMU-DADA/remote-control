#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""随包发出去的 APK：带原生库的话必须**自带 arm64-v8a**。

为什么要有它
============

`docs/13-macos-port.md` 的"已知风险"表里写着：

    新增应用 APK 含预编译 `.so` | 原生 arm64 ROM 上**没有翻译层兜底**，
    必须自带 `arm64-v8a` | 集成应用时检查 APK 的 `lib/` 目录

**"集成应用时检查"是一条没人执行的规矩** —— 全仓库搜 `arm64-v8a`，命中的
全是 darwin 脚本在查 **guest 的 ABI**（`ro.product.cpu.abilist`），
没有一处去看 APK 的 `lib/`。

今天不会出事：控制台那个 APK 只有 16KB、6 个条目、**没有 `lib/`**（纯 Java）。
但规矩是针对**将来**集成带 `.so` 的应用写的 —— 那时候忘了检查，
表现是**在 arm64 ROM 上装不上或起不来**，而且报错未必指向架构。

所以把它变成会报的检查。

它查什么
========

扫仓库里**会被发出去**的 APK（`dev/05-controller-app/build/*.apk`、
`artifacts/**/*.apk`、`dev/04-android-rom/artifacts/**/*.apk`）：

* 没有 `lib/` → 通过（纯 Java/Kotlin，与架构无关）；
* 有 `lib/` → **必须含 `arm64-v8a/`**，否则报错；
* 顺便把每个 APK 的 ABI 清单打出来 —— "它到底带了什么"本来就该一眼看得到。

比不了什么
==========

它看的是**仓库里的 APK 文件**，不是烧进 `vendor.img` 之后的样子。
真要确认"ROM 里那份是哪个"，得挂载镜像去看 —— 那一步不在本工具范围内。
"""

import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 会被发出去的 APK 所在处（相对仓库根）
APK_GLOBS = [
    "dev/05-controller-app/build/*.apk",
    "artifacts/**/*.apk",
    "dev/04-android-rom/artifacts/**/*.apk",
    "dev/04-android-rom/.run/arm64-probe/**/*.apk",
]

# 这些 ABI 目录出现在 `lib/` 下才算数（APK 里的 ABI 目录名）
REQUIRED_ABI = "arm64-v8a"


def apks():
    seen = {}
    for pattern in APK_GLOBS:
        for p in ROOT.glob(pattern):
            if p.is_file():
                seen[str(p.relative_to(ROOT))] = p
    return sorted(seen.items())


def main() -> int:
    found = apks()
    if not found:
        print("APK 原生库体检：仓库里没找到会被发出去的 APK")
        print("  （这不是失败 —— 只是没有可查的对象。构建过之后再看。）")
        return 0

    problems, checked = [], 0
    print(f"APK 原生库体检：{len(found)} 个")
    for rel, path in found:
        try:
            with zipfile.ZipFile(path) as z:
                names = z.namelist()
        except (zipfile.BadZipFile, OSError) as exc:
            problems.append(f"{rel}: 打不开（{exc}）—— 这本身就该查")
            continue
        checked += 1
        abis = sorted({m.split("/")[1] for m in names
                       if m.startswith("lib/") and m.count("/") >= 2})
        size_kb = path.stat().st_size / 1024
        if not abis:
            print(f"  {rel:<56} {size_kb:>8.1f} KB  纯 Java（无 lib/）")
            continue
        print(f"  {rel:<56} {size_kb:>8.1f} KB  ABI: {', '.join(abis)}")
        if REQUIRED_ABI not in abis:
            problems.append(
                f"{rel}: 带原生库但没有 {REQUIRED_ABI}/（只有 {', '.join(abis)}）—— "
                f"arm64 ROM 上没有翻译层兜底，装不上或起不来")

    if problems:
        print()
        print("  ✗ 有问题：")
        for p in problems:
            print(f"      {p}")
        print()
        print("  这条规矩在 docs/13-macos-port.md 的「已知风险」表里 —— 原文写的是")
        print("  「集成应用时检查 APK 的 lib/ 目录」，但没有任何代码在执行它。")
        print("  补齐办法：给应用带上 arm64-v8a 的原生库，或者确认它本来就该是纯 Java。")
        return 1

    print(f"  ✓ {checked} 个 APK：带原生库的都含 {REQUIRED_ABI}（或本来就是纯 Java）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
