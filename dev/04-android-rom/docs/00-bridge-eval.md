# 评估：用「x64 安卓 ROM + ARM 翻译层」替代「QEMU 跑 arm64 安卓」

> ⚠️ **历史文档 —— 写于当时，不代表现状。**
> 本文是一次评估的过程记录。文中的"需要你确认的决定性问题"**后来已经有了实际答案**：
> 这份 ROM 已经建出来并投入使用（见 [01-design.md](01-design.md)、
> [../README.md](../README.md)），当前产品名是 `remote_control_x64_arm64`。
> 保留本文是为了留下"为什么这么选"的来龙去脉与实测数据。

> 评估对象：**用 libhoudini 做一份兼容 arm64 应用的 x86_64 安卓 ROM，然后直接 KVM 跑 x64 安卓**
> 评估方式：**本机实测**（下载、解包、启动、装应用、跑基准），不是文献综述。命令见文末第 6 节。
> 结论一句话：**方向成立，但 libhoudini 这个工具选错了；正确的东西是 Google 官方的 `libndk_translation`，而且不用自己造。**

---

## 0. 结论速览

| 命题 | 判定 | 实测依据 |
|---|---|---|
| x86_64 安卓 + KVM 能跑 | ✅ 成立 | 本机 `--fast` 通道已在用；本次同架构镜像实测 **开机 34.4 秒** |
| x64 安卓能跑 **arm64 应用** | ✅ **成立** | 官方镜像上装 + 跑含 arm64 原生库的 APK，进程映射 22 条 `/system/lib64/arm64/*.so`，无崩溃 |
| 用 **libhoudini** 实现 | ❌ **不成立** | 官方源 **Android 8 起就没有 arm64 变体**（HTTP 矩阵 + 包内 `file` 双重验证） |
| 正确的翻译层 | ✅ `libndk_translation`（Google 官方，随 AVD 镜像分发） | 镜像内 2.4 MB 翻译器 + 59 个 aarch64 系统库 |
| ABI 覆盖面 | ⚠️ **取决于 API 级别**（§2.5） | Android 11 镜像四 ABI 全支持（含 32 位 ARM）；Android 12 镜像纯 64 位 |
| 必须自己造 ROM 吗 | ⚠️ **不需要也能用**；要造，AOSP 里有现成脚手架 | `device/generic/goldfish/emulator64_x86_64_arm64/` |
| 把 libndk **搬进自己的镜像** | ✅ **已实测成功**（§2.6） | 23 MB 载荷 + 10 行属性（4 增 6 改）→ 同一份镜像从「拒绝 arm64」变成「arm64 应用正常运行」 |

**方向对、工具错**：想达到"x64 上跑 arm64 应用"，不需要碰 houdini，也不需要自己编译翻译层——
Google 的 `system-images;android-31;google_apis;x86_64` 就是这份东西，一条 `curl` 加 34 秒开机即可用。

---

## 1. 先把提议拆成三个独立命题

"x64 ROM + libhoudini + KVM" 其实是三件事捆在一起，混在一起谈必然谈不清：

| 命题 | 真正的技术问题 | 本次判定 |
|---|---|---|
| **A. 宿主能跑 x86_64 安卓** | KVM / WHPX 是否可用 | ✅ 早就成立，无争议 |
| **B. arm64 应用能在 x86_64 安卓上跑** | 有没有可用的 ARM→x86 翻译层 | ✅ 有（但不是 houdini） |
| **C. 翻译层能不能装进"我们的 ROM"** | 专有二进制 + 属性 + SELinux 接线 | ✅ **已实测可搬**（§2.6：23 MB 载荷 + 10 行属性，端到端跑通） |

提议里"libhoudini"只是 B 的一种**实现选择**，而它恰好是唯一走不通的那一种。

---

## 2. 实测证据

### 2.1 libhoudini：arm64 变体止步于 Android 7（2017）

Android-x86 的 houdini 按宿主/客体架构分三个变体：
`_x` = ARM32→x86，`_y` = ARM32→x86_64，**`_z` = ARM64→x86_64**。

对官方源（`dl.android-x86.org`）逐条 HEAD 实测：

