# 13 · macOS 宿主支持：arm64 ROM 产品 + 第三套宿主脚本

> **状态：计划 + 首轮核查已完成（2026-10-03）。**
> §7.0 是**实测记录**（已核实，带 sha1 与文件清单）；§1–§6、§8 的其余部分仍是**计划与估算**。
> 本文两条线都还没有写出任何代码。
>
> 相关：[`14-macos-host-notes.md`](14-macos-host-notes.md)（把 remote-control 做成 macOS 被控端的 API 与权限调研）
> · [`09-why-not-full-arm64-sim.md`](09-why-not-full-arm64-sim.md)（"跨架构全系统模拟"曾被我方否决的实测结论）
> · [`03-delivery.md`](03-delivery.md)（当前的"一份 ROM，两个 x64 平台"）

---

## 0. 一句话结论

**加 macOS 不是"再写一套脚本"，而是要新增一条 arm64 ROM 产品线 + 第三套宿主脚本；两条线可以独立落地、互不阻塞。**

而**这条 arm64 产品线比现在的 x86_64 那条更简单**：它把 `libndk_translation` 翻译层整套删掉，
结构退回到上游 `sdk_phone64_arm64` 的形状。

| 交付物 | 内容 | 新增 | 改动 |
|---|---|---|---|
| **A. arm64 ROM 产品** | 原生 arm64 goldfish ROM（跑 remote-control），Apple Silicon 模拟器用它 | **约 280–420 行** | 约 **120–200 行**（构建系统参数化） |
| **B. macOS 宿主脚本** | 第三套运行时控制面 + 打包（`macos/`、`packaging/bin/darwin/`） | **约 1500–2200 行** | 约 **400–600 行**（共享层的 GNU→BSD 适配） |

两条线相加，约等于**"三分之二套"**现有骨架的量（现有：Linux 侧 `scripts/emulator.sh` 818 + `run-linux.sh` 354 + `packaging/bin/linux` 570；Windows 侧 `windows/emulator.ps1` 735 + `run-windows.ps1` 233 + `packaging/bin/windows` 690）。

**一句话的分工**：ROM 那条腿是"换产品"，脚本那条腿是"换宿主"。**先说清一个反直觉的点——这两条腿的必要性不对称：**

- 没有 B（macOS 脚本），Mac 用户照样能手工跑：自己装 SDK 模拟器、自己敲 `emulator` 命令。难用，但可行。
- 没有 A（arm64 ROM），**Apple Silicon 用户根本跑不起来**：现有 ROM 是 x86_64 guest，
  而 ARM 宿主上不存在 x86_64 的硬件加速路径（§1.3）。这不是难用，是不成立。

> **所以如果只能先做一条，先做 A。**

---

## 1. 背景：现在的两套是什么，为什么 Mac 不能白捡

### 1.1 现状盘点（已核实）

| 层 | 文件 | 行数 |
|---|---|---|
| 共享控制面 | `scripts/common.sh` 204、`emulator.sh` 818、`run-linux.sh` 354、`release.sh` 682、`package-rom.sh` 163、`apply-overlay.sh` 128、`build-rom.sh` 95、`status.sh` 74、`accept.sh` 64、`fetch-payload.sh` 149 | ~2700 |
| Windows 专有 | `windows/emulator.ps1` 735、`run-windows.ps1` 233、`fetch-emulator.ps1` 131、`fetch-images.ps1` 112、`preflight.ps1` 85 | ~1300 |
| Linux 打包 | `packaging/bin/linux/`：`lib.sh` 242 + `start-headless/stop/status/verify` | ~570 |
| Windows 打包 | `packaging/bin/windows/`：`common.ps1` 262 + `start-headless/stop/status/verify` | ~690 |

**Windows 那套是 Linux 那套的完整重写**——同一组能力（建/起/停/查/验）有两份不同语言的实现。
所以"加 macOS"= 加**第三份骨架**，而不是加一个 `case` 分支。

### 1.2 交付面已经是"两平台"的形态

`release.sh` 里已经有平台分派表（`release.sh:85-91`）：

```bash
platform_tag()    { case "$1" in linux) printf 'linux-x86_64' ;; windows) printf 'windows-x86_64' ;; esac; }
platform_label()  { case "$1" in linux) printf 'Linux x86_64（KVM）' ;; windows) printf 'Windows x86_64（WHPX）' ;; esac; }
platform_hostos() { case "$1" in linux) printf 'linux' ;; windows) printf 'windows' ;; esac; }
platform_backend() { case "$1" in
    linux)   printf 'qemu/linux-x86_64/qemu-system-x86_64-headless' ;;
    windows) printf 'qemu/windows-x86_64/qemu-system-x86_64.exe' ;; esac; }
```

注意 `hostos` 这一列：SDK 清单里 macOS 的 host-os 就叫 `macosx`，`release.sh:142` 与
`windows/fetch-emulator.ps1:9` 的注释都写明了「每个包有按 host-os 分的多个 archive（linux/windows/macosx）」。
**下载与解包链路本来是平台无关的**，硬编码在 x86_64 上的只有两处：

```bash
# prepare_runtime() 里的后端校验与裁剪
local backend="$rt/emulator/$(platform_backend "$plat")"
[ -s "$backend" ] || die "运行时里没有 x86_64 无头后端：$backend
    这个包带不动本 ROM（guest 是 x86_64，别的架构后端不行）"
...
case "$(basename "$qdir")" in
    "$hostos-x86_64") ;;                 # ← 只保留 <hostos>-x86_64
    *) rm -rf "$qdir" ;;                 # ← 其余全删
esac
```

### 1.3 为什么 Apple Silicon 上现有 ROM 走不通（已核实 + 推演）

| 宿主 | guest | 加速路径 | 判断 |
|---|---|---|---|
| Linux x86_64 | x86_64 | KVM | ✅ 已实跑 |
| Windows x86_64 | x86_64 | WHPX | ⚠️ 未实跑（见 `03-delivery.md`），但同架构 |
| Intel Mac | x86_64 | HVF（Hypervisor.framework） | 推演可行，**未实测** |
| **Apple Silicon** | **x86_64** | **无** | ❌ **只有 TCG 全软件翻译** |

最后一行就是 `09-why-not-full-arm64-sim.md` 里那条判据的镜像版——那份文档的结论句是
「宿主 x86_64 **没有 arm64 KVM** ⇒ 只能 **TCG**」，并给了实测锚点：
同一台机器上 KVM 开机 **23.8 s**，TCG 是 **5–8 分钟**量级、典型负载 **5–15× 慢**，
而且那条路最终**卡在图形栈**（`vendor.hwcomposer-2.3` 缺 `libgralloctypes.so`）没能走到 `boot_completed`。

把宿主换成 arm64、guest 保持 x86_64，困境一模一样。**这是"不成立"，不是"慢一点"。**

> **已实测坐实（§7.0.2）**：Apple Silicon 的官方模拟器包里**只有 `qemu/darwin-aarch64/` 一个后端目录**，
> 没有任何 x86_64 后端。所以这一行不是"我们推演 TCG 会很慢"，而是**包层面根本不提供这条路**。
> 想要在 M 系列 Mac 上跑，**只能走方案 A**。

> ⚠️ **不要把这条结论误读成"arm64 模拟器也不行"。** 被否决的是**跨架构**（arm64 宿主跑 x86_64 guest）；
> 本文方案 A 是**同架构**（arm64 宿主跑 arm64 guest），正好是那份文档推荐的那一类。

---

## 2. 方案 A：新增 arm64 ROM 产品

### 2.1 核心判断：它比现有产品更简单，不是更复杂

上游 AOSP 12 **本来就把 arm64 模拟器产品当一等目标维护**（已核实，全部在本地已同步的树里）：

| 前提 | 证据（远程 AOSP 树路径） |
|---|---|
| arm64 模拟器设备树存在 | `device/generic/goldfish/emulator64_arm64/BoardConfig.mk`（`TARGET_ARCH := arm64`、`TARGET_CPU_ABI := arm64-v8a`） |
| arm64 内核预编译已同步 | `kernel/prebuilts/5.10/arm64/kernel-5.10`（48.9 MB）与 `kernel-5.10-gz`（19.9 MB） |
| arm64 内核模块已同步 | `kernel/prebuilts/common-modules/virtual-device/5.10/arm64/`（含 `virtio_input.ko` 等） |
| vendor 侧产品配对 | `device/generic/goldfish/64bitonly/product/arm64-vendor.mk` + `device/generic/goldfish/arm64-kernel.mk`（`TARGET_KERNEL_USE ?= 5.10`，`EMULATOR_KERNEL_FILE := kernel/prebuilts/5.10/arm64/kernel-5.10-gz`） |
| **`/dev/uinput` 在 arm64 内核里可用** | `kernel/prebuilts/5.10/arm64/modules.builtin` 含 `kernel/drivers/input/misc/uinput.ko`（**内建**）；`System.map` 有 `uinput_open` / `uinput_write` |

**最后一条是这份方案能成立的关键**：触控后端不用换、`remote-control.te` 里那一堆 uinput 规则不用改、
`selftest.cpp` 的 `/dev/uinput` 探针不用改。

### 2.2 arm64 产品与 x86_64 产品的逐项差异

| 项 | `remote_control_x64_arm64`（现有） | `remote_control_arm64`（新增） |
|---|---|---|
| `TARGET_ARCH` / `TARGET_CPU_ABI` | `x86_64` | `arm64` / `arm64-v8a` |
| `TARGET_NATIVE_BRIDGE_*`（4 个变量） | 有 | **删**（无翻译层） |
| `bridge/` 目录 + `bridge-copy.mk`（80+ 条 `PRODUCT_COPY_FILES`） | 有 | **删** |
| `PRODUCT_ENFORCE_ARTIFACT_PATH_REQUIREMENTS := relaxed` + 13 行 `ALLOWED_LIST` | 有（放行外来载荷） | **删**（不再有外来文件） |
| `ro.dalvik.vm.isa.arm/arm64`、`ro.enable.native.bridge.exec` | 有 | **删** |
| `ro.dalvik.vm.native.bridge=libndk_translation.so` | 有（写在 vendor 分区） | **必须不写**——写了 ART 会去找一个不存在的库 |
| `BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true` | 有 | **删** |
| `BOARD_SEPOLICY_DIRS += device/generic/goldfish/sepolicy/x86` | 有 | **改**：arm64 上游没有对应目录，按实际需要定（见 §2.5） |
| vendor 继承 | `64bitonly/product/x86_64-vendor.mk` | `64bitonly/product/arm64-vendor.mk` |
| 内核 | `kernel/prebuilts/5.10/x86_64/kernel-5.10` | `kernel/prebuilts/5.10/arm64/kernel-5.10-gz` |
| `config.ini.xl` vs `advancedFeatures.ini.arm` | `config.ini.xl` | arm64-vendor 用的是 `advancedFeatures.ini.arm` |
| `PRODUCT_SHIPPING_API_LEVEL` | 31 | 31（相同） |
| `QEMU_USE_SYSTEM_EXT_PARTITIONS` / `PRODUCT_USE_DYNAMIC_PARTITIONS` | 都 true | 保持 true（不制造新的分区形态差异） |

`emulator/config.ini`（97 行，硬件唯一真源：1280×720@320dpi、`hw.ramSize=6144`、`hw.cpu.ncore=4`、`hw.gpu.mode=auto`）
**两个产品共用，不用改** —— 它不参与 AOSP 构建，由 `run-*.sh` / `package-rom.sh` 在运行期覆盖。

