# windows · 在 x64 Windows 上跑同一份 ROM

> 宿主：Windows 10 1809+ / 11（x64）
> Guest：**Linux 侧编出来的同一份镜像**（`out/target/product/remote_control_x64_arm64/`）
> 加速：**WHPX（Windows Hypervisor Platform）** —— 同架构才有加速，这正是本项目的意义

---

## 为什么这条路在 Windows 上能成立

| | 老的 arm64 路线（跨架构全系统模拟，已实测评估后放弃） | 本项目（x86_64 + 翻译层） |
|---|---|---|
| guest 架构 | arm64 → 只能 TCG，Windows 的 Hyper-V/WHPX **帮不上忙** | **x86_64 → WHPX 直接加速** |
| 模拟器版本 | 34+ 版本已移除 arm64 软件后端，只能用旧版 | 任何现代版本（37.x）都支持 |
| arm64 应用 | 原生跑（但整机慢 10~40 分钟开机） | 用户态翻译层跑（开机几十秒） |

---

## 三步

```powershell
cd windows

.\fetch-emulator.ps1     # 1. 拿 emulator.exe（SDK）+ platform-tools 里的 adb
.\fetch-images.ps1       # 2. 从 Linux 构建机拉同一份镜像（按 SHA256SUMS 校验）
.\preflight.ps1          # 3. 前置检查：虚拟化/WHPX、磁盘、镜像齐全性、与 Linux 侧指纹比对
.\run-windows.ps1        # 4. 启动 + 验收（abilist / 翻译层 / arm64 应用）
```

> 日常开机关机用 **`emulator.ps1`**（`run-windows.ps1` 是"启动 + 验收"的
> 一次性流程，`emulator.ps1` 管实例的整个生命周期）：

```powershell
.\emulator.ps1 list                    # 有哪些实例、在不在跑、占多大
.\emulator.ps1 start  default          # 起（不存在会自动创建）
.\emulator.ps1 stop   default          # 优雅关机（等进程真的退出）
.\emulator.ps1 kill   default          # 卡住了才用：强杀
.\emulator.ps1 clone  default  dev2    # 复制一台（连已装应用一起）
.\emulator.ps1 delete dev2             # 停掉并删光
```

命令表和注意事项与 Linux 侧**完全对应**，见
[`../docs/12-emulator-control.md`](../docs/12-emulator-control.md)。

> **必需文件里最容易漏的是 `-qemu` 家族**（`system/vendor/product-qemu.img` + `ramdisk-qemu.img`）：
> 前者是带 GPT 分区表的包装版，后者是「系统 ramdisk + vendor ramdisk」的合并版
> （first-stage 的 `fstab.ranchu` 在里面）。只给裸 `*.img` 会以
> `partition(s) not found in /sys` / `failed to find device default fstab` 卡死并无限重启——
> 这是本机实测踩过的（见 [`../docs/02-build-traps.md`](../docs/02-build-traps.md) 坑 8）。

前置：OpenSSH 客户端（Win10 1809+ 自带）、≥15 GB 磁盘、≥8 GB 内存、
**BIOS 里开启虚拟化**（WHPX 需要；若用 Hyper-V/WSL2 已开启则天然满足）。

---

## 硬件参数：和 Linux 侧同一份 config.ini

`emulator.ps1` / `run-windows.ps1` 都从 `..\emulator\config.ini` 读，
不在这两个脚本里写死默认值：

| 项 | 默认 |
|---|---|
| 屏幕 | 1280x720 横屏 @320dpi |
| CPU / 内存 | 4 核 / 6144 MB |
| 数据分区 | 16G |
| GPU | `auto`（自适应：有真显卡就 `host`，否则 `swiftshader_indirect`；`host` 起不来还会再退一次） |

> ⚠️ 以前这两个脚本的参数默认值是写死的（`-MemoryMB 4096`、`-Gpu
> swiftshader_indirect`），而命令行**优先于** config.ini —— 于是
> config.ini 里改内存根本没用。现在只在**显式传参**时才覆盖。

---

## 前置条件与注意事项

| 项 | 说明 |
|---|---|
| 虚拟化 | `-accel on` 走 WHPX；若报 `WHPX is not installed`，到「启用或关闭 Windows 功能」勾选 *Windows 虚拟机监控程序平台* |
| 镜像 | 只需 `system.img / vendor.img / ramdisk.img / kernel-ranchu / encryptionkey.img / userdata.img / build.prop / advancedFeatures.ini / source.properties` + `system\build.prop` |
| `system\build.prop` | 模拟器靠它判断 guest 架构；AOSP 产物里天然有（`out/.../system/build.prop`），scp 时**必须带上 system 子目录** |
| `initrd` | 模拟器 `-initrd` 指向 `<sysdir>\initrd`；AOSP 产物里有，缺了就 `copy ramdisk.img initrd` |
| 首次启动 | WHPX 下几十秒；比 arm64 TCG 的 10~40 分钟快一个量级 |
| 反病毒 | 首次启动可能被实时防护拖慢，可临时排除镜像目录 |

---

## ⚠️ 未在本机验证

本机是 Linux（无 Windows），因此：

- `run-windows.ps1` 的**参数与流程**在 Linux 上用**同源代码的 SDK emulator 37.2.11** 验证过
  （同一份 `google_apis;x86_64` 镜像 29.0 秒开机、属性一致，见评估文档 §2.2）；
- 但 **WHPX 路径本身没有跑过**——首次在 Windows 上执行时请把 `run-windows.ps1` 的输出贴回来，
  有问题大概率出在：WHPX 未启用、镜像目录少文件、或 adb 版本过旧。
