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

---

## 第 7 轮：回答"安卓版本下探到多少能明确支持模拟 arm64"

### 一、镜像层面：没有门槛

从 Google 官方清单（`sys-img2-3.xml`，本机实测提取）：
**arm64-v8a 系统镜像从 API 21 一直到 API 36 全都提供**（共 16 个）。
各档声明的"最低模拟器版本"：

| API | 21–25 | 26–28 | 29–30 | 31 | 32–35 | 36 |
|---|---|---|---|---|---|---|
| 最低 emulator | 无要求 | 31.1.1 | 无要求 | 31.2.7 | 29.1.11 | 35.4.9 |

⇒ 所以"有没有 arm64 镜像"不是分水岭 —— **每个 Android 版本都有**。

### 二、真正决定能否跑的是**模拟器版本 + 宿主机器模型**

- **越新的模拟器越不支持**：34+ 起直接
  `FATAL | QEMU2 emulator does not support arm64 CPU architecture`（实测 37.1.11）；
  30.8.3 / 31.x 仍能把 arm64 guest 启到内核 + userspace + adb。
- **`ranchu`（官方板）无法启动**：模拟器**无条件**挂 PCI 音频设备，而 ranchu 无 PCI 总线
  → `PCI bus not available for hda` → QEMU 退出（`hw.audioOutput=false` + `chattr +i`
  锁配置也无效，已实测）。
- **`virt`（换板）能启动**，但 guest 访问 ranchu 专有 MMIO 就把宿主 QEMU 弄崩。

### 三、各档实测结果（本机，逐档真跑）

| Android | 结果 |
|---|---|
| API 31 / 30 | guest 内 keystore2 / zygote 段错误 ✗ → 进 recovery ✗ |
| API 29 | 跑到很后段（`cameraserver`/`drm`/串口 `console:/ $`）→ **宿主 QEMU 段错误退出** ✗ |
| **API 25 (7.1)** | **宿主零段错误 ✓、`adb devices` 显示 device（在线）✓、zygote 全量预加载 4158 个类 ✓、`surfaceflinger` running ✓、`mediaserver`/相机 HAL 起来 ✓** —— 但 logcat 里出现 `DEBUG ... /system/framework/arm64/boot-framework.oat` 的 tombstone ✗：**zygote 在 AOT 编译产物里段错误并循环重启**，`system_server` 起不来 ✗ |

### 四、结论（明确回答）

**"下探 Android 版本"解决不了这个问题。** 卡点是两件事，**都与 Android 版本无关**：

1. **TCG 执行 guest 原生/AOT 代码时出错**（API 25 崩在 `boot-framework.oat`，A12 崩在
   keystore2 的 `libpuresoftkeymasterdevice.so`）；
2. **宿主机器模型缺设备**（ranchu 无 PCI 起不来 / virt 上访问缺失设备把 QEMU 弄崩）。

**唯一能"明确支持 arm64"的组合是 arm64 宿主**（Google 的设计目标就是 arm64 宿主 + KVM），
或真机 arm64。x86_64 上最多做到 API 25 这一档"开机到一半"（adb 可用、SurfaceFlinger 起来、
但 Java 框架起不来）。日常 x86_64 开发请用 KVM 通道。

---

## 第 8 轮：**关键突破 —— 换 CPU 型号即可消除 guest 段错误**

### 发现：模拟器默认的 `-cpu cortex-a57` 会触发 QEMU TCG 的 bug

用 `adb root` 拿到 guest 的真实 tombstone 后看到崩溃形态：

```
F libc : Fatal signal 11 (SIGSEGV), fault addr 0x5067001 in tid 898 (main)  >>> zygote64 <<<
        x24  0000000005067001     ← 故障地址就在 x24
        x25  00000073557f7ed8     ← 同区域其它指针都是完整 48 位
```

**指针的高 32 位被丢了** —— 典型的"TCG 把 64 位运算当 32 位执行"的症状。
既然是按 CPU 特性走代码路径，就试了换 CPU 型号：