### 2.3 删掉翻译层之后，产品文件小多少

| 文件 | 现有行数 | arm64 版估算 | 说明 |
|---|---|---|---|
| `BoardConfig.mk` | 104（其中约 45 行是桥的注释与变量、约 30 行是 sepolicy 分区的长注释） | **约 90** | 桥那段整段消失，sepolicy 注释保留（选哪个分区这个坑两边都要防） |
| `product/*.mk` | 168（其中约 60 行是桥的 `ALLOWED_LIST` 与属性） | **约 110** | 剩下的是 `remote-control` 的 `PRODUCT_PACKAGES` / `PRODUCT_COPY_FILES`，两个产品共享 |
| `device.mk` | 35 | **约 55** | arm64 版要 `inherit` 上 `emulator64_arm64/device.mk`，但**要补回** x86_64 版里那两件上游缺的东西：BIOS host 包（`bios.bin`/`vgabios-cirrus.bin`）与以太网特性声明（`android.hardware.ethernet.xml`，桥接模式的必要前提） |
| `AndroidProducts.mk` | 8 | **约 12** | 加一条 `PRODUCT_MAKEFILES` 条目，两个产品并列 |
| `sepolicy/` | 3 个文件（`remote_control.te`、`file_contexts`、`remote_control_controller.te`） | **共享** | 见 §2.5 |

### 2.4 构建系统要改哪些行

产品名目前**写死在 `scripts/common.sh:27`**：

```bash
PRODUCT_NAME="remote_control_x64_arm64"
LUNCH_TARGET="$PRODUCT_NAME-userdebug"
PRODUCT_OUT="${PRODUCT_OUT:-$AOSP_DIR/out/target/product/$PRODUCT_NAME}"
```

往下游牵出去的地方（已核实位置）：

| 文件:行 | 现状 | 改法 |
|---|---|---|
| `scripts/common.sh:27` | 写死 | `PRODUCT_NAME="${PRODUCT:-remote_control_x64_arm64}"` |
| `scripts/apply-overlay.sh:92-125` | 桥的搬运 + `bridge-copy.mk` 生成 + 4 个载荷文件的存在性检查 | 按产品类型分支：arm64 产品**整段跳过** |
| `scripts/build-rom.sh:58` | 前置检查 `bridge/bridge-copy.mk` 存在 | 同上，按产品跳过 |
| `scripts/package-rom.sh:146` | `bridge="$DEVICE_DST/remote_control_x64_arm64/bridge/system"` 的校验 | 同上 |
| `scripts/status.sh:27-29` | 显示 `bridge-copy.mk` 的规则条数 | 同上，arm64 显示"无翻译层" |
| `scripts/release.sh:36,479,480` | `ROM_DIR`、`.release-meta.json` 里的 `product` / `lunch` | 从 `$PRODUCT_NAME` 与 `$LUNCH_TARGET` 取，不写死 |
| `tools/integrate-sepolicy.sh:34,37` | 设备树路径与 `TARGET` 默认值 | 参数化 |

合计约 **120–200 行**，其中大部分是"加一个产品类型分支"的机械改动。

### 2.5 两个产品共享 sepolicy —— 不要复制一份

`BoardConfig.mk` 里那段长注释记录了 `BOARD_SEPOLICY_DIRS`（vendor 策略）与
`SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS`（system_ext 策略）的**双向取舍**，实测两头都撞过：

- vendor 策略：✅ 看得见 HAL 类型（`hal_graphics_allocator_default`）／❌ 看不见平台私有类型（`odsign_prop` → `unknown type`）
- system_ext 策略：反过来

当前选 **vendor**，理由是「抓帧那条链依赖 HAL 类型，而它是这个服务的立身之本」。

**这条取舍与 guest 架构无关**，所以：

- 策略文件本身（`remote_control.te` / `file_contexts` / `remote_control_controller.te`）**一份就够**
- 新产品的 `BoardConfig.mk` 里那条 `BOARD_SEPOLICY_DIRS` 指向**同一个目录**
  （`device/remote_control/remote_control_x64_arm64/sepolicy`），或者把目录改名成中性名
  （如 `device/remote_control/common/sepolicy`）——**代价是 `apply-overlay.sh` 的路径要跟着改**
- ⚠️ 若为了少改路径而**复制**一份 sepolicy，就制造了第二处真源，以后必然漂移。**不要复制。**

### 2.6 与翻译层绑定的校验要加闸门

这些脚本/断言在 arm64 产品上会失效——不是"需要改"，而是"需要跳过"：

| 位置 | 内容 |
|---|---|
| `scripts/apply-overlay.sh:119-125` | 检查 4 个翻译层载荷文件存在（`libndk_translation.so`、`ndk_translation_program_runner_binfmt_misc_arm64`、`ndk_translation.rc`、`ld.config.arm64.txt`） |
| `scripts/run-linux.sh:280` | guest 内读 `/proc/sys/fs/binfmt_misc/` 判断翻译层是否注册 |
| `scripts/run-linux.sh:292-338` | 用 `/data/local/tmp/arm64-probe` 验 arm64 机器码与 arm64 应用的映射库条数 |
| `packaging/bin/linux/verify.sh:54,85` | 同样两件事（`binfmt_misc` + `/proc/$PID/maps` 里 `/system/lib64/arm64/` 条数） |
| `tools/check-bridge-symbols.sh`（102 行） | 整份都是翻译层符号检查 |
| `scripts/package-rom.sh:143` | 输出里带 "system.img sha256" 等桥相关摘要 |

**做法**：给它们一个统一的 `产品类型` 判据（例如 `PRODUCT` 前缀或一个 `HAS_BRIDGE=0/1` 变量），
arm64 产品下走"跳过并如实打印 ⊘ 未适用"，而不是静默通过——静默通过会让体检报告骗人。

### 2.7 硬约束：这份 ROM 仍然不能在 Mac 上编

AOSP 12 在 macOS 上编完整 ROM（含 Linux 内核、x86_64-only 的构建工具链）不是现实路径
（需要区分大小写卷、80 GB+ 空间、Xcode 之外的一整套 prebuilt）。

**所以 Mac 只拿产物，构建仍在 Linux 构建机上——和今天 Windows 用户的处境完全一样。**
这**不增加工作量**，但它决定了交付说明里**不能承诺"Mac 用户能自己 build ROM"**。

### 2.8 落地顺序（建议由小到大）

1. **上游通路验证**（最便宜否决点）：`lunch sdk_phone64_arm64-userdebug && m`，确认能出可启动的 arm64 镜像。
   这一步用的是上游产品，**不碰我们的设备树**，失败了后面全不用谈。
   ⚠️ 目标名与产物目录名**不是一个**：`lunch` 用 `sdk_phone64_arm64-userdebug`，
   而产物落在 `out/target/product/emulator64_arm64/`（跟 `PRODUCT_DEVICE` 走）。
   写错不会报错，只会静默回落到 `aosp_arm`。见 §7.0.1 的表。
2. 加一个**最小** `remote_control_arm64` 产品：先不带 sepolicy、以 root 跑，只验"能开机 + `adb` 通 + `/dev/uinput` 存在 + `remote-control` 在 `/system/bin/`"。
3. 搬 sepolicy 与 `remote-control.rc`，开机自启，跑 `tools/functional-sweep.py` 的 33 条命令体检。
4. 再进方案 B。

---

## 3. 方案 B：macOS 宿主（第三套脚本）

### 3.1 平台能力对照（这张表就是"要写多少"的答案）

| 能力 | Linux（现有） | Windows（现有） | macOS（要写） |
|---|---|---|---|
| 虚拟化加速 | KVM | WHPX | **Hypervisor.framework（`hvf`）**，前置检查 `sysctl kern.hv_support` |
| 后端路径 | `qemu/linux-x86_64/qemu-system-x86_64-headless` | `qemu/windows-x86_64/qemu-system-x86_64.exe` | **已实测**：`qemu/darwin-aarch64/qemu-system-aarch64-headless`（§7.0.2）；Intel Mac 包为 `emulator-darwin_x64-*` |
| 模拟器 / adb 路径 | `runtime/emulator/emulator` | `runtime\emulator\emulator.exe` | `runtime/emulator/emulator`（同 Linux） |
| 进程枚举 | 遍历 `/proc/[0-9]*` 读 `comm` + `cmdline`（`common.sh:182-199`） | `Get-CimInstance Win32_Process`（`common.ps1:127`） | **`ps -eo pid,comm,args`**（无 `/proc`） |
| GPU 探测 | `/dev/dri/renderD*` 字符设备可读写（`common.sh:93`） | `Get-CimInstance Win32_VideoController`（`common.ps1:65`） | **`system_profiler SPDisplaysDataType`** |
| 校验和 | `sha1sum` / `sha256sum` | `sha1sum` / `sha256sum` | **`shasum -a 1` / `shasum -a 256`**（`sha*sum` 默认不存在） |
| 打包锁 | `flock -n 9`（`release.sh:616`、`package-rom.sh:58`） | — | **无 `flock`** → `mkdir` 锁或 python 锁 |
| 数值/时间格式化 | `stat -c %s`、`numfmt --to=iec`、`date -Iseconds` | — | **`stat -f%z`、自己算 IEC、`date -u +%Y-%m-%dT%H:%M:%SZ`** |
| 稀疏归档 | `tar --sparse --numeric-owner`（`emulator.sh:589,680`） | — | `bsdtar` 的 `--sparse` 语义不同，且**默认写入扩展属性** → 必须 `COPYFILE_DISABLE=1` / `--no-mac-metadata` |
| 网络桥接 | `-net-tap tap0` + `tools/net-bridge.sh`（177 行） | ❌ 未实现 | ❌ **同样不适用**（见 §3.5） |
| 解压后的可执行位 | `unzip` 保留 | — | **`unzip` 丢可执行位** → 必须补 `chmod +x`（`release.sh:324` 那句要覆盖 `runtime/`） |
| 从互联网下载的包 | — | — | **Gatekeeper / quarantine**（见 §3.4） |
| 自启 | systemd / supervisord | 计划任务 | **launchd（LaunchAgent，理由见 `14-macos-host-notes.md` §6）** |
| 脚本解释器 | 宿主 bash（≥4） | PowerShell | **`/bin/bash` 是 3.2**（见 §3.3 第 6 条） |

### 3.2 要新写的文件

| 文件 | 参照 | 估算行数 |
|---|---|---|
| `macos/emulator.sh` | `scripts/emulator.sh`(818) 的子集 | 300–450 |
| `macos/run-macos.sh` | `run-linux.sh`(354) / `run-windows.ps1`(233) | 150–250 |
| `macos/fetch-emulator.sh` | `windows/fetch-emulator.ps1`(131)，host-os 取 `macosx` | 80–120 |
| `macos/fetch-images.sh` | `fetch-images.ps1`(112) | 60–100 |
| `macos/preflight.sh` | `preflight.ps1`(85) | 80–120 |
| `packaging/bin/darwin/`：`lib.sh` / `start-headless.sh` / `stop.sh` / `status.sh` / `verify.sh` | `bin/linux/`(570) + `bin/windows/`(690) | 350–550 |

### 3.3 共享层要改的点（少，但每一条都会真拦人）

1. **`host_gpu_available()`（`common.sh:88-117`）在 macOS 上永远返回"没有 GPU"** —— 它探的是
   `/dev/dri/renderD*`，于是 `-gpu auto` 被判成 `swiftshader_indirect`。**有显卡却用软渲染**，是最容易
   被"跑起来了"掩盖的性能问题。改法：按 OS 分支，macOS 用 `system_profiler`。
