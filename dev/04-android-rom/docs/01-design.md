# 设计记录：这份 ROM 为什么这么写

> 每条决策都附 AOSP 源码位置或本机实测，便于日后升级 AOSP 版本时逐条复核。
> 全部实测证据见 [`00-bridge-eval.md`](00-bridge-eval.md)。

---

## 1. 为什么是「x86_64 ROM + 用户态翻译层」而不是「跨架构模拟 arm64 Android」

| 方案 | 结果 |
|---|---|
| x86_64 宿主 + arm64 guest（QEMU TCG） | ❌ 实测判死：arm `ranchu` 板无 PCI，模拟器却无条件挂 PCI 音频 → QEMU 退出；换 `virt` 板后 guest 一碰 ranchu 专有 MMIO 就把宿主 QEMU 打崩。模拟器 34+ 更是直接移除该后端 |
| **x86_64 宿主 + x86_64 guest + ARM 翻译层** | ✅ 同架构硬件加速（KVM/WHPX），官方镜像实测 34.4 秒（31.3.10）/ 29.0 秒（37.2.11）开机，arm64 应用可装可跑 |

翻译层选 **`libndk_translation`**（Google，随 AVD 系统镜像分发），不选 libhoudini：
houdini 的 arm64 变体（URL 里的 `_z` 系列）**官方源上只到 Android 7**，8/9 系列只剩 32 位 ARM 翻译器。

---

## 2. 设备树为什么长这样

### 2.1 复用 Google 的 arm64 桥设备目录

`device/generic/goldfish/emulator64_x86_64_arm64/` 是 AOSP 里**现成**的"x86_64 + arm64 桥"设备目录
（README 原文：*"This only supports 64bit abi and ndk-translated arm64 abi"*）。
Google 官方 `google_apis;android-31;x86_64` 镜像的设备名正是 `emulator64_x86_64_arm64`。
我们的 `device.mk` 直接继承它，只补 BIOS 主机包。

### 2.2 为什么自建 BoardConfig 而不是改上游那块

上游那块板没有 `BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES`，而我们的载荷必须用它（见 §3.2）。
自建板级 = 不改 AOSP 上游任何文件，且这个"放行开关"只作用于我们自己的产品。

### 2.3 ABI 列表是自动的（不要手写）

`BoardConfig.mk` 里：

```make
TARGET_CPU_ABI := x86_64
TARGET_NATIVE_BRIDGE_ARCH := arm64
TARGET_NATIVE_BRIDGE_ABI := arm64-v8a
```

链路（`build/make/core/board_config.mk:263-312` → `sysprop.mk:49-53`）：

```
TARGET_CPU_ABI_LIST_64_BIT = x86_64,arm64-v8a      ← 原生 ABI 在前，桥 ABI 在后
        ↓ 写入三个分区
ro.system.product.cpu.abilist64 / ro.vendor... / ro.odm... = x86_64,arm64-v8a
        ↓ init 启动时派生（property_service.cpp:959-1018，优先级 product→odm→vendor→system）
ro.product.cpu.abilist = x86_64,arm64-v8a
```

**已实测的坑**：这个派生只认**第一个非空**的分区属性。只改 `system` + `vendor` 时，
`ro.odm.product.cpu.abilist64=x86_64` 仍会胜出 → 应用装不上（`ABI arm64-v8a not supported`）。
自编 ROM 里由 build 统一生成，天然一致；这条坑主要是"手工改镜像"时会踩。

---

## 3. 翻译层怎么进 ROM

### 3.1 载荷清单（90 个文件 / 23 MB）

由 `scripts/fetch-payload.sh` 从官方镜像提取，见 `README.md` §2 的目录树。
其中两个文件最容易漏：`system/etc/ld.config.arm.txt` 与 `ld.config.arm64.txt`
——它们给 ARM 侧 linker 提供命名空间配置，缺了应用起来就崩。

#### ⭐ 实测澄清：那 59 个 arm64 系统库**不是** Google 专有二进制

构建日志里会出现 **61 条 `overriding commands for target .../system/lib64/arm64/...` 警告**
（`Makefile:61` 是我们的 `PRODUCT_COPY_FILES`，`base_rules.mk:525` 是 AOSP 模块安装规则）。
一查才发现：**AOSP 自己会把模块编成 arm64 变体并装到 `/system/lib64/arm64/` 与 `/system/bin/arm64/`**
——这是 `TARGET_NATIVE_BRIDGE_ARCH/ABI` 触发的原生桥构建路径，产出与官方镜像的**同 61 个文件**。

实测证据（构建进行中取样）：

```
$ file out/.../system/lib64/arm64/ld-android.so
ELF 64-bit LSB shared object, ARM aarch64 ...

$ sha256sum <我们构建产出> <payload 里的同名文件>
3be279f8ebd077f25477f57ec0b6ded9e072e4cb6d0e5002ed2d2a233bfec843   （构建产出）
3be279f8ebd077f25477f57ec0b6ded9e072e4cb6d0e5002ed2d2a233bfec843   （官方镜像）
```

**结论**：
- 真正专有的只有 **翻译器本体**：`libndk_translation.so` + 20 个 `libndk_translation_proxy_*.so`
  + `ndk_translation_program_runner_binfmt_misc_arm64` + 4 个 binfmt 规则 + `ndk_translation.rc`
  + 两个 `ld.config.arm*.txt`（这两个构建不产出）；
