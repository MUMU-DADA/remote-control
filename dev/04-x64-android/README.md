# dev/04-x64-android · 同架构 x86_64 Android ROM（可跑 arm64 应用）

> **本目录即验证环境（轨 04）**：自编一份 x86_64 Android 12，让它在 **x86_64 Linux（KVM）** 与
> **x86_64 Windows（WHPX）** 上**同架构**运行，并且能跑 **arm64 应用**，用来免真机验证 `autod`。
>
> 目标一句话：**不要跨架构模拟，要同架构 + 用户态翻译层。**
>
> 历史：本目录原为 `dev/06-x64-android`；更早的 `dev/04-emulator`（跨架构全系统 arm64 模拟）
> 经实测评估后移除，结论见 [`docs/09-why-not-full-arm64-sim.md`](docs/09-why-not-full-arm64-sim.md)。

---

## 0. 当前状态（第 12 轮 · 收口）

| 目标 | 状态 |
|---|---|
| G1 自编 x86_64 Android 12 ROM | ✅ `EXIT=0`，62083 个编译目标 |
| G2 Linux x86_64 同架构跑起来（KVM） | ✅ **已实测**：`sys.boot_completed=1`，设备 `autosnap_x64_arm64` |
| G3 能跑 arm64 应用 | ✅ **已实测**：自建探针 APK（纯 arm64-v8a）装+跑，`primaryCpuAbi=arm64-v8a`，16 条 `/system/lib64/arm64/*` 映射，JNI 返回 `kernel=x86_64` |
| G4 Windows x86_64 跑同一份 ROM（WHPX） | ⊘ **明确不由 agent 验证**（用户决定）；工程部分已交付：脚本 + 镜像 + `preflight.ps1` + 首次运行对照表 + **同 build id 等价性**（Windows 稳定包 15917651 与 Linux 包同 build，Linux 侧实跑全绿） |
| G5 arm32 下放（备选） | 📄 预案见 [`docs/06-arm32-fallback.md`](docs/06-arm32-fallback.md) |

```bash
# 一次性复现（构建→自检→打包→启动验收）
./scripts/accept.sh
```

---

## 1. 目标与判定标准

| # | 目标 | 判定方式 |
|---|---|---|
| G1 | 自编 x86_64 Android 12 ROM | `lunch autosnap_x64_arm64-userdebug && m` 产出 `system.img` / `vendor.img` / `ramdisk.img` / `kernel-ranchu` |
| G2 | Linux x86_64 上同架构跑起来 | KVM 加速启动，`sys.boot_completed=1`，开机时间几十秒量级 |
| G3 | 能跑 **arm64** 应用 | 设备 `ro.product.cpu.abilist` 含 `arm64-v8a`；arm64 原生库的 APK 能装、能起、进程里映射 `/system/lib64/arm64/*.so` |
| G4 | Windows x86_64 上跑同一份 ROM | 同一份镜像产物 + `emulator.exe`（SDK 37.x）+ WHPX |
| G5 | （备选）arm64 不行时下放到 arm32 | 换成带 `armeabi-v7a` 的翻译层载荷 + 四 ABI 板级配置（见 §5） |

**技术路线**（不是拍脑袋，依据是本仓库的实测评估
[`docs/00-bridge-eval.md`](docs/00-bridge-eval.md)）：

- 翻译层用 Google 官方的 **`libndk_translation`**（来自 `system-images;android-31;google_apis;x86_64`）。
  **不是 libhoudini**——houdini 的 arm64 变体只到 Android 7，公开源上 8/9 系列根本没有 `_z`。
- ROM 自编，但**不改 AOSP 上游一行**：设备树与载荷以本项目为真源，由 `scripts/apply-overlay.sh`
  同步到 `aosp/device/autosnap/`；`lunch` 能发现它是因为 Soong 会递归扫 `device/*/*/AndroidProducts.mk`。
- 内核与框架都是 **x86_64**（`x86_64-kernel.mk`），ARM 只在用户态被翻译——这才是"同架构"的含义。

---

## 2. 目录结构

