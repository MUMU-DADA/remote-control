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

**已尝试但不够的修法**：在 system 镜像的 `init.rc` 的 `on early-init` 里加
`write /proc/sys/abi/tagged_addr_dis 1`（补丁确实执行了：日志里能看到我插入的
`TCG-TBI-PATCH` kmsg 标记，init.rc 行号也从 17 变成 21）。
但 keystore2 的崩溃现场仍是 `tagged_addr_ctrl: 0000000000000001` —— 因为
**TBI 是由 init 在自身 bionic 初始化时开启的、并被所有子进程继承**，
early-init 这个时机已经太晚（sysctl 只能拦住之后的 `prctl`，拦不住继承）。
→ 下一步要在**内核/CPU 层面**关掉 TBI（例如用 QEMU 的 CPU ID 寄存器覆盖
把 `ID_AA64MMFR2_EL1.TBI` 之类关掉），而不是在 init.rc 里补。

⚠️ **改 system.img 的坑**：构建会生成 `system-qemu.img`、`vendor-qemu.img`、
`ramdisk-qemu.img`、`product-qemu.img`、`system_ext-qemu.img`（见
`build/make/core/Makefile` 的 `INSTALLED_QEMU_*`），**guest 实际挂的是这些副本**。
只改 `system.img` 不会生效（用日志里 init.rc 的行号即可判定），
必须再用 `m` 重新生成这些 `*-qemu.img`；而**直接删掉它们会让 first-stage init 崩溃、guest 重启**。



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

---

## 第 3 轮进展：又清掉一个阻塞，并定位到新的（宿主侧）阻塞

### 修复 ✓：`Rebooting into recovery`（启动被踢进 recovery）

症状（自制 A12 与成品 A11/A12 **三个镜像完全一致** → 说明是环境问题，不是镜像问题）：

```
init: Failed to set encryption policy of /data/misc ...: Directory not empty
avc: denied { execute_no_trans } for comm="init" path="/vendor/bin/toybox_vendor"
init: ls -laZ /data/misc returned failure: 255
init: Setting <policy> on /data/misc failed!
init: Rebooting into recovery
```

`ENOTEMPTY` 本身是**正常**的（vold 会先建 `/data/misc/vold`，而 fscrypt 策略只能设在空目录上），
AOSP 的兜底逻辑会去跑 `ls -laZ`（`libfs_mgr` 的诊断输出）——这里那条 `ls`
（PATH 里的 `/vendor/bin/ls → toybox_vendor`）执行失败，于是 init.rc 第 657 行的
`mkdir /data/misc 01771 system misc encryption=Require` 被判定失败 → recovery。

**解法**：把 system 镜像 `init.rc` 第 657 行的 `encryption=Require` 改成
**`encryption=Attempt`**（init.rc 的标准取值：尽力而为、失败不致命）。
→ **`Rebooting into recovery` 归零** ✓，启动继续走到 APEX 解压、各 HAL、`zygote` 阶段。

（同时把构建产物里的 `root/init.environ.rc` 补进了模拟器实际加载的 `initrd` —— 原本缺失。）

### 当前阻塞 ✗（新，在宿主侧）

guest 跑到 **~50–70 秒（HAL 阶段）** 时 **QEMU 自身段错误**，模拟器随之退出：

```
dmesg: qemu-system-aar[...]: segfault at 0 ip 0000000000dd68b1 / 0000000000dc6cbb
```

两个 IP 相邻 → 同一处代码，确定性复现；此时 **guest 侧零崩溃**（不是 guest 的问题）。

已试：`-feature -VirtioInput,-VirtioMouse,-VirtioWifi,-VirtioVsockPipe,-VirtconsoleLogcat`
（关掉 11 个多点触控 + 鼠标 + wifi + vsock + 串口）→ **仍崩** ✗。

判断：这是为了绕开"ranchu 无 PCI"而硬换 `virt` 板的代价 —— 模拟器给 arm 板挂的 PCI
设备（其中 `-soundhw hda` 无法用 feature 关掉）在 `virt` 上跑到某个操作时把 QEMU 弄崩。

**下一步**：① 用 `-verbose` 列出 `virt` 下实际保留的 PCI 设备逐个 bisect；
② 或换用真正带 PCI 的 arm 机器（如 `sbsa-ref`）来承载这些设备。

---

## 第 4 轮：宿主侧段错误的根因彻底定位

上轮留下的"QEMU 自己段错误"（`dmesg: segfault at 0 ip 0xdd68b1`，core dump 显示
`rdi=0`，即空指针解引用）本轮查清了：

