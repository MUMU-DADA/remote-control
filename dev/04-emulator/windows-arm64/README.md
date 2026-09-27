# windows-arm64 · 在 x64 Windows 上跑编好的 arm64 ROM

> 宿主：Windows 10 1809+ / 11（x64）
> Guest：开发机上 `sdk_phone64_arm64-userdebug` 编出来的 **arm64 安卓镜像**
> 用途：交付前在真实 Windows 环境整机运行验证

原理与硬约束见 [`../README.md`](../README.md)。关键一条：**arm64 guest 在 x86_64 宿主上
只能纯软件模拟（TCG）**，Windows 的 Hyper-V / WHPX 帮不上忙——那只加速 x86_64 guest。

---

## 三步

```powershell
cd dev\04-emulator\windows-arm64

.\fetch-emulator.ps1     # 1. 拿到 emulator.exe（内含 qemu-system-aarch64.exe）+ adb
.\fetch-images.ps1       # 2. 从开发机 scp 镜像过来（带 md5 校验）
.\run-emulator.ps1       # 3. 启动，等到开机完成
```

也可以双击 `run-emulator.bat`。

---

## 前置条件

| 项 | 要求 | 说明 |
|---|---|---|
| OpenSSH 客户端 | Win10 1809+ 自带 `ssh.exe` / `scp.exe` | 没有就到「设置 → 应用 → 可选功能」装 |
| 磁盘 | ≥ 15 GB | 镜像 4~6 GB + 模拟器 ~1.5 GB + userdata 覆盖层 |
| 内存 | ≥ 8 GB | 默认给模拟器 4 GB |
| VC++ 运行库 | 启动失败时装 VC++ 2015-2022 x64 | 模拟器是原生程序 |
| 虚拟化 / Hyper-V | **不需要** | 跨架构用不上硬件加速，省心 |
| 网络 | 能访问 `dl.google.com` 才能自动下模拟器 | 不能就照下面「下载不到怎么办」 |

`fetch-emulator.ps1` 会**先找机器上已有的 SDK**（`ANDROID_SDK_ROOT` / `ANDROID_HOME` /
`%LOCALAPPDATA%\Android\Sdk`，即 Android Studio 装的那份），找到就直接用、什么都不下。

---

## ⚠️ 唯一的真不确定点：模拟器版本

| 事实 | 影响 |
|---|---|
| 开发机 AOSP 12 树里自带模拟器 **30.8.3**（与镜像同源，最稳） | 只有 linux/darwin 预编译包，**没有 Windows** |
| Google 只保留当前版本，老包已删（实测 `emulator-windows_x64-7595944.zip` → **HTTP 404**） | 无法让 Windows 用上与镜像同源的 30.8.3 |
| 当前 stable 渠道是 **37.1.11**（脚本自动解析，421 MB） | Windows 只能用新版 |

### 实测结论：**新版模拟器不支持在 x86_64 宿主上跑 arm64**，必须钉旧版

在开发机上用同一份 arm64 镜像实测（Linux 版与 Windows 版同源代码）：

```
INFO  | Found build target architecture: arm64
FATAL | QEMU2 emulator does not support arm64 CPU architecture        ← emulator 37.1.11
```

| 模拟器版本 | x86_64 宿主跑 arm64 guest |
|---|---|
| ≤ 33.x | ✅ 带 `qemu-system-aarch64`，接受 arm64 镜像 |
| 34+（含当前 stable 37.1.11） | ❌ 直接 `FATAL` |

而 Google **只保留当前版本**：老包在官方源全部 404
（实测 `emulator-windows_x64-7595944.zip` → 404）。**国内镜像还留着旧包**（实测腾讯镜像）：

| 包 | `dl.google.com` | 腾讯镜像 |
|---|---|---|
| `emulator-windows_x64-7595944.zip`（30.8.3） | 404 | 404 |
| `emulator-windows_x64-8807927.zip` | 未测 | **HTTP 200，380MB** |
| `emulator-windows_x64-15917651.zip`（37.1.11） | 200 | 200 |

所以 Windows 侧要这样装：

```powershell
.\fetch-emulator.ps1 -RepoBase https://mirrors.cloud.tencent.com/AndroidSDK -BuildId 8807927
```

（`-BuildId` 直接拼 zip 名；清单里没有旧版本，因此**不做 SHA1 校验**。）

### ⚠️ 还有一道尚未解决的关卡：arm64 的 PCI 音频设备

不管 30.8.3 还是 31.x，arm64 镜像都会卡在这里：