2. **`emu_pid_for_port()`（`common.sh:182-199`）读 `/proc`** —— macOS 无 `/proc`，改用 `ps`。
   ⚠️ 那段注释里记录的坑（`pgrep -f "qemu-system.* -port N"` **会把调用者自己匹配上**，
   可能导致 kill 到无辜进程）在 macOS 上**同样存在**，别退回 `pgrep -f`。
3. **`flock` 不存在** —— `release.sh:616` 与 `package-rom.sh:58` 的 `exec 9>"$LOCK"; flock -n 9` 要换实现。
4. **GNU 工具名** —— `sha1sum`/`sha256sum`/`stat -c`/`numfmt`/`date -Iseconds`/`du --apparent-size`/`sed -i`/`readlink -f`
   全部要 macOS 分支（`shasum`、`stat -f%z`、`date -u +…`、`du -shA`、`perl -pi -e`、`cd`+`pwd -P`）。
   `release.sh:95` 的 `require_tools` 白名单要按平台变。
5. **`tar` 语义** —— `emulator.sh` 的导出/导入靠 `tar --sparse` 把 48 G 表观、551 M 实占的稀疏镜像压小
   （注释在 `emulator.sh:472-475`），并用 `tar -tf` 的**条目数**做读回校验（`:597`）。
   bsdtar 默认会把扩展属性写成 `._*` 旁文件，**条目数会变、校验会误判**；`--sparse` 的语义也不同。
   必须显式关掉元数据写入，并重新确认稀疏与条目数两条断言。
6. **`declare -A`（`release.sh:46`）需要 bash 4+** —— macOS 自带 bash 3.2。要么改写法，要么在 preflight 里
   强制要求 `brew install bash` 并用绝对路径解释器。**建议改写法**：只为两个关联数组就让 Mac 用户装 bash，
   是给交付加无谓的门槛。
7. **`setsid`（`emulator.sh:137`、`packaging/bin/linux/start-headless.sh:141`）** —— macOS 无 `setsid`，
   用 `nohup … &` + `disown`；若要开机自启则落到 launchd plist。
8. **`unzip` 丢可执行位** —— 包内 `runtime/emulator/emulator`、`runtime/platform-tools/adb` 要显式 `chmod +x`。
   Linux 侧的等价代码在 `release.sh:324,339`，Mac 侧必须覆盖 `runtime/` 这一层。

### 3.4 打包与 Gatekeeper

- **quarantine**：从浏览器下载的 zip 会给解出的每个文件打 `com.apple.quarantine`，`emulator` 一执行就被拦。
  要么在 `START-HERE.md` 里写清步骤，要么在 `start-headless.sh` 里检测并提示 `xattr -dr com.apple.quarantine`。
- **签名/公证**：包里的 `emulator`、`adb` 是**上游已签名**的，问题只在分发层。
  是否给自己的包做签名+公证，取决于分发方式（内网 vs 公网）——**这是一个决定，不是默认动作**。
- **包内文档**：`release.sh` 用模板替换生成 `START-HERE.md`（`:348-390`），现在只认
  `if plat == "linux" … else …` 两个分支，要加第三个；`adb` 的路径拼接也是二元判断（`:384`）。

### 3.5 桥接模式：macOS 上没有等价物，直接降级

`-net-tap` 是 Linux 的 tun/tap。macOS 没有对应机制，而且这**不是 macOS 特有的缺口**——
Windows 侧本来就没实现（`packaging/README.md`：「`tools/net-bridge*.sh` **仅 linux 包**；Windows 侧 `-net-tap` 没实现」）。

**macOS 走同一条降级路径**：模拟器默认用户态 NAT + 宿主端口转发。`tools/net-bridge.sh`(177) +
`net-bridge-ifup.sh`(21) 不进 mac 包，`run-macos.sh` 里不出现 `-net-tap` 分支。

> ⚠️ 于是「mac 包没有桥接」**不是待修的缺陷，而是与 windows 包一致的能力面**。
> 文档里要写成"已知限制"，别写成 TODO。

### 3.6 不适用于 macOS 的部分（别顺手移植）

以下脚本与"Linux 构建机 + Android 目标设备"强绑定，Mac 上一律不需要：

`tools/setup-host.sh`（`apt-get`/`systemctl`）、`tools/integrate-aosp.sh`、`tools/build-*.sh`、
`tools/deploy-remote-control.sh`（`adb`+`setenforce`）、`tools/verify-cuttlefish.sh`、
`tools/lan-up.sh`（supervisord）、`tools/integrate-sepolicy.sh`、`tools/remote-control-supervisord.sh`、
`dev/02-native-daemon/Makefile`（主机版，见 §5 说明）、`dev/05-controller-app` 的构建链。

Mac 包 = **运行时控制面 + 打包**这一层，不包含构建链。

---

## 4. 两条线的关系：三个产品，别只保一条

用户决策：**Intel Mac 也要支持 → 两条 ROM 线都保留**。于是产品矩阵是：

| 产品 | `lunch` 目标 | guest 架构 | 跑在哪 | 状态 |
|---|---|---|---|---|
| 现有 | `remote_control_x64_arm64-userdebug` | x86_64 + 翻译层 | Linux x86_64（KVM）、Windows x86_64（WHPX）、**Intel Mac**（HVF，未实测） | ✅ 已建出并使用 |
| 新增 | `remote_control_arm64-userdebug` | arm64 原生 | **Apple Silicon**（HVF）、arm64 Linux 宿主（KVM，未实测） | 📄 本文方案 A |

**两边的公共部分**：`remote-control` 服务本体、`emulator/config.ini`、`packaging/` 的模板与
`START-HERE.md` 骨架、`release.sh` 的分派表结构、`docs/` 的验收口径。

**两边必须分开的部分**：设备树（BoardConfig/vendor/内核）、翻译层的存在与否、验收里与翻译层相关的断言（§2.6）。

> ⚠️ **arm64 产品不是"为了 macOS 顺便做的"，它自己就有价值**：
> 现有 x86_64 那条线的加速依赖 KVM/WHPX/HVF，**arm64 宿主（Apple Silicon、ARM 云主机、ARM 工作站）
> 一条都吃不到**。所以方案 A 同时也是"能不能在 ARM 宿主上跑"的答案。

---

## 5. 与 `14-macos-host-notes.md` 的边界（避免两处都写、说法还不一样）

`14-macos-host-notes.md` 研究的是**另一个问题**：把 `remote-control` 本身移植成 macOS 上的被控端
（用 `CGEventPost` 注入、用 ScreenCaptureKit 抓屏、TCC 权限怎么拿、要不要 bundle 化）。

| 问题 | 归哪份 |
|---|---|
| Mac 当**宿主机**跑安卓模拟器（本文） | **本文** |
| Mac 当**被控端**（截图/注入 Mac 自己的桌面） | `14-macos-host-notes.md` |
| 「macOS 宿主脚本要写多少行」 | **本文 §3** |
| 「macOS 上 `SOCK_SEQPACKET` 不可用」 | `14-macos-host-notes.md` §5（**对本文不适用**——被控端在 guest 里，socket 是 Linux 的） |
| 「root LaunchDaemon 注入不了输入」 | `14-macos-host-notes.md` §6（**对本文不适用**，同上） |

> 两份文档里**唯一会互相影响**的点是 §2.7：ROM 只能在 Linux 上编。
> 若将来真要在 Mac 上做构建，`14` 的 §7（交叉编译与签名/公证）是入口。

---

## 6. 改动量汇总

| 类别 | 新增 | 改动 | 参照物 |
|---|---|---|---|
| A. arm64 ROM 产品 | 280–420 行（设备树 4 个文件 + 软链） | 120–200 行（构建系统参数化 + 桥分支） | 现有 `BoardConfig.mk` 104 + `product` 168 + `device.mk` 35 |
| B. macOS 宿主脚本 | 1500–2200 行 | 400–600 行（共享层 GNU→BSD 适配） | `emulator.sh` 818 + `run-linux.sh` 354 + `bin/linux` 570；`emulator.ps1` 735 + `run-windows.ps1` 233 + `bin/windows` 690 |
| **合计** | **约 1800–2600 行** | **约 520–800 行** | 现有两套骨架的 **约 2/3** |

**没有算进去的**（因为不该算）：Mac 上的 ROM 构建链（不做，§2.7）、桥接网络（不做，§3.5）、
`14-macos-host-notes.md` 涉及的被控端改造（另一条产品线）。

---

## 7. 风险与未决事项

### 7.0 实施记录（2026-10-03，实测）

动手前先把"能推翻方案"的三条验了。**结论：方案 A 的三个前提全部成立**，其中两条比预期更好。

#### 7.0.1 arm64 ROM 早就编出来过 —— 不用再编一遍

`aosp/out/target/product/emulator64_arm64/` 里有一套 **2026-09-28 的完整产物**（19 GB），
构建指纹是 `Android/sdk_phone64_arm64/emulator64_arm64:12/SP1A.210812.016.C2/root09280433:userdebug/test-keys`。
关键文件都在：`system.img` 778 MB、`system-qemu.img` 4.3 GB、`super.img` 4.3 GB、
`product-qemu.img` 278 MB、`system_ext-qemu.img` 133 MB、
`kernel-ranchu` 19.9 MB（`gzip compressed data, was "kern.patched"`）、`ramdisk-qemu.img` 4.2 MB、`vendor_boot.img` 100 MB。

> ⚠️ **目标名有两个，别混**（本轮实测踩到，写错过一次）：
>
> | 概念 | 值 |
> |---|---|
> | `lunch` 目标 | **`sdk_phone64_arm64-userdebug`** |
> | `PRODUCT_DEVICE`（也是 `ANDROID_PRODUCT_OUT` 的目录名） | **`emulator64_arm64`** |
>
> 写成 `lunch emulator64_arm64-userdebug` **不会报错**——`lunch` 找不到该产品时会**静默回落**到
> `aosp_arm`（`TARGET_ARCH=arm`，32 位 ARM），然后在错误的输出目录里编出一堆用不上的东西。
> 判断依据只有一行：`lunch` 之后必须核对 `TARGET_PRODUCT` 与 `ANDROID_PRODUCT_OUT`。

> 含义：**§2.8 的第 1 步（"先编上游产品验通路"）事实上已完成**。剩下的不是"能不能编"，是"编出来的东西怎么起"。

#### 7.0.2 macOS(aarch64) 模拟器包里有 arm64 后端，而且**没有** x86_64 后端

从 SDK 清单下载并逐项核对（稳定渠道，未用镜像的二手信息）：

| 项 | 实测值 |
|---|---|
| 包 | `emulator-darwin_aarch64-16428233.zip` |
| 大小 | **416,112,708 字节**（与清单 `complete/size` 一致） |
| sha1 | **`3af4fe44ce82b3d88ae5678a53735f27ad729c15`** —— 与清单逐字符相同 ✓ |
| 包内 `Pkg.Revision` | **37.2.12**（`Pkg.BuildId=16428233`） |
| `emulator/qemu/` 下的后端 | **`darwin-aarch64`**（唯一一个） |
| arm64 guest 后端 | ✅ `qemu/darwin-aarch64/qemu-system-aarch64`、`qemu-system-aarch64-headless` |
| x86_64 guest 后端 | ❌ **不存在**（`emulator/qemu/darwin-aarch64/` 是唯一目录） |
| 顺带 | 包里自带 `qemu-img`（§3.6 提到的 `tools/diagnose-*.sh` 依赖它可以少一个 Homebrew 依赖） |

