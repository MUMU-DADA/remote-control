# 04 · 模拟器验证环境（QEMU）

> **不是产品轨道，是 `02-native-daemon` 的验证基础设施。**
> 目标：**不依赖真机**，在 x86_64 宿主上把 arm64 安卓跑起来，用来验证 `autod`。

| 路径 | 宿主 | 跑什么 | 用途 |
|---|---|---|---|
| [`linux-arm64/`](linux-arm64/README.md) | 开发机（Debian 13 x86_64，无显示） | 本机编的 arm64 镜像：**原版**（验证 `autod`）/ **自制 ROM** | 开发内循环 + 本平台验证 |
| [`windows-arm64/`](windows-arm64/README.md) | Windows x64 | **同一份 arm64 自制 ROM**（scp 过去） | 交付环境的整机运行验证 |

> ⭐ **新方向评估（全部结论实测）** → [`X86_64-ARM64-BRIDGE-EVAL.md`](X86_64-ARM64-BRIDGE-EVAL.md)
> 「x86_64 安卓 + ARM 翻译层」能否替代 arm64 TCG：**方向成立，但 libhoudini 的 arm64 变体止步于 Android 7**；
> 正确工具是 Google 官方的 `libndk_translation`，官方 `google_apis;x86_64` 镜像 **34 秒开机且能跑 arm64 应用**。
> 并且已实测：把它搬进**自己的** x86_64 镜像 = 23 MB 载荷 + 10 行属性（§2.6），arm64 应用照跑。
> 注意 ABI 覆盖面随 API 级别变化：**要跑 32 位 ARM 应用得选 Android 11**，Android 12 镜像是纯 64 位（§2.5）。

> **两个平台都要跑真实 ROM。** 镜像只编一次，两边共用；
> 平台差异只有模拟器版本与显示方式，见下面「两个平台跑同一份 ROM」。

---

## 开发测试怎么分层（选对层，别每改一行都等 40 分钟）

| 层 | 命令 | 耗时 | 能发现什么 | 要模拟器 |
|---|---|---|---|---|
| **0 · 主机单测** | `cd dev/02-native-daemon/tests && sudo make run` | 秒 | 注入后端、socket、协议、分发、`SCM_RIGHTS` 帧通道 | 不要 |
| **1 · 主机整链路** | `tools/build-autod.sh` → `autod-host` / `autodctl-host` | 秒 | 客户端 ↔ 服务端完整回环（截图后端用 `fake_screencap` 替身） | 不要 |
| **2 · 编译验证** | 容器内 `m autod autodctl` | 分钟 | AOSP 头/API/bionic 用法对不对（`jni.h` 那类坑就是这样抓到的） | 不要 |
| **3 · x86_64 模拟器** ⭐ | `./build-images.sh --fast` + `./run-emulator.sh --fast` | 首次编译 1~2 小时，之后**开机几十秒** | 真实 SurfaceFlinger 截图、真实 `/dev/uinput` 触控、init.rc、SELinux | 要，KVM 加速 |
| **4 · arm64 模拟器 / 真机** | `./build-images.sh` + `./run-emulator.sh` | 编译 1~2 小时，开机 10~40 分钟 | ABI 相关差异、与产品一致的行为；**也是 Windows ROM 的来源** | 要，只能 TCG |

**日常内循环走第 3 层。** x86_64 guest 与 arm64 guest 的源码路径完全一样
（截图走 libgui/SurfaceFlinger、触控走 uinput、协议与分发跟架构无关），
但同架构 + KVM 让开机从 40 分钟变成几十秒，改完 `m autod` + `adb push` + 重启服务是秒级。
只有 ABI 相关的疑虑（指针宽度、对齐、bionic 差异）才必须回到第 4 层。

> **最容易踩的坑**：`lunch aosp_arm64-userdebug` 编出来的是 **GSI**，产物在
> `out/target/product/generic_arm64/`，**永远起不了模拟器**（没有 `kernel-ranchu` /
> `ramdisk.img` / `vendor.img`）。它只适合 `m autod` 这类单模块编译验证，
> 不要拿它当模拟器镜像 —— 这也是"看起来模拟器跑不起来"的真正原因。

编译成本：arm64 与 x86_64 是两套 target 产物（host 工具与大部分中间产物复用），
`out/` 会各占几十 GB。磁盘不够时可以 `rm -rf out/target/product/<不要的那个>`。

---

## 两个平台跑同一份 ROM（交付验证路径）

真实 ROM 要在 **Linux 开发机**与 **Windows x64** 上都跑一遍，两边共用**同一份 arm64 镜像产物**：

```
开发机编一次 → out/target/product/emulator64_arm64/
     ├── 本机跑     : ./run-emulator.sh                     （AOSP 自带模拟器 30.8.3）
     └── 拷到 Windows: windows-arm64\fetch-images.ps1        （SDK 模拟器 37.x）
```

