# windows · 在 x64 Windows 上跑同一份 ROM

> 宿主：Windows 10 1809+ / 11（x64）
> Guest：**Linux 侧编出来的同一份镜像**（`out/target/product/autosnap_x64_arm64/`）
> 加速：**WHPX（Windows Hypervisor Platform）** —— 同架构才有加速，这正是本项目的意义

---

## 为什么这条路在 Windows 上能成立

| | 老的 arm64 路线（`dev/04-emulator/windows-arm64/`） | 本项目（x86_64 + 翻译层） |
|---|---|---|
| guest 架构 | arm64 → 只能 TCG，Windows 的 Hyper-V/WHPX **帮不上忙** | **x86_64 → WHPX 直接加速** |
| 模拟器版本 | 34+ 版本已移除 arm64 软件后端，只能用旧版 | 任何现代版本（37.x）都支持 |
| arm64 应用 | 原生跑（但整机慢 10~40 分钟开机） | 用户态翻译层跑（开机几十秒） |

---

## 三步

```powershell
cd windows

.\fetch-emulator.ps1     # 1. 拿 emulator.exe（SDK 37.x）+ platform-tools 里的 adb
.\fetch-images.ps1       # 2. 从 Linux 构建机 scp 同一份镜像（带 sha256 校验）
.\run-windows.ps1        # 3. 启动 + 验收（abilist / 翻译层 / arm64 应用）
```

前置：OpenSSH 客户端（Win10 1809+ 自带）、≥15 GB 磁盘、≥8 GB 内存、
**BIOS 里开启虚拟化**（WHPX 需要；若用 Hyper-V/WSL2 已开启则天然满足）。

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
