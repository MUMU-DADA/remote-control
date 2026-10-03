# 进度与阶段

> 目标：**x86_64 Linux + x86_64 Windows 上同架构运行的自编 Android ROM，可跑 arm64 应用**
> （arm64 若不成立，下放到 arm32）
> 位置：`/root/AutoSnapshotAndroid/dev/04-x64-android`（原 `dev/06-x64-android`）
>
> ⚠️ **时效**：下面的轮次日志**止于第 13 轮（release 打包）**。
> 第 14 轮起的逐轮记录在 [`docs/13-macos-port.md`](docs/13-macos-port.md) **§7.0.1 起**
> （arm64 原生 ROM 产品线、macOS 宿主支持 —— 那两轮只有那一条 cron，不再在这里重复）。
> **当前状态的权威位置是 [`README.md`](README.md) §0 的目标表**（G1–G8），本文的轮次日志是过程记录。
>
> （下面各小节标题里的「（本轮）」是**写那一节时**的轮次，不是「现在这一轮」——
>  历史上的写法，保留原样。）

---

## 阶段与状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| **P0 · 立项与设备树** | 项目目录、设备树、载荷提取、注入脚本、产品能在 `lunch` 里被识别 | ✅ 完成（`TARGET_CPU_ABI_LIST=x86_64,arm64-v8a` 已验证） |
| **P1 · 全量编译** | `m` 产出完整 x86_64 镜像（system/vendor/product/ramdisk/kernel-ranchu） | ✅ **完成**（`EXIT=0`，62083 个目标；后又以 `QEMU_DISABLE_AVB=true` 增量重编） |
| **P2 · Linux 侧验收** | KVM 启动 + arm64 应用实跑（abilist / 装 / 起 / maps） | ✅ **全部通过**（多轮复跑：23.8 s / 28.5 s / 30.2 s / 41.2 s 开机；结论性证据见 `docs/07-verification-report.md`） |
| **P3 · Windows 侧验收** | 同一份镜像 + SDK emulator + WHPX | ⊘ **明确不由 agent 验证**（用户决定）；工程部分已交付：脚本 + 镜像 + `preflight.ps1` + 期望输出对照表 + 同 build id 等价性 |
| **P4 · arm32 下放（可选）** | 若需要 32 位 ARM：切 API 30 基座 + 四 ABI 板级配置 | ⬜ 备选（预案见 [`docs/06-arm32-fallback.md`](docs/06-arm32-fallback.md)） |
| **P5 · ROM 定制** | 把 `remote-control` 等自制组件编进 `/system` | ✅ **已完成**（`remote-control` / `rcctl` / `remote-control-launch` 都在产物里，开机自启见 [`docs/05-adding-components.md`](docs/05-adding-components.md)；表格早先误标为"⬜ 待 P2"） |
| **P6 · release 打包** | 一个平台一个 zip（无头运行环境 + 镜像 + 模板） | ✅ 已完成（两个平台同 build id，Linux 那份真启动验收；见 [`docs/15-release-packaging.md`](docs/15-release-packaging.md)） |
| **P7 · arm64 原生 ROM 产品线** | 给 Apple Silicon 用的原生 arm64（无翻译层） | ✅ **已实测**：`EXIT=0`，产物全是 aarch64，`abilist64=arm64-v8a`（第 14 轮） |
| **P8 · macOS 宿主支持** | 第三套宿主脚本 + `release.sh` 的 darwin 平台 | 🟡 代码完成、离线验证通过；**缺真机**（第 15 轮起，见 [`docs/13-macos-port.md`](docs/13-macos-port.md)） |

---

## 已完成的实测结论（P0 依据）

来源：`dev/04-x64-android/docs/00-bridge-eval.md`（全部为本机实测）

1. **libhoudini 不可用**：官方源 `_z`（arm64）变体只到 Android 7；8/9 系列只有 32 位 ARM 翻译器。
2. **libndk_translation 可用**：官方 `google_apis;x86_64`(API 31) 镜像 `abilist=x86_64,arm64-v8a`，
   34.4 秒（emulator 31.3.10）/ 29.0 秒（37.2.11）开机，arm64 APK 装 + 跑 + 映射 22 条 arm64 库。