| 系列（对应 Android 版本） | `_x` | `_y`（arm32 on x86_64） | `_z`（**arm64** on x86_64） |
|---|---|---|---|
| 6（A6） | 200 | 200 | 200 |
| 7（A7） | 200 | 200 | **200 ← 最后一版，2017-10-09** |
| 8（A8） | 404 | 200 | **404** |
| 9（A9） | 404 | 200（2020-02-11） | **404** |

**结论：公开渠道能拿到的 houdini，`_z`（arm64）只到 Android 7；Android 8、9 只剩 32 位 ARM 翻译器。**

包内字节级复核（`unsquashfs` + `file`），不是靠文档：

| 包 | 文件数 | 关键文件 | `file` 结论 |
|---|---|---|---|
| `9_y.sfs`（A9，最新可用） | 250 | `houdini`、`libhoudini.so` | 两个都是 **ELF 32-bit i386**；**没有** `houdini64`、**没有** arm64 库 |
| `7_z.sfs`（A7，最后的 arm64 版） | 167 | `houdini64`、`libhoudini.so`、`nb/libz.so` | 宿主侧 x86-64，`nb/libz.so` 是 **ELF 64-bit ARM aarch64** |

即：**把 houdini 装进 Android 12 的 x86_64 ROM 并指望它跑 arm64 应用 —— 无米之炊。**
9_y 是给 Android 9 的 32 位翻译器，版本差 3 个大版本；7_z 是给 Android 7 的 arm64 翻译器，差 5 个大版本。

> 补充：Chrome OS 的 ARC 里还有更新的 houdini（可从 Chrome OS 恢复镜像提取），但它与 Chrome OS 那一版的
> Android framework 绑定，社区做法也是"Android 版本必须对齐"。这不构成对我们 AOSP 12 的可用路径。

### 2.2 官方镜像的翻译层：`libndk_translation`

用的镜像是免 GApps 的 `system-images;android-31;google_apis;x86_64`（`x86_64-31_r14.zip`，1402 MiB，
SHA1 `9aedd3e8…f29` 与 SDK 清单一致）。启动后：

```
设备名          product:sdk_gphone64_x86_64  device:emulator64_x86_64_arm64   ← 名字就是答案
开机耗时        boot time 34378 ms            （emulator 31.3.10，-accel on = KVM）

ro.product.cpu.abilist       = x86_64,arm64-v8a      ← 声明支持 arm64
ro.product.cpu.abilist32     = （空）                 ← 见 2.5，32 位不支持
ro.dalvik.vm.native.bridge   = libndk_translation.so ← 注意在 /vendor/build.prop
ro.enable.native.bridge.exec = 1                     ← 在 /system/build.prop
ro.dalvik.vm.isa.arm         = x86
ro.dalvik.vm.isa.arm64       = x86_64
```

**模拟器版本兼容性**（这条对交付轨道很关键）：换用 **37.2.11**（Windows 侧唯一还能拿到的版本）
复跑同一份镜像 → **29.0 秒开机，属性完全一致**（`x86_64,arm64-v8a` + `libndk_translation.so`）。
即：x86_64 guest 不存在 `windows-arm64` 那样的"新版模拟器不支持"问题，**31.x 与 37.x 都能跑**。

翻译层载荷清单（这是"移植到自编 ROM"时要搬的全部东西）：

| 类别 | 路径 | 规模 |
|---|---|---|
| 翻译器 + proxy 库 | `/system/lib64/libndk_translation*.so` | 21 个，3.5 MB |
| aarch64 系统库 | `/system/lib64/arm64/` | 59 个，18 MB |
| arm64 链接器/入口 | `/system/bin/arm64/{linker64,app_process64}` | 901 KB + 11 KB |
| binfmt 执行器 | `/system/bin/ndk_translation_program_runner_binfmt_misc_arm64` | 58 KB |
| binfmt 规则 | `/system/etc/binfmt_misc/{arm_exe,arm_dyn,arm64_exe,arm64_dyn}` | 4 个小文件 |
| 接线脚本 | `/system/etc/init/ndk_translation.rc` | 见下 |