**两条结论**：

1. **方案 A 的前提成立** —— Apple Silicon 上有能跑 arm64 guest 的官方后端。这是本文档最想确认的一条。
2. **Apple Silicon 上不存在"跑现有 x86_64 ROM"的选项** —— 不是慢，是**根本没有那个后端**。
   这从"我们推演 TCG 会很慢"升级成了"包层面就不支持"。**§1.3 那张表的最后一行可以改成硬结论。**

#### 7.0.3 SDK 清单里的三平台对齐情况

稳定渠道（channel-0）同一个版本的包**三平台齐全**，这消掉了 §7.2 里"三平台对齐"的顾虑：

| 包 | 版本 | 大小 |
|---|---|---|
| `emulator-linux_x64-16428233.zip` | 37.2.12 | 333 MB |
| `emulator-darwin_aarch64-16428233.zip` | 37.2.12 | 396 MB ← **Apple Silicon** |
| `emulator-darwin_x64-16428233.zip` | 37.2.12 | 466 MB ← **Intel Mac** |
| `emulator-windows_x64-16428233.zip` | 37.2.12 | 434 MB |
| `platform-tools_r37.0.1-{linux,darwin,win}.zip` | 37.0.1 | —（darwin 那份同时适用两种 Mac） |

> 注意：项目现在用的是 **37.1.11**（`docs/03-delivery.md` 记录的 `emulator-windows_x64-15917651.zip`）。
> 加 macOS 时如果要"三平台同 build id"，就是整条线一起升到 37.2.12。**这是一次版本决策，不是 macOS 独有的改动。**
> 升级要重新验收：`docs/08-emulator-version-notes.md` 记录过 37.2.11 已移除 arm64 后端 ——
> 那条结论针对的是**跨架构**场景（x86_64 宿主跑 arm64 guest），与本方案的 arm64 宿主 + arm64 guest 不是同一件事。

#### 7.0.4 一条被挡住的验证（不隐瞒）

想让这套 arm64 ROM 在**本机 Linux 上真跑一次开机**当基线，**没成功**，原因是模拟器版本而不是 ROM：

| 尝试 | 结果 |
|---|---|
| 直接用产物目录当 `-sysdir` | `ERROR: Your system directory is missing the 'vendor.img' image file` —— arm64 产物**没有 `vendor.img`**（x86_64 那个产品有；差异待查，见 §7.2） |
| 加 `-vendor vendor_boot.img` / `-qemu -audiodev none` | 均失败，停在 `qemu-system-aarch64-headless: PCI bus not available for hda` |
| AOSP 自带模拟器 | 是 **30.8.3**，`prebuilts/android-emulator/` 下**没有 `qemu/` 目录**；`docs/08-emulator-version-notes.md` 已记录它"太旧，镜像要求 ≥31.2.7" |

**关于 `vendor.img`：已定位并修好（本轮）**

两个产品的对比显示 x86_64 的 `vendor.img`/`vendor-qemu.img` 时间戳（08:12/08:13）**晚于**其他镜像，
说明它是**单独的镜像目标**产物，不是 `droid` 全量构建的副产品。arm64 那套是"编到能开机为止"的，
从没编过这个目标。修法：

```bash
# 在构建容器里
lunch sdk_phone64_arm64-userdebug     # ⚠️ 不是 emulator64_arm64-userdebug（会静默回落）
m -j8 vendorimage
```

实测产出 `vendor.img` 102,883,328 字节 + `vendor-qemu.img` 105,906,176 字节，`m` 退出码 0。
**两个坑**：① 上次被中断的编译会留下 `out/.lock`，新编译会报
`Tried to lock out/.lock ... timed out polling every 1s until 10s`，需先 `rm -f out/.lock`；
② 产物目录是 `out/target/product/emulator64_arm64`（跟 `PRODUCT_DEVICE` 走，不是 lunch 名）。

**补齐后重试启动：QEMU 那关过了，但模拟器太旧**

补上 `vendor.img` 后，启动一路推进到了比之前远得多的位置：

```
qemu-system-aarch64-headless: PCI bus not available for hda      ← 这行其实是**非致命**的
emulator: Requested console port 5570: Inferring adb port 5571.
emulator: feeding guest with passive gps data, in headless mode
emulator: INFO: userspace-boot-properties.cpp:249: Sending adb public key [...]
（随后干净退出，退出码 0）
```

"启动 1 秒后干净退出、日志停在 `Sending adb public key`" **正是 `docs/08-emulator-version-notes.md`
记录过的症状**——那份文档的结论是 AOSP 自带模拟器 **30.8.3 太旧，镜像要求 ≥31.2.7**。
本轮复现了同一症状，且**排除了 `vendor.img` 这个曾以为是原因的因素**。

> 所以 `hda` 那条报错**不是**根因，别再往音频设备方向查。真正的天花板是模拟器版本：
> Linux 侧手上只有 30.8.3（AOSP 自带）和 27.x（`/opt/android/emulator-new`），
> 而 **37.2.12 的 Linux 包必须在 Linux 上验证时需要联网下载 333 MB** ——
> 这条验证的正确落点仍是**目标 Mac + darwin-aarch64 包**。

> 正确的验证路径是：**在目标 Mac 上用 37.2.12 的 darwin-aarch64 包起这套 arm64 ROM**。
> 在没有 Mac 的构建机上无法完成这一步，别再花时间试 Linux 侧的组合。

#### 7.0.5 已写出 arm64 产品的设备树（本轮）

`dev/04-x64-android/device/` 下新增一个产品，原先的 x86_64 产品与共享 sepolicy **一个字节没动**：

```
device/
├── AndroidProducts.mk                      ← 改为两个产品并列
├── remote_control_arm64/                   ← 新增（本轮）
│   ├── BoardConfig.mk                       TARGET_ARCH := arm64，无 TARGET_NATIVE_BRIDGE_*
│   ├── device.mk                            inherit emulator64_arm64/device.mk + BIOS + 以太网
│   └── product/remote_control_arm64.mk      PRODUCT_NAME := remote_control_arm64
└── remote_control_x64_arm64/                ← 原样（含 sepolicy，被两个产品共用）
```

写的时候落进文件里的几条关键判断（都在注释里）：

1. **`QEMU_USE_SYSTEM_EXT_PARTITIONS` 与 `PRODUCT_USE_DYNAMIC_PARTITIONS` 必须写在 `include` 之前**——
   `BoardConfigEmuCommon.mk` 用 `ifeq` 决定动态分区列表是四个独立分区还是
   `system/product`+`system/system_ext` 的 GSI 布局，而 make 的条件在 include 那一刻求值。
2. **sepolicy 复用同一份**，`BOARD_SEPOLICY_DIRS` 指向 `remote_control_x64_arm64/sepolicy`——
   历史命名但内容与架构无关。文件里明确写了"不要为了路径好看复制一份"，理由是那会造成第二处真源。
3. **翻译层那三条属性一条都不能抄**，尤其 `ro.dalvik.vm.native.bridge=libndk_translation.so`：
   在原生 arm64 上设了它，ART 会去找一个不存在的库。这条单独写了一段注释防止以后"对齐两个产品"时被顺手抄过来。
4. **`BUILD_BROKEN_DUP_RULES` 先不设**，注释里留了一行"报 duplicate rules 时把它打开"——不预先放行没被证明必要的东西。
5. `PRODUCT_MODEL` 写成 `remote-control arm64 (native, no translation layer)`，
   便于 `getprop ro.product.model` 一眼区分两个产品。

**尚未验证**：`m` 全量构建、开机、服务起没起（见 §7.1 #3/#5）。
**但构建系统层面已验通**，见下。

#### 7.0.6 新产品的 lunch 已验通（本轮）

```
✓ remote_control_arm64-userdebug        （新增）
✓ remote_control_x64_arm64-userdebug    （原有，未被破坏）
```

停在 arm64 产品上核对到的关键变量：

| 变量 | 值 | 说明 |
|---|---|---|
| `TARGET_PRODUCT` | `remote_control_arm64` | |
| `ANDROID_PRODUCT_OUT` | `/aosp/out/target/product/remote_control_arm64` | |
| `PRODUCT_MODEL` | `remote-control arm64 (native, no translation layer)` | 一眼区分两个产品 |
| `TARGET_NATIVE_BRIDGE_ARCH` | **空** | ✓ 证明翻译层没被带进来 |
| `PRODUCT_PACKAGES` 里 | `remote-control`、`remote-control-launch`、`rcctl`、`pm` | 四个都在 |

**ABI 对照**（这条最能说明两个产品的本质差别）：

| 产品 | `TARGET_CPU_ABI_LIST` |
|---|---|
| `remote_control_arm64` | `arm64-v8a` |
| `remote_control_x64_arm64` | `x86_64,arm64-v8a` ← 翻译层带来的第二项 |

**过程中修掉一个真错误**（验证器抓到的，不是我推出来的）：

```make
# device/remote_control/remote_control_arm64/BoardConfig.mk:32
error: cannot assign to readonly variable: PRODUCT_USE_DYNAMIC_PARTITIONS
dumpvars failed with: exit status 1
```

`PRODUCT_USE_DYNAMIC_PARTITIONS` 走到 `board_config.mk` 时已经 **readonly**（在 `config.mk` 更早处固化），
**只能在产品 mk 里设**。顺带发现 x86_64 那个产品**两边都写了**——BoardConfig 里那行之所以没炸，
是因为产品侧已经先把它设成同一个值、早于 BoardConfig 被求值。**本产品不复制那个巧合**，
只在产品 mk 里设一次，BoardConfig 里留了一段注释说明为什么不能放这儿。

#### 7.0.7 arm64 服务本体已编出（本轮）

在**新产品**下编服务本体，验证 `Android.bp` 的平台私有库依赖在 arm64 目标上同样成立：

```bash
lunch remote_control_arm64-userdebug
m -j8 remote-control rcctl remote-control-launch     # m 退出码 = 0
```

三个产物全部是**真 ARM aarch64 ELF**：

| 产物 | 大小 | 类型 | 关键 NEEDED |
|---|---|---|---|
| `system/bin/remote-control` | 1,033,224 | ELF 64-bit LSB pie, **ARM aarch64** | `libbase` `libbinder` `libcutils` `libgui` `liblog` `libmediandk` `libui` `libutils` |
| `system/bin/rcctl` | 55,848 | 同上 | `libjnigraphics` `libbase` `liblog` |
| `system/bin/remote-control-launch` | 47,152 | 同上 | 只有 `libc` `libm` `libdl`（壳够薄，符合设计） |

**没有翻译层混进来**（`system/lib64` 下 `libndk_translation*` 命中 **0**、`system/lib64/arm64` 条目 **0**），
这正是 arm64 产品应有的样子。

> 编译期开关的字符串核对（`strings` 计数）：
> `surfaceflinger` 5 处、`screencap` 16 处、`uinput` 19 处、**`libndk_translation` 0 处**。
> 说明这一版二进制里同时带了 SF 与 screencap 两条截图后端、以及 uinput 注入后端，
> 而且**没有任何翻译层残留**。

含义：**arm64 ROM 这条线现在"服务能在目标上编出来"已经成立**，剩下的验证是"放进镜像能不能开机自启"。

#### 7.0.8 方案 B 的头两个脚本（本轮）

放在 `dev/04-x64-android/macos/`（与 `windows/` 平行）：