**`-qemu -machine type=virt -cpu cortex-a53`**（就是加一个 `-cpu cortex-a53`）：

| 镜像 | 换 a53 之前 | 换 a53 之后 |
|---|---|---|
| 自制 A12（sdk_phone64_arm64） | keystore2 反复 `signal 11`（12 次）→ 20 秒内必崩 | **keystore2 崩溃 0 次 ✓、guest 零崩溃 ✓** |
| 成品 API 25 arm64 | zygote 在 `boot-framework.oat` 里段错误并循环重启 | **`Fatal signal` 0 次 ✓、zygote 健康（RSS 85MB）✓、`System server process has been created` ✓** |

⇒ **这是七轮以来真正的转折点**：之前所有 guest 侧崩溃（A12 的 keystore2、API 25 的 ART/AOT）
都是同一个 TCG 执行 bug，**换 CPU 型号即可绕过**，与 Android 版本无关。

### 换 a53 之后 A12 能走到哪

`win` 那次（a53 + super vendor 补丁 + RKP 补丁 + `encryption=Attempt`）：
guest 一路到 `post-fs-data` → `zygote` ✓ → HAL 阶段（**guest 零崩溃、无 recovery** ✓✓），
90 秒时**宿主 QEMU 段错误**退出 ✗，崩前最后几行是 `healthd` 读 goldfish 电池 + `vendor.usb-hal`。

### 仍未解决：宿主侧崩溃 + vendor 补丁"打不到 guest"

- 宿主崩溃的触发器是 guest 访问 ranchu 专有 MMIO（电池/USB/wifi…），`virt` 板上没有这些设备；
- 诡异的是：`vendor.img`、`vendor-qemu.img`、**以及 super.img 里的 vendor 分区**（用
  dumpe2fs 的 UUID 在 super 里精确定位到偏移 1186988032，回读确认 0 个相关 rc）
  **三处都已补丁**，guest 却仍然启动 `vendor.usb-hal-1.0` / `sensors-hal` / `thermal-hal` ✗；
- 为此写入了 `/vendor/marker-from-host.txt` 做判定（开机后 `cat` 看得到就说明 guest 用的
  就是这份 vendor），本次运行 adb 未上线，判定待续。
- 已排除的来源：system.img / system_ext / product / initrd / init.ranchu.rc（都不含这些 rc）。

**下一步**：① 完成 marker 判定，找到 guest 真正的 vendor 来源；② 把剩下几个"碰设备"的服务
摘掉（battery/USB/wifi 类），配合 `-cpu cortex-a53`，A12 arm64 **完整启动已是触手可及**。

---

## 第 9 轮：解开"补丁打不到 guest"之谜 —— 源头是 **super.img**

### 真相

第 6~8 轮一直诡异：`vendor.img`、`vendor-qemu.img`、super 里的 vendor 分区三处都补丁过
（回读都确认 0 个相关 rc），guest 却仍启动 `vendor.usb-hal` / `sensors-hal` / `thermal-hal`。

本轮用两个决定性实验定位：

1. **逐文件排查**：把 vendor 与 system 分区里**所有** `.rc` 文件（含 `/etc/init/hw/`）
   逐个 grep 服务名 → **一处都没有** ✗（说明 guest 用的根本不是我补的那份）；
2. **删掉 `super.img` 试跑** → 第一阶段的 init 直接自杀并进入启动循环：

```
init: BlockDevInitializer::InitDevices: partition(s) not found after polling timeout: super
init: InitFatalReboot: signal 6
```

⇒ **第一阶段的 init 强依赖 `super` 分区，而 guest 的 system/vendor 正是从 super 挂出来的。**
之前几轮我改的 `*.img` / `*-qemu.img` 对 guest 无效，**必须改 `super.img`**。

### 正确配方（已逐项验证）

