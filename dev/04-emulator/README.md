# 04 · 模拟器验证环境（QEMU）

> **不是产品轨道，是 `02-native-daemon` 的验证基础设施。**
> 目标：**不依赖真机**，在 x86_64 宿主上把 arm64 安卓跑起来，用来验证 `autod`。

| 路径 | 宿主 | 跑什么 | 用途 |
|---|---|---|---|
| [`linux-arm64/`](linux-arm64/README.md) | 开发机（Debian 13 x86_64，无显示） | `sdk_phone64_arm64` **原版镜像**（本机自己编） | 验证 `autod` 的截图与触控 |
| [`windows-arm64/`](windows-arm64/README.md) | Windows x64 | 编好之后的 **arm64 ROM 镜像** | 交付前的整机运行验证 |

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