**现象**：把 guest 的 HAL 一个个摘掉，崩溃就"转移到下一个"：
`vendor.usb-hal` → `vendor.wifi_hal_legacy` → `vendor.power-default` …
**规律**：凡是访问 **ranchu 专有 MMIO 设备**（goldfish 电池 / pstore / USB / WiFi / power …
连内核驱动读电池也算）的访问，在 `virt` 板上都会让 QEMU 崩。

**根因（两条路都堵）**：

| 板子 | 状态 |
|---|---|
| `ranchu`（官方支持、设备齐全） | **没有 PCI 总线** ✗，而模拟器**无条件**要挂 PCI 音频设备（`-soundhw hda` 或 `virtio-snd-pci`，两个模拟器版本都一样，无 feature 可关、`hw.audioOutput=false` 与 `-no-audio` 都不影响它）→ QEMU 创建设备时直接 `exit(1)` |
| `virt`（换板绕法） | 能过 PCI 这一关、能把 guest 启到 zygote ✓，但 guest 一碰 ranchu 专有设备就把宿主 QEMU 弄崩 ✗ |

→ **结论：这套 AOSP 模拟器（30.8.3 / 31.x）在 x86_64 宿主上跑不了 arm64 Android**，
与 Google 从 34+ 起移除 x86_64 上的 arm64 支持是一致的。

### 已验证可用的替代路径

1. **x86_64 guest + KVM**（秒级启动，本机已验证可用 ✓）—— `--fast` 通道，日常开发用它；
2. **真机 arm64**（adb）—— 做 AArch64 真实行为验证；
3. **arm64 宿主**（如 ARM 云主机）—— 在那里 arm64 模拟器可走硬件加速。

### 本轮之前修好的两项（对 arm64 宿主同样有价值）

- `patches/0001-tcg-arm64-pure-soft-rkp.patch`：keystore2 的 `GenerateBcc()` 崩溃 → 12 次降到 0；
- system 镜像 `init.rc` 的 `mkdir /data/misc … encryption=Require` → **`Attempt`**：
  消除 `Rebooting into recovery`（三个镜像一致复现的那个阻塞）。

### 仍未试的一条（如果还要继续）

**AVD 模式**：模拟器在 AVD 模式下会从 AVD 的 `config.ini` 生成硬件配置，
`hw.audioOutput = no` 有可能**真正**去掉音频设备 → 那么 `ranchu` 可用 → guest 拿到
真设备 → 不再触碰缺失的 MMIO。需要在 SDK 布局里补 `package.xml` 元数据
（之前 AVD 模式报的 `Package path is not valid` 正是缺它）。

---

## 第 5 轮：AVD 模式也试过了（结论不变，并补一条关键证据）

**动机**：`-sysdir`（构建）模式会**重新生成**硬件配置，我改 `config.ini` 里的
`hw.audioOutput = false` / `hw.audioInput = false` 都不影响 `-soundhw hda`。
AVD 模式理论上会**以 AVD 的 config.ini 为准**，所以值得一试（顺带还能解决 `misc` 分区）。

**做法**：手工搭了正规 SDK 布局 + AVD（`abi.type=arm64-v8a`、`image.sysdir.1`、
`hw.audioInput/Output=no` 等），并把 `system-images/` 链到模拟器真正认的 SDK 根。

**结果**：模拟器**始终拒绝**这个 AVD —— 依次遇到
`Cannot find AVD system path` → `Broken AVD system path`（无论 sysdir 用相对/绝对、
符号链接/硬链接、镜像用 Google 原版还是自制、元数据 `source.xml`/`package.xml` 补齐与否）。
日志显示它把 `argv[0]` 的**真实路径**当作程序目录（
`program directory: .../prebuilts/android-emulator/linux-x86_64`），
因此在树外的镜像布局上反复判定失败 —— 30.8.3 的这个校验属于"只认标准 SDK 安装"的怪癖。

**但核心问题已经在构建模式下得到答案**：同一个模拟器代码里，
`hw.audioOutput=false` + `hw.audioInput=false` + `-no-audio` **都不能**阻止音频设备创建，
而 30.8.3/31.x 的 QEMU 只提供 `ac97`/`es1370`/`hda` 三张声卡（**全是 PCI**），
`-soundhw none` 不被接受 → **ranchu 板（无 PCI）永远起不来**。
所以 AVD 模式即使能启动，也绕不开这一条。

**至此定论（有完整证据链）**：这套 AOSP 模拟器在 **x86_64 宿主上无法运行 arm64 Android**；
开源侧可行的 arm64 Android 模拟只有两条：**arm64 宿主**（AOSP 模拟器 / Cuttlefish + KVM）
与 **真机 arm64**。日常 x86_64 开发用 KVM 通道（已验证可用）。

清理：AOSP 树里临时加的 `system-images` 链接已移除；AVD 尝试物保留在
`dev/04-emulator/linux-arm64/.run/asdk` 与 `/root/avd`（`.run/` 已 gitignore）供复现。