3. **可移植**：把载荷搬进一份普通 AOSP 风格 x86_64 镜像（23 MB 载荷 + 10 行属性），
   同一份镜像从"拒绝 arm64 安装"变成"arm64 应用正常运行"。
4. **属性三处**：`ro.product.cpu.abilist` 由 init 按 product→odm→vendor→system 派生，
   三处分区属性必须一致（只改 system+vendor 不生效）。
5. **API 30 有四 ABI**（含 arm32），API 31 纯 64 位——这是 arm32 下放路线的依据。

---

## P0 交付物清单（本项目内）

- `device/`：完整设备树（BoardConfig / device.mk / product mk / AndroidProducts.mk）
- `payload/`：90 个文件的翻译层 + `MANIFEST.sha256`（可校验）
- `scripts/`：`fetch-payload.sh` / `apply-overlay.sh` / `build-rom.sh` / `run-linux.sh` / `common.sh`
- 注入后 AOSP 侧落点：`aosp/device/remote_control/`（**不改上游任何文件**，`--revert` 可清）

---

## 构建记录

| 时间 | 动作 | 结果 |
|---|---|---|
| 本轮 | `apply-overlay.sh` 注入 90 个载荷文件 + 生成 90 条拷贝规则 | ✅ |
| 本轮 | `lunch remote_control_x64_arm64-userdebug` | ✅ `TARGET_ARCH=x86_64`、`abilist=x86_64,arm64-v8a` |
| 本轮 | 第 1 次 `m`：artifact path 检查拦下载荷 | ❌ → 加 `PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST`（坑 1） |
| 本轮 | 第 2 次 `m`：残留 `out/.lock` | ❌ → 清锁（坑 2） |
| 本轮 | 第 3 次 `m`：AOSP 树里 `remote-control` 模块 `libwebp_vendored` 变体不匹配 | ❌ → 本侧开 `ALLOW_MISSING_DEPENDENCIES`（坑 3，属别人在途改动） |
| 本轮 | 第 4 次 `m` 全量构建（`-j12`，容器 `remote-control-builder`） | ⏳ 编译中（`[2% 1544/62183]`，0 error）日志 `aosp/out/remote-control-build.log` |
| 本轮 | 自建 arm64 探针 APK（`tools/build-probe-apk.sh`） | ✅ `artifacts/arm64-probe.apk`（16 KB，纯 arm64-v8a，已签名） |

## 第 2 轮（本轮）

| 动作 | 结果 |
|---|---|
| 构建推进 | `[30% 19183/62183]`，0 error；`kernel-ranchu` 已产出（22 MB） |
| `scripts/status.sh` | ✅ 一眼看清 载荷/注入/构建/产物/设备 五项状态 |
| `scripts/package-rom.sh` | ✅ 打包可交付 ROM 目录（含 `SHA256SUMS` + `MANIFEST.txt` + 补 `source.properties`） |
| `scripts/run-linux.sh` | ✅ 加"SDK 模拟器失败自动回退 AOSP 模拟器"；验收末尾打印 ROM 指纹供两侧对照 |
| `scripts/apply-overlay.sh` | ✅ 新增 `--patch-initrc` / `--unpatch-initrc`（首启卡 recovery 时的已知解法，默认不开） |
| `docs/03-delivery.md` | ✅ 两侧交付步骤、必需文件表、同一份 ROM 的证明方式、交付前硬约束 |
| `docs/04-acceptance-runbook.md` | ✅ 验收排错手册（起不来/卡 recovery/ABI 不对/翻译层没接线/应用崩/性能异常） |
| `tools/check-bridge-symbols.sh` | ✅ 翻译层动态依赖自检（已修 3 个自身 bug：本地化 readelf、`@LIBC` 版本后缀、APEX 内 libc/libm） |
| 构建中期验证 | ✅ 载荷 21 个翻译器 + rc + ld.config + binfmt 规则已进产物；**执行器 0755 保住了**；三个分区 build.prop 属性全部正确 |
| `tools/build-probe-apk.sh` | ✅ 自建 arm64 探针 APK 打通（纯 arm64-v8a，16 KB） |

