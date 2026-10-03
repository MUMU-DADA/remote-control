# AutoSnapshot 无头虚拟机 · @VER@（@PLATFORM_LABEL@）

**解压就能跑**的 Android 12 虚拟机：模拟器本体（无头）、系统镜像、配置模板都在包里，
不需要宿主机装 Android SDK、不需要联网。

| 项 | 值 |
|---|---|
| 发布版本 | `@VER@` |
| 平台 | @PLATFORM_LABEL@（guest 是 x86_64，另有 ARM64 用户态翻译层） |
| 打包时间 | @BUILT_AT@ |
| ROM 指纹 | `@ROM_FINGERPRINT@` |
| 运行时 | `@RUNTIME_PKG@`（@RUNTIME_VER@ build @BUILD_ID@） |
| 镜像 | `images/`（@IMAGES_SIZE@，含 `SHA256SUMS` + `MANIFEST.txt`） |

---

## 1. 快速开始

@QUICKSTART@

跑起来之后：

* 设备序列号就是 `emulator-<端口>`（默认 `emulator-5580`），用包内 adb 直接连：
  `@ADB_EXAMPLE@`
* 停机器：@STOP_CMD@（**先 sync 再关**，别直接杀进程 —— 见 §5）
* 验收：@VERIFY_CMD@（ABI / 翻译层 / arm64 机器码 / arm64 应用，四组）

---

## 2. 包里有什么

```
@ROOT_DIR@/
├── START-HERE.md          ← 本文件
├── RELEASE.json           ← 版本、ROM 指纹、运行时 build id、入口清单
├── SHA256SUMS             ← 整包逐文件校验
├── bin/                   ← 入口脚本（@ENTRY_NAMES@）
├── images/                ← 虚拟机镜像（ROM 交付目录，只读）
├── runtime/               ← 无头运行环境（模拟器 + qemu x86_64 后端 + adb）
│   └── RUNTIME.txt        ← 包名/版本/build id/来源 URL/sha1
├── templates/             ← 模板：config.ini（硬件）/ instance.env（实例登记）
└── tools/                 ← 验收探针（arm64-probe、arm64-probe.apk）@TOOLS_EXTRA@
```

`images/` 是**只读**的：启动时会在 `.run/sysdir-<端口>/` 里建一份工作目录，
把镜像链接进去，模拟器写的状态（`userdata-qemu.img`、快照、`build.avd/`）全落在
`.run/` 下，不会污染镜像本身。

---

## 3. 硬件参数（屏幕 / 内存 / 核数 / GPU）

改 [`templates/config.ini`](templates/config.ini) —— 它是**唯一真源**，
启动脚本每次都会读它。命令行只在**显式传参**时覆盖（命令行优先于它）。

| 键 | 默认 | 说明 |
|---|---|---|
| `hw.lcd.width` / `height` / `density` | 1280 / 720 / 320 | 无头运行时也决定截图分辨率 |
| `hw.ramSize` | 6144 | MB |
| `hw.cpu.ncore` | 4 | 核数 |
| `hw.gpu.mode` | auto | 有可用 GPU 就用 `host`，否则软件渲染；**起不来会自动退软件渲染** |
| `disk.dataPartition.size` | 32G | 上限不是预分配；改它要 reset 才生效 |

详见 [`templates/README.md`](templates/README.md)。

---

## 4. 多开几台

同一份镜像可以起多台（各自独立的数据与快照）：

```
@MULTI_EXAMPLE@
```

端口从 5580 起偶数分配（模拟器拿 `port+1` 当 console 口）。
**同时只让一台上物理 LAN** —— guest 的 MAC 是 QEMU 默认值，所有实例相同。

---

## 5. ⚠️ 三条别踩

1. **停机器必须先 `sync`。** `adb emu kill` 是硬断电不是关机：实测"写完不 sync
   直接停"会让最近写的数据**整个消失**（等 15 秒再停也一样）。包里的 stop 脚本
   已经先 `adb shell sync` 了，别绕过它去 `kill -9`。
2. **`initrd` 与 `config.ini` 不在工作目录里做链接。** 模拟器会**透过符号链接**
   重写 `initrd`，链过去就会改到 `images/` 里那份，`SHA256SUMS` 当场对不上。
   两个平台的工作目录构建逻辑都写死了这条例外。
3. **改过 `config.ini` 之后旧快照全部作废。** 快照要求硬件配置与存档时逐项一致，
   不一致模拟器会拒绝加载（`The emulator hardware cannot load snapshot`）。

---

## 6. ⚠️ 交付/再分发约束

| 项 | 说明 |
|---|---|
| **翻译层许可** | `images/` 里的 `libndk_translation*` 是 Google 专有二进制（随 SDK 系统镜像分发，SDK 许可**不含再分发**）。内部使用/开发无碍；**对外交付整机前必须过法务** |
| ABI 覆盖 | 本 ROM 是 `x86_64,arm64-v8a`（纯 64 位）：**32 位 ARM（armeabi-v7a）应用装不上** |
| 性能 | 串行依赖浮点实测退化 21~23×（整数/哈希约 1.1×）——目标应用先做性能验收 |
| 版本绑定 | 翻译层与 Android 版本绑定：Android 12（API 31）的载荷只能配 API 31 的框架 |
| 加速 | 同架构才有加速：Linux/KVM、Windows/WHPX。没有硬件虚拟化会退到纯软件模拟（很慢） |