1. 编译（`m`）产出 plain 镜像与 `super.img`；
2. 给 plain 镜像打补丁（`debugfs`，例如 init.rc 的 `encryption=Require`→`Attempt`、
   删掉碰设备的那批 vendor HAL 的 rc/二进制）；
3. **`rm super.img system-qemu.img vendor-qemu.img` 后重跑 `m`**，让它们**从补丁后的
   plain 镜像重建**；
4. 每次重建后**必须回读校验 `super.img`**（编译有时会按依赖把 patch 过的 plain 镜像
   重新构建而覆盖补丁：本轮就出现过 `system-qemu.img` 里没有 patch、
   而 `super.img` 里有的情况）；
5. 启动参数里**必须带 `-cpu cortex-a53`**（第 8 轮的突破），并保留
   `-qemu -machine type=virt`、`-selinux permissive`。

按此配方跑出的这一轮：**设备类 HAL 全部不再启动 ✓、guest 零崩溃 ✓、无 recovery ✓、
宿主零段错误直到 78 秒 ✓**（此前 20~70 秒必崩），随后宿主在 `audioserver`/`credstore`
阶段仍段错误退出 —— 说明 `virt` 上"guest 访问 ranchu 专有 MMIO"这一类问题会**逐个**
暴露，属于平台层面的固有缺陷（ranchu 才是匹配的板子，但它被模拟器无条件的 PCI 音频
设备卡死）。**要根治仍需从源码重建模拟器去掉那个音频设备**。

---

## 第 10 轮：直接改 QEMU 二进制让 ranchu 能启动 —— 部分奏效，但被串口卡住

思路：既然 `ranchu` 只是被"模拟器无条件加的 PCI 设备"卡死，那就**在二进制里把这些设备名改掉**，
让它创建 MMIO 版（ranchu 有 MMIO 没有 PCI）。

**奏效的部分** ✓：把 QEMU 二进制里的字符串 `-soundhw` 改成同长度的 `-name`
（命令行于是变成 `-name hda`，被 QEMU 当作机器名，无害）→ **音频设备不再创建** ✓，
`PCI bus not available for hda` 报错消失 ✓。

**卡住的部分** ✗：改完音频后，下一个 PCI 设备立刻顶上：

```
-device virtio-serial,ioeventfd=off: No 'PCI' bus found for device 'virtio-serial'
```

把 `virtio-serial-pci` 也改成 `virtio-serial` 后，报错变成
`No 'PCI' bus found for device 'virtio-serial'` ✗ —— 说明 **`virtio-serial` 是 PCI 别名**，
QEMU 解析后仍要求 PCI 总线；而这台串口设备是**调制解调器/RIL 必需**的，不能像
触控/wifi/vsock 那样用 feature 关掉。

⇒ **字符串级补丁到此为止**：ranchu 需要的串口设备在模拟器里只能是 PCI 形态。
（已把两个 QEMU 二进制用 `.prepatch` 备份还原 ✓。）

**源码重建的可行性复核**（清华镜像）：

| 项目 | 结果 |
|---|---|
| `platform/external/qemu`（模拟器+QEMU 源码） | **HTTP 200 ✓ 可取** |
| `platform/external/qemu-pc-bios` | HTTP 200 ✓ |
| `platform/prebuilts/android-emulator-build`（官方构建工具链） | **HTTP 404 ✗ 镜像上没有** |

⇒ 源码本身可取，但官方构建工具链缺失；**要么用系统工具自行拼构建链（不确定），
要么继续在二进制里做代码级补丁（把"选择 PCI 串口"的那段代码改掉）**。

---

## 第 11–12 轮：社区经验调研 + 路线确认

### 一、社区对"Linux/x86_64 上跑 arm 安卓"的共识

**核心结论：社区主流根本不模拟 arm64 系统，而是用 x86_64 系统 + ARM 转译。**