```
qemu-system-aarch64-headless: PCI bus not available for hda
emulator: Done with QEMU main loop
```

模拟器给 arm 的 `ranchu` 机器挂 PCI 设备（`intel-hda`、`virtio-serial-pci`），
而 arm ranchu **没有 PCI 总线** → QEMU 致命退出。已排除：`-no-audio` / `-audio none` /
`QEMU_AUDIO_DRV=none` / 改 `hardware-qemu.ini` / AVD 模式（AVD 直接段错误）。
完整记录见 [`../linux-arm64/README.md`](../linux-arm64/README.md) 的「成品镜像与当前已知阻塞」。

判断：大概率是**镜像 feature 与模拟器不配套**（用 Google 成品镜像时还会警告
`unexpected system image feature string`）。**自己编的镜像**（与本机模拟器同源）是下一个要试的，
开发机上已经开编。若自制镜像同样卡住，Windows 侧的 arm64 只剩：
真机 / arm64 宿主 / WSL2（见下面降级 C）。

万一启动失败，按顺序试：

### 降级 A：换渠道

```powershell
.\fetch-emulator.ps1 -Channel beta      # 37.2.11
.\fetch-emulator.ps1 -Channel dev       # 37.3.1
```

> 清单里目前**没有 canary 渠道的 emulator**，脚本会明确告诉你并列出可用版本。

### 降级 B：改用 AVD 方式（新版对 `-sysdir` 老模式的兼容最可能出问题）

把镜像目录摆成 SDK system image 的样子，然后建 AVD：

```
%LOCALAPPDATA%\Android\Sdk\system-images\android-31\aosp_arm64\arm64-v8a\
    system.img  vendor.img  ramdisk.img  kernel-ranchu  advancedFeatures.ini …
    source.properties          ← 手工加，内容如下
```

```ini
# source.properties
Pkg.Desc=Android SDK Platform 31
Pkg.Revision=1
AndroidVersion.ApiLevel=31
SystemImage.Abi=arm64-v8a
SystemImage.TagId=default
SystemImage.TagDisplay=Default Android System Image
```

```powershell
# -k 的 tag 段必须与目录名一致（这里 tag 用 aosp_arm64，把上面路径的 aosp_arm64 对应上）
avdmanager create avd -n aosp12-arm64 -k "system-images;android-31;aosp_arm64;arm64-v8a" --abi arm64-v8a
emulator -avd aosp12-arm64 -gpu swiftshader_indirect -accel off
```

`avdmanager` 来自 Android Studio 或 SDK 的 `cmdline-tools`（需要 JDK）。

### 降级 C：WSL2 + 开发机同款 30.8.3（版本完全匹配）

```powershell
wsl --install -d Ubuntu-22.04
```

```bash
# WSL 里
sudo apt-get install -y libpulse0 libgl1
mkdir -p ~/emu && cd ~/emu
scp -r root@192.168.0.108:/root/AutoSnapshotAndroid/aosp/prebuilts/android-emulator/linux-x86_64 ./emulator
./emulator/emulator -sysdir /mnt/c/<镜像目录> -datadir ./data \
    -gpu swiftshader_indirect -accel off -no-snapshot
```

WSLg 会把模拟器窗口显示出来（Win11 / Win10 装了 WSLg 的话）；没有 WSLg 就加 `-no-window`
配合 `adb`。多花一次下载，但版本与镜像同源，是最稳的一条路。

---

## 下载不到怎么办（国内网络）

按推荐顺序：

```powershell
# 1) 本机有代理工具（v2ray/Clash 之类，常见端口 10809）
.\fetch-emulator.ps1 -Proxy http://127.0.0.1:10809

# 2) 换镜像源（★ 已实测：腾讯镜像与 Google 官方同源同字节）
.\fetch-emulator.ps1 -RepoBase https://mirrors.cloud.tencent.com/AndroidSDK

# 3) 自己下好 zip 再喂进来（本地文件或 URL 都行）
.\fetch-emulator.ps1 -EmulatorZipUrl D:\pkgs\emulator-windows_x64-15917651.zip

# 4) 先看看会下什么、多大、sha1 是多少（不下载）
.\fetch-emulator.ps1 -DryRun
```

**镜像可用性实测**（脚本默认会下的那个包，2026-09-28）：

| 来源 | `repository2-3.xml` | `emulator-windows_x64-15917651.zip` |
|---|---|---|
| `dl.google.com/android/repository` | HTTP 200 | HTTP 200，441926448 字节 |
| `mirrors.cloud.tencent.com/AndroidSDK` | HTTP 200（与官方同大小） | HTTP 200，441926448 字节（**与清单声明一致**） |