### 第 5 轮补充：又有三种绕法被实测排除（含一条决定性证据）

在 AVD 模式之外，本轮还试了两种"直击要害"的绕法，都被实测排除：

1. **用包装脚本替换 QEMU 二进制**（想过滤掉所有 PCI 设备参数）——失败 ✗。
   教训：模拟器打印的那条 QEMU 命令行是它**内部构造**的，它并不是用 QEMU 参数去 exec
   `qemu-system-*`；而 `qemu-system-*` 本身就是**模拟器二进制**，靠 `argv[0]` 判断自己
   该当"模拟器前端"还是"QEMU"。所以从外面拦不到设备列表。（包装已还原，无残留。）

2. **把 `hardware-qemu.ini` 设成不可写**（`chattr +i`）并写入 `hw.audioOutput = false`
   / `hw.audioInput = false`，逼模拟器读我的配置 —— **`-soundhw hda` 照样出现** ✗。
   ⇒ **决定性结论：音频设备是模拟器代码里无条件创建的，与任何配置/开关无关**
   （`hw.audioOutput`、`hw.audioInput`、`-no-audio` 全部无效）。
   而 30.8.3/31.x 的 QEMU 只提供 ac97/es1370/hda 三张声卡（全为 PCI），`-soundhw none` 不被接受。
   ⇒ ranchu（无 PCI 总线）在这套二进制下**不可能启动**。

3. AVD 模式（见上）——被模拟器"只认标准 SDK 安装"的 sysdir 校验拦住 ✗。

### 剩下唯一一条没走的路

**从源码构建模拟器并打一个小补丁**（把音频设备改成条件创建，或按 `hw.audioOutput` 判断）：
- 本树是 `.repo` 检出，可以只同步 `platform/external/qemu`（android-12.0.0_r34 对应分支）；
- 模拟器自带构建脚本与 prebuilts（`prebuilts/android-emulator/` 已在树里）；
- 补丁生效后 `ranchu` 可启动，guest 拿到**真正的 goldfish 设备**，之前所有
  "访问缺失 MMIO 导致 QEMU 崩"的问题一并消失，**有望一路启动到底**。
- 代价：同步 + 构建，量级为小时级；有一定不确定性（模拟器构建链较老）。

---

## 第 6 轮：镜像级就地打补丁的可行性与边界

**新掌握的可靠手法（可复用）**：模拟器实际传给 QEMU 的 `vendor-qemu.img` / `system-qemu.img`
是**整盘镜像**（GPT + 一个分区，分区起始 LBA 2048）。可以不解包整个 `super`，
直接按 GPT 解析出分区偏移 → `dd` 出分区 → `debugfs` 改内容 → `dd` 写回，
即可**在编译之外**修改 guest 看到的分区（已实测生效：删掉的 rc 文件在回读中确认消失）。

**但本轮也撞到了两个硬边界**：

1. **guest 的 `/vendor` 内容会被"还原"**：无论把 `vendor-qemu.img`、`vendor.img`
   两份都打上补丁（确认两边都不再有 usb/wifi/sensors/thermal/health/media 的 rc），
   guest 仍然会启动 `vendor.usb-hal-1.0`（74 s 处照样宿主段错误）。
   说明这些服务定义来自**模拟器内部的设备/分区映射**，而不是我能直接改的那几个文件；
   继续靠"删文件"来回避崩溃是打不赢的（这也印证了第 4/5 轮的结论）。

2. **崩溃点随被删掉的组件"移动"**：usb → wifi → power/health（电池）→ …，
   每次都是 guest 访问 ranchu 专有 MMIO 时把宿主 QEMU 弄崩（此时 guest 侧零崩溃）。

**关于"从源码重建模拟器"这条唯一剩下的路，本轮把可行性摸清了**：

- ✅ 本树用**清华 AOSP 镜像**同步（`mirrors.tuna.tsinghua.edu.cn/git/AOSP/...`，
  `android-12.0.0_r34`），镜像上**有** `platform/external/qemu`（实测 HTTP 200）；
- ⚠️ 但 AOSP manifest（1048 个项目）里**不含** `platform/external/qemu`，
  且构建它所需的 `prebuilts/android-emulator-build` **不在树里**；
- 结论：需要额外取 qemu 源码 + 构建工具链，属**小时级、且有不确定性**的工作；
  补丁目标很明确（让音频设备按 `hw.audioOutput` 条件创建，或直接不创建），
  一旦成功，`ranchu` 可启动、guest 拿到真设备，前面所有"访问缺失 MMIO 导致 QEMU 崩"的问题会一并消失。

清理：`super.img` 已恢复，产物目录保持可用；无编译/模拟器残留。