| 文件 | 职责 | 已验证 |
|---|---|---|
| `macos/fetch-emulator.sh` | 按 **host-os=macosx + host-arch** 选包、下载、校验 sha1、解包、**补可执行位** | ✅ 选包逻辑对真实清单验过；`bash -n` 通过；在 Linux 上正确自挡 |
| `macos/preflight.sh` | 系统/架构、**`kern.hv_support`**、必需工具、模拟器与后端架构、adb、磁盘、Gatekeeper/quarantine、ROM 完整性 | ✅ `bash -n` 通过；在 Linux 上正确报"不是 macOS"并继续做完其余检查 |

`fetch-emulator.sh` 的选包逻辑用真实 SDK 清单做了六组验证（这次没有真机，只能验到这一层）：

| 包 | host-os | host-arch | 选中 |
|---|---|---|---|
| emulator | macosx | aarch64 | `emulator-darwin_aarch64-16428233.zip` 396 MiB |
| emulator | macosx | x64 | `emulator-darwin_x64-16428233.zip` 466 MiB |
| emulator | linux | x64 | `emulator-linux_x64-16428233.zip` 333 MiB |
| emulator | windows | x64 | `emulator-windows_x64-16428233.zip` 434 MiB |
| platform-tools | macosx | aarch64 / x64 | `platform-tools_r37.0.1-darwin.zip` 15 MiB（**两种 Mac 共用一份**） |
| emulator | macosx | riscv64 | ✗ 正确报"没有该组合的 archive" |

`preflight.sh` 把 macOS 上"不报错但静默变慢/静默失败"的四类坑做成了可判定项：
后端架构与本机是否匹配、HVF 是否可用、可执行位是否被 `unzip` 丢掉、
宿主 bash 是不是 3.2（`release.sh` 用了 `declare -A`，需 bash 4+）。

**补记（同一轮的延伸）：包内 `bin/darwin/` 五个脚本也已写出** ——
`lib.sh` / `start-headless.sh` / `stop.sh` / `status.sh` / `verify.sh`，
与 `bin/linux/`、`bin/windows/` 两侧语义逐条对齐，文件数都是 5。

平台相关的重写只有四处（其余逻辑与 linux 版一致）：

| 项 | Linux | macOS |
|---|---|---|
| 进程枚举 | 遍历 `/proc/*/comm` + `cmdline` | `ps -Ao pid=,comm=,args=` + awk 按字段精确比 `-port` |
| GPU 探测 | `/dev/dri/renderD*` 可读写 | `system_profiler SPDisplaysDataType`，判据是**有没有 Metal** |
| 虚拟化 | `/dev/kvm` 可读写 | `sysctl -n kern.hv_support` = 1 |
| 后端路径 | 写死 `linux-x86_64` | **按本机架构拼**（`darwin-aarch64` / `darwin-x64`） |
| 后台化 | `setsid nohup` | `nohup … &` + `disown`（macOS 没有 setsid） |

**两处有意与 linux 版不同**：

1. **没有 `--bridge`**：传了会**明确拒绝**并说明"这不是待修的缺陷 —— windows 包同样没有桥接"，
   而不是静默忽略。（macOS 没有 `-net-tap` 等价物，见 §3.5。）
2. **`verify.sh` 的判据按产品分派**：原生 arm64 断言"没有翻译层残留 + 服务是 aarch64"；
   x86_64 桥产品断言"翻译层在场 + 服务是 x86_64"。核心检查是**读文件头里的 ELF 架构**
   （`od` 读 `e_machine` 偏移 18：`0x3E`=x86-64、`0xB7`=aarch64），
   不信设备自述、不信接口返回的 ok —— 这是上游被坑三次换来的纪律。

**这些脚本怎么验的（手上没有 Mac）**：做了个**离线测试台** ——
在 Linux 上用命令 stub 把 `uname`/`sw_vers`/`sysctl`/`system_profiler`/`ps`/`adb`
换成假实现并 PATH 前置，让脚本以为自己在 macOS 上跑。18 个用例：

| 用例 | 期望 | 结果 |
|---|---|---|
| arm64 镜像 + arm64 产物 | 全绿 | ✅ 11 项检查 0 失败 |
| **故意混入 x86_64 产物** | 三项架构全报错 | ✅ 4 项失败（`x86-64` vs 期望 `aarch64`） |
| x86_64 镜像交给 arm64 的 Mac | `status.sh` 标红"跑不了" | ✅ 明确报不匹配 |
| `kern.hv_support=0` | 报错 + 提示会退 TCG | ✅ |
| 无 Metal | `resolve_gpu_mode` 退 `swiftshader_indirect` | ✅ |
| GPU 回退链（host 起不来） | 自动退 swiftshader，仍不行则报"起不来"+日志路径 | ✅ |
| 进程枚举 | 命中真 qemu；**调用者自己的命令行不被误算** | ✅ 恰好 1 个 |
| `--bridge` / 奇数端口 | 明确拒绝 | ✅ 退出码 1 |
| 设备不在线 | `verify --device` 报失败、退出码 1，**不假装通过** | ✅ 6 项失败 |
| 设备在线（stub 自述 arm64） | 14 项检查一致 | ✅ 只差"服务进程在跑"一项（夹具限制） |

**测试夹具本身也修过一次**：`adb` stub 原来是空文件（永远返回 0），
于是"设备起来了"被误判成真、`start-headless.sh` 一路走到"等开机"都不报错。
换成可被 `STUB_ADB_OK` 控制的真 stub 之后才暴露 ——
**这正是"判据不能只看命令返回 0"的一个现场例子**（和项目里
「接口说成功而设备没动」那三次是同一类错误）。

**踩到并修掉的一处**：`set -o pipefail` 下 `XA="$(xattr -l ... | grep -c ...)"`
会因为 `xattr` 在该文件"无扩展属性"时返回非 0 而带出非 0 退出码，导致脚本提前结束。
改成 `XA="$(...)" || XA=0`。

#### 7.0.9 共享脚本的产品分支 + `fetch-images`（本轮）

**一、`scripts/` 的五个脚本改成产品感知**（`PRODUCT=x64_arm64`（默认）/ `PRODUCT=arm64`）

改动的核心是把"这个产品要不要翻译层"收敛成**一个变量**，而不是散在五处硬编码：

```bash
# scripts/common.sh
PRODUCT="${PRODUCT:-x64_arm64}"      # 新增，默认值与改动前一致
case "$PRODUCT" in
    x64_arm64) HAS_BRIDGE=1 ;;
    arm64)     HAS_BRIDGE=0 ;;
    *) <报错并列出可选值> ;;
esac
PRODUCT_NAME="remote_control_$PRODUCT"
LUNCH_TARGET="$PRODUCT_NAME-userdebug"
PRODUCT_OUT="${PRODUCT_OUT:-$AOSP_DIR/out/target/product/$PRODUCT_NAME}"
PRODUCT_HOST_ARCH=$([ "$HAS_BRIDGE" = 1 ] && printf 'x86_64' || printf 'arm64')
```

| 文件 | 改了哪几处（按行号） | 行为差别 |
|---|---|---|
| `common.sh` | `PRODUCT_NAME` 那一行（原来写死 `remote_control_x64_arm64`） | 新增 `PRODUCT` / `HAS_BRIDGE` / `PRODUCT_HOST_ARCH`；非法值立即报错并列出可选项 |
| `apply-overlay.sh` | 载荷校验段、同步载荷段、落点校验段 | `HAS_BRIDGE=0` 时**整段跳过**载荷（并明确打印"跳过"而不是静默）；新增"残留 bridge 目录会被清掉"；**两个产品的设备树一起校验** |
| `build-rom.sh` | 前置检查 | `HAS_BRIDGE=0` 时不再要求 `bridge-copy.mk`；改为检查当前产品的设备树目录存在 |
| `status.sh` | "AOSP 注入"一节 | 两个产品**分别**显示是否就位 + 翻译层载荷状态 + 当前 PRODUCT 及其性质 |
| `package-rom.sh` | `source.properties`、`MANIFEST.txt` | 见下 |

**两个设计判断（都写进了注释）**：

1. **设备树"两个产品一起同步"，不是只同步当前产品。** `AndroidProducts.mk` 里两个产品并列，
   AOSP 树里缺任何一个，对应产品的 `lunch` 目标就会消失——只同步当前产品会让并行开发互相踩。
   差别只在"要不要搬翻译层载荷"，那是 `x64_arm64` 独有的。
2. **`source.properties` 里的 `SystemImage.Abi` 必须与产品实际 guest 架构一致**：
   `x86_64` → `x86_64`，`arm64` → `arm64-v8a`。模拟器靠它判断 guest 是不是本机架构，
   写错会让它按错误架构布置机器。同理 `MANIFEST.txt` 里 arm64 产品会如实打印
   "无翻译层"并提示"跑不了纯 x86 应用"。

**回归验证**（项目纪律："别把 Linux 弄坏"）：

| 用例 | 结果 |
|---|---|
| 默认 `PRODUCT`（`status.sh` 全跑） | ✅ 输出与改动前一致（载荷 91 文件 / 拷贝规则 91 条 / 产物 5.7 G / 设备在线） |
| `PRODUCT=arm64 ./apply-overlay.sh` | ✅ 打印"跳过翻译层载荷 ✓ 不适用"，退出码 0 |
| `PRODUCT=arm64 ./apply-overlay.sh --check` | ✅ 同上，且**明确说明没查载荷**（避免"检查通过"被误读） |
| `PRODUCT=bogus ./apply-overlay.sh` | ✅ 报错并列出可选项 |
| `PRODUCT=arm64 ./status.sh` | ✅ 正确显示"当前 PRODUCT：arm64（原生 arm64，无翻译层）" |
| 五个脚本 `bash -n` | ✅ 全过 |

**二、`macos/fetch-images.sh`（方案 B 第三个脚本）**

从构建机把 ROM 交付目录拉到 Mac。算法与 `windows/fetch-images.ps1` 一致
（比对远端大小 → `scp` → 按 `SHA256SUMS` 校验），但有三处**有意不同**：

1. **远端缺必需文件时直接失败**，不学 Windows 版那样只打个 warning 走到最后。
   那版会在中途的解压/校验阶段给出更迷惑的报错。
2. **`initrd` 不只是"检查存在"**：QEMU 拿到不存在的 initrd 会
   "主循环立刻结束且不报错"（`docs/02-build-traps.md`），所以脚本会**读回确认非空**，
   两种来源（`ramdisk-qemu.img` 合并版 / 裸 `ramdisk.img`）分别给出不同级别的提示。
3. **quarantine 默认只报告，不清除**：清除等于替用户声明"这个来源我信"，
   而这可能正是从网上下载的归档。要清必须显式 `--dequarantine`。

macOS 专有处理：`shasum -a 256`（没有 `sha256sum`）、本地目录拷贝优先 `ditto`
（带扩展属性最稳）、**没有 `ditto` 时回退 `cp -a`**（对 `.img` 等价，且让脚本能在开发机上测）。

**离线验证**（造 fixture 跑四个用例）：

| 用例 | 期望 | 结果 |
|---|---|---|
| 正常路径（源里**故意不给** `initrd`） | 自动由 `ramdisk-qemu.img` 生成、SHA256SUMS 全过 | ✅ 退出码 0，"校验通过（10 个文件）" |
| 篡改一个镜像 | 必须失败并指出是哪个文件 | ✅ 退出码 1，"校验不符：system-qemu.img" |
| 删掉一个必需文件 | 必须失败 | ✅ 退出码 1，"缺文件：vendor-qemu.img" |
| `initrd` 与 `ramdisk*` 都没有 | 必须失败并说清后果 | ✅ 退出码 1，"既没有 initrd 也没有 ramdisk*.img —— 模拟器起来会静默退出" |