## 第 3 轮（本轮）

| 动作 | 结果 |
|---|---|
| 载荷自洽性预检 | ✅ 59 个 aarch64 库共 363 条 NEEDED，唯一"缺"的是内核提供的 `linux-vdso.so.1` → 载荷是闭合集合 |
| **澄清 arm64 侧车来源** | ✅ 构建日志里 61 条 `overriding commands for target .../lib64/arm64/...` 一查到底：**AOSP 自己会把模块编成 arm64 变体**装到 `/system/lib64/arm64/`；`ld-android.so` 与官方镜像**逐字节相同**（sha256 `3be279f8…`）→ 真正专有的只有翻译器本体，这些库与我们的框架同源，不存在版本错配风险（详见 `docs/01-design.md` §3.1） |
| `tools/verify-rom.sh` | ✅ 新增 ROM 自检：产物齐全 / 翻译层落位 / **侧车字节一致性** / 三处 build.prop / 首启风险提示 |
| `windows/fetch-images.ps1` | ✅ 改为默认从**打包目录**取件，并按 `SHA256SUMS` 逐文件校验 |
| 构建体检 | CPU 满载（us 60% + sy 18%，wa 0，无活跃 swap）→ 属正常 CPU 密集，非内存/IO 瓶颈；估计还需 1.5~2.5 小时 |

## 第 4 轮（本轮）：验收流水线彩排

用官方 `google_apis;x86_64` 镜像把**验收流程先跑一遍**，避免把工具问题误判成 ROM 问题。
结果：`run-linux.sh --verify` 全部检查项通过，只有"设备名"按预期失败（官方镜像不是我们的 ROM）。

彩排抓出并修掉的问题（都是**会挡住验收**的真问题）：

| # | 问题 | 后果 | 修法 |
|---|---|---|---|
| 1 | 探针 APK 的 `resources.arsc` 被压缩 | `pm install` 直接失败（targetSdk ≥ 30 要求 stored + 4 字节对齐） | `zip -X -0` 单独打 arsc + `zipalign -f -p 4`，并加自检 |
| 2 | 验收脚本 binfmt 检查写成 `arm64_exe.*arm64_dyn` | `ls` 是字母序（`arm64_dyn` 在前）→ 误报 | 拆成两条独立检查 |
| 3 | 启动探针前未清 logcat | 匹配到上一轮的 `PROBE_RESULT`，假绿 | 启动前 `logcat -c` |
| 4 | 宿主 `/tmp` 是 16 GB tmpfs，被写满 | clang 报 No space、`docker exec` 直接失败（而 `df -h /` 看着还有空间） | `scripts/common.sh` 统一把 `TMPDIR` 指到数据盘；清 core dump |
| 5 | 探针 Java 侧用 `System.getProperty("ro.product.cpu.abilist")` | logcat 打出 `abilist=?` | 改用 `Build.SUPPORTED_ABIS` |

彩排证据（官方镜像，自建探针 APK）：

```
[✓] pm install --abi arm64-v8a   Success
[✓] 进程存活                     5204
[✓] 映射的 arm64 库条数          16
[✓] primaryCpuAbi                arm64-v8a
[✓] 探针原生返回值   PROBE_RESULT arm64-v8a native ok | built_for=arm64-v8a | kernel=x86_64 | ptr=64 bit
[✓] aarch64 ELF 执行             ARM64_OK machine=x86_64
```

坑 5、6 已记入 [`docs/02-build-traps.md`](docs/02-build-traps.md)。

## 第 5 轮（本轮）：启动路径彩排 + P5 文档

