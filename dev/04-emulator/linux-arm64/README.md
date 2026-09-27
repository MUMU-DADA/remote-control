# linux-arm64 · 开发机跑原版 arm64 安卓

> 宿主：本机 Debian 13 x86_64（**无显示**、VMware 虚拟机、16 vCPU / 31 GiB）
> Guest：`sdk_phone64_arm64-userdebug`，自己从源码编出来的 **AOSP 12 原版 arm64 镜像**
> 用途：不依赖真机，验证 `autod` 的截图与触控链路

三个硬约束（跨架构只能 TCG、只有 goldfish 能跑、`aosp_arm64` 是 GSI）见
[`../README.md`](../README.md)。**动手前先读那一节，能省掉几小时的弯路。**

---

## 两条通道：arm64（默认）/ x86_64（`--fast`）

| | arm64（默认） | x86_64（`--fast`） |
|---|---|---|
| lunch 目标 | `sdk_phone64_arm64-userdebug` | `sdk_phone64_x86_64-userdebug` |
| 产物目录 | `out/target/product/emulator64_arm64/` | `out/target/product/emulator64_x86_64/` |
| 硬件加速 | 无（跨架构，只能 TCG） | **KVM** |
| 开机耗时 | 10~40 分钟 | **几十秒** |
| 定位 | 最终验证；**Windows 侧 ROM 的来源** | 开发机日常内循环 |

```bash
./build-images.sh --fast     # 编 x86_64 镜像（与 arm64 是两套 target 产物，host 工具复用）
./run-emulator.sh --fast     # 起 x86_64 模拟器（脚本会自动用 -accel on 走 KVM）
./smoke-autod.sh --fast      # 用 x86_64 的产物冒烟
```

源码级验证（截图链路 / 触控注入 / 协议 / 分发）两者完全等价，
只有 ABI 相关的疑虑（指针宽度、对齐、bionic 差异）才必须回到 arm64。

切换方式三选一：`--fast` 只影响本次；`export EMU_ABI=x86_64` 影响整个 shell；
`LUNCH_TARGET=... PRODUCT_DEVICE=...` 可完全自定义目标。

---

## 实测踩到的坑（2026-09-28 在本机逐条验证）

跑通这条路的成本几乎全在这几个坑上，记下来免得重踩。

### 1. 必须让模拟器进"构建模式"，光有 `-sysdir` 不够

单独用 `emulator -sysdir <dir>`，30.8.3 会直接报：

```
emulator: ERROR: No AVD specified. Use '@foo' or '-avd foo' to launch a virtual device named 'foo'
```

它需要 **`ANDROID_PRODUCT_OUT` 环境变量**才进入"从构建产物直接启动"的模式。
`source build/envsetup.sh`（`lunch`）会设这个变量，所以官方流程里看不出来。
`run-emulator.sh` 已经替你设好。

### 2. 它靠 `<sysdir>/system/build.prop` 判断 guest 架构

只给 `-sysdir` 而缺这个文件时，模拟器**退回宿主架构 x86_64** —— 日志里会出现
x86 专属的 `pc_memory_init` 与 CPUID 警告，arm64 内核根本不会启动。

- AOSP 产物天然有 `system/build.prop` ✓
- SDK 成品镜像的 `build.prop` 在**顶层** ✗ → 要手工补一份到 `system/build.prop`

### 3. `-initrd` 指向的是 `<sysdir>/initrd`，不是 `ramdisk.img`

AOSP 产物里有这个文件；SDK 成品镜像没有 → QEMU 拿到不存在的 initrd，
**主循环立刻结束且不报任何错**（日志停在 `emulator: Done with QEMU main loop`）。
补 `cp ramdisk.img initrd` 即可。

### 4. `-gpu swiftshader_indirect` 在 Debian 13 上让模拟器段错误

```
dmesg: emulator[261357]: segfault at 94 ip 00000000004434b0 ... in emulator[...]
```

崩在初始化阶段，日志停在"下发 adb 公钥"之后，**没有任何错误输出**，极难定位。
改用 **`-gpu guest`**（guest 侧软件渲染）即正常 —— 画面仍然真实合成，
`screencap` / `captureDisplay()` 不受影响。`run-emulator.sh` 已把默认值改成 `guest`。

### 5. 新版模拟器不再支持在 x86_64 宿主上跑 arm64

拿 SDK 最新的 37.1.11 跑同一份 arm64 镜像：

```
INFO | Found build target architecture: arm64
FATAL | QEMU2 emulator does not support arm64 CPU architecture
```

Google 在新版里去掉了 ARM 的软件模拟后端。**arm64 guest 只能用旧版模拟器** ——
AOSP 树自带的 30.8.3 正好是"旧版 + 与镜像同源" ✓。这也意味着 Windows 侧
不能直接用最新版（见 [`../windows-arm64/README.md`](../windows-arm64/README.md)）。