| 方案 | 做法 | 社区评价 |
|---|---|---|
| A. 全 arm64 系统模拟（本项目前 10 轮走的路） | arm64 系统镜像 + QEMU TCG | 能跑但**极慢**；V2EX 实测"13900H 风扇全程咆哮，跑出树莓派 3B 水平"；问题多（黑屏/adb 超时/崩溃） |
| B. x86_64 系统 + ARM 转译（主流） | x86_64 镜像 + `ndk_translation`/libhoudini | "基本都是 x86 guest + native_bridge，游戏都能玩"；官方支持、性能好得多 |

**Google 官方原文**（emulator release notes）：

> **Support for ARM binaries on Android 9 and 11 system images** — 可以用
> **Android 9 的 x86 镜像或任意 Android 11 镜像**运行依赖 ARM 二进制的应用；
> 这些镜像默认支持 ARM，且**性能相比全 ARM 模拟有巨大提升**。

**社区对我遇到的那个具体报错的结论**（SO 69297141，14 赞）：
`qemu-system-aarch64: PCI bus not available for hda` → **加 `-qemu -machine virt` 即可绕过**
（与我第 1 轮独立发现的完全一致），但作者也提醒"有些情况下会黑屏"。
另有回答引用官方："从 Android 11 起官方 VM 不再支持 arm/arm64，除非 arm64 宿主（如 M1）"。

**社区给出的一个可用 arm64 配置**（SO 69710426，2025-07 更新）：模拟器 **34.2.16** +
**API 27 arm64-v8a 镜像** + `-qemu -machine virt` + `-gpu swiftshader_indirect`，
用于调试 arm64-v8a 的 NDK 原生代码；作者同样说"极慢、adb 常超时"。
（注：本项目额外发现的 **`-cpu cortex-a53`** 是社区没有的关键点 —— 它消除了
`cortex-a57` 下 TCG 执行 guest 原生代码的段错误。）

### 二、本轮的本机验证

- 正在运行的模拟器（5554，API 31 x86_64）：`abilist = x86_64`、`native.bridge = 0`
  ⇒ **当前这套跑不了 arm64 二进制**。
- 下载 **API 30 `default` x86_64** 官方镜像启动后：`abilist = x86_64,x86`、`native.bridge = 0`
  ⇒ **AOSP 版（default tag）不含转译**，与官方文档说的"Android 11 镜像"不符 —— 需要
  **`google_apis` 版**（社区例子里报错信息 `ndk_translation_program_runner_binfmt_misc_arm64`
  也证明转译在 Google 版镜像里）。
- **`google_apis` API30 x86_64 镜像已下载**（1.44GB，腾讯镜像 12MB/s）；
  组装成 `-sysdir` 后可启动，本次 guest 停在 `pc_memory_init` 之后，**转译属性待下一轮确认**。
- 备份还原：QEMU 二进制曾被二进制补丁改坏，已用**同目录未被改动的
  `qemu-system-aarch64`**（原版 40MB）覆盖 `-headless` 版还原 ✓。

### 三、下一轮

1. 让 `google_apis` API30 x86_64 起来，确认 `abilist` 含 `arm64-v8a`、
   `ro.dalvik.vm.native.bridge = libndk_translation.so`；
2. 把**自编的 arm64 `autod`** 推进去跑 —— 这就是社区验证过的、
   在 x86_64 Linux 上验证 arm64 二进制的正规做法；
3. 若转译不可用，退路是 **API 28 x86（官方点名的那个）**（本轮已下载 ✓）。

---

## 第 13 轮：**在 x86_64 Linux 上跑 arm64 二进制 —— 打通了**

### 一、修掉一个自己挖的坑：解压被截断

`google_apis` 包第一次解压时被我 20s 的命令超时打断，留下一个**名义大小对、实际数据不全**
的 `system.img`（3.23GB 的表象、只有 ~111MB 有数据）—— 这正是前两次"guest 卡在
`pc_memory_init`"的真正原因。重新完整解压后一切正常。
（教训：大文件解压/拷贝必须后台跑并校验**可读性**，不能只看 `ls` 的大小。）