`ndk_translation.rc` 就是接线方式本身（`ro.dalvik.vm.isa.arm64` 之类属性在 build.prop 里，它负责注册 binfmt_misc）：

```
on early-init && property:ro.enable.native.bridge.exec=1
    mount binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
on property:ro.enable.native.bridge.exec=1 && property:ro.dalvik.vm.isa.arm=x86
    copy /system/etc/binfmt_misc/arm_exe   /proc/sys/fs/binfmt_misc/register
    copy /system/etc/binfmt_misc/arm_dyn   /proc/sys/fs/binfmt_misc/register
on property:ro.enable.native.bridge.exec=1 && property:ro.dalvik.vm.isa.arm64=x86_64
    copy /system/etc/binfmt_misc/arm64_exe /proc/sys/fs/binfmt_misc/register
    copy /system/etc/binfmt_misc/arm64_dyn /proc/sys/fs/binfmt_misc/register
```

### 2.3 活体验证：arm64 真的在跑（不是"属性看起来对"）

**① aarch64 原生 ELF 直接执行**（NDK r26d 静态编译，推到设备后直接跑）：

```
HELLO_FROM_ARM64
machine=x86_64          ← uname 报宿主内核，符合预期
acc=4999999950000000
```

**② arm64 应用：安装 → 启动 → 检查进程**（F-Droid 上取的小应用，含 `lib/arm64-v8a/libbinderdetector.so`）：

| 步骤 | 结果 |
|---|---|
| `pm install --abi arm64-v8a` | `Success` |
| 包管理器判定 | `primaryCpuAbi=arm64-v8a`、`secondaryCpuAbi=null` |
| 启动 | pid 6157，`mResumedActivity: com.oF2pks.kalturadeviceinfos/.MainActivity` |
| 进程映射 | **22 条**含 `arm64` 的映射（`/system/lib64/arm64/libc.so`、`libdl.so`、`libc++.so` …） |
| APK 里的 .so | ELF 头 `03 00 b7 00` → ET_DYN，**e_machine = 0xB7 = AArch64** |
| 存活 | `ALIVE`，crash buffer 无 native crash |

**③ A/B 对照**（同一台机器、同一 API 级别，只有渠道不同）：

| 镜像 | 装 arm64 APK |
|---|---|
| `default;x86_64`（无翻译层，即 `--fast` 现在跑的那份） | ❌ `java.lang.IllegalArgumentException: ABI arm64-v8a not supported on this device` |
| `google_apis;x86_64`（带翻译层） | ✅ `Success`，应用正常运行 |

### 2.4 性能：不是"均匀地慢"，而是**分化**

同一台设备上，同一个基准程序分别以原生 x86_64 和 arm64（翻译）运行：

| 负载 | 原生 x86_64 | 翻译 arm64 | 比值 |
|---|---|---|---|
| 整数依赖链（200M 轮 LCG） | 356 ms | 412 ms | **1.16×** |
| FNV 哈希（5M × 43B，访存+整数） | 256 ms | 297 ms | **1.16×** |
| FP 吞吐（4 路独立 double 累加） | 176 ms | 173 ms | **0.98×** |
| FP **串行依赖链**（50M 次 double mul-add） | 94 ms | **2036 ms** | **21.6×** |

读法：**常见逻辑（整数、字符串、访存、可并行的浮点）几乎无损；串行依赖的浮点链会灾难性变慢。**
这条比"翻译大概慢 2~5 倍"之类的经验说法有用得多——它说明风险是**特定代码形态**，不是整体性能。

> 注意这是微基准，不代表真实应用整体。真实应用的 Java/ART 部分不翻译，只有 `lib*.so` 走翻译层。

### 2.5 ABI 覆盖面：想要 32 位 ARM 应用，就得选 **Android 11**

这条差异比"翻译层能不能用"更容易被忽略：同一渠道、同为 x86_64，只是 API 级别不同，ABI 覆盖面完全不同。

| 镜像 | x86_64 | x86 | arm64-v8a | **armeabi-v7a** |
|---|---|---|---|---|
| `android-30;google_apis;x86_64`（Android 11） | ✅ | ✅ | ✅ | ✅ **实测跑通** |
| `android-31;google_apis;x86_64`（Android 12） | ✅ | ❌ | ✅ | ❌ **装不上** |

