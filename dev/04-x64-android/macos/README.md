# macos · 在 Mac 上跑同一份 ROM

> **状态：脚本已就位，尚未在真 Mac 上跑过。**
> 本文写的是**怎么用**，不是"已验证可用"。设计依据与实测记录见
> [`../docs/13-macos-port.md`](../docs/13-macos-port.md)（尤其 §3 与 §7.0.8）。

---

## 为什么这条路在 macOS 上能成立

与 Windows 侧同源：都是"一份自编 ROM + 官方 SDK 模拟器 + 自带 adb"。

**但 Mac 上有一件事和 Windows 不一样，必须先分清架构：**

| 宿主 | 能跑哪份 ROM | 原因 |
|---|---|---|
| **Apple Silicon（arm64）** | **必须用 arm64 原生 ROM**（`remote_control_arm64`） | 官方 macOS/aarch64 模拟器包里**只有 `qemu/darwin-aarch64/` 一个后端**，没有任何 x86_64 后端 —— 不是慢，是根本没有那条路（§7.0.2 实测） |
| **Intel Mac（x86_64）** | 现有 `remote_control_x64_arm64` | macOS/x64 包里有 x86_64 后端；走 HVF 加速（**尚未实测**） |

两条 ROM 线的产品矩阵见 [`../docs/13-macos-port.md`](../docs/13-macos-port.md) §4。

---

## 三步

```bash
cd dev/04-x64-android/macos

# 1) 自检：这台 Mac 到底能不能跑
./preflight.sh

# 2) 准备运行时（模拟器 + adb），自动按本机架构选包
./fetch-emulator.sh                 # 默认 ./sdk
#   Intel Mac 强制选 x64：  ./fetch-emulator.sh --arch x64
#   只用某个本地包：        ./fetch-emulator.sh --emulator-zip ~/Downloads/emulator-darwin_aarch64-*.zip

# 3) 把 ROM 拉到本机（从构建机 scp，或从 U 盘/本地目录拷）
./fetch-images.sh                                     # 默认从 192.168.0.108 拉 x64_arm64 那份
./fetch-images.sh --remote-dir .../rom-remote_control_arm64     # 拉 arm64 那份
./fetch-images.sh --local /Volumes/usb/rom-xxx        # 从本地目录拷，不走网络
./fetch-images.sh --dequarantine                      # 拉完顺手清 quarantine
```

**这三步只准备"料"。** 起停查那套脚本（`bin/`）是**发布包内**的，见下一节。

---

## 两条使用路径

### A. 你现在就在源码树里开发（用 `macos/` 三个脚本）

上面三步跑完，手工起机器：

```bash
# 参数照抄 ../scripts/run-linux.sh 的 launch()，把宿主路径与 -accel 换掉：
cd dev/04-x64-android/macos
ANDROID_PRODUCT_OUT="$PWD/images" ANDROID_BUILD_TOP="$PWD" \
  ./sdk/emulator/emulator -sysdir "$PWD/images" -datadir /tmp/avd -port 5580 \
    -no-window -gpu host -accel on -memory 6144 -cores 4 -no-audio -no-boot-anim \
    -no-snapshot &
./sdk/platform-tools/adb -s emulator-5580 wait-for-device
```

> 为什么这里还要手工拼参数：`../scripts/` 那套里的 `run-linux.sh` / `emulator.sh`
> **是 Linux 宿主侧的**（探 `/dev/dri`、用 `/proc` 找进程、`setsid`）。
> Mac 的对应实现已经写好，但它们在**发布包内**（下一条），不是源码树脚本。

### B. 你是最终用户（用发布包里的 `bin/`）

发布包（`release/autosnap-<版本>-darwin-aarch64.zip`）解压后是这样：

```
autosnap-<版本>-darwin-aarch64/
├── START-HERE.md            ← 首读
├── RELEASE.json             ← 版本 / 平台 / 运行时来源
├── bin/                     ← **macOS 版**（本次新增）
│   ├── lib.sh               公共函数：路径、config.ini、实例登记、adb/模拟器定位
│   ├── start-headless.sh    无头启动（默认 -no-window，后台 + 等开机）
│   ├── stop.sh              优雅停（**先 sync 再 kill**）
│   ├── status.sh            宿主 + 实例 + ROM 指纹
│   └── verify.sh            验收（宿主 / 产品类型 / ELF 架构 / 设备 / 服务）
├── templates/               config.ini（硬件唯一真源）、实例登记说明
├── images/**                ROM 交付目录
├── runtime/**               SDK 模拟器（含 qemu 后端）+ platform-tools（adb）
└── tools/                   arm64 探针 APK 等
```