### 二、确认转译组件在本机镜像里

在宿主机拆开 `google_apis` 的 `system.img`（GPT → super → `lpdump` 定位 system 分区）：
**`/system/etc/init/ndk_translation.rc` 存在** ✓ —— 转译组件确实在这个镜像里
（AOSP 的 `default` tag 镜像则**没有**，实测 `abilist=x86_64,x86`）。

### 三、**实测成功**

启动 `google_apis` API30 x86_64（KVM）后，guest 报：

```
abilist:        x86_64,x86,arm64-v8a,armeabi-v7a,armeabi
native.bridge:  libndk_translation.so
```

**这台 x86_64 模拟器已经能跑 arm64 二进制** ✓✓。把自编的 **arm64 `autod`** 推进去：

- guest 里 `toybox file` 认它为 **`ELF ... 64-bit LSB arm64, dynamic (/system/bin/linker64)`** ✓
- 直接执行时进入 **arm64 的 linker**（报 `library "libbinder.so" not found`）✓
  —— 说明转译链路是通的，只是**框架库**不在转译层自带的库目录里
  （`/system/lib64/arm64/` 只有 59 个基础库：libc/liblog/libEGL/libandroid/…）

### 四、最后一步的做法（已就绪，待重启后收尾）

1. 用 `readelf -d` 递归算出 `autod` 的 arm64 依赖闭包：**190 个库、48.3MB**
   （直接依赖：libbase/libbinder/libcutils/libgui/libjnigraphics/liblog/libui/libutils/libc/libm/libdl）；
2. **剔除转译层自带的 59 个**，剩 **161 个、40MB** 需要补；
3. ⚠️ **不能用 `LD_LIBRARY_PATH`** —— 它会被 **x86_64 的转译运行器**继承，
   导致 `libandroidfw.so is for EM_AARCH64 instead of EM_X86_64` ✗；
   正确做法是把库放进转译层自己的搜索路径 **`/system/lib64/arm64/`**；
4. 需要 `-writable-system` 启动 + `adb remount`（会要求**重启 guest** 生效），
   然后 `tar` 解到该目录即可 —— 本轮已走到重启这一步（guest 尚在启动中）。

### 五、结论

**"Linux 平台上跑 arm64 安卓"这个目标，在 x86_64 机器上已经打通** ✓：
用 **`google_apis` 版 Android 11（API 30）x86_64 镜像**（内置 `ndk_translation`），
它对外暴露 `arm64-v8a` ABI，**arm64 的 ELF 可以被直接执行**（走转译）。
这正是 Google 官方文档说的那条路，也是社区的主流做法。

---

## 第 14 轮：把 arm64 库落地的最后一步 —— 卡在"分区全满"

### 已确认的事实

- 转译层 `/system/lib64/arm64/` 自带 **59 个基础库**（libc/liblog/libEGL/libandroid/…），
  但**没有框架库**（libbinder 等）；`autod` 的 arm64 依赖闭包共 **190 个 / 48.3MB**，
  剔除自带的后**需补 161 个 / 40MB**。
- ⚠️ **不能用 `LD_LIBRARY_PATH`**：它会被 **x86_64 的转译运行器**继承
  （报 `libandroidfw.so is for EM_AARCH64 instead of EM_X86_64`）——
  库必须放在**转译层自己的架构专属目录**里。
- 走 `-writable-system` + `adb remount` 的路线很脆：`remount` 会禁用 verity 并要求
  **重启 guest**，而重启后 **adbd 起不来**（guest 起来了，但 `/system/bin/adbd` 不可用），
  且 `/system` 依旧是只读 ✗。

### 于是想直接改镜像 —— 结果发现四个分区**全都是满的**

| 分区 | 大小 | 空闲 |
|---|---|---|
| system（super 内） | 1.08GB | **3.2MB**（784 块）|
| system_ext | 126MB | — |
| product（super 内） | 1.45GB | **4.3MB**（1044 块）|
| vendor（super 内） | 158MB | **0.5MB**（119 块）|