**这次测试抓到一个真 bug**：脚本原本只走 `ditto`，而 `ditto` **只存在于 macOS** ——
在开发机上直接报 `ditto: 未找到命令` 然后死掉。已加回退分支。

#### 7.0.10 sepolicy 分区：一次"照抄注释而非照抄代码"的失败（本轮）

**症状**：arm64 全量构建在 1% 处失败，卡在 vendor 策略：

```
FAILED: out/target/product/remote_control_arm64/obj/ETC/vendor_sepolicy.cil_intermediates/vendor_sepolicy.cil
device/remote_control/remote_control_x64_arm64/sepolicy/remote_control.te:404:
    ERROR 'unknown type odsign_prop' at token ';' on line 42076:
allow remote_control odsign_prop:file { getattr open read map };
checkpolicy:  error(s) encountered while parsing configuration
```

**根因**：我在 arm64 的 `BoardConfig.mk` 里写的是

```make
BOARD_SEPOLICY_DIRS += device/remote_control/remote_control_x64_arm64/sepolicy   # ← 错的
```

而 x86_64 产品**实际生效**的是

```make
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/remote_control/remote_control_x64_arm64/sepolicy
```

`odsign_prop` 是**平台私有属性类型**，vendor 策略看不见它 → 编译期直接失败。

**为什么会写错**：x86_64 那份 BoardConfig 里有一段很长的注释，标题是
「策略分区是个双向取舍，实测两头都撞过」，结尾写着「**当前选 vendor**」。
我读了注释，没逐行核对哪一行是生效的（那行 vendor 写法其实是**被 `#` 注释掉的**）。
**注释与代码相反，而我信了注释。**

**两个修复**：

1. `remote_control_arm64/BoardConfig.mk` 改用 `SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS`，
   并把"注释与生效行相反"这件事直接写进文件（含失败原文），防止下一个人重犯。
2. **顺手纠正了 x86_64 那份的过时注释**：把那段双向取舍标注为
   「历史记录，不要照着它做决定」，旧写法显式标成「**不要启用**」。
   理由写清楚了：**注释会被当成事实抄走**——这次就是。

**验证**：

| 检查 | 结果 |
|---|---|
| 容器里 `get_build_var BOARD_SEPOLICY_DIRS` | `device/generic/goldfish/sepolicy/common`（**不含** remote-control）✓ |
| 容器里 `get_build_var SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS` | `device/remote_control/remote_control_x64_arm64/sepolicy` ✓ |
| 重跑全量构建 | `unknown type odsign_prop` 命中 **0** 次，构建推进过 1% 原失败点 ✓ |

**顺带踩到的第二个坑（流程性的）**：我第一次是直接 `cp` 到 `aosp/device/remote_control/` 下改的，
然后**又跑了一次 `apply-overlay.sh`** —— 它按设计用项目目录覆盖 AOSP 落点，
把我手改的版本**覆盖回旧版**，于是"修好了"的构建照样失败，白跑一轮。

> **正确顺序永远是：改 `dev/04-x64-android/device/` 下的源文件 → 跑 `apply-overlay.sh` 同步 → 构建。**
> AOSP 树里的那份是**产物**，不是真源（`apply-overlay.sh` 文件头写明了这一点，我还是绕过去了）。
> 判断"改动到底有没有生效"最省事的办法不是看文件，而是问构建系统：
> `docker exec ... bash -lc "source build/envsetup.sh && lunch ... && get_build_var 变量名"`。

#### 7.0.11 release 打包接入 macOS（本轮）

`scripts/release.sh` 加了 `darwin` 平台，按**"只加不改"**做，默认行为保持不变（逐项验过）。

**两个新的平台维度**：

```
--platform linux|windows|darwin|both|all     # all = 三平台；both 仍是 linux+windows
--darwin-arch aarch64|x64                    # 默认 aarch64（Apple Silicon）
```

> ⚠️ `both` **必须仍是 linux+windows**。它原来是默认值，很多地方按"两个平台"假设它。
> 我一度把 `both` 改成三个平台，结果**默认调用直接失败**（darwin/aarch64 要 arm64 ROM，
> 而默认那份是 x86_64）——等于把所有人的默认行为弄坏。三平台用 `all`。

**`sdk_archive` 加了 host-arch 维度**（这条是必须的，不是锦上添花）：

| host-os | host-arch | 包 |
|---|---|---|
| macosx | aarch64 | `emulator-darwin_aarch64-16428233.zip`（396 MB） |
| macosx | x64 | `emulator-darwin_x64-16428233.zip`（466 MB） |
| macosx | （无字段） | `platform-tools_r37.0.1-darwin.zip`（两种 Mac 共用一份） |

只看 host-os 会挑到错的那份——而错的那份**装得上、起不来**（Apple Silicon 的包里
没有 x86_64 后端）。但 host-arch **不能当硬条件**：platform-tools 的 darwin 包没有
这个字段，所以规则是"清单给了就比，没给就放过"。

**平台与 ROM 的配套断言**（新，这条是本轮最有价值的判断）：

`assert_rom_matches_platforms` 从 ROM 的 `build.prop` 读出 guest 架构，
与目标平台的要求比对，不配套就**明确拒绝**并给出分两次的命令：

```
[x] 目标平台与这份 ROM 不配套：
    - darwin 需要 guest=arm64 的 ROM，当前 ROM 是 guest=x86_64

    一次调用只能打一份 ROM（ROM_DIR=...）。
    正确的做法是分两次： ...
```

理由：`release.sh` 一次调用只认**一份 ROM**（`ROM_DIR` 是单个目录），
而 darwin-aarch64 要 arm64 原生 ROM、linux/windows/Intel-Mac 要 x86_64+翻译层 ROM。
不判的话会产出"darwin-aarch64 包装着 x86_64 镜像"这种包，
用户解压后模拟器起不来，而报错完全指不到真因。判据取 **build.prop 里的事实**，
不是我们以为的产品名。

**三分支替换**：原来 quickstart / adb 示例路径 / stop-verify 命令 / `check_zip` 的入口脚本名
都是 `linux` vs `else` 的二元判断——darwin 会被当成 windows（渲染出 powershell）。
四处都改成 `[ "$plat" = windows ]` 取 ps1，其余取 sh。

**START-HERE.md 模板两处修正**：

1. 平台行原来**写死**「guest 是 x86_64，另有 ARM64 用户态翻译层」——对 arm64 包是错的
   → 改成 `@GUEST_DESC@`，按平台取。
2. 新增 `@PLATFORM_HINT@`：darwin 包渲染出一段引用块，明说这机器只能跑哪种 ROM。

**冒烟三处改进**：

- 新增 `host_can_smoke`：在 Linux 上打 darwin 包是常规操作，但**不能真去执行** Mach-O 二进制。
  不判的话冒烟会以一条与代码无关的报错把打包整体拖挂。现在明确打印
  「跳过冒烟，只做结构检查」并说明原因。
- 新增 `resolve_smoke_adb`：macOS 上没有 `/usr/bin/adb`（那是 linux 的路径），
  不换的话 darwin 包会永远走"没有可用的 adb"这条**静默**分支。
- 冒烟循环原来只按 zip 名里有没有 `linux` 决定跑不跑，改成按 `platform_list` 逐个来。

**验证（六组，全部实跑）**：

| 用例 | 结果 |
|---|---|
| A `darwin/x64` + x86_64 ROM | ✅ 选到 `emulator-darwin_x64-16428233.zip`（466 MiB） |
| B `darwin/aarch64` + arm64 ROM | ✅ 选到 `emulator-darwin_aarch64-16428233.zip`（396 MiB） |
| C **默认 `both`** + x86_64 ROM | ✅ 与改动前一致（linux 333 MB / windows 434 MB） |
| D `--platform all` + x86_64 ROM | ✅ 拒绝（darwin 需要 arm64 ROM） |
| E `--platform all` + arm64 ROM | ✅ 拒绝（linux/windows 需要 x86_64 ROM，两条都列出来了） |
| F 非法 `--darwin-arch` / `--platform` | ✅ 明确报错，退出码 1 |

**过程中修掉自己引入的两个真 bug**（都写进代码注释了）：

1. **`--list` 里把平台名当 host-os 传**给 `sdk_archive`（`darwin` vs `macosx`）。
   lookup 静默返回空，界面显示"清单里没有这个组合"——看着像清单缺包，其实是参数传错。
   **靠 `bash -x` 才定位到**（在此之前我反复怀疑清单、缓存、python 解析，全猜错了）。
2. **`require_mac_tools` 的 die 消息里写了 `$(...)` 与 `$PATH`** —— 双引号里会先展开，
   bash 在**解析期**就报 `未预期的记号 "(" 附近有语法错误`，整个脚本过不了 `bash -n`。

**仍未做的**：`packaging/templates/` 里没有 mac 专属模板（目前三平台共用同一套
`config.ini` / `instance.env`，这没问题）；`--slim` 在 darwin 上的裁剪未经真机验证。

#### 7.0.12 产物目录被模拟器污染：一个 32 GB 稀疏文件的教训（本轮）

**症状**：arm64 交付目录的表观体积是 **39 GB**（实际占用 6.2 GB），而 x86_64 那份是 6.2 GB。
逐文件看，罪魁是 `userdata-qemu.img` —— **表观 34,359,738,368 字节（32 GiB）的稀疏文件**，
实占只有 197 KB。

**来源**：`-sysdir` 指向哪个目录，模拟器就往**那个目录**写运行期状态，**`-datadir` 拦不住它**。
本项目里踩了两次，是同一个原因的两个受害点：

| 受害点 | 怎么中的 | 后果 |
|---|---|---|
| AOSP 产物目录 `out/target/product/remote_control_arm64/` | 开发时常用 `-sysdir $PRODUCT_OUT` 迭代 | 再打包一次就会把 32 GB 的东西带进交付目录 |
| **交付目录** `artifacts/rom-remote_control_arm64/` | §7.0.4 那次排查直接拿交付目录当 `-sysdir` 启动 | 交付目录当场被撑到 39 G |

**危害不止是体积**：`userdata-qemu.img` 是**跑过的用户数据**（应用、账号、输入内容都在里面）。
把它打包发给最终用户是**数据泄漏**，不只是包大了几倍。

**修法（两道闸门，都在 `package-rom.sh`）**：

1. **清源头**：`clean_product_out_junk()` 在打包前把产物目录里的运行期文件清掉并如实报告
   （清之前/之后的表观体积都打出来）。清的是可再生的运行期状态：
   `userdata-qemu.img(+.qcow2)`、`*.img.qcow2`、`build.avd`、`modem_simulator`、
   `hardware-qemu.ini`、`emu-launch-params.txt`、`version_num.cache`、`read-snapshot.txt`、`*.lock`。
2. **交付目录白名单**：`verify_dest_allowlist()` 在拷贝完、写派生文件之前扫一遍 `$DEST`，
   只放行 `REQUIRED/OPTIONAL` + `initrd/source.properties/MANIFEST.txt/SHA256SUMS/config.ini`
   + `system/vendor/product/odm/system_ext/build.avd` 这些目录，其余一律 `die`。
   为什么用**白名单而不是黑名单**：模拟器写什么列不全（这次是新发现的一项）。
   这道闸门正常情况下不会触发（`$DEST` 是脚本自己 `rm -rf` 后重建的）——
   触发就说明拷贝逻辑或清单变了，那是 bug，必须停下来看。