### 6. SDK 成品镜像带版本依赖

成品镜像的 `source.properties` 会写 `Pkg.Dependencies=emulator#31.2.7`，
要求模拟器 ≥ 该版本。用 30.8.3 跑 API 31 的**成品镜像**会因不匹配而启动失败；
**自己编的镜像没有这个问题**（同一棵树、同一版本）。

### ⭐ arm64 的关键修复：把 QEMU 机器换成带 PCIe 的 `virt`

**症状**（曾让 arm64 完全跑不起来，且日志毫无线索）：

```
qemu-system-aarch64-headless: PCI bus not available for hda
emulator: Done with QEMU main loop          ← QEMU 创建设备失败，直接退出
```

没有 guest 内核输出，`adb` 永远看不到设备，看起来像"arm64 在 x86_64 上跑不了"。

**根因**：模拟器给 arm64 guest 挂了 **16 个 PCI 设备** ——
`virtio-serial-pci`、`virtio_input_multi_touch_pci_1..11`、`virtio-wifi-pci`、
`virtio-vsock-pci`，外加 `-soundhw hda`（开了 `VirtioSndCard` 特性则变成
`virtio-snd-pci`）。而 arm 的 **`ranchu` 机器没有 PCI 总线** → QEMU 建不出设备、直接退出。

**解决**：用 `-qemu` 透传，把机器换成带 PCIe 的 `virt`：

```bash
./run-emulator.sh            # 脚本已内置，arm64 自动加 -qemu -machine type=virt
# 想退回原机器： EMU_MACHINE_OVERRIDE=none ./run-emulator.sh
```

实测对比（同一份成品镜像、同一台机器）：

| | 改之前 | 改之后 |
|---|---|---|
| QEMU 存活 | < 1 秒即退出 | 持续运行 |
| `PCI bus not available` | 出现 | **0 次** |
| `adb devices` | 空 | `emulator-5554` 出现并上线 |

**排除过的无效尝试**（记下来免得再走一遍）：

| 尝试 | 结果 |
|---|---|
| `-no-audio` / `-audio none` / `QEMU_AUDIO_DRV=none` | 音频设备照样被拼进命令行 |
| 改 `hardware-qemu.ini` 的 `hw.audio*` | 构建模式下由模拟器重写，无效 |
| 镜像 `advancedFeatures.ini` 加 `VirtioSndCard = on` | 只是把 `hda` 换成 `virtio-snd-pci`，仍是 PCI |
| AVD 模式（`-avd`，配置里音频已关） | 模拟器段错误 / 报 Broken AVD system path |
| 换 31.x 模拟器 | 同样的 PCI 报错 |

### 当前进展与未解问题（2026-09-28，逐条实测）

**已解决 ✓**

| 项 | 证据 |
|---|---|
| arm64 模拟器在 x86_64 Linux 上**能启动** | QEMU 持续运行，不再秒退 |
| arm64 guest **内核 + userspace 起来** | `-show-kernel` 可见 `init`/`ueventd`/HAL 服务全启 |
| **adb 认到设备** | `adb devices` → `emulator-5556` |
| 分区挂载正常 | `dm-0..dm-3` = system/system_ext/product/vendor，`/vendor` 挂载成功 |
| vendor HAL 正常 | `class_start hal` succeeded，无 HAL 崩溃 |

**未解决 ✗**：guest 起来后 **zygote 段错误**（API 30 镜像）／**keystore2 段错误**
（API 31 与自制 A12 镜像都一样），导致启动无法完成 → init 走 `abort_fuse` → 模拟器退出。

**崩溃签名（已定位到具体机制）**：

```
signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0xfdffff864f0d0011
#00 libpuresoftkeymasterdevice.so (keymaster::PureSoftRemoteProvisioningContext::GenerateBcc(bool)+144)
```

故障地址高字节 `0xfd` 是 **TBI（Top Byte Ignore）标签** —— guest 启用了 arm64 的
tagged-address ABI，Scudo/堆标签把校验值写进指针高字节，而 **2021 版 QEMU fork 的 TCG
在 `virt` 机器上没有正确屏蔽高字节** → 解引用即 SEGV。

**修法方向**（下一步）：在最早期的 init 里执行
`write /proc/sys/abi/tagged_addr_dis 1` —— 内核禁掉这个 ABI 后，bionic 的
`prctl(PR_GET_TAGGED_ADDR_CTRL)` 会失败、自动走无标签路径（x86_64 上本来就是这样）。
BoringSSL 自检在同一台机器上从 fail 变 pass，也说明这类"看起来像加密库崩溃"的问题
根子在 TCG 的 CPU 特性模拟上。