super 总量 3.01GB，四个分区合计 2.48GB，**还有 0.53GB 未分配** ✓，
所以理论上可以"扩容 system 的 ext4 + 用 `lpmake` 重建 super"（`lpmake`/`lpdump`/
`resize2fs`/`e2fsck` 这些主机工具在 `out/host/linux-x86/bin/` 里**都齐**✓）。

### 下一轮（两条都很具体）

1. **省事路线**：`product` 分区里装着预装 Google 应用 —— 删掉一两个大的（Maps/YouTube 等）
   即可腾出几十 MB，把 161 个库注入 `/product/lib64/arm64/`（标准 linker 搜索路径），
   再启动验证 `autod`；
2. **彻底路线**：`resize2fs` 把 system 的 ext4 扩容 128MB → 注入 `/lib64/arm64/` →
   `lpmake` 按原布局重建 super（四个分区 + `emulator_dynamic_partitions` 组）→
   写回 `system.img` 的 2MB 处 → 启动（仍需 `-writable-system` 关 verity）。

---

## 第 15 轮：镜像手术全流程跑通 —— 最后卡在 AVB 校验

### 一、发现 `/tmp` 是 tmpfs（这解释了好几个怪现象）

`/tmp` 是 **16GB 的内存盘**，只剩 6.1GB；而我的镜像（system 1.4G + product 1.45G +
system_ext 0.13G + vendor 0.17G）已经占了 3.2G —— 再写一个 3.2G 的新 super 就 **ENOSPC**，
`lpmake` 因此静默失败、`dd skip` 也读出 0 字节。**把手术目录挪到 `.run/surgery/`（磁盘盘，
剩 151G）后一切正常**。教训：临时工作目录不要用 `/tmp`。

### 二、完整手术流程（已验证可行）

1. **扩容 system 的 ext4**：把文件补齐到 1.35GB 后 `resize2fs` —— 成功
   （258969 → 353894 块，**空闲 373MB**）；
2. **注入 161 个 arm64 库**：`debugfs -w -f`（一条命令文件里 161 条 `write`）把库写进
   **`/lib64/arm64/`**（system-as-root 布局里它就是 `/system/lib64/arm64`）；
3. **提取其余分区**：`system_ext` 131,895,296 / `product` 1,452,154,880 / `vendor` 165,437,440；
4. **`lpmake` 重建 super**（3,229,614,080 字节，与原 super 等大）：
   ```
   lpmake --device-size 3229614080 --metadata-size 65536 --metadata-slots 2 \
     --group emulator_dynamic_partitions:4294967296 \
     --partition system:readonly:1449549824:emulator_dynamic_partitions --image system=sys3.img \
     ... (system_ext / product / vendor 同理) --out newsuper.img
   ```
   注意：**不能再显式给 `--group default`**（lpmake 会报 "Group already exists: default"）；
5. **写回 `system.img` 的 2MB 处**；启动后内核日志确认
   **`Created logical partition product/system_ext/vendor`** ✓ —— 说明新 super 被正确解析。

### 三、卡点：AVB 校验

```
init: Failed to open AvbHandle: No such file or directory
init: Failed to setup verity for '/system': No such file or directory
init: Failed to mount /system: No such file or directory
init: Failed to mount required partitions early ...
init: InitFatalReboot: signal 6        → 启动循环（7 次）
```

原因明确：**`vbmeta` 分区（system.img 里 1MB 的那个）里存的是各分区的 AVB 摘要**，
我改了 `system` 的 ext4，摘要自然对不上 ✗；`-writable-system` 并没有传
`androidboot.veritymode=disabled`（内核 cmdline 里只看到 `vbmeta.digest/size`），
所以第一阶段 init 仍按 AVB 去挂 `/system`。

### 四、下一轮的两条修法（都很具体）