| | Linux 开发机 | Windows x64 |
|---|---|---|
| 模拟器来源 | AOSP 自带 `prebuilts/android-emulator/linux-x86_64` | 脚本自动下载 SDK emulator |
| 版本 | **30.8.3**（与 AOSP 12 树同源） | **37.1.11**（Google 只留最新，老包已 404） |
| 加速 | arm64 guest → 只能 TCG | arm64 guest → 只能 TCG（Hyper-V/WHPX 无效） |
| 开机 | 10~40 分钟 | 10~40 分钟（同一个物理限制） |
| 镜像 | 本机编，直接引用宿主路径 | `fetch-images.ps1` scp 过来 + md5 校验 |
| adb | AOSP 编出的 `out/host/linux-x86/bin/adb` | platform-tools（脚本一并下载） |
| 显示 | 无 DISPLAY → `-no-window` + swiftshader | 有窗口（加 `-Headless` 可无窗口） |

**唯一的不对称是模拟器版本**（Linux 同源、Windows 新两个大版本），所以：

1. Windows 侧备了三条降级预案（换渠道 / AVD / WSL2 用同款 30.8.3），见该平台 README；
2. **这个版本风险可以在 Linux 上提前验证**，不必等装好 Windows 才发现：

```bash
# 在同一台开发机上，用 SDK 版 37.x 跑同一份镜像（guest 侧行为与 Windows 一致）
curl -sSL -o /tmp/emu37.zip \
  https://mirrors.cloud.tencent.com/AndroidSDK/emulator-linux_x64-15917651.zip
unzip -q /tmp/emu37.zip -d /tmp/emu37

cd dev/04-emulator/linux-arm64
EMULATOR_BIN=/tmp/emu37/emulator/emulator ./run-emulator.sh --arm64
```

- 能开机 → Windows 侧用 37.x 基本没悬念
- 起不来 → 提前切到降级方案

> 已核对：37.1.11 的 `-sysdir` / `-datadir` / `-accel` / `-gpu` / `-wipe-data` /
> `-writable-system` 等参数**全部保留**，`run-emulator.ps1` 传的参数在 37.x 上合法。

### 交付验证清单（两个平台各过一遍）

- [ ] 模拟器起来，`adb devices` 有设备；`getprop ro.build.version.sdk` = **31**
- [ ] `adb root` 成功（userdebug 镜像）
- [ ] `/dev/uinput` 存在（autod 的触控后端依赖它）
- [ ] `autod` 起得来；`autodctl info` / `capture` / `tap` / `swipe` 全通
- [ ] 截图**不是全黑**（`smoke-autod.sh` 会自动判定）
- [ ] 两个平台的 `ro.build.fingerprint` 一致（证明跑的是同一份 ROM）

### 4. arm64 在 x86_64 宿主上要换 QEMU 机器（否则必挂）

模拟器给 arm64 guest 挂了 16 个 PCI 设备（`virtio-serial-pci`、
`virtio_input_multi_touch_pci_*`、`virtio-wifi-pci`、`virtio-vsock-pci`、`-soundhw hda`），
而 arm 的 `ranchu` 机器**没有 PCI 总线** → QEMU 直接退出，日志只有一行：

```
qemu-system-aarch64-headless: PCI bus not available for hda
emulator: Done with QEMU main loop
```

**解法**：`-qemu -machine type=virt`（`run-emulator.sh` 在 arm64 下已自动追加）。
完整排查过程与无效尝试见 [`linux-arm64/README.md`](linux-arm64/README.md)。

---

## 先读这一节：三个硬约束

后面所有脚本都是围绕这三条写的，绕不过去。

### 1. 跨架构只能软件模拟（TCG），没有加速

宿主 x86_64 + guest arm64 ⇒ **`/dev/kvm` 完全用不上**（KVM 只能同架构）。

```
$ emulator-check accel
accel:
0
KVM (version 12) is installed and usable.      ← 这是给 x86_64 guest 的，对本轨道无效
```

后果：**首次启动 10~40 分钟量级，界面操作明显卡顿。** 这是物理限制，不是配置错误。
想要秒级启动只有两条路：换 arm64 宿主（Apple Silicon / 鲲鹏），或改用真机。

### 2. 只有 Android 模拟器（goldfish / ranchu）能跑 AOSP 镜像

AOSP 的 arm64 镜像依赖 goldfish 系列虚拟设备：

| 设备 | 作用 | 没有它会怎样 |
|---|---|---|
| `goldfish_pipe` | adb / qemud 通道 | `adb devices` 永远是空的 |
| virtio-gpu + goldfish 显示 | 出图 | SurfaceFlinger 起不来，截图全黑 |
| `goldfish_events` | 输入 | 触控事件收不到 |