```
dev/04-x64-android/
├── README.md                       ← 本文件
├── PLAN.md                         ← 阶段划分与当前状态
├── device/                         ← 设备树（注入 aosp/device/autosnap/ 的唯一真源）
│   ├── AndroidProducts.mk
│   └── autosnap_x64_arm64/
│       ├── BoardConfig.mk          ← x86_64 + TARGET_NATIVE_BRIDGE_ABI=arm64-v8a
│       ├── device.mk
│       └── product/autosnap_x64_arm64.mk
├── payload/                        ← 翻译层（从官方镜像提取，90 个文件 / 23 MB）
│   ├── system/lib64/libndk_translation*.so       (21)
│   ├── system/lib64/arm64/*.so                   (59)
│   ├── system/bin/arm64/{linker64,app_process64}
│   ├── system/bin/ndk_translation_program_runner_binfmt_misc_arm64
│   ├── system/etc/binfmt_misc/{arm,arm64}_{exe,dyn}
│   ├── system/etc/init/ndk_translation.rc
│   ├── system/etc/ld.config.arm{,64}.txt
│   └── MANIFEST.sha256
├── emulator/
│   └── config.ini                  ← 模拟器显示配置（720x1280 @320dpi），本项目唯一真源
├── scripts/
│   ├── common.sh
│   ├── fetch-payload.sh            ← 从官方镜像取翻译层（下载 → 启动 → adb pull → 校验）
│   ├── apply-overlay.sh            ← 注入 AOSP（幂等 / --check / --revert / --patch-initrc）
│   ├── build-rom.sh                ← 容器内 lunch + m（后台 + 日志 + --status）
│   ├── package-rom.sh              ← 打包可交付 ROM 目录（SHA256SUMS + MANIFEST.txt）
│   ├── run-linux.sh                ← Linux/KVM 启动 + 验收（含 arm64 应用）
│   └── status.sh                   ← 一眼看清 载荷/注入/构建/产物/设备
├── tools/
│   ├── build-probe-apk.sh          ← 自建 arm64 探针 APK（纯 arm64-v8a，16 KB）
│   ├── arm64-probe/                ← 探针源码（manifest / Activity / JNI）
│   ├── check-bridge-symbols.sh     ← 翻译层动态依赖自检（启动前发现版本错配）
│   ├── net-bridge.sh               ← 桥接模式：建 br0 把上行网卡桥进去（带自动回滚）
│   └── net-bridge-ifup.sh          ← 模拟器拉起 TAP 时的回调，把它挂进桥
├── windows/                        ← Windows 侧（同一份镜像）
├── docs/                           ← 设计记录与实测结论
├── artifacts/                      ← 产物软链与构建清单
└── .run/                           ← 运行期（datadir、日志、截图）
```

> **屏幕分辨率 / 密度**：模拟器不读 `build.prop` 里的密度，只认自己的 `config.ini` ——
> 首次启动时由它生成 `<sysdir>/hardware-qemu.ini` 的 `hw.lcd.width/height/density`。
> ROM 构建装进去的是 goldfish 的 `data/etc/config.ini.xl`（**1440x2960 @560dpi**，
> Pixel 3 XL 尺寸）；`emulator/config.ini` 把它统一覆盖成 **720x1280 @320dpi**
> —— 配 `-gpu swiftshader_indirect`（纯 CPU 软件光栅化）时像素量约降到 1/4.6，
> 是这套配置里最省的一项。三个生效点都取同一份文件：
> `run-linux.sh`（写进 `.run/sysdir-<port>/`）、`package-rom.sh`（写进交付目录）、
> `windows/run-windows.ps1`（写进 `windows/images/`）。
> **不参与 AOSP 构建，改完不需要重编 ROM。**

---

## 3. 快速开始

```bash
cd dev/04-x64-android

./scripts/fetch-payload.sh          # 1. 取翻译层（约 1.4 GB 下载 + 一次官方镜像启动）
./scripts/apply-overlay.sh          # 2. 注入 AOSP 树（幂等，可 --revert）
./tools/build-probe-apk.sh          # 3. 自建 arm64 探针 APK（纯 arm64-v8a，验收用）

./scripts/build-rom.sh              # 4. 全量构建（后台；--status 看进度）
./scripts/status.sh                 #    随时看 载荷/注入/构建/产物/设备 五项状态
./tools/check-bridge-symbols.sh     #    （可选）翻译层依赖自检，启动前排雷

./scripts/accept.sh                 # 5. 一条龙：自检 → 依赖检查 → 打包 → 启动验收
                                    #    （等价于 verify-rom + check-bridge-symbols + package-rom + run-linux）
./scripts/run-linux.sh              #    只做启动验收
./scripts/package-rom.sh            #    只做打包（含 sha256 清单）
```