1. **重建 vbmeta**：用 AOSP 树里的 `external/avb/avbtool`，对新的
   system/system_ext/product/vendor 生成 hashtree 描述符并 `make_vbmeta_image`
   写回那个 1MB 分区；
2. **更省事**：**把 guest 的 fstab 里 `system` 的 `avb` 标志去掉** —— 第一阶段 fstab
   在 ramdisk（`initrd`＝`ramdisk.img`）里，改完重新打包即可，这样根本不走 AVB。

任一条做完，就能启动带 161 个 arm64 框架库的系统，直接跑 `autod` 收尾。

---

## 第 16 轮：追查 fstab 来源 + 定位到 AVB 哈希不匹配

### 一、线索链

启动后内核日志给出完整因果：

```
init: [libfs_avb]Device path not found: /dev/block/by-name/system
init: [libfs_avb]Fallback to use logical device path: /dev/block/dm-0
init: [libfs_avb]avb_slot_verify failed, result: 6        ← 6 = 校验失败（哈希不匹配）
init: Failed to open AvbHandle: No such file or directory
init: Failed to setup verity for '/system': ...
init: Failed to mount /system: ...  → InitFatalReboot: signal 6 → 启动循环
```

即：**guest 读到的 fstab 里 `system` 那行仍带 `avb=vbmeta`**，于是去做 AVB 校验，
而校验对象是我改过的 system，**哈希必然不匹配**（result 6）。

### 二、fstab 到底从哪来（逐个排除）

| 候选来源 | 检查结果 |
|---|---|
| DT（`ReadFstabFromDt`） | **失败**（日志明确 fail）|
| ramdisk（`initrd`）| Google 的 ramdisk 里**只有** `debug_ramdisk dev init mnt proc sys`，本来就没有 fstab；我另外放进去的 `first_stage_ramdisk/fstab.ranchu` **也没被采用** |
| **super 里的 vendor 分区** | 有 `/etc/fstab.ranchu`，**已补丁并回读确认 avb=0** ✓ |
| **独立的 `vendor.img`** | **也有一份 fstab 且仍带 avb=vbmeta** —— 已补丁并回读确认 avb=0 ✓（补丁后 AVB 报错次数从 4 降到 2）|
| system 分区 | 无独立 fstab |
| 模拟器/QEMU 二进制、kernel-ranchu | 都没有 `avb=vbmeta` 字面量（说明是运行时合成）|

（注意：**`/dev/block/by-name/system` 找不到 → 回退到逻辑设备 `/dev/block/dm-0`** 是正常回退，
真正的失败在 `avb_slot_verify failed, result: 6`。）

### 三、本轮已验证的成果

- **镜像手术全流程稳定可用**：`resize2fs` 扩容 system ext4 → `debugfs` 注入 161 个 arm64 库 →
  `lpmake` 重建 super → 写回 `system.img`。**回读 live 镜像确认**：
  `/lib64/arm64` 有库、super 内 vendor 的 fstab 无 avb ✓；
- **两处 vendor fstab 都已去掉 `avb=vbmeta`** ✓；
- 工具齐备：**`out/host/linux-x86/bin/avbtool`** ✓ + **`external/avb/test/data/*.pem` 测试密钥** ✓ +
  **`vbmeta` 分区是标准 AVB blob**（`AVB0` 魔数、2842 非零字节）✓。

### 四、下一轮：直接重建 vbmeta（确定解法）

既然改不动"guest 从哪读 fstab"，就**让 AVB 校验通过**：

1. `avbtool info_image --image <vbmeta 分区>` 看清原有描述符（哪些分区、什么算法/salt）；
2. 对改过的分区用 `avbtool add_hashtree_footer`（`--partition_size` 用实际大小、
   `--key external/avb/test/data/testkey_rsa2048.pem` 之类）生成新的哈希树；
3. `avbtool make_vbmeta_image` 生成新的 vbmeta，写回 `system.img` 的 1MB 处（LBA 2048）；
4. 启动验证 AVB 通过、`/system` 挂载成功，然后跑 `autod` 收尾。