**这些设备只存在于 AOSP fork 的 QEMU 里**（`external/qemu`），上游 qemu.org 的
`qemu-system-aarch64` 没有它们。所以：

> 本轨道说的"QEMU 配置" = **配置 Android 模拟器**。
> 模拟器本身就是 QEMU，只是带了 ranchu 板级与 goldfish 设备。

上游 QEMU 唯一能做的是串口看内核早期启动日志，对本项目的验证目标没有价值，
因此本轨道不提供纯 QEMU 路径（避免给你一个跑不通的脚本）。

### 3. `aosp_arm64-userdebug` 起不了模拟器

Android 12 里 **`aosp_arm64` 是 GSI，不是模拟器目标**，源码注释写得很明确：

```
# build/make/target/product/aosp_arm64.mk
# The system image of aosp_arm64-userdebug is a GSI for the devices with:
# - ARM 64 bits user space ...
```

它的产物只有 system 侧镜像，**没有 `kernel-ranchu` / `ramdisk.img` / `vendor.img`**，
而这三样是模拟器启动的最低要求。

| 目标 | 产物目录 | 用途 |
|---|---|---|
| `aosp_arm64-userdebug` | `out/target/product/generic_arm64` | 编 GSI / 单模块（`m autod`） |
| **`sdk_phone64_arm64-userdebug`** | `out/target/product/emulator64_arm64` | **模拟器（本轨道）** |
| `sdk_phone64_x86_64-userdebug` | `out/target/product/emulator64_x86_64` | 追求启动速度时的替代（可上 KVM，但不是 arm64） |

两者并存，互不冲突：改 `autod` 代码时用哪个目标编都行（同一份 ABI）。

---

## 快速开始

### 开发机（验证 autod）

```bash
cd dev/04-emulator/linux-arm64

./check-env.sh              # 前置检查，缺什么会直接告诉你补什么
./build-images.sh           # 容器内 lunch sdk_phone64_arm64-userdebug && m（首次数小时 / ~100GB）
./run-emulator.sh           # 启动 + 等待 boot_completed + adb root（首次 10~40 分钟）
./smoke-autod.sh            # 阶段 1 冒烟：push autod/autodctl 并跑 info/capture/tap/swipe
```

### Windows（验证编好的 ROM）

```powershell
cd dev\04-emulator\windows-arm64

.\fetch-emulator.ps1        # 自动下载 SDK emulator（内含 qemu-system-aarch64.exe）+ platform-tools
.\fetch-images.ps1          # 从开发机 scp 拉 arm64 镜像（带 md5 校验）
.\run-emulator.ps1          # 启动
```

细节与前置条件见各自的 `README.md`。

---

## 与 `02-native-daemon` 的衔接

`autod` 的落地分两阶段，本轨道都能覆盖：

### 阶段 1：push 二进制，跳过 sepolicy（**推荐先做这个**）

模拟器是 `userdebug`，`adb root` 可用；把二进制丢进 `/data/local/tmp` 直接跑，
不碰 `/system`、不碰 SELinux：

```bash
./smoke-autod.sh
```

覆盖的目标只有一个：**确认 `captureDisplay()` 拿得到真实帧、uinput 点得中。**

### 阶段 2：编进 ROM（含 `init.rc` + sepolicy）

要让 `autod` 出现在 `/system/bin` 并开机自启，得把它加进产品：

```bash
# 在容器里
cd /aosp
echo 'PRODUCT_PACKAGES += autod autodctl' >> \
    device/generic/goldfish/64bitonly/product/sdk_phone64_arm64.mk
m -j12 systemimage
```

> `frameworks/native/cmds/autod/` 里已经有你的 `daemon/` `client/` `init/` `sepolicy/`，
> 所以模块名就是 `autod` / `autodctl`（见 `daemon/Android.bp`）。
> `init/autod.rc` 与 `sepolicy/` 要真正生效，还需要按
> [`../02-native-daemon/README.md`](../02-native-daemon/README.md) 阶段 2 的清单接进产品。

编完的镜像就是 Windows 侧要跑的那个"开发好后的 arm64 ROM"。

---

## 常见故障