API 30 那份的实测值：

```
ro.product.cpu.abilist   = x86_64,x86,arm64-v8a,armeabi-v7a,armeabi
ro.product.cpu.abilist32 = x86,armeabi-v7a,armeabi
/system/lib/arm      59 个（32 位 ARM 系统库）   /system/bin/arm/{linker,app_process}
/system/lib64/arm64  59 个                      /system/etc/ld.config.arm.txt + ld.config.arm64.txt
```

活体验证（同一个 APK，只强制走 32 位 ARM）：`pm install --abi armeabi-v7a` → `Success`，
`primaryCpuAbi=armeabi-v7a`，运行后进程映射 **17 条 `/system/lib/arm/*.so`**（libm 等），Activity 在前台，无崩溃。

**为什么会这样**：SDK 仓库里 32 位的 `x86` 镜像**只到 API 30**，API 31 起只剩 `x86_64`；镜像也就从
"四 ABI"变成"纯 64 位"。而 AOSP 公开树的 `build/make/target/board/emulator_x86_64_arm64/BoardConfig.mk`
恰好就是四 ABI 那一版（`TARGET_2ND_ARCH := x86` + `TARGET_NATIVE_BRIDGE_2ND_ARCH := arm`）——
Google 的 Android 11 镜像正是照它做的。

→ **决策含义**：只要目标应用里有 **32 位 ARM（armeabi-v7a）** 原生库，这条路就要以
**Android 11 为基座**；或者自编一份四 ABI 的 ROM 并另配带 arm32 的载荷（跨版本混用载荷不可取，
社区共识是"翻译层必须与 Android 版本匹配"，本次未验证混用）。**只跑 arm64 应用的话，Android 12 那份就是对的。**

---

### 2.6 关键补充：把 libndk **搬进自己的镜像** —— 已实测成功

上面证明的是"Google 的镜像能用"。真正的问题是"**我们的** x86_64 ROM 能不能用"。
做法：拿一份**同一 API 级别的普通 AOSP 风格 x86_64 镜像**（`default;x86_64`，就是现在 `--fast` 跑的那份，
没有翻译层、`abilist` 只有 `x86_64`），把翻译层搬进去，看它是否变成"能跑 arm64 应用"。

**结果：完全成功。** 改造前后对比：

| 项 | 改造前 | 改造后 |
|---|---|---|
| `ro.product.cpu.abilist` | `x86_64` | `x86_64,arm64-v8a` |
| `ro.dalvik.vm.native.bridge` | 无 | `libndk_translation.so`（zygote 启动参数里可见 `-XX:NativeBridge=`） |
| `/proc/sys/fs/binfmt_misc/` | 空 | `arm64_exe arm64_dyn arm_exe arm_dyn` |
| aarch64 ELF 直接执行 | — | ✅ `HELLO_FROM_ARM64` |
| `pm install --abi arm64-v8a` | ❌ `ABI arm64-v8a not supported on this device` | ✅ `Success`，`primaryCpuAbi=arm64-v8a` |
| 应用实际运行 | — | ✅ 进程存活、**29 条** `/system/lib64/arm64/*.so` 映射、Activity 在前台 |
| 性能 | — | 与官方镜像一致（INT 1.07×，FP 串行 22.7×） |

**要搬的东西 = 23 MB 载荷 + 10 行属性**（4 行新增 + 6 行改值）。

载荷（`tar` 一下即可，来源就是官方镜像的 `/system`）：

```
/system/lib64/libndk_translation.so + libndk_translation_proxy_lib*.so   (21 个, 3.5 MB)
/system/lib64/arm64/                                                     (59 个, 18 MB)
/system/bin/arm64/{linker64,app_process64}
/system/bin/ndk_translation_program_runner_binfmt_misc_arm64
/system/etc/binfmt_misc/{arm_exe,arm_dyn,arm64_exe,arm64_dyn}
/system/etc/init/ndk_translation.rc
/system/etc/ld.config.arm.txt  /system/etc/ld.config.arm64.txt     ← 易漏：ARM 侧 linker 命名空间
```