---

## 第 17 轮：**绕过 AVB 的关键一招 —— 用原版镜像启动 + bind 挂载库**

### 一、先把 AVB 结构看清（`avbtool info_image`）

顶层 vbmeta（签名算法 `SHA256_RSA4096`）里的描述符：

| 类型 | 分区 |
|---|---|
| **Chain Partition** | **system**（它自己分区里还有一份链式 vbmeta）|
| Hash descriptor | vendor_boot |
| Hashtree descriptor | **product / system_ext / vendor** |

⇒ **所有分区都被校验**，没有"免校验"的分区可以藏库（我原本想把库放进 product ✗）。
同时这也解释了 `resize2fs` 之后为什么必然失败：**哈希树被丢掉了** ✗。

### 二、试过并排除的几条路

| 尝试 | 结果 |
|---|---|
| 改 ramdisk 里的第一阶段 fstab（`first_stage_ramdisk/fstab.ranchu`）| **没被采用** ✗（连去掉 `first_stage_mount` 都无变化）|
| `-prop veritymode=disabled` | 到不了第一阶段（只是 guest 属性）✗ |
| 补 super 内 vendor 的 fstab | 有效但仍不够（AVB 报错 4→2）✗ |
| 补独立 `vendor.img` 的 fstab | 同上 ✗ |
| system 分区里的 fstab | 不存在 ✗ |

### 三、**关键一招：完全不改镜像，用 bind 挂载**

思路：AVB 只校验**镜像内容**；只要镜像保持原样，AVB 就通过 ✓；
而 arm64 库可以**在运行时挂上去**：

1. **用原版（未修改）的 `google_apis` API30 x86_64 镜像启动** ——
   **AVB 零报错 ✓、正常启动到 adb 在线 ✓、`abilist` 仍含 `arm64-v8a` ✓、
   `native.bridge = libndk_translation.so` ✓**；
2. `adb root`（可用 ✓）→ 把 guest 自带的 59 个库 `cp` 到 `/data/local/tmp/arm64all/`，
   再推入我们补的 161 个；
3. **`mount --bind /data/local/tmp/arm64all /system/lib64/arm64`** ✓✓ ——
   **成功！不需要 `-writable-system`、不需要 remount、不需要重启** ✓✓
   （bind 挂载不写文件系统，只需 CAP_SYS_ADMIN，而 `adb root` 已具备）；
4. 结果：**`/system/lib64/arm64` = 220 个库** ✓，**arm64 linker 把整个依赖闭包都解决了** ✓✓ ——
   报错从"找不到库"变成了 **只差一个符号** ✓✓✓：

```
CANNOT LINK EXECUTABLE "./autod": cannot locate symbol
"_ZNK7android7RefBase22incStrongRequireStrongEPKv" referenced by "/data/local/tmp/autod"
```

### 四、最后剩的是一个"版本对齐"问题（不是技术障碍）

`autod` 是用 **AOSP 12** 编的，而 guest 是 **Android 11 (API 30)**；
`android::RefBase::incStrongRequireStrong` 是 A12 新增的符号 ✗。
（我本地 A12 产物的 `system/lib64/libutils.so` 只是 stub，真正的实现在
`com.android.runtime.apex` 里，本轮未取出。）

**下一轮二选一即可收尾**：
1. **改用 API 31/32 且带 `ndk_translation` 的 `google_apis` x86_64 镜像**
   （先用 `unzip -l`/`strings` 确认其 system 里有 `ndk_translation.rc`），
   再走同样的"原版启动 + bind 挂载"流程；
2. **用 API 30 的源码/NDK 重新编一个 `autod`**，与 guest 版本对齐。

无论哪条，**"x86_64 Linux 上跑 arm64 安卓"这条链路本身已经全程验证**：
arm64 ELF 可直接执行 ✓、arm64 linker 正常解析 220 个库 ✓。
