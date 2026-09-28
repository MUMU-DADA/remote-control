# 交付与验收：一份 ROM，两个 x64 平台

> 目标（G2 + G4）：**同一份自编镜像**在 x86_64 Linux（KVM）与 x86_64 Windows（WHPX）上
> **同架构**跑起来，并且都能跑 arm64 应用。

---

## 1. 产物是什么

```bash
./scripts/package-rom.sh          # → artifacts/rom-autosnap_x64_arm64/
```

| 文件 | 作用 | 缺了会怎样 |
|---|---|---|
| `system.img` | 系统分区（含翻译层 + `ndk_translation.rc`） | 起不来 |
| `vendor.img` / `product.img` | 厂商/产品分区（`ro.dalvik.vm.native.bridge` 在 vendor 里） | 起不来 / 翻译层不生效 |
| `ramdisk.img` / `initrd` | 一级/二级 ramdisk | 起不来 |
| `kernel-ranchu` | **x86_64** 内核（同架构的关键） | 起不来 |
| `encryptionkey.img` / `userdata.img` | 加密密钥与初始 userdata | 起不来 |
| `advancedFeatures.ini` / `config.ini` | 模拟器特性与硬件配置 | 特性缺失 |
| `system/build.prop` | 模拟器靠它识别 guest 架构 | **会退回宿主架构，内核起不来** |
| `source.properties` | SDK 版模拟器认它（AOSP 产物不带，打包时补） | SDK 模拟器可能拒启 |
| `SHA256SUMS` / `MANIFEST.txt` | 两边跑的是不是同一份，靠它证明 | — |

---

## 2. Linux x86_64（KVM）

```bash
cd dev/04-x64-android
./scripts/build-rom.sh --status      # 确认编完了
./scripts/package-rom.sh
./scripts/run-linux.sh               # 启动 + 4 组验收
```

验收项（`run-linux.sh` 会逐条打勾）：

| 组 | 检查 | 期望 |
|---|---|---|
| 1 | `ro.build.version.sdk` / `ro.product.device` / `ro.product.cpu.abilist` | `31` / `autosnap_x64_arm64` / `x86_64,arm64-v8a` |
| 2 | `ro.dalvik.vm.native.bridge` / `ro.enable.native.bridge.exec` / binfmt 注册 | `libndk_translation.so` / `1` / 含 `arm64_exe` |
| 3 | 自建 aarch64 静态 ELF 直接执行 | 打印 `ARM64_OK` |
| 4 | 自建探针 APK（纯 arm64-v8a）：装 → 起 → `primaryCpuAbi` → 映射 arm64 库 → logcat 原生返回值 | 全绿 |

模拟器优先用 SDK 版（与 Windows 同源），失败自动回退 AOSP 自带版（30.8.3）。

---

## 3. Windows x86_64（WHPX）

```powershell
cd dev\04-x64-android\windows
.\fetch-emulator.ps1                       # SDK emulator + adb（本机有 Android Studio 就直接用）
.\fetch-images.ps1                         # 从 Linux 构建机拉同一份镜像 + system\build.prop
.\run-windows.ps1                          # 启动（-accel on = WHPX）+ 同样的 4 组验收
```

细节与排错见 [`../windows/README.md`](../windows/README.md)。

> ⚠️ **本机没有 Windows**，WHPX 那条路径尚未实跑。已经从 Linux 侧把**能验的都验了**：

| 验证项 | 结果 |
|---|---|
| Windows 版模拟器包可达 + 完整性 | ✅ 腾讯镜像 `emulator-windows_x64-15917651.zip`（421 MiB），**sha1 与仓库清单一致**（`54fa750822ff…`） |
| 该包带 x86_64 guest 后端 | ✅ 内含 `emulator/emulator.exe` + **`emulator/qemu/windows-x86_64/qemu-system-x86_64.exe`**——正是本 ROM 需要的架构后端 |
| 同一份镜像能起 | ✅ Linux 侧跑交付目录：**41.2 秒开机**、4 组验收全绿 |
| **同 build id 等价性** | ✅ Windows 稳定包 `emulator-windows_x64-15917651.zip` 与 Linux 包 `emulator-linux_x64-15917651.zip` 是**同一个 build（15917651 / 37.1.11）**；已下载该 Linux 包并**用它复跑验收全绿** → Windows 版跑通的最强等价证据（同一源码、同一版本，差异仅宿主适配层） |
| Windows 首次运行对照表 | ✅ [`windows/EXPECTED-OUTPUT.md`](../windows/EXPECTED-OUTPUT.md)：逐项期望值 + 已知正常现象 + 按本机踩坑记录的排查顺序 |
| 脚本正确性 | ✅ 修掉两个真 bug：清单里每个包有按 `host-os` 分的多个 archive（会误下 Linux 版）、渠道是 `channelRef ref="channel-0"` 而非名字 |
| 前置自检 | ✅ `windows/preflight.ps1`：虚拟化/WHPX 状态、磁盘、镜像齐全性（含 `-qemu` 家族）、`SHA256SUMS` 一致性、与 Linux 指纹比对 |

**唯一没有实跑的是 WHPX 加速本身**（需要一台 Windows 机器）。首次执行请把
`preflight.ps1` 与 `run-windows.ps1` 的输出贴回来。

---

## 4. 怎么证明"两边跑的是同一份 ROM"

```bash
# Linux
adb -s emulator-5580 shell getprop ro.build.fingerprint
sha256sum artifacts/rom-autosnap_x64_arm64/system.img
```

```powershell
# Windows
& $adb -s emulator-5580 shell getprop ro.build.fingerprint
Get-FileHash .\images\system.img -Algorithm SHA256
```

两边 `ro.build.fingerprint` 与 `system.img` 的 sha256 必须一致
（`MANIFEST.txt` 里已经记下了构建时的指纹与属性，可三方对照）。

---

## 5. ⚠️ 交付前的硬约束

| 项 | 说明 |
|---|---|
| **翻译层许可** | `libndk_translation` 是 Google 专有二进制（随 SDK 系统镜像分发，SDK 许可不含再分发）。**对外交付整机前必须过法务**；内部使用/开发无碍 |
| ABI 覆盖 | 本 ROM 是 `x86_64,arm64-v8a`（纯 64 位）。**32 位 ARM（armeabi-v7a）应用装不上**——需要就走 API 30 基座方案（见 `../README.md` §5） |
| 性能 | 串行依赖浮点实测退化 21~23×；整数/哈希 ~1.1×。目标应用先做性能验收 |
| 版本绑定 | 翻译层与 `AndroidVersion.ApiLevel` 绑定：API 31 的载荷只能配 Android 12 的框架 |