| 动作 | 结果 |
|---|---|
| **启动路径彩排**（`PRODUCT_OUT=<官方 sysdir> ./scripts/run-linux.sh`） | ✅ 抓出并修掉 1 个会挡住验收的真 bug：**缺 `ANDROID_BUILD_TOP`** 时 SDK 模拟器报 `missing the 'kernel-qemu' image file`（已移除的跨架构路线脚本导出了它，本项目脚本没导）→ 已补，坑 7 入档 |
| `PRODUCT_OUT` 可覆盖 | ✅ `common.sh` 支持 `PRODUCT_OUT=` 覆盖，便于用官方镜像做彩排 |
| 开机耗时日志匹配 | ✅ 37.x 说 `Boot completed in`、30.x 说 `boot time`，正则两者都认 |
| `docs/05-adding-components.md` | ✅ P5 路线文档：把自己的系统组件（如 `remote-control`）编进 ROM 的三种方式、落点、SELinux 验证、以及两个现状约束（`ALLOW_MISSING_DEPENDENCIES` / 载荷瘦身） |
| 彩排终态 | ✅ 启动→等开机→4 组验收全流程跑通，仅"设备名"按预期失败（官方镜像） |

## 第 6 轮（本轮）

| 动作 | 结果 |
|---|---|
| 抗 `/tmp` 挤爆 | ✅ 宿主 `/tmp`（16G tmpfs）被外部写入占满 → `docker exec` 失败；实测容器进程在宿主 PID 命名空间可见，于是 `build-rom.sh`/`status.sh` 的状态判断与停止信号**改用宿主 `pgrep`/`pkill`**，不再依赖 docker exec |
| `scripts/accept.sh` | ✅ 一条龙脚本：构建结束判定（`EXIT=0`）→ ROM 自检 → 翻译层依赖检查 → 打包 → 启动验收；`--skip-boot` 可只做前三步 |
| README | ✅ 快速开始改为以 `accept.sh` 为主入口 |
| 旁注 | 你们的 `frameworks/native/cmds/remote-control/daemon/Android.bp` 在 10:37 又改过一次，`libwebp_vendored` **只剩模块定义、已无消费者引用** → 那个变体不匹配问题应该没了，下次可以用严格模式验证：`ALLOW_MISSING_DEPS=0 ./scripts/build-rom.sh` |

## 第 7 轮（本轮）

| 动作 | 结果 |
|---|---|
| 项目自审 | ✅ 11 个脚本：可执行位齐、`bash -n` 全过、`common.sh` 引用路径正确；文档内链全部可达（先前两条"断链"是我校验时基准目录取错） |
| **arm32 下放预案**（`docs/06-arm32-fallback.md`） | ✅ 用实测把关键未知项钉死：**API 30 镜像有完整 32 位栈**（`/system/lib/libndk_translation.so` 1654288 B + `/system/bin/ndk_translation_program_runner_binfmt_misc` + `/system/lib/arm` 59 个 + `abilist32=x86,armeabi-v7a,armeabi`），而 **API 31 是纯 64 位**（我们载荷里的 `arm_exe` 规则在 API 31 镜像里指向一个不存在的 runner —— 死接线）。给出路 A（Android 11 基座，推荐）与路 B（同树改 4 ABI + 借 API 30 翻译器，未验证高风险）及 5 条验收标准 |
| 守望任务升级 | ✅ 新守望：构建结束（`EXIT=`）后**自动跑 `tools/verify-rom.sh`**，下一轮直接拿结果 |

## 第 8 轮（本轮）

| 动作 | 结果 |
|---|---|
| `run-linux.sh` 验收对象升级 | ✅ **优先验收"打包后的交付目录"**（`artifacts/rom-*`）：它带 `source.properties`（SDK 版模拟器更认）、且才是真正要交付的那份；打包目录不全时自动回落到构建产物目录。新增 `--from-product-out` 强制走产物目录 |
| 构建 | 59% → 继续推进，0 error；守望任务会在结束时自动跑 `tools/verify-rom.sh` |

坑的完整记录见 [`docs/02-build-traps.md`](docs/02-build-traps.md)。