属性（**三个分区都要改，漏一个就不生效**）：

| 文件 | 改动 |
|---|---|
| `/system/build.prop` | `abilist` / `abilist64` → `x86_64,arm64-v8a`；追加 `ro.dalvik.vm.isa.arm=x86`、`ro.dalvik.vm.isa.arm64=x86_64`、`ro.enable.native.bridge.exec=1` |
| `/vendor/build.prop` | `ro.vendor.product.cpu.abilist{,64}` → `x86_64,arm64-v8a`；追加 `ro.dalvik.vm.native.bridge=libndk_translation.so` |
| `/odm/etc/build.prop` | `ro.odm.product.cpu.abilist{,64}` → `x86_64,arm64-v8a` ← **最容易漏** |

再 `restorecon` 重打标签（`ld.config.*.txt` 必须是 `system_linker_config_file`，
`lib64/arm64/*` 与 `libndk_translation*.so` 是 `system_lib_file`），重启即可。

### 这一步踩到的三个真坑（文档查不到，只有动手才会遇到）

**① 属性改三处，而且 `odm` 的优先级比 `vendor` 高。** 读 AOSP 源码
（`system/core/init/property_service.cpp`）：

- 所有 build.prop 先读进一个 map，**后读的覆盖先读的**（`ro.` 属性同样如此），顺序为
  `/system` → `/system_ext` → `/vendor/default` → `/vendor` → `/odm`(etc) → `/product`(etc)。
  所以 `/system/build.prop` 里那句 `ro.dalvik.vm.native.bridge=0` 不碍事——日志里能看到
  `Overriding previous property 'ro.dalvik.vm.native.bridge':'0' with new value 'libndk_translation.so'`。
- 但**平凡的 `ro.product.cpu.abilist` 并不写在 build.prop 里**，而是 init 启动时按
  **product → odm → vendor → system** 的优先级，从分区前缀属性派生出来的
  （`property_initialize_ro_cpu_abilist()`）。
  实测：只改 `system`+`vendor` 时，`ro.odm.product.cpu.abilist64=x86_64` 仍然胜出 → 应用照旧装不上；
  **补上 odm 才生效。**

**② `ld.config.arm.txt` / `ld.config.arm64.txt` 属于载荷。** 少了它们，ARM 侧 linker 命名空间不对，
应用起来就崩。这两个文件在官方镜像里带 `system_linker_config_file` 标签。

**③ Android 11+ 的 `adb remount` 要跑两次。** 第一次只是"启用 overlayfs"，输出
`Now reboot your device for settings to take effect`；重启后再 `adb remount` 才真正可写
（启动模拟器时也要带 `-writable-system`）。另外这份镜像要求模拟器 **≥ 31.2.7**。

### 落到真正的自编 ROM 上

上面是"运行时打补丁"（overlayfs）的验证方式，用来证明**这套载荷在 AOSP 风格镜像上成立**。
换成真编译（`lunch sdk_phone64_x86_64-userdebug && m`），对应的是构建级改动，而且 AOSP 已经做好一半：

| 要做的事 | 落点 |
|---|---|
| ABI 列表（system/vendor/odm 三处 `abilist`） | ✅ **自动**：BoardConfig 设 `TARGET_NATIVE_BRIDGE_ARCH := arm64` / `TARGET_NATIVE_BRIDGE_ABI := arm64-v8a`，`board_config.mk` + `sysprop.mk` 会写进三个分区 |
| 翻译层文件 | `PRODUCT_COPY_FILES`（开发期也可以用 Magisk 模块，更省事） |
| `ro.enable.native.bridge.exec`、`ro.dalvik.vm.isa.*`、`ro.dalvik.vm.native.bridge` | ⚠️ **公开树里没有**（`device/`、`build/` 全树搜不到），要自己用 `PRODUCT_PROPERTY_OVERRIDES` / `PRODUCT_VENDOR_PROPERTY_OVERRIDES` 加 |
| 内核 | 需要 `CONFIG_BINFMT_MISC=y`（模拟器内核已带，实测通过） |