**验证**：

| 项 | 修前 | 修后 |
|---|---|---|
| 交付目录表观体积 | 39 G | **6.2 G** |
| 实际占用 | 6.2 G | 5.7 G |
| `userdata-qemu.img` | 在（32 GiB 表观） | **已清** |
| `build.avd` / `hardware-qemu.ini` / `emu-launch-params.txt` / `version_num.cache` | 在 | **已清** |
| `sha256sum -c SHA256SUMS` | —— | ✅ 全部通过 |
| `source.properties` 的 `SystemImage.Abi` | —— | ✅ `arm64-v8a` |
| `MANIFEST.txt` 翻译层段 | —— | ✅ 如实写"无翻译层" |

**顺带澄清一个我之前说错的估计**：文档 §7.2 里"arm64 包体积可能变大也可能变小"那条，
现在有数了 —— arm64 交付目录 **6.2 G 表观 / 5.7 G 实占**，与 x86_64 那份基本持平
（少掉翻译层 80+ 个文件，但 arm64 系统库更大，两者大致抵消）。

> ⚠️ **给后来人的规矩**：**永远不要拿交付目录当 `-sysdir`**。要在交付目录上试启动，
> 先把 `images/` 拷出来或让模拟器用它自己的 `sysdir-<port>`（包内 `bin/start-headless.sh`
> 正是这么做的：`build_sysdir` 把 `images/` **软链**进 `.run/sysdir-<port>/`，
> 只有 `initrd` 和 `config.ini` 是实文件 —— 那两条的注释就在 lib.sh 里）。

#### 7.0.13 真打一次 darwin 包（本轮，stage-only）

不再只验"选包对不对"，而是**把整条打包流程跑完**（`--stage-only`：铺出完整包目录树，
不打 zip；这样能验到 `prepare_runtime` / `render_start_here` / `write_manifest` 全部逻辑）：

```bash
bash release.sh --platform darwin --images .../artifacts/rom-remote_control_arm64 \
     --stage-only --keep-stage --no-verify-images
```

**结果：跑通**，产物 `autosnap-<版本>-darwin-aarch64`（6.9 G，其中 images 5.7 G + runtime 1.3 G，
401 项 sha256）。

**这一跑抓到四个真问题**（全部只靠"读代码"看不出来）：

| # | 问题 | 症状 | 根因 |
|---|---|---|---|
| 1 | `packaging/bin/macos/` 目录名不对 | `cp: stat .../packaging/bin/darwin/. 失败` | `release.sh` 按**平台名**找目录（`bin/linux`、`bin/windows`），所以必须是 `bin/darwin`。已 `git mv` 改名并更新全部引用 |
| 2 | START-HERE 渲染崩 | `NameError: name 'tag' is not defined` | 渲染的 python 里**没有 `tag` 变量**（tag 折进了 `rootdir`）。我写 `tag.endswith("aarch64")` 判架构 → 改成 `rootdir.endswith("aarch64")` |
| 3 | `RELEASE.json` 里写的是 **windows 后端** | `runtime.backend = emulator/qemu/windows-x86_64/qemu-system-x86_64.exe` | `write_manifest` 的 python 里有一处**硬编码的 linux/else 二元判断**（`"backend": ... if plat == "linux" else "windows..."`）。这是我在 §7.0.11 修的那四处三分支之外的**第五处**，藏在 python 块里。改成由 bash 侧算好传进来 |
| 4 | START-HERE 第 6 节对 arm64 包**说错话** | 渲染出「翻译层许可 … Google 专有二进制」「本 ROM 是 `x86_64,arm64-v8a`」「串行浮点退化 21~23×」 | 那一整节的五行都是 **x86_64 桥产品专属**。arm64 包的读者会读到对他完全不成立的话 —— **交付文档说错话比不说更糟**（他会以为包里有专有二进制，或以为装不上 arm64 应用） |

第 4 条的修法值得单说：把第 6 节表体改成 `@DIST_ROWS@` 占位符，
由 `dist_rows()` 按**产品类型**渲染 —— 判据取 **`$ROM_DIR/system/build.prop` 的
`abilist64`**（文件里的事实），不看平台名：

| | 桥产品（abilist64 含 x86_64） | 原生 arm64（只有 arm64-v8a） |
|---|---|---|
| 翻译层许可 | Google 专有二进制，交付前过法务 | **不适用**（无翻译层，这是它的优势） |
| ABI 覆盖 | `x86_64,arm64-v8a`，32 位 ARM 装不上 | `arm64-v8a`，**任何 x86/x86_64 应用也装不上** |
| 性能 | 串行浮点退化 21~23× | **没有翻译层开销** |
| 版本绑定 | 翻译层与 Android 版本绑定 | 不适用 |
| 加速 | 按平台：KVM / WHPX | 按平台：Hypervisor.framework（darwin） |

**这一条我又踩了一次自己的坑**：第一版 `dist_rows` 用 `rootdir` 拼路径读 build.prop，
而 `rootdir` 在渲染时是相对的 → 读不到 → 判据落空 → **静默落到"原生 arm64"那一支**，
把 x86_64 桥产品渲染成了原生描述（回归测试当场抓到）。修法是两条：
① 判据改从 bash 显式传的 **`$ROM_DIR`（绝对路径）** 读；
② **取不到判据时要打 stderr 告警并按平台保守回退**，不假装知道。
—— 教训与 §7.0.10 那次（照抄注释）同源：**判据取不到时的默认值必须安全且可见**。

**验证（两个产品都实跑）**：

| 包 | 第 6 节渲染 | 结果 |
|---|---|---|
| `darwin-aarch64` + arm64 ROM | 原生行（不适用 / arm64-v8a / 无翻译层开销 / Hypervisor.framework） | ✅ |
| `linux-x86_64` + x86_64 ROM（回归） | 桥行（专有二进制 / `x86_64,arm64-v8a` / 21~23× / KVM） | ✅ |

另外确认包内结构正确：`bin/` 五个脚本且都有可执行位、`runtime/emulator/qemu/darwin-aarch64/`
后端存在、`RUNTIME.txt` 的包名/版本/后端/adb 全对、`RELEASE.json` 的
`platform=darwin-aarch64` + `runtime.backend=emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless`。

**仍未做的**：真正打 zip（`check_zip` 的结构自检、`--smoke`）与在真 Mac 上启动。

#### 7.0.14 把 macOS 那层的验证固化成 `tools/test-macos-port.sh`（本轮）

前面 §7.0.8 / §7.0.13 的验证都是**会话里的临时脚本**，跑完就散了。
本轮把它固化成仓库脚本，跟项目已有的 `tools/test-release.sh` 同一套路子
（假 zip + 假 ROM，**不联网、不要真 Mac、一分钟跑完**）：

```bash
bash tools/test-macos-port.sh        # 91 项
bash tools/test-release.sh           # 65 项（原有的，linux/windows）
```

**为什么值得固化**：macOS 那一层的代码平时只有"在 Mac 上跑"才会被走到。
这些断言就是把 Linux 构建机**伪装成一台 Mac**——不这么做，那部分代码
要等到有人第一次在 Mac 上解压才第一次被执行。

配套给 `release.sh` 补了 `--darwin-emulator-zip` / `--darwin-platform-tools-zip`：
linux/windows 早就有对应的覆盖参数，darwin 缺了这两个就**离线测不了**
（原来 darwin 分支只有联网走 SDK 清单这一条路）。

五组覆盖：

| 组 | 验什么 |
|---|---|
| [0] 静态 | 全部脚本语法；darwin 脚本不许出现 bash 4 语法（`declare -A` / `mapfile` / `${v^^}`）；没有指向旧目录名 `packaging/bin/macos` 的**代码**引用 |
| [1] 平台分派 | 六个派生函数 × 三个平台（tag / 后端目录 / host-os / host-arch / guest 架构 / 后端路径）逐值比对 |
| [2] 平台–ROM 配套 | 六种组合的接受/拒绝（`darwin/aarch64`+arm64 ✓、`darwin/aarch64`+x86_64 ✗、`darwin/x64`+x86_64 ✓、`linux`+arm64 ✗、`both`+x86_64 ✓、`all`+x86_64 ✗） |
| [3] 真打 darwin 包 | 假 zip 离线跑完 `release.sh` → 断言产物结构、`bin/` 五脚本+可执行位、`RUNTIME.txt` 后端、**`RELEASE.json` 的后端**（这轮修过）、START-HERE 无残留占位符 / 命令是 bash 不是 powershell / 第 6 节按产品类型渲染 / Apple Silicon 提示 |
| [4] 回归 | 同一个假 ROM 打 linux 包，第 6 节必须仍是**桥产品**的行、且不带 Apple Silicon 提示 |
| [5] mac 版 lib.sh | 命令 stub（`uname`/`sysctl`/`system_profiler`/`ps`/`df`）伪装 macOS，验 `hv_usable`、GPU 自适应、按架构取后端路径、进程枚举（含"不误算调用者自己的命令行"）、`build_sysdir` 的 initrd/config.ini 实文件规则；另断言 **`verify.sh` 的架构判据是读 ELF 头**而不是信设备自述 |

**这个测试脚本自己也有过 4 个缺陷，都在注释里留了记录**（因为每一条都是"检查写得不对 →
开始骗人"，值得后来人看见）：

1. **自指误报**：检查"有没有旧目录名残留"时，那串字面量就写在检查脚本里 → 每次必然命中自己。
2. **只查代码不查文档**：`docs/13-macos-port.md` 里「`packaging/bin/macos` → `darwin`」
   是记录改名的**历史说明**，不是残留引用。把它算进去就是天天误报，
   然后所有人学会忽略这个检查 —— **那比没有检查更糟**。
3. **抽函数块时不要 source `common.sh`**：它在非 Linux/未配置时会 `die`，
   而 `die` 会 `exit` 掉整个子 shell → 四项断言全空，看着像"函数坏了"。
4. **漏了 `uname` stub**：`lib.sh` 开头就有平台守卫（非 Darwin 拒绝加载）。
   漏了它，[5] 组全空 —— 也是"看着像坏了，其实是守卫在正常工作"。
   现在**守卫本身也占一个断言**。

另：那几条 bash 4 语法检查必须先**剔掉注释行**再查 —— 这几条恰好写在 lib.sh 的注释里
当反例（"只用 bash 3.2 语法：无 declare -A…"），不剔注释就会把说明文字判成违规。

#### 7.0.15 `macos/run-darwin.sh`：补上源码树侧那个缺口（本轮）

§3 里 `macos/` 那三个脚本（`preflight` / `fetch-emulator` / `fetch-images`）只负责**备料**，
起机器那一步原来在 README 里写的是"手工拼参数，照抄 `run-linux.sh` 的 `launch()`"。
本轮把它补成一个**薄封装**：`macos/run-darwin.sh`。

**为什么是薄封装，不是再写一套**：起停逻辑（探 GPU、建工作目录、GPU 回退链、等开机）
已经在 `packaging/bin/darwin/` 里写好，并被 `tools/test-macos-port.sh` 离线测过。
源码树与发布包的区别**只有路径约定**（发布包是 `bin/` 与 `images/ runtime/ templates/` 同级；
源码树里脚本在 `packaging/bin/darwin/`，镜像与运行时在别处）。所以这个脚本只做一件事：
把 `AUTOSNAP_*` 指对位置，再 `exec` 包内脚本。
参数照抄一份的结果一定是**两边漂移** —— 这个项目已经在别处吃过这个亏。

