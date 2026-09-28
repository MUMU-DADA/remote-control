# Windows 首次运行 · 期望输出对照表

> 用法：在 Windows 上跑完 `preflight.ps1` 与 `run-windows.ps1`，
> 把输出对照本表。**任何一项与"期望"不符，先看右列的处置**。

---

## 1. `preflight.ps1` 期望

| 检查 | 期望 | 不符时 |
|---|---|---|
| CPU 虚拟化 | `CPU 虚拟化已在固件层启用` | 进 BIOS/UEFI 打开 VT-x / AMD-V |
| WHPX 功能 | `Windows Hypervisor Platform 已启用` | 「启用或关闭 Windows 功能」勾选 *Windows 虚拟机监控程序平台* → 重启 |
| 磁盘 | 可用 ≥ 20 GB（镜像 5.7 GB + 解包与覆盖层） | 换盘或清理 |
| 必需镜像 | `system-qemu.img / vendor-qemu.img / product-qemu.img / ramdisk-qemu.img / kernel-ranchu / encryptionkey.img / userdata.img / advancedFeatures.ini / config.ini / system\build.prop` 全部 `[OK]` | 缺哪个重跑 `fetch-images.ps1` |
| `SHA256SUMS` | `校验通过（N 个文件）` | 传输损坏 → 重跑 `fetch-images.ps1 -Force` |
| 指纹行 | `ro.system.build.fingerprint = AutoSnap/autosnap_x64_arm64/...` | 与 Linux 侧不一致说明不是同一份 ROM |

---

## 2. `run-windows.ps1` 期望（对照 Linux 侧实测值）

本机 Linux 侧用**与 Windows 稳定版同一 build id（15917651）的 Linux 包**跑过同一份交付目录，
所以下面这些值两边必须一致：

| 项 | 期望值 |
|---|---|
| 启动加速 | `-accel on`（WHPX）；日志里不应出现 `falling back to TCG` |
| 开机耗时 | **几十秒**（Linux 侧实测 41.2 秒；WHPX 同量级） |
| `ro.build.version.sdk` | `31` |
| `ro.product.device` | `autosnap_x64_arm64` |
| `ro.product.cpu.abilist` | `x86_64,arm64-v8a` |
| `ro.dalvik.vm.native.bridge` | `libndk_translation.so` |
| `ro.enable.native.bridge.exec` | `1` |
| `/proc/sys/fs/binfmt_misc/` | 含 `arm64_exe`、`arm64_dyn` |
| `pm install --abi arm64-v8a` | `Success` |
| `primaryCpuAbi` | `arm64-v8a` |
| 进程映射 | `/proc/<pid>/maps` 里有若干 `/system/lib64/arm64/*.so`（Linux 侧 16 条） |
| logcat `ARM64PROBE` | `PROBE_RESULT arm64-v8a native ok | built_for=arm64-v8a | kernel=x86_64 | ptr=64 bit` |
| `ro.build.fingerprint` | 与 Linux 侧**完全一致** |

> 最后一条是"两边跑的是同一份 ROM"的硬证据：
> `AutoSnap/autosnap_x64_arm64/autosnap_x64_arm64:12/SP1A.210812.016.C2/root09280236:userdebug/test-keys`

---

## 3. 已知的正常现象（别当成故障）

| 现象 | 说明 |
|---|---|
| 首次启动比后续慢 | 首次要建 userdata、跑 dexopt |
| 日志里 `unexpected system image feature string` | AOSP 自编镜像与模拟器特性字符串不完全对齐，无影响 |
| 没有 `initrd` 文件也没关系 | 模拟器会自己从 `ramdisk-qemu.img` 生成（`preflight.ps1` 会提示） |
| `adb devices` 一开始显示 `offline` | 正常，等开机完成 |
| Windows 防火墙弹窗 | 允许即可（adb 需要本地端口） |

---

## 4. 真出问题时的排查顺序（按本机踩坑记录）

| 症状 | 大概率原因 | 处置 |
|---|---|---|
| guest 无限重启，日志有 `InitFatalReboot` | 少了 `ramdisk-qemu.img`（first-stage 找不到 fstab） | 重跑 `fetch-images.ps1`；确认必需清单里的 `-qemu` 家族齐全 |
| 卡在 `partition(s) not found in /sys` | 少了 `*-qemu.img`（GPT 包装版） | 同上 |
| `vbmeta digest error isn't allowed` | 镜像带了 AVB（构建侧问题，交付镜像不应有） | 在 Linux 侧以 `QEMU_DELETE_AVB=true` 重编（`build-rom.sh` 默认已开） |
| 模拟器报 `missing the 'kernel-qemu' image file` | 缺 `ANDROID_PRODUCT_OUT` / `ANDROID_BUILD_TOP` 环境变量 | `run-windows.ps1` 已设；手工起模拟器时别漏 |
| 报 WHPX 不可用 | 前置检查没过 | 先跑 `preflight.ps1` |
| 装 arm64 APK 报 `ABI arm64-v8a not supported` | 属性没生效（不是这份 ROM） | 核对 `ro.product.cpu.abilist` |

本机 Linux 侧这三种启动失败都真实踩过并修好，完整记录见
[`../docs/02-build-traps.md`](../docs/02-build-traps.md) 坑 7、8。