```bash
./bin/status.sh                       # 先看宿主与 ROM 对不对
./bin/start-headless.sh               # 起一台，等开机完成
./bin/verify.sh --port 5580           # 验收
./bin/stop.sh --port 5580             # 停（先 sync）
```

> ⚠️ **`bin/macos/` 这五个脚本还没打进发布包** —— `../scripts/release.sh` 目前只认
> `linux` / `windows` 两个平台（它的三分派表与 `START-HERE.md` 模板分支还没加 mac）。
> 也就是说路径 B **尚未可用**，先把脚本按路径 A 用起来。
> 打包那一层是独立一步，见 [`../docs/13-macos-port.md`](../docs/13-macos-port.md) §8 的 #16。

---

## 硬件参数：和另两个平台同一份 config.ini

`../emulator/config.ini` 是唯一真源（1280×720 @320dpi、`hw.ramSize=6144`、`hw.cpu.ncore=4`、`hw.gpu.mode=auto`）。
它**不参与 AOSP 构建**，由启动脚本在运行期覆盖进工作目录。

Mac 侧要注意的一条：`hw.gpu.mode=auto` 在 Linux 上靠探 `/dev/dri/renderD*` 判断，
**macOS 没有 `/dev/dri`**，所以 mac 版的 GPU 判定要换成 `system_profiler SPDisplaysDataType`
（对应 Windows 侧的 `Get-CimInstance Win32_VideoController`）。这条**还没写**，
在 mac 版启动脚本落地前，先手工指定 `-gpu host` 或 `-gpu swiftshader_indirect`。

---

## 前置条件与注意事项

| 项 | 说明 |
|---|---|
| Xcode Command Line Tools | `xcode-select --install` —— 提供 `python3`（脚本用它解析 SDK 仓库清单） |
| 硬件虚拟化 | `preflight.sh` 会查 `sysctl kern.hv_support`。**为 0 就只能退到 TCG**，开机从 20 秒变几分钟（量级差，见 [`../docs/09-why-not-full-arm64-sim.md`](../docs/09-why-not-full-arm64-sim.md)）。在虚拟机里跑 macOS 通常拿不到 HVF |
| 磁盘 | 数据分区本身 32 G，加镜像与快照要 **60 GiB 以上**可用空间 |
| Gatekeeper / quarantine | 从浏览器下载的 zip 会给解出的**每个文件**打 `com.apple.quarantine`，模拟器一执行就被拦。`preflight.sh --rom-dir DIR` 会检查；解除：`xattr -dr com.apple.quarantine <目录>` |
| **`unzip` 丢可执行位** | macOS 上解压后 `emulator`/`adb` 是 0644，直接执行报 `Permission denied`。`fetch-emulator.sh` 已自动补，**手工解压时要自己 chmod** |
| 宿主 bash 是 3.2 | macOS 自带的是 2007 年的 GPLv2 版。本目录两个脚本都只用 3.2 语法；但 `../scripts/release.sh` 用了 `declare -A`（需 bash 4+），**在 Mac 上跑打包要先 `brew install bash`** |
| 网络桥接 | **macOS 没有 `-net-tap` 的等价物**，与 Windows 侧一样走模拟器默认用户态 NAT。这不是待修的缺陷，是与 windows 包一致的能力面（见 `../docs/13-macos-port.md` §3.5） |

---

## ⚠️ 未在真机验证

本文与两个脚本都**没有在真 Mac 上执行过**（编写环境是 x86_64 Linux）。
已经验到的只有这些：

| 项 | 验证方式 |
|---|---|
| 选包逻辑（按 host-os + host-arch + 渠道） | 对**真实 SDK 清单**跑了 6 组，含一组应当失败的不存在组合（§7.0.8） |
| 两个脚本的语法 | `bash -n` 通过 |
| 在非 macOS 上会正确拒绝 | 在 Linux 上实跑，`preflight.sh` 报"不是 macOS"并继续做完其余检查 |
| macOS 包确有 arm64 后端 | 下载 `emulator-darwin_aarch64-16428233.zip`（416,112,708 字节，sha1 与清单一致）后列包内目录（§7.0.2） |

**首次在 Mac 上跑，请把 `preflight.sh` 与 `fetch-emulator.sh` 的输出贴回**，
好把上面这张表换成真机结论。