- 那 59 个 arm64 库与本 ROM 的框架**同源同版本**，因此不存在"ARM 侧与 x86_64 侧版本错配"的风险；
- 冲突是**无害**的：两边字节相同（`copy-file-to-target` 后定义的规则生效）。
  想更干净的话，后续可以把这 59 个库从载荷里去掉（载荷会从 23 MB 降到约 3.6 MB），
  但那会改动正在跑的构建输入，**留到验收通过后再做**。

### 3.2 为什么用 `PRODUCT_COPY_FILES` 而不是 prebuilt 模块

AOSP 默认**禁止**用 `PRODUCT_COPY_FILES` 拷贝 ELF（`build/make/core/Makefile:43-56` 的
`check-elf-prebuilt-product-copy-files`，提示改用 `cc_prebuilt_binary` / `cc_prebuilt_library_shared`）。

- 载荷 90 个文件、目的地跨 `lib64`、`lib64/arm64`、`bin`、`bin/arm64`、`etc` 五处，
  做成 prebuilt 模块既冗长又容易在 `relative_install_path` 上出错；
- Google 自己处理同一份 ndk_translation 载荷时，用的就是这个开关
  （`device/google/cuttlefish/vsoc_x86/BoardConfig.mk:39-41`，注释还带着 bug 号 b/156534160）。

所以：`BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true` + 90 条显式拷贝规则
（由 `apply-overlay.sh` 从目录树生成，不手写）。

权限位：`copy-file-to-target` 用的是 `cp`（不是 `cp -p`），新文件权限取自源文件，
所以载荷里 `bin/**` 的可执行位必须保留——`apply-overlay.sh` 用 `cp -a` 并且会断言这件事。

### 3.3 属性为什么一半放 system、一半放 vendor

| 属性 | 位置 | 依据 |
|---|---|---|
| `ro.dalvik.vm.isa.arm=x86`<br>`ro.dalvik.vm.isa.arm64=x86_64`<br>`ro.enable.native.bridge.exec=1` | `/system/build.prop`<br>（`PRODUCT_SYSTEM_PROPERTIES`） | 与官方镜像一致 |
| `ro.dalvik.vm.native.bridge=libndk_translation.so` | `/vendor/build.prop`<br>（`PRODUCT_VENDOR_PROPERTIES`） | 官方镜像就在这里；**不能**放 system |

为什么不放 system：`build/make/target/product/runtime_libart.mk:95-96` 里已经有一条
**强赋值** `PRODUCT_SYSTEM_PROPERTIES += ro.dalvik.vm.native.bridge=0`。
同一分区重复定义 sysprop 会被 `post_process_props.py:112-143` 判为
`error: found duplicate sysprop assignments`（除非开 `BUILD_BROKEN_DUP_SYSPROP`，
而那个开关的语义是"第一条赢"= 仍是 `0`）。
放 vendor 分区既没有重复问题，又因为 build.prop 的加载顺序（system → system_ext → vendor/default
→ vendor → odm → product，**后读覆盖先读**，见 `property_service.cpp:1071-1085`）能真正生效。

### 3.4 artifact path 放行

载荷落在 `generic_system.mk` 声明的路径范围内，kati 会直接报错
（`artifact_path_requirements.mk:26` 的 `all_offending_files` 检查）。
按上游 `aosp_x86_arm.mk`（同样是 x86+ARM 桥的 GSI）的做法，
用 `PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST` 逐条显式放行。

> 这条是**第一次构建实际撞到的错误**，错误信息只给了 `error: Build failed.`，
> 真正的文件清单在它上面那段 `warning:` 里——排查时别只看最后一行。

---

## 4. 不碰 AOSP 上游

`scripts/apply-overlay.sh` 把所有东西同步到 `aosp/device/remote_control/` 一个目录：

- 能 `lunch` 到我们的产品，是因为 Soong 的 finder 会递归扫描
  `device/`、`vendor/`、`product/` 下的 `AndroidProducts.mk`
  （`build/soong/ui/build/finder.go:134-137`）——**不需要**改 `device/generic/goldfish/AndroidProducts.mk`；
- `--revert` 一条命令清理干净；AOSP 树可以随时重同步。

---

## 5. 验收标准（对应 `scripts/run-linux.sh` / `windows/run-windows.ps1`）

| 编号 | 检查 | 期望 |
|---|---|---|
| A1 | `ro.build.version.sdk` | 31 |
| A2 | `ro.product.device` | `remote_control_x64_arm64` |
| A3 | `ro.product.cpu.abilist` | `x86_64,arm64-v8a` |
| B1 | `ro.dalvik.vm.native.bridge` | `libndk_translation.so` |
| B2 | `ro.enable.native.bridge.exec` | 1 |
| B3 | `/proc/sys/fs/binfmt_misc/` | 含 `arm64_exe` / `arm64_dyn` |
| C1 | 自建 aarch64 静态 ELF 直接执行 | 打印 `ARM64_OK` |
| D1 | `pm install --abi arm64-v8a <APK>` | `Success` |
| D2 | 进程存活 + `/proc/<pid>/maps` 里有 `/system/lib64/arm64/` | 条数 ≥ 1 |
| D3 | `primaryCpuAbi` | `arm64-v8a` |

D 组现用项目自建探针 APK（纯 `arm64-v8a`，sha256 固定在验收脚本里）；此前使用的 F-Droid APK 已替换，验收不再依赖外部 APK 镜像。