所以国内网络直接用第 2 条：`-RepoBase` 会把清单和 zip 都从镜像取，SHA1 校验照样过。
装了 Android Studio 的话连下载都不需要 —— 第一步就会命中它自带的 SDK。

---

## 脚本参数速查

### `fetch-emulator.ps1`

| 参数 | 默认 | 说明 |
|---|---|---|
| `-Dest` | `.\sdk` | 下载/解包目录 |
| `-Channel` | `stable` | `stable` / `beta` / `dev` / `canary` |
| `-RepoBase` | `https://dl.google.com/android/repository` | 换镜像改这里 |
| `-RepoXmlPath` | 空 | 直接用本地仓库清单 XML（离线/内网） |
| `-EmulatorZipUrl` | 空 | 直接指定包（本地路径或 URL），跳过解析 |
| `-Proxy` / `-ProxyUser` / `-ProxyPassword` | 空 | 走代理下载 |
| `-SkipPlatformTools` | 关 | 不装 adb |
| `-Force` | 关 | 无视已有文件，重下 |
| `-DryRun` | 关 | 只解析并打印将要下载的内容 |

### `fetch-images.ps1`

| 参数 | 默认 | 说明 |
|---|---|---|
| `-Remote` | `root@192.168.0.108` | 开发机地址 |
| `-RemoteDir` | `/root/AutoSnapshotAndroid/aosp/out/target/product/emulator64_arm64` | 镜像目录（**不是** `generic_arm64`） |
| `-Dest` | `.\images` | 落到哪 |
| `-Force` | 关 | 已存在的文件也重拉 |
| `-SkipVerify` | 关 | 跳过 md5 比对 |

同名同大小的文件默认跳过，所以**可以反复执行**（断了接着拉没下完的那几个）。
只有密码认证时，脚本把所有文件放进一次 `scp` 调用，因此只提示一次密码。

### `run-emulator.ps1`

| 参数 | 默认 | 说明 |
|---|---|---|
| `-EmulatorDir` | 自动探测 | 含 `emulator.exe` 的目录 |
| `-ImagesDir` / `-DataDir` | `.\images` / `.\data` | 镜像目录 / userdata 覆盖层 |
| `-Gpu` | `swiftshader_indirect` | 也可试 `angle_indirect`（走 DirectX，可能更快）、`host` |
| `-MemoryMB` / `-Cores` | 4096 / 4 | 内存吃紧就调小 |
| `-Port` | 5554 | 多开时改 |
| `-Headless` | 关 | 不显示窗口 |
| `-NoSnapshot` | 关 | **默认开快照**（第二次启动快得多）；异常时配合 `-WipeData` 冷启动 |
| `-WipeData` | 关 | 清 userdata |
| `-WritableSystem` | 关 | 允许 `adb remount`（推 `/system/bin` 时用） |
| `-NoWait` | 关 | 起了就返回 |
| `-Stop` | 关 | 停掉该端口的模拟器 |

---

## 常见问题

| 现象 | 处理 |
|---|---|
| 启动后一直停在开机动画 | TCG 正常速度，首次 10~40 分钟；看 `.\data\emulator-5554.out.log` |
| `adb devices` 空 | 模拟器窗口开着的话看它自己的报错；确认 `-sysdir` 指向 `images\` |
| 报缺 `system.img` / `kernel-ranchu` | 镜像没拉全，或拉成了 GSI 产物（`generic_arm64`） |
| 模拟器闪退 | 装 VC++ 2015-2022 x64 运行库；看 `.err.log` 尾部 |
| 窗口黑屏 | 换 `-Gpu angle_indirect` 或退回 `swiftshader_indirect` |
| 提示 Hyper-V / WHPX 不可用 | **正常**，跨架构本来就不用它；脚本已传 `-accel off` |
| 快照后行为诡异 | `.\run-emulator.ps1 -WipeData -NoSnapshot` 冷启动 |
| 路径太长报错 | 项目别放在很深的中文/长路径下，`images\` 目录尽量短 |

---

## 相关文档

- 开发机侧怎么编出这些镜像 → [`../linux-arm64/README.md`](../linux-arm64/README.md)
- `autod` 主线与阶段划分 → [`../../02-native-daemon/README.md`](../../02-native-daemon/README.md)
- 为什么不能用 qemu.org 的 qemu-system-aarch64 → [`../README.md`](../README.md) 硬约束 2