---

## ⭐ 里程碑：自编 ROM 编译通过 + Linux/KVM 验收全绿（第 9 轮）

```
镜像   remote-control/remote_control_x64_arm64/remote_control_x64_arm64:12/SP1A.210812.016.C2/...:userdebug/test-keys
启动   KVM 加速，adb 可见设备，sys.boot_completed=1
属性   ro.product.device = remote_control_x64_arm64
       ro.product.cpu.abilist = x86_64,arm64-v8a        ← 声明支持 arm64
       ro.dalvik.vm.native.bridge = libndk_translation.so
       ro.enable.native.bridge.exec = 1

验收 1/4  SDK=31 / device=remote_control_x64_arm64 / abilist=x86_64,arm64-v8a            ✓✓✓
验收 2/4  native.bridge=libndk_translation.so / exec=1 / binfmt arm64_exe+arm64_dyn ✓✓✓
验收 3/4  自建 aarch64 静态 ELF 直接执行 → ARM64_OK                                ✓
验收 4/4  自建探针 APK（纯 arm64-v8a）：装 Success / 进程活 / 16 条 arm64 库映射 /
          primaryCpuAbi=arm64-v8a / 原生返回值 "arm64-v8a native ok | kernel=x86_64" ✓✓✓✓✓
```

### 首启踩的三个坑（都已修，见 `docs/02-build-traps.md` 坑 8）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | `InitFatalReboot` 无限重启，报 `failed to find device default fstab` | 交付目录只拷了 `ramdisk.img`（系统半截），first-stage 的 `fstab.ranchu` 在 **vendor ramdisk** 那半，AOSP 把它合并成 `ramdisk-qemu.img` | 交付目录必须含 `ramdisk-qemu.img`，`initrd` 优先由它生成 |
| 2 | 找不到分区：`partition(s) not found in /sys, waiting for their uevent` → 超时重启 | 裸 ext4 的 `system.img` 没有分区表；模拟器需要 **GPT 包装**的 `*-qemu.img` | 交付目录含 `system/vendor/product/system_ext-qemu.img` |
| 3 | `[libfs_avb] vbmeta digest error isn't allowed` → `Failed to mount required partitions early` | 产品默认带 AVB 构建，模拟器镜像不需要也没法验 | 构建加 `QEMU_DISABLE_AVB=true`（AOSP 只读不写的正规开关） |

> **教训**：能编过 ≠ 能开机。前两轮的自检只看了"文件在不在/属性对不对"，
> 真正定位靠的是给模拟器加 **`-show-kernel`** 直接看 guest 的 first-stage init 输出
> （`run-linux.sh` 现在支持 `--show-kernel`）。


---

## 第 10 轮：干净复现 + Windows 侧可验证项

| 动作 | 结果 |
|---|---|
| **一条命令从头复现** | ✅ `./scripts/accept.sh` → 自检 → 依赖检查 → 打包 → **启动验收全部通过**（开机 41.2 秒） |
| 交付目录保持干净 | ✅ `run-linux.sh` 改为用**符号链接工作目录**启动：模拟器写的状态文件（`hardware-qemu.ini`/`userdata-qemu.img`/`build.avd`…）落在 `.run/sysdir-<port>/`，不再污染交付目录 |
| 交付清单修正 | ✅ `ro.dalvik.vm.native.bridge` 报 **vendor 的生效值**（system 里那条固定是 0，会误导）；`initrd` 由 `ramdisk-qemu.img` 生成（实测确认） |
| `windows/fetch-emulator.ps1` 两个真 bug | ✅ ①清单里每个包有**按 host-os 分的多个 archive**，直接取第一个会下到 **Linux 版** → 改为按 `host-os=windows` 挑；②渠道不是名字而是 `channelRef ref="channel-0"` + `<channel id="channel-0">Stable</channel>` → 改为按名字映射，找不到再回退 |
| `windows/preflight.ps1`（新增） | ✅ 一次跑完：虚拟化/WHPX 状态、磁盘空间、镜像齐全性（含 `-qemu` 家族）、`SHA256SUMS` 一致性、与 Linux 侧指纹比对 |
| 交付物校验 | ✅ 交付目录 5.7 GB，`sha256sum -c SHA256SUMS` 全数通过 |