它比"透传"多做一件事：**启动前的架构自检**。两种 Mac 各自只有**一个**后端，
配错镜像会"装得上、起不来"，而且报错完全指不到真因。所以它在启动前比对
`uname -m` 与镜像 `build.prop` 的 `abilist64`，不匹配就直接说清楚该换哪份 ROM：

```
[x] 本机是 Apple Silicon，但镜像的 abilist64 是「x86_64,arm64-v8a」（含 x86_64）。
    Apple Silicon 的模拟器包**没有** x86_64 后端 —— 这份镜像起不来。
    换 arm64 那份： cd .. && PRODUCT=arm64 ./scripts/package-rom.sh
```

**写它的过程中踩到一个坑**（已记进注释与测试）：`AUTOSNAP_TEMPLATES` 原来我指到
`packaging/templates/` —— 那个目录里**只有 README**，`config.ini` 的真源是
`emulator/config.ini`（`common.sh` 的 `EMULATOR_CONFIG` 就是它，`release.sh` 打包时才拷进去）。
指过去的结果是 `模板缺失：…/packaging/templates/config.ini`（实测踩到）。

**验证**（stub 伪装的 macOS 上，端到端）：

| 输入 | 结果 |
|---|---|
| arm64 Mac + arm64 镜像 | ✅ 过自检 → 委托包内脚本 → 建出工作目录（`initrd` 实文件、大件软链） |
| arm64 Mac + x86_64 镜像 | ✅ 拒绝，并给出该换哪份 ROM |
| **Intel** Mac + arm64 镜像 | ✅ 拒绝（Intel 的包里没有 aarch64 后端） |
| 缺 `sdk/` 或镜像 | ✅ 明确报错并指出先跑哪个脚本（不静默失败） |

测试台加了 `[6]` 组守这几条，现在共 **99 项**。

**顺带**：`[6]` 组第一次跑又抓到一个假阳性 —— `run-darwin.sh` 的**注释**里写着
「不要自己拿产物目录当 `-sysdir` 启动」，而检查是"文件里不许出现 `-sysdir`"。
这是同一个模式今天第三次出现（bash 4 语法、旧目录名、`-sysdir`），
所以现在规则统一成：**凡是"检查代码里出现的字符串"，一律先 `strip_comments`**。

### 7.1 仍需在真机上验的

| # | 事项 | 为什么重要 | 怎么验 | 状态 |
|---|---|---|---|---|
| 1 | **arm64 guest 在 Apple Silicon 上是否真走 HVF** | 若退到 TCG → 量级问题（§1.3 锚点：23.8 s vs 5–8 min） | `emulator -accel-check`、`-verbose` 日志里找 `hvf` | ⏳ 待真机 |
| 2 | **这套 arm64 ROM 能不能真走到 `boot_completed`** | Linux 侧被 30.8.3 旧模拟器挡住（§7.0.4 已复现，并排除 vendor.img） | 目标 Mac + 37.2.12 darwin-aarch64 包，`-accel on` | ⏳ 待真机 |
| 3 | ~~新产品的设备树能否 lunch 通~~ | —— | —— | ✅ **已验通**（§7.0.6） |
| 4 | ~~`m remote-control rcctl` 在 arm64 产品下能否编出~~ | —— | —— | ✅ **已编出**（§7.0.7，三个产物均为 ARM aarch64 ELF） |
| 5 | **arm64 ROM 全量构建 + 放进镜像** | 服务能编 ≠ 能装进镜像 | `m` 全量 → 查 `system/bin/` 与 `system/etc/init/` | 🔄 **进行中**（本轮已启动；服务三件套与 rc 已进镜像树） |
| 5b | **服务在 arm64 guest 里能否开机自启** | 全量构建成功不等于服务真起来了；rc/sepolicy 在 arm64 上还没跑过 | 起模拟器 → `ps -A \| grep remote-control` + `dumpsys` | ⏳ 待验 |
| 6 | **arm64 包体积与开机时间**（相对 x86_64） | §7.2 里那条"可能变大也可能变小"的估算 | 全量构建后实测，替换估算 | ⏳ 待验 |

### 7.2 已知风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| **arm64 产物没有 `vendor.img`**（§7.0.4 实测；x86_64 产品有 `vendor.img` + `vendor-qemu.img`） | 模拟器直接拒绝启动（`missing the 'vendor.img' image file`），交付包也无法按现有 `REQUIRED` 清单组装 | §7.1 #3 先查清原因（`m vendorimage` / `installed-files-vendor.txt` / 动态分区配置差异），再决定 `package-rom.sh` 的清单是否按产品分叉 |
| arm64 内核 5.10 与 API 31 的配对（`EMULATOR_KERNEL_FILE` 在 `arm64-kernel.mk` 里是 `kernel-5.10-gz` 变体） | 起不来 / 起得慢 | 产物已在（§7.0.1），在真机上直接试；这是 `sdk_phone64_arm64` 的官方配对 |
| **模拟器版本决策**：项目现用 37.1.11，而 macOS 三包只有 37.2.12（稳定）/37.3.2（beta） | 加 macOS 就得整条线升版本，Windows/Linux 的验收要重跑 | §7.0.3：稳定渠道三平台同版本齐全，一次升级解决；升级后按 `docs/03-delivery.md` 的口径重验 |
| arm64 系统库体积比 x86_64 大（同一套代码编 arm64 通常更大），加上**不再有翻译层文件**（80+ 个小文件） | arm64 包体积**可能变大也可能变小，未实测** | 按 §7.1 #4 实测对比，别按直觉写进文档 |
| `dev/05-controller-app` 的 APK 若含预编译 `.so` | 原生 arm64 ROM 上**没有翻译层兜底**，必须自带 `arm64-v8a` | 编产品前先 `unzip -l` 看一眼 `lib/` 目录 |
| 两个产品的 `remote-control` 行为差异（例如 `Ro属性` 上报、`abilist`） | 体检脚本的期望值要按产品区分 | 让 `functional-sweep.py` 的期望设备感知（它已经支持"已知平台限制"这个机制） |

### 7.3 未决（需要人来定，不是技术能定的）

1. **签名与公证**：mac 包是否做签名+公证？取决于分发范围（内网 = 可不做并写清 quarantine 步骤；公网 = 基本必须）。
2. **Intel Mac 的支持期限**：两条 ROM 线都保 → 构建与验收成本翻倍（每个改动要验两个产品）。若 Intel Mac 用户占比很低，应显式写下"何时下线"。
3. **是否把 sepolicy 目录改成中性名**（§2.5）：改动面 vs 目录语义，二选一。

---

## 8. 实施检查清单

> 完成后逐条改勾，并把**实际数字**（体积、开机时间、行数）补进对应小节，替换掉估算。

**A. arm64 ROM**

- [x] 1. 上游 arm64 产品能编出来 —— **已完成**（§7.0.1：`emulator64_arm64` 全套产物在，2026-09-28）
- [x] 2. `macosx` host-os 的 SDK emulator 包含 arm64 guest 后端 —— **已确认**（§7.0.2：`qemu/darwin-aarch64/qemu-system-aarch64(-headless)`，sha1 与清单一致）
- [ ] 3. Apple Silicon 上确认走 HVF（§7.1 #1；需真机）
- [ ] 3b. arm64 ROM 在真机上走到 `boot_completed`（§7.1 #2；本机被 30.8.3 旧模拟器挡住，见 §7.0.4）
- [x] 4. 新增 `device/remote_control_arm64/`（`BoardConfig.mk` / `device.mk` / `product/*.mk`） —— **已完成**（§7.0.5）
- [x] 5. `AndroidProducts.mk` 两个产品并列，`lunch` 都能识别 —— **已验通**（§7.0.6）
- [x] 6. `apply-overlay.sh` / `build-rom.sh` / `package-rom.sh` / `status.sh` 加产品分支 —— **已完成并回归**（§7.0.9；`release.sh` 未改，它属打包层，见 #16）
- [ ] 7. sepolicy 复用同一份（不复制，§2.5），`remote-control.rc` 自启验证
- [ ] 8. `tools/functional-sweep.py` 跑 33 条命令（含 uinput 触控）
- [ ] 9. §2.6 的 6 处翻译层断言加闸门并如实打印 ⊘
- [ ] 10. `dev/05-controller-app` 的 APK 确认有 `arm64-v8a` 库
- [x] 10b. `m remote-control rcctl remote-control-launch` 在 arm64 产品下编出 —— **已完成**（§7.0.7）
- [ ] 10c. arm64 ROM 全量构建（`m`），并把服务装进镜像

**B. macOS 宿主脚本**

- [x] 11. `preflight` 检查 `kern.hv_support` / 架构 / Gatekeeper / 必需工具 —— **已写出并验语法**（§7.0.8）
- [x] 12. `fetch-emulator` 走 `macosx` host-os + host-arch 选包 —— **已写出，选包逻辑对真实清单验过 6 组**（§7.0.8）
- [x] 12b. `macos/fetch-images.sh` —— **已写出并离线验过 4 个用例**（§7.0.9）
- [x] 13. 建/起/停/查/验五个动作的 mac 版 —— **已写出并离线验过 18 个用例**（§7.0.8 补记）
- [ ] 13b. 上述五个脚本在**真 Mac** 上跑通（离线测试台不能替代真机）
- [x] 14. `packaging/bin/darwin/` 五个脚本 —— **已完成**（与另两套同为 5 个文件）
- [ ] 15. 共享层的 8 条改动（§3.3）落完，且**在 Linux 上回归一遍**（别把 Linux 弄坏）
- [x] 16. `release.sh` 五分派表 + `START-HERE.md` 三分支 + `--platform{all}` / `--darwin-arch` + 平台-ROM 配套断言 —— **已完成，六组实跑验过**（§7.0.11）
- [x] 16b. `release.sh --platform darwin` 全流程铺包（`--stage-only`）—— **已在 Linux 上实跑通**（§7.0.13，6.9 G 包，两个产品的第 6 节渲染都验过）
- [ ] 16c. 真机上打**完整 zip**（含 `check_zip` 结构自检与 `--smoke`）—— 需要真 Mac
- [ ] 17. 包内 `chmod +x`、quarantine 提示、`sha*sum` 与 `stat` 的 BSD 路径
- [ ] 18. 从 zip 解压 → 启动 → 跑 `verify.sh` 全绿（Mac 上的"解压即用"验收）
- [ ] 19. `macos/README.md` 的"未在真机验证"表换成真机输出（首次在 Mac 上跑时把 preflight/fetch 输出贴回）

---

## 9. 本文与项目其它文档的约定

沿用仓库既有规矩，本文遵守：

1. **同一件事只有一处权威描述**（见 `docs/README.md` 开头的"权威性"声明）。
   产品矩阵以本文 §4 为准；被控端移植以 `14-macos-host-notes.md` 为准。
2. **历史文档加状态横幅**。本文是**计划**，实施后要改状态，并把估算替换成实测。
3. **实测与推演分开标**。本文凡"未实测/未核实"都逐条标了，实施时不要删标注，要**改成实测值**。
4. **踩过的坑带注释留在代码里**（如 `common.sh` 里 `pgrep` 与 `/dev/dri` 那两段）。
   macOS 分支要**继承同样的注释纪律**——否则下一个改的人会重犯。