---

## 3. AOSP 公开树里已经有这套东西的脚手架

这点很关键：**"x86_64 + arm64 桥"不是 hack，是 AOSP 的一等公民**。

| 位置 | 内容 |
|---|---|
| `device/generic/goldfish/emulator64_x86_64_arm64/BoardConfig.mk` | `TARGET_NATIVE_BRIDGE_ARCH := arm64`、`TARGET_NATIVE_BRIDGE_ABI := arm64-v8a` |
| `build/make/target/board/emulator_x86_64_arm64/BoardConfig.mk` | 同上，并带 `TARGET_NATIVE_BRIDGE_2ND_ARCH := arm`（armeabi-v7a） |
| `device/google/cuttlefish/vsoc_x86_64/BoardConfig.mk` | Cuttlefish 的 x86_64 板同样是 arm64 桥 |
| `device/google/cuttlefish/vsoc_x86/BoardConfig.mk` | `USE_NDK_TRANSLATION_BINARY` → `ndk_translation_prebuilt` |
| `build/make/core/board_config.mk` / `sysprop.mk` | `TARGET_NATIVE_BRIDGE_ABI` 自动并入 `ro.product.cpu.abilist` |

也就是说：**属性、ABI 列表、ART 的 native bridge、linker namespace、binfmt 注册，AOSP 全都写好了。**
公开树里唯一缺的是**翻译层二进制本身**（`libndk_translation` 是 Google 专有，只随 SDK 系统镜像分发；
搬运清单、属性落位与 SELinux 标签见 §2.6，已实测）。

顺带修正一个现状认知：本机 `out/target/product/emulator64_arm64/` 有完整自制镜像，
但 **`emulator64_x86_64/` 里一个关键镜像都没有**（只有 `kernel-ranchu`、`dtb.img`、`encryptionkey.img`）——
所以现在 `--fast` 跑的是 **Google 的成品 `default` 镜像**，不是自编 ROM。"x86_64 ROM"这件事目前还没有产物。

---

## 4. 三条路线怎么选

| 路线 | 做法 | 代价 | 何时选 |
|---|---|---|---|
| **A. 直接用官方镜像**（推荐先做） | 下 `google_apis;x86_64` → `run-emulator.sh` 指过去（Linux KVM / Windows WHPX） | 半天，已实测可跑 | 目标是"有个快的、能跑 arm64 应用的安卓环境"（开发内循环、Windows 交付验证） |
| **B. 自编 x86_64 ROM + 移植翻译层** | `lunch sdk_phone64_x86_64-userdebug` + 把 §2.6 的载荷按 `PRODUCT_COPY_FILES`（开发期也可先用 Magisk 模块）塞进去 | **1~2 天**：§2.6 已把载荷、属性落位、SELinux 标签实测清楚，再加一次系统镜像编译 | 需要把 `remote-control` 烧进 `/system` 开机自启、需要定制 ROM、或者要对外交付整机 |
| **C. arm64 真机 / arm64 宿主** | 二手 Pixel 6 / ARM 云主机 | 千元级 | **验证 aarch64 真实行为与真实延迟**——翻译层给不了这个答案 |

**建议组合：A（日常与 Windows 交付环境） + C（最终 arm64 验证）。B 只在明确需要整机交付时做。**

顺带说清三条**不要再投入**的路：

1. ❌ **在 x86_64 宿主上跑 arm64 guest（QEMU TCG）** —— `linux-arm64/README.md` 已用实测判死（ranchu 无 PCI +
   `virt` 板上 ranchu 专有设备让宿主 QEMU 段错误），且模拟器 34+ 直接不再支持。
2. ❌ **houdini on Android 12** —— 见 §2.1，没有 arm64 变体。
3. ⚠️ **把翻译层当作 arm64 真机验证的替代品** —— 翻译层跑的是 x86_64 内核 + x86_64 framework，
   `remote-control` 在那里是 x86_64 二进制；它能验证"arm64 **应用**兼容性"，**不能**验证"我们的 arm64 二进制在真机上对不对"。

### 对 Windows 交付轨道的连带影响（重要）

