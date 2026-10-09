#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""配置键的三段链子：`config.ini` → 管理器拼的 `-prop` → daemon 读的属性。

为什么要有它
============

guest 里那个服务是靠模拟器的属性桥配置的：

    emulator/config.ini          service.auth = 1          ← 真源
        ↓  管理器读出来拼成
    -prop "qemu.rc.auth=1"                                  ← 宿主侧命令行
        ↓  模拟器把它暴露成
    vendor.qemu.rc.auth                                     ← guest 里看见的
        ↓  daemon 读
    AndroidProperty("vendor.qemu.rc.auth")                  ← 真正生效的地方

2026-10-09 三段逐个对照过，**目前完全对得上**（6 个键一一对应）。
但这条链上任何一个环节写错，表现都是**静默失效**：键名拼错 → 属性没设上 →
daemon 读到空 → 那个功能悄悄停在默认值，**没有任何东西会报错**。

所以要有这个工具。

它查三件事
==========

  A. 代码里 `config_get` / `Get-ConfigValue` 引用的键，必须在 `config.ini` 里存在
     （引用了不存在的键 = 永远拿到默认值）
  B. `config.ini` 里的 `service.*` 键，**每一套管理器**都要拼出对应的 `-prop`
     （某条血统漏拼 = 那个平台少一个功能）
  C. daemon 读的 `vendor.qemu.rc.*`，必须是管理器拼得出来的那些
     （读了没人设的属性 = 永远默认值）

命名差异用下面的 MAPPING 显式记着，不靠猜。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
X64 = ROOT / "dev" / "04-android-rom"
DAEMON = ROOT / "dev" / "02-native-daemon" / "daemon"
CONFIG_INI = X64 / "emulator" / "config.ini"

# 要检查"是否拼出对应属性"的**每一套**管理器（两条血统都在内）
# ⚠️ 值是 None = **必须自己拼**；是字符串 = **不用自己拼**，并说明为什么。
#    第一版这里只有一份路径列表，而"某个目录一个属性都没拼"会被**静默跳过** ——
#    正是这个项目反复在修的那类问题（第 10 轮"没分类就报错"、第 16 轮"不许静默少跑"）。
#    2026-10-09 实测就是这么漏掉 windows/ 的：它确实不自己拼，
#    而是复用 packaging/bin/windows/console.ps1 里那份共享实现 ——
#    功能上没缺口，但**工具当时看不出来，也不会说**。
MANAGER_DIRS = {
    X64 / "scripts": None,
    X64 / "packaging" / "bin" / "linux": None,
    X64 / "packaging" / "bin" / "darwin": None,
    X64 / "packaging" / "bin" / "windows": None,
    X64 / "windows": "复用 packaging/bin/windows/console.ps1 的共享实现（同一份代码，不是另写一套）",
}

# config.ini 的键名 ↔ 属性名。差异是历史原因，显式记着。
MAPPING = {
    "service.adb_enabled": "adb",
    "service.auth": "auth",
    "service.bind": "bind",
    "service.enabled": "enabled",
    "service.port": "port",
    "service.token": "token",
}


def manager_props():
    """每套管理器各自拼出了哪些 qemu.rc.* 属性。"""
    per_dir = {}
    for d in MANAGER_DIRS:
        if not d.is_dir():
            continue
        found = set()
        for f in list(d.glob("*.sh")) + list(d.glob("*.ps1")):
            t = f.read_text(encoding="utf-8", errors="replace")
            # -prop "qemu.rc.auth=1"   /   'qemu.rc.bind=$bind'   /   "qemu.rc.port=$port"
            found |= set(re.findall(r"qemu\.rc\.([a-z_]+)\s*=", t))
        if found:
            per_dir[str(d.relative_to(ROOT))] = found
    return per_dir


def daemon_props():
    found = set()
    for f in list(DAEMON.glob("*.cpp")) + list(DAEMON.glob("*.h")):
        t = f.read_text(encoding="utf-8", errors="replace")
        found |= set(re.findall(r"vendor\.qemu\.rc\.([a-z_]+)", t))
    return found


def main() -> int:
    problems = []

    # ── A ────────────────────────────────────────────────────────────────
    defined = set(re.findall(r"^([a-zA-Z][a-zA-Z0-9_.]*)\s*=",
                             CONFIG_INI.read_text(encoding="utf-8"), re.M))
    refs = {}
    for f in list(X64.glob("**/*.sh")) + list(X64.glob("**/*.ps1")):
        if any(p in f.parts for p in ("aosp", "vendor", ".run", "artifacts", "images")):
            continue
        t = f.read_text(encoding="utf-8", errors="replace")
        got = set(re.findall(r'\bconfig_get\s+"?([a-zA-Z][a-zA-Z0-9_.]*)"?', t))
        got |= set(re.findall(r"\bGet-ConfigValue\s+['\"]?([a-zA-Z][a-zA-Z0-9_.]*)[\"']?", t))
        if got:
            refs[str(f.relative_to(ROOT))] = got
    all_refs = set().union(*refs.values()) if refs else set()
    print(f"A) config.ini 定义 {len(defined)} 个键；代码引用 {len(all_refs)} 个")
    for k in sorted(all_refs - defined):
        where = [f for f, s in refs.items() if k in s][:3]
        problems.append(f"代码引用了 config.ini 里不存在的键 `{k}`（永远拿到默认值）：{where}")

    # ── B ────────────────────────────────────────────────────────────────
    svc_keys = {k for k in defined if k.startswith("service.")}
    want = {MAPPING.get(k) for k in svc_keys if k in MAPPING}
    unknown = svc_keys - set(MAPPING)
    for k in sorted(unknown):
        problems.append(f"config.ini 里有 service 键 `{k}` 没写进本工具的 MAPPING（不知道它对应哪个属性）")
    per_dir = manager_props()
    print(f"B) config.ini 的 service 键 {len(svc_keys)} 个 → 期望的属性 {sorted(want)}")
    for d in sorted(MANAGER_DIRS, key=str):
        rel = str(d.relative_to(ROOT))
        props = per_dir.get(rel, set())
        note = MANAGER_DIRS[d]
        if not props:
            if note is None:
                problems.append(rel + " 一个 qemu.rc.* 属性都没拼 —— 要么它漏了，要么本工具的 MANAGER_DIRS 该给它写个理由")
            else:
                print(f"     {rel:<40} 不自己拼：{note}")
            continue
        missing = want - props
        print(f"     {rel:<40} 拼出 {len(props)} 个")
        if missing:
            problems.append(f"{rel} 少拼了属性 {sorted(missing)} —— 那个平台会静默少功能")

    # ── C ────────────────────────────────────────────────────────────────
    read = daemon_props()
    built = set().union(*per_dir.values()) if per_dir else set()
    print(f"C) daemon 读 {len(read)} 个 vendor.qemu.rc.*：{sorted(read)}")
    orphan = read - built
    if orphan:
        problems.append(f"daemon 读了没人拼的属性 {sorted(orphan)} —— 永远默认值")

    if problems:
        print()
        print("  ✗ 链子断了：")
        for p in problems:
            print(f"      {p}")
        print()
        print("  这条链上写错的表现是**静默失效**（功能悄悄停在默认值），不会报错。")
        return 1
    print()
    print("  ✓ 三段链子对得上：config.ini → 各套管理器的 -prop → daemon 读的属性")
    return 0


if __name__ == "__main__":
    sys.exit(main())