Windows 侧：同一份打包产物 + `emulator.exe` + WHPX，见 [`windows/README.md`](windows/README.md)。



---

## 4. 与官方镜像的差异（就这些）

| 项 | 官方 `google_apis;x86_64`(API 31) | 本项目 ROM |
|---|---|---|
| 内核 / 框架架构 | x86_64 | **x86_64（相同）** |
| `ro.product.cpu.abilist` | `x86_64,arm64-v8a` | **相同**（由 `TARGET_NATIVE_BRIDGE_ABI` 自动生成） |
| 翻译层 | `libndk_translation.so`（Google 专有） | **同一份载荷**（提取自官方镜像） |
| 产品名 / 设备名 | `sdk_gphone64_x86_64` / `emulator64_x86_64_arm64` | `autosnap_x64_arm64` |
| 可控性 | 不可改 | 可加 `autod`、改 framework、砍组件 |

**为什么值得自编**：官方镜像虽然能跑，但它是"别人的 ROM"——不能加系统服务、不能改
framework、`/system` 里塞不进东西、也没法做交付裁剪。自编之后这些都是自由的。

---

## 5. 风险与已知限制（实测得出，别当理论）

| 限制 | 说明 | 处置 |
|---|---|---|
| **翻译层是 Google 专有二进制** | 只随 SDK 系统镜像分发，许可不含再分发 | 自用/开发无碍；**对外交付前必须过法务** |
| API 31 镜像**纯 64 位** | `abilist32` 为空 → `armeabi-v7a` 应用装不上 | 需要 32 位 ARM 就走 §5 的 arm32 方案（G5） |
| 串行浮点退化 | 依赖链 double 运算实测 21~23× 慢（整数/哈希 ~1.1×） | 目标应用先做性能验收 |
| 翻译层与 Android 版本绑定 | 载荷的 ARM 侧 bionic 与框架版本配套 | **API 31 的载荷只能配 API 31 的 ROM** |
| **桥接模式要动宿主网络** | 建 `br0` 会把上行网卡的 IP/默认路由搬走；若那正是 SSH 网卡，会短暂断链 | 用 `tools/net-bridge.sh`（自带 systemd 自动回滚）；不桥接则一切照旧 —— 见 §6 的桥接文档 |

**arm32 下放方案（G5，未做）**：Google 的 API 30（Android 11）镜像是**四 ABI**
（`x86_64,x86,arm64-v8a,armeabi-v7a,armeabi`，`/system/lib/arm` 59 个 32 位 ARM 库），
实测 32 位 ARM 应用能装能跑。要 32 位支持，就得把整条链换成 **API 30 基座**
（板级 `TARGET_NATIVE_BRIDGE_2ND_ARCH := arm` + API 30 载荷），而不是在 API 31 上打补丁。

---

## 6. 相关文档

- 路线评估与全部实测证据 → [`docs/00-bridge-eval.md`](docs/00-bridge-eval.md)
- 设备树为什么这么写（逐条 AOSP 依据） → [`docs/01-design.md`](docs/01-design.md)
- 构建中实际撞到的坑与解法 → [`docs/02-build-traps.md`](docs/02-build-traps.md)
- 交付与验收（Linux/Windows 两侧怎么跑、怎么证明是同一份） → [`docs/03-delivery.md`](docs/03-delivery.md)
- 验收排错手册（症状 → 查什么 → 怎么修） → [`docs/04-acceptance-runbook.md`](docs/04-acceptance-runbook.md)
- **验收报告（结论性证据记录）** → [`docs/07-verification-report.md`](docs/07-verification-report.md)
- 往 ROM 里加自制组件（如 `autod`） → [`docs/05-adding-components.md`](docs/05-adding-components.md)
- **arm32 下放预案**（目标里的"如果 arm64 不行"） → [`docs/06-arm32-fallback.md`](docs/06-arm32-fallback.md)
- **为什么不做"跨架构全系统模拟 arm64"**（实测结论 + 可复用发现） → [`docs/09-why-not-full-arm64-sim.md`](docs/09-why-not-full-arm64-sim.md)
- **网络桥接模式**（让模拟器落到物理局域网，`-net-tap`） → [`docs/10-network-bridge.md`](docs/10-network-bridge.md)
- **快照与多实例**（7 秒从快照恢复、一键再开一台机器、MAC 硬限制） → [`docs/11-snapshots-and-multi.md`](docs/11-snapshots-and-multi.md)
- 进度与阶段 → [`PLAN.md`](PLAN.md)