`windows-arm64/README.md` 现在卡在两处：arm64 guest 只能 TCG，且模拟器 34+ 不再支持 x86_64 宿主跑 arm64。
**换成 x86_64 guest 后这两条同时消失**：x86_64 guest 在任何现代模拟器上都能硬件加速（Windows 走 WHPX），
同一份 `google_apis;x86_64` 镜像两边通用，且 31.x / 37.x 模拟器都能启动（§2.2 实测）。
**这可能是本次评估对交付路径最大的实际收益。**

---

## 5. 风险与**本次未验证**项

诚实标注边界，别把"评估结论"当"已验证的实现"：

| 项 | 状态 |
|---|---|
| 官方镜像能跑 arm64 应用 | ✅ 本次实测（含 A/B 对照） |
| **把翻译层移植进自编 AOSP 12 x86_64 ROM** | ✅ **已完成并验收**：构建级落位（`PRODUCT_COPY_FILES` + 属性）已进入自编 ROM；release 包的启动验收覆盖翻译层接线、binfmt、aarch64 ELF 与 arm64 探针 APK（见 [`15-release-packaging.md`](15-release-packaging.md)） |
| 翻译层对"你们要跑的那个应用"是否够用 | ⚠️ **未测**。必须先拿真实 APK 试（尤其是重原生计算的、带 JNI 的、有反模拟检测的） |
| 32 位 ARM 应用 | ❌ 该镜像不支持（§2.5） |
| 许可 / 可分发性 | ⚠️ `libndk_translation` 是 Google 专有（只随 SDK 镜像分发），houdini 是 Intel 专有。**内部开发测试无碍；若要随整机交付给客户，必须先过法务** |
| 反模拟 / 反翻译 | ⚠️ 通用风险：部分应用会检测 native bridge / 模拟器特征 |

社区成例（移植翻译层这件事已经有人做过，可作参考实现）：

- `sickcodes/Droid-NDK-Extractor`、`RawPikachu/libndk_translation_Module` —— 从 AVD 镜像提取成模块
- `ilhan-athn7/android_proprietary_native_bridge` —— Magisk/KernelSU 模块，覆盖 BlissOS / Waydroid / Redroid
- `casualsnek/waydroid_script` —— Waydroid 的 `libndk` / `libhoudini` 一键安装
- DEF CON 29《Sleight of ARM: Demystifying Intel Houdini》 —— houdini 内部机制

### 顺带纠一个网上流传的错误配方

中文技术博客（含一篇搜出来排第一的）让人这么干：

```bash
setprop ro.dalvik.vm.native.bridge houdini     # ❌ 不可能生效
setprop ro.enable.native.bridge.exec 1         # ❌ 不可能生效
setprop ro.dalvik.vm.native.bridge64 houdini64 # ❌ 这个属性根本不存在
```

`ro.*` 是**只读属性**，启动后 `setprop` 无效；真正的落位是**镜像里的 build.prop**
（本次实测：`ro.dalvik.vm.native.bridge` 在 `/vendor/build.prop`，`ro.enable.native.bridge.exec`
与 `ro.dalvik.vm.isa.*` 在 `/system/build.prop`）。照这种博客改，永远只会得到
`INSTALL_FAILED_NO_MATCHING_ABIS`。

---

## 6. 复现命令（本次评估的全部操作）