| 现象 | 原因 | 处理 |
|---|---|---|
| `libpulse.so.0: cannot open shared object file` | 缺系统库 | `apt-get install -y libpulse0`（本机已装） |
| `libGL.so.1: cannot open shared object file` | 缺系统库 | `apt-get install -y libgl1`（本机已装） |
| `adb devices` 一直为空 | 用了上游 QEMU，或镜像不是 goldfish 目标 | 用本目录的脚本；确认产物目录是 `emulator64_arm64` |
| 启动卡在开机动画 30 分钟以上 | TCG 的正常速度 | 耐心；`-gpu swiftshader_indirect` 已是最优无显示方案 |
| 截图全黑 | 显示后端没起来 | 用 `-gpu swiftshader_indirect`，别用 `-gpu off` |
| `/dev/uinput` 不存在 | 内核没编进去 | `adb shell modprobe uinput`；仍不行则改用真机验证触控 |
| `emulator: command not found` | 没进 `lunch` 环境 | 用本目录的 `run-emulator.sh`，它用绝对路径调 `prebuilts/android-emulator/` |

---

## 相关文档

- `autod` 主线与阶段划分 → [`../02-native-daemon/README.md`](../02-native-daemon/README.md)
- 本机环境（依赖、磁盘、镜像源） → [`../../docs/07-environment.md`](../../docs/07-environment.md)
- Android 12 触控约束 → [`../../docs/06-constraints.md`](../../docs/06-constraints.md)

---

## 结论（2026-09-28）：目标已由 `dev/06-x64-android` 达成

用户用**另一条路线**把"Linux 上跑 arm64 安卓"跑通了，实测证据见
[`../../06-x64-android/docs/07-verification-report.md`](../../06-x64-android/docs/07-verification-report.md)：

| 目标 | 结果 |
|---|---|
| 自编 **x86_64** Android 12 ROM | ✅ EXIT=0，62083 个编译目标 |
| **x86_64 Linux（KVM）** 同架构运行 | ✅ **`sys.boot_completed=1`**，开机 **23.8 s** |
| **arm64 应用能跑** | ✅ 自建纯 arm64-v8a 探针 APK：装成功、进程活、**16 条 `/system/lib64/arm64/*` 映射**、`primaryCpuAbi=arm64-v8a`、JNI 返回 `kernel=x86_64` |

### 两条路线的对比（为什么 06 的方案对、04 这条路走不通）

| | **04（本目录，全 arm64 系统模拟）** | **06（x86_64 ROM + 官方翻译层）** |
|---|---|---|
| 思路 | 让模拟器跑一个**真正的 arm64 系统** | 编一份 **x86_64 系统**，用 Google 官方的 **`libndk_translation`** 翻译 arm64 **用户态** |
| 硬件加速 | 只能 **TCG**（x86_64 宿主上没有 arm64 KVM） | **KVM**（同架构虚拟化） |
| 速度 | guest 时间≈实时，开机要 5~10 分钟 | **开机 23.8 秒** |
| 依赖 | 模拟器的 `ranchu` 机器必须能起（见下） | 官方镜像里的翻译层载荷 |
| 结果 | ranchu 打通了、boot 推进到 zygote+SurfaceFlinger，但**卡在图形栈** ✗ | **完整启动 + arm64 应用实测通过** ✅ |

**06 方案的关键**：翻译层不是运行时打补丁塞进去的，而是靠 **`TARGET_NATIVE_BRIDGE_ABI=arm64-v8a`**
让**编译系统**把 `/system/lib64/arm64/*`（59 个库）、`ndk_translation.rc`、`ld.config.arm{,64}.txt`、
`binfmt_misc` 注册项**直接写进 ROM** —— 这正是 04 在第 18 轮撞墙的地方：
运行时用 `adb push` / `mount --bind` / `debugfs` 塞库，
要么被 namespace 挡住 ✗、要么新增文件对 guest 不可见 ✗（见第 23、27 轮）。

### 04 这条路线的完整收获（仍有价值，已全部记录在本目录）

1. **`-cpu cortex-a53`**（第 8 轮）：模拟器默认 `cortex-a57` 会触发 TCG 执行 guest 原生代码的段错误
   （A12 的 keystore2、API 25 的 ART/AOT 崩溃都是它）✓
2. **`ranchu` 可以打通**（第 19 轮）：改 QEMU 二进制里的**设备别名表**
   （把 `virtio-serial` 的实现从 PCI 版换成 MMIO 版 `virtio-serial-device`）、
   字符串补丁 `-soundhw`→`-name` 去掉无条件的 PCI 音频设备、`ioeventfd=off`→`max_ports=511` ✓
3. **内核补丁 `ramoops`→`noramop`**（第 22 轮）：消除 guest 内核在 pstore 上的 panic ✓
4. **镜像手术的边界**（第 23、27 轮）：`debugfs` 只能"替换/删除已有文件"，
   **新增文件对 guest 不可见**（带 `shared_blocks` 的 e2fsdroid 镜像尤其如此）✓
5. 结论：**x86_64 宿主上想跑 arm64 安卓，正确的做法是"同架构 + 用户态翻译"（06 路线），
   而不是"跨架构全系统模拟"（04 路线）** ✓