⚠️ **改 system.img 的坑**：模拟器/构建会生成 `system-qemu.img`、`vendor-qemu.img`、
`ramdisk-qemu.img`、`product-qemu.img`、`system_ext-qemu.img`（见
`build/make/core/Makefile` 的 `INSTALLED_QEMU_*`），**guest 实际挂的是这些副本**。
只改 `system.img` 不会生效（日志里 init.rc 行号仍是原始值即可判定），
必须先用 `m` 重新生成这些 `*-qemu.img`；而**直接删掉它们会让 first-stage init 崩溃、guest 重启**。


已排除的尝试（都无效）：

| 尝试 | 结果 |
|---|---|
| `-cores 1 / 2 / 4 / 6`（排除 MTTCG 竞态） | 单核同样崩 |
| `-cpu cortex-a53`（关掉加密扩展，排除 TCG 密码学 bug） | BoringSSL 自检从 fail 变 pass，但 keystore2 仍崩 |
| `-prop dalvik.vm.usejit=false`（排除 JIT/指令缓存一致性） | zygote 仍崩 |
| 清理历史 qcow2/`data` 覆盖层 + 全新 datadir | 同样崩 |
| `-memory 3072 / 4096` | 无变化 |
| 换模拟器版本（30.8.3 / 31.x） | 都崩，位置相同 |

判断：这更像 **2021 年的 QEMU fork 的 arm64 TCG 与现代 Android 用户态代码的兼容问题**
（Google 也正是因此在 34+ 版本里去掉了 x86_64 宿主上的 arm64 支持）。

**下一步（最有希望的对照实验）**：用**自己编的 arm64 镜像**（`sdk_phone64_arm64`，
与模拟器同一棵树、同一版本）再跑一次 —— 若同样崩，即可确认是 TCG 层面的限制，
届时 arm64 只能走真机 / arm64 宿主；日常开发用 `--fast`（x86_64 + KVM）那条已经验证可用的路。

### 成品镜像（`get-stock-image.sh`）

`get-stock-image.sh` 下载 Google 官方的 arm64 成品镜像
（默认 `system-images;android-31;default;arm64-v8a`，606MB，SHA1 与清单一致 ✓；
`--api 29/30/31` 可切换，`run-stock-image.sh` 直接以构建模式启动它 ——
不需要 AVD、不需要 SDK 布局）。用途：还没编出自己的镜像时先验证环境，
或在自制镜像出问题时做对照（与自制镜像**共用同一套启动参数**，
所以能直接判断问题出在环境还是镜像）。

镜像里的 `build.prop` 在顶层，构建模式需要 `system/build.prop`；
`initrd` 也缺 —— 这两个由 `get-stock-image.sh` / `run-stock-image.sh` 自动补齐（见坑 2、3）。

---

## 前置条件

```bash
./check-env.sh          # 一次性检查，缺什么会直接给修复命令
```

| 依赖 | 说明 | 本机状态 |
|---|---|---|
| `libpulse0` | 模拟器二进制硬依赖，缺了连 `-version` 都跑不起来 | ✅ 已装 |
| `libgl1` | 非 headless QEMU 后端 / Qt 依赖 | ✅ 已装 |
| `prebuilts/android-emulator` | AOSP 自带模拟器 **30.8.3**，含 `qemu/linux-x86_64/qemu-system-aarch64` | ✅ 已同步 |
| `kernel/prebuilts/5.10/arm64/kernel-5.10-gz` | arm64 模拟器用的 GKI 内核 | ✅ 已同步 |
| `out/target/product/emulator64_arm64/` | 镜像产物 | ⏳ 需 `build-images.sh` |
| `adb` | 宿主没装，用 AOSP 编出来的 | ⏳ 随构建产出 |

`prebuilts/qemu-kernel`、`sdk`、`external/qemu` 不用管：前者只服务 x86/arm32 模拟器，
后两者是模拟器自身源码（我们用预编译包）与 SDK 打包，都不参与这条路径。

---

## 三步跑通

### 1. 编镜像

```bash
./build-images.sh               # 全量：lunch sdk_phone64_arm64-userdebug && m
```

- 首次：**数小时**，`out/` 涨到 ~100GB（见 `docs/07-environment.md` 的占用预期表）
- 改完 `autod` 只想编二进制：`./build-images.sh --modules`（分钟级）
- 想后台跑：`./build-images.sh --detach`，然后 `tail -f .run/build-*.log`

产物目录是 **`out/target/product/emulator64_arm64/`**，关键文件：

```
system.img  vendor.img  ramdisk.img  kernel-ranchu     ← 起模拟器的最低要求
userdata.img  encryptionkey.img  advancedFeatures.ini
```

### 2. 起模拟器

```bash
./run-emulator.sh
```