```bash
cd dev/04-android-rom    # 注：本节为历史记录，当时在 dev/04-emulator/linux-arm64（该目录已移除）

# 1) 取官方"x86_64 + arm64 桥"镜像（1402 MiB，腾讯镜像与 Google 清单同源同 sha1）
curl -O https://mirrors.cloud.tencent.com/AndroidSDK/sys-img/google_apis/x86_64-31_r14.zip

# 2) 解包 + 补"构建模式"需要的两个文件（同 get-stock-image.sh 的坑 2、3）
mkdir -p .run/eval-google/sysdir && cd .run/eval-google/sysdir
unzip -q ../x86_64-31_r14.zip && mv x86_64/* . && rmdir x86_64
mkdir -p system && cp build.prop system/build.prop && cp ramdisk.img initrd
cd "$OLDPWD"

# 3) 起（KVM；镜像要求 emulator ≥ 31.2.7）
EMU_ABI=x86_64 \
EMULATOR_BIN=/opt/android/emu31b/emulator/emulator \
PRODUCT_OUT="$PWD/.run/eval-google/sysdir" EMULATOR_PORT=5560 \
./run-emulator.sh --datadir "$PWD/.run/eval-google/datadir"

# 4) 看它是不是"能跑 arm64 的那一份"
adb -s emulator-5560 shell getprop ro.product.cpu.abilist    # x86_64,arm64-v8a
adb -s emulator-5560 shell getprop ro.dalvik.vm.native.bridge # libndk_translation.so

# 5) 装一个带 arm64 原生库的 APK，确认它真的以 arm64 身份在跑
adb -s emulator-5560 install --abi arm64-v8a app.apk
adb -s emulator-5560 shell pidof <包名> | xargs -I{} adb -s emulator-5560 shell "grep -c arm64 /proc/{}/maps"
```

 houdini 可用性矩阵复现（一条命令）：

```bash
for s in 6 7 8 9; do for v in x y z; do
  printf "%s_%s %s\n" $s $v "$(curl -sSI http://dl.android-x86.org/houdini/${s}_${v}/houdini.sfs | head -1 | tr -d '\r')"
done; done
```

「把 libndk 搬进自己的 x86_64 镜像」复现（§2.6，前提：普通 AOSP 风格 x86_64 镜像 + 模拟器 ≥ 31.2.7）：

```bash
A=adb                                        # 5560=官方带翻译层镜像，5562=目标镜像
$A -s emulator-5562 root; $A -s emulator-5562 remount; $A -s emulator-5562 reboot   # 第 1 次：只启用 overlayfs
$A -s emulator-5562 root; $A -s emulator-5562 remount                              # 重启后再来一次才 rw

# a) 打包载荷（≈23 MB）
$A -s emulator-5560 shell 'cd / && tar -cf /data/local/tmp/p.tar \
  system/lib64/libndk_translation.so system/lib64/libndk_translation_proxy_lib*.so \
  system/lib64/arm64 system/bin/arm64 \
  system/bin/ndk_translation_program_runner_binfmt_misc_arm64 \
  system/etc/binfmt_misc system/etc/init/ndk_translation.rc \
  system/etc/ld.config.arm.txt system/etc/ld.config.arm64.txt'
$A -s emulator-5560 pull /data/local/tmp/p.tar .
$A -s emulator-5562 push p.tar /data/local/tmp/
$A -s emulator-5562 shell 'cd / && tar -xf /data/local/tmp/p.tar'

# b) 三个分区改属性（§2.6 的表，odm 最易漏），然后重打标签 + 重启
$A -s emulator-5562 shell 'restorecon -RF /system/lib64 /system/bin /system/etc'
$A -s emulator-5562 reboot

# c) 验收
$A -s emulator-5562 shell getprop ro.product.cpu.abilist          # 期望 x86_64,arm64-v8a
$A -s emulator-5562 shell 'pm install --abi arm64-v8a -r /data/local/tmp/app.apk'
```

---

## 7. 需要你确认的一个决定性问题

上面所有技术判定都成立，但**选哪条路线取决于一件事**，而这件事文档里没有明确写：

> **这个 x64 安卓 ROM，是"开发/测试用的快速环境"，还是"要交付给客户的产品运行时"？**

- 若是**开发/测试环境** → 走路线 A，今天就能用；`remote-control` 继续用 arm64 真机/宿主做最终验证。
- 若是**产品运行时**（客户在 PC 上跑，且必须跑他们的 arm64 APK）→ 走路线 A 起步、B 落地，
  但**先拿客户真实 APK 在官方镜像上做一次兼容性与性能验收**，尤其确认：
  ① 是不是 64 位（32 位 ARM 装不上）；② 有没有重原生计算（FP 串行链 21× 那个坑）；
  ③ 有没有反模拟检测。

建议顺序：**先在路线 A 上用真实 APK 做验收 → 通过了再谈路线 B 的 ROM 工程。**
反过来（先做 ROM 再验证应用兼容性）风险最高。
