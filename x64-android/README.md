# x64-android · 同架构 x86_64 Android ROM（可跑 arm64 应用）

> **与 `dev/04-emulator/` 的区别**：那边是"用模拟器验证 `autod`"的**开发内循环**；
> 这里是**独立的 ROM 项目**——自编一份 x86_64 安卓，让它在 **x86_64 Linux（KVM）** 与
> **x86_64 Windows（WHPX）** 上**同架构**运行，并且能跑 **arm64 应用**。
>
> 目标一句话：**不要跨架构模拟，要同架构 + 用户态翻译层。**

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
[`../dev/04-emulator/X86_64-ARM64-BRIDGE-EVAL.md`](../dev/04-emulator/X86_64-ARM64-BRIDGE-EVAL.md)）：

- 翻译层用 Google 官方的 **`libndk_translation`**（来自 `system-images;android-31;google_apis;x86_64`）。
  **不是 libhoudini**——houdini 的 arm64 变体只到 Android 7，公开源上 8/9 系列根本没有 `_z`。
- ROM 自编，但**不改 AOSP 上游一行**：设备树与载荷以本项目为真源，由 `scripts/apply-overlay.sh`
  同步到 `aosp/device/autosnap/`；`lunch` 能发现它是因为 Soong 会递归扫 `device/*/*/AndroidProducts.mk`。
- 内核与框架都是 **x86_64**（`x86_64-kernel.mk`），ARM 只在用户态被翻译——这才是"同架构"的含义。

---

## 2. 目录结构

```
x64-android/
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
├── scripts/
│   ├── common.sh
│   ├── fetch-payload.sh            ← 从官方镜像取翻译层（下载 → 启动 → adb pull → 校验）
│   ├── apply-overlay.sh            ← 注入 AOSP（幂等 / --check / --revert）
│   ├── build-rom.sh                ← 容器内 lunch + m（后台 + 日志 + --status）
│   └── run-linux.sh                ← Linux/KVM 启动 + 验收（含 arm64 应用）
├── windows/                        ← Windows 侧（同一份镜像）
├── docs/                           ← 设计记录与实测结论
├── artifacts/                      ← 产物软链与构建清单
└── .run/                           ← 运行期（datadir、日志、截图）
```

---

## 3. 快速开始

```bash
cd x64-android

./scripts/fetch-payload.sh          # 1. 取翻译层（约 1.4 GB 下载 + 一次模拟器启动）
./scripts/apply-overlay.sh          # 2. 注入 AOSP 树（幂等，可 --revert）
./scripts/build-rom.sh              # 3. 全量构建（后台；--status 看进度）
./scripts/run-linux.sh              # 4. Linux/KVM 启动 + 验收
```

Windows 侧见 [`windows/README.md`](windows/README.md)：同一份产物 + `emulator.exe` + WHPX。

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

**arm32 下放方案（G5，未做）**：Google 的 API 30（Android 11）镜像是**四 ABI**
（`x86_64,x86,arm64-v8a,armeabi-v7a,armeabi`，`/system/lib/arm` 59 个 32 位 ARM 库），
实测 32 位 ARM 应用能装能跑。要 32 位支持，就得把整条链换成 **API 30 基座**
（板级 `TARGET_NATIVE_BRIDGE_2ND_ARCH := arm` + API 30 载荷），而不是在 API 31 上打补丁。

---

## 6. 相关文档

- 路线评估与全部实测证据 → [`../dev/04-emulator/X86_64-ARM64-BRIDGE-EVAL.md`](../dev/04-emulator/X86_64-ARM64-BRIDGE-EVAL.md)
- 设备树为什么这么写（逐条依据） → [`docs/01-design.md`](docs/01-design.md)
- 进度与阶段 → [`PLAN.md`](PLAN.md)