脚本做的事：用绝对路径调 `prebuilts/android-emulator/linux-x86_64/emulator`
（包内的 `lib64`、Qt、`libtcmalloc` 由它自己加进 `LD_LIBRARY_PATH`，**不要直接敲
`qemu/linux-x86_64/qemu-system-aarch64`**，那样会缺库）、传 `-sysdir` 指向产物目录、
等到 `sys.boot_completed=1`、`adb root`，最后把设备信息与 `/dev/uinput` 状态打出来。

默认参数（可用环境变量覆盖）：

| 参数 | 值 | 原因 |
|---|---|---|
| `-no-window` | 固定 | 本机没有 `DISPLAY`，也没装 Xvfb |
| `-gpu swiftshader_indirect` | `EMULATOR_GPU` | guest 侧真实合成画面 → `captureDisplay()` 有内容；不要用 `-gpu off` |
| `-accel off` | 固定 | 跨架构本来就没加速，显式写出来 |
| `-memory 4096` / `-cores 4` | `EMULATOR_MEMORY_MB` / `EMULATOR_CORES` | TCG 吃内存 |
| `-datadir .run/data` | `--datadir` | userdata 覆盖层留在项目里，不污染 AOSP 的 `out/` |
| 超时 2700s | `BOOT_TIMEOUT_S` | TCG 首次启动慢 |

> **第一次启动请按 10~40 分钟预期。** 这是 x86_64 宿主跑 arm64 guest 的物理下限，
> 不是脚本问题。后续启动会快一些（仍是分钟级）。

### 3. 冒烟测试

```bash
./smoke-autod.sh
```

按 `02-native-daemon` 的**阶段 1** 路径走：`adb push` 到 `/data/local/tmp`、
root 身份前台跑、完全不碰 `/system` 与 SELinux。会依次验证：

1. `info` 协议往返
2. `capture`（PNG，设备上走 `AndroidBitmap_compress`，和 `screencap` 同一条路）
3. `capture --raw`（PPM）→ 拉回开发机**随机 seek 采样**，自动判定画面是
   全黑 / 近乎纯色 / 有真实内容
4. `tap` 屏幕中心、`swipe` 中线向上

产物落在 `.run/shots/`。

---

## 常用操作

```bash
./run-emulator.sh --no-wait          # 起了就返回
./run-emulator.sh --wipe-data        # userdata 坏了就重来
./run-emulator.sh --writable-system  # 允许 adb remount（想推 /system/bin 时）
./run-emulator.sh --stop             # 停掉
./run-emulator.sh --tail             # 跟控制台日志
./smoke-autod.sh --stop              # 停掉设备里的 autod

# 直接连
ADB=aosp/out/host/linux-x86/bin/adb
$ADB -s emulator-5554 shell
$ADB -s emulator-5554 logcat -s autod:*
```

想用 `../02-native-daemon/tools/deploy_cuttlefish.sh`（它把二进制推进 `/system/bin`）：
先 `./run-emulator.sh --writable-system`，再执行那个脚本——它是纯 adb 的，对模拟器同样有效。

---

## 故障排查

| 现象 | 处理 |
|---|---|
| `Cannot find system image` / 缺 `kernel-ranchu` | 产物目录拿错了：必须是 `emulator64_arm64`，`generic_arm64` 是 GSI |
| `adb devices` 空 | 看 `.run/emulator-5554.log`；多半是镜像不是 goldfish 目标 |
| 卡开机动画很久 | TCG 正常速度；`./run-emulator.sh --tail` 看控制台 |
| 截图全黑 | 确认 `EMULATOR_GPU=swiftshader_indirect`；`--no-frame-check` 跳过后手工看 PNG |
| `/dev/uinput` 缺失 | 脚本会自动 `modprobe uinput`；仍没有就是内核没编进去，触控只能等真机 |
| `-datadir` 相关报错 | 去掉它再试：`./run-emulator.sh --datadir "$HOME/.android/avd"`，或改回默认目录 |
| 模拟器起来但 `emulator-5554` 被别的实例占用 | `EMULATOR_PORT=5556 ./run-emulator.sh` |

---

## 为什么不是 Cuttlefish

`02-native-daemon/README.md` 早期把 Cuttlefish 定为验证路径，这里说明为什么换掉：

1. **Android 12 的 Cuttlefish 是 `launch_cvd` 老工具链**，不是新版 `cvd`；
2. **Cuttlefish 不支持跨架构**：crosvm 没有 TCG，arm64 guest 只能在 arm64 宿主上跑，
   本机是 x86_64，直接出局；
3. Cuttlefish 需要 KVM + 一堆宿主配置（网络、`virsh`、专用内核），
   而模拟器这条路 AOSP 自带、零额外依赖。

代价是慢（TCG）。但 `autod` 的验证目标是"能拿到帧、能点中"，慢不影响结论。