### 交付清单（两边对照用）

```
指纹     remote-control/remote_control_x64_arm64/remote_control_x64_arm64:12/SP1A.210812.016.C2/root09280236:userdebug/test-keys
system   system.img sha256 = 243298b2fb1aa4d9631ddd1435ef8bc8b0a2266cdf65d67bf00e11382622eec2
目录     artifacts/rom-remote_control_x64_arm64/（5.7 GB，SHA256SUMS + MANIFEST.txt）
```


---

## 第 11 轮：同 build id 等价性验证（G4 的关键补强）

思路：Windows 和 Linux 的模拟器包是**同一源码、同一 build id** 的两次编译，
所以"用与 Windows 稳定版同 build id 的 Linux 包跑通"就是 Windows 跑通的最强等价证据。

| 项 | 值 |
|---|---|
| Windows 稳定包 | `emulator-windows_x64-15917651.zip`（421 MiB，sha1 `54fa750822ff…` 与清单一致） |
| Linux 同 build 包 | `emulator-linux_x64-15917651.zip`（318 MiB，sha1 `1b1f78891abf…` 与清单一致） |
| 版本 | 两者都是 **37.1.11 / build 15917651** |
| 交付目录内容核验 | Windows 包内含 `emulator/qemu/windows-x86_64/qemu-system-x86_64.exe`（ROM 需要的 x86_64 后端） |
| 用该 Linux 包复跑验收 | ✅ **验收全部通过**，开机 **23.8 秒**（比 37.2.11 的 41.2 秒更快），4 组全绿，指纹一致 |
| 复跑方式 | `EMULATOR_BIN=$PWD/.run/emu-parity/emulator/emulator ./scripts/run-linux.sh --port 5584`（脚本支持覆盖模拟器） |

新增文档：[`windows/EXPECTED-OUTPUT.md`](windows/EXPECTED-OUTPUT.md)——
"Windows 首次运行期望输出对照表"：逐项期望值、已知正常现象、按本机踩坑记录的排查顺序。

### 第 11 轮附带修掉的两个"交付卫生"问题

| 问题 | 现象 | 修法 |
|---|---|---|
| 模拟器**透过符号链接重写** `initrd` | 跑完验收后交付目录的 `initrd` 变了，`SHA256SUMS` 校验失败 | `run-linux.sh` 搭工作目录时**不再链接 `initrd`**（模拟器本来就会自己生成），让它落在 `.run/` 里 |
| 并发打包互相踩 | 两个 `package-rom.sh` 实例同时跑（被 kill 的旧实例仍在写 `MANIFEST.txt`），导致清单里出现 `MANIFEST.txt` 自己的哈希 | 加 `flock` 互斥锁（`flock -n 9 \|\| die`） |

修完后交付目录复核：**`sha256sum -c SHA256SUMS` 20/20 通过**。

---

## 第 13 轮（本轮）：release 打包 —— 一个平台一个 zip

> 需求：新增 release 打包输出脚本；zip 里要有**虚拟机完整无头运行环境 + 对应的虚拟机镜像 + 模板**；
> 打包脚本要能输出 **linux 和 windows 两个平台**的成品。
>
> （第 12 轮是文档收口轮，只在 README/文档索引上落笔，没在 PLAN 单列。）

| 动作 | 结果 |
|---|---|
| `scripts/release.sh`（新增） | ✅ 一条命令产两个平台的 zip；`--list` / `--stage-only` / `--zip-only --reuse-zip` / `--smoke` 都可单独跑 |
| `packaging/`（新增） | ✅ 包内骨架的**源码**：`bin/linux`（lib/start-headless/stop/status/verify）+ `bin/windows`（同名同语义的 ps1）+ `START-HERE.md` 模板 + `templates/README.md` |
| 运行时取法 | ✅ 按 SDK 清单 + 渠道 + `host-os` 现取**同一 build id**（Stable：`emulator-{linux,windows}_x64-16428233.zip`，37.2.12 / build 16428233），sha1 校验后解包，只留本平台的 x86_64 后端；版本与来源写进包内 `runtime/RUNTIME.txt` 与 `RELEASE.json` |
| 镜像进包方式 | ✅ `cp -al` 硬链接进 staging（5.7 GB 不复制第二份），打包前 `sha256sum -c images/SHA256SUMS` 验一遍，整包清单直接复用 ROM 那份（只改写路径） |
| `tools/test-release.sh`（新增） | ✅ **65 项**：假 ROM + 假运行时端到端真跑 `release.sh` + 全部脚本语法（bash -n / pwsh 语法分析）+ 包内自述与 Windows 工作目录构建 |
| `docs/15-release-packaging.md` | ✅ 包结构、怎么打、运行时装哪一版、模板是什么、怎么验、**6 条坑记录**、上限与后续 |
| 真包实测 | ✅ 两个 zip 都产出并解压核验：Linux 那份**真启动**（`Boot completed in 25283 ms`）+ 四组验收全绿 + 包内 `SHA256SUMS` 逐文件校验通过 |

### 这一轮踩到并修掉的 7 个真问题

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | `curl: (6) Could not resolve host: emulator-linux_x64-16428233.zip` | SDK 清单里的 `complete/url` 是**相对路径**，直接当 URL 用 | 不以 `http` 开头就拼 `$SDK_MIRROR/` |
| 2 | 包内 `sha256sum -c` 必然失败 | 清单的中间文件写在包根，被自己的 `find` 收进清单，打完包就消失了 | 中间文件挪到 staging 目录 |
| 3 | 清单里没有 `RELEASE.json`、且行数对不上 | 先算清单后写 json；计数又漏算了 `RELEASE.json` | 先写 `RELEASE.json` 再算 `SHA256SUMS`，计数按"除自己之外"算 |
| 4 | 清单漏 `images/MANIFEST.txt` | ROM 自带的 `SHA256SUMS` 是在写 `MANIFEST.txt` **之前**算的（`package-rom.sh` 的顺序），所以镜像里有文件不在那份清单里 | 用 `comm` 找出"镜像里清单没覆盖的文件"补算（`comm` 必须与 `sort` 同用 `LC_ALL=C`） |
| 5 | 结构自检把 `system-qemu.img`/后端/模板**全报成缺失** | `unzip -Z1 \| grep -qxF` 里 grep 一命中就退出，unzip 吃 SIGPIPE(141)，`pipefail` 下成了"明明有却报没有" | 清单先落成文件再比对（`smoke_linux` 里 `unzip ... \| head -1` 是同一个坑，一起修了） |
| 6 | 第 4 组验收没打印失败项就退出 | `adb shell pidof` 找不到进程返回非 0，`set -e` 把脚本**静默**带走 | helper 一律 `\|\| true`；包名改成**从设备上发现**（`pm list packages -3`），不再写死 `org.remotecontrol.arm64probe`（真包名已是 `org.autosnap.arm64probe`） |
| 7 | 冒烟全绿，`release.sh` 却以**失败**退出、staging 也不清理 | `smoke_linux` 最后一句是 `[ "$CLEAN_SMOKE" = 1 ] && rm -rf ...`：条件为假时 AND 列表返回 1 → **函数返回 1** → `set -e` 在"清理/完成"前把脚本带走 | 改成 `if` 并 `return 0`。通用陷阱：函数/`if` 块的最后一句别留 `[ 条件 ] && 命令` |

> 教训和第 12 轮一样：**"命令没报错"不等于"事情做成了"**。
> 这轮 6 个问题里有 4 个是"看起来成功、其实清单/自检是错的"，
> 全靠 `tools/test-release.sh` 的假端到端 + 真包的 `--smoke` 两次才逼出来。
