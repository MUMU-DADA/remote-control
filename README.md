# AutoSnapshotAndroid

在 Android 系统层对外提供**屏幕截图、触摸注入、设备管理**服务。
产物是一个 native daemon（`remote-control`），通过 HTTP / WebSocket / Unix socket 三条传输对外。

**先读 [`docs/01-selection.md`](docs/01-selection.md)** —— 为什么不用内核方案、三种落点怎么比、硬约束在哪。

---

## 现状

| 项 | 状态 |
|---|---|
| 目标 Android | **12**（编译期 API 30；运行时下限 Android 8，见 [`docs/03-reference.md`](docs/03-reference.md)） |
| 架构 | native daemon（`remote-control`） |
| 协议 | **v7，33 条命令**，三条传输走同一个 `Dispatcher` |
| 截图 | ✅ SurfaceFlinger 直连（8–12 ms/帧）／screencap exec（~197 ms） |
| 触控 | ✅ `/dev/uinput`（Android 12 的 `IInputManager` 是 Java-only，native 调不了） |
| 应用管理 | ✅ 列表、启动、停止、APK 安装（上传上限 4 GiB） |
| 文件管理 | ✅ 共享存储根（`/sdcard`）下的读写；相对路径仍相对下载目录 |
| 画面流 | ✅ WebSocket + MJPEG，jpeg/webp/png/h264，可中途改帧率/画质/分辨率 |
| 屏幕方向 | ✅ `0/90/180/270`（设备转不动时退到换显示尺寸，如实回报） |
| 鉴权 / 服务开关 | ✅ 令牌鉴权、软开关（不真停进程） |
| 验证 | 单元/集成 **497 项检查**（13 个套件）、文档一致性 **98 项**、真浏览器端到端 6 组 |

**全功能体检**：`python3 tools/functional-sweep.py [host:port]`

覆盖 33 条命令的 HTTP 面（截图/触控/按键/应用管理/文件/剪贴板/旋转/画面流/电源…）。
它和别的测试最大的区别是**判据取设备侧证据，不信接口返回** ——
这是被坑出来的：电源接口一直回 `ok:true` 而设备纹丝不动、画面流一切正常而抓帧
一帧没成功、应用管理退出码骗人，三次都是"接口说成功、设备没动"。

所以它看的是：重启→`uptime` 归零、按键→焦点窗口真的变了、启动应用→前台包名变了、
截图→字节熵不是纯黑、旋转→显示尺寸真的翻转。

> 平台上确实做不到的项（如 Android 10+ 后台进程写不了剪贴板）单独记为
> **已知限制**、不计入失败 —— 否则它恒退出 1，当不了回归闸门。
> 标注不是永久豁免：一旦该项意外通过，脚本会提示复核。

> ⚠️ **性能结论要连着分辨率一起看。** 上面是 720p 的口径。
> 完整实测、测量方法与测量纪律见 [`docs/06-capture-performance.md`](docs/06-capture-performance.md)。

截图与视频流已加入专项优化：抓帧不再等待长手势的全局操作锁，
JPEG/PNG/WebP 流共享同内容、同参数的编码结果；PNG 滤波减少重复扫描，
H.264 优先使用系统 libyuv 转换像素。传输写入有 1 秒期限，控制台解码与绘制
优先呈现最新帧。720p 模拟器的 4 路 PNG 对照中，静态投递约 30→61fps，
设置页滚动约 32–34→59fps；保持流连接时静态 PNG 截图 p50 为 167→116ms。
环境、原始记录及统计限制见[证据索引](docs/evidence/stream-opt-2026-10-04/README.md)，
这些结果不能直接用于估计动态游戏或真机硬件编码收益。

---

## 三分钟跑起来

不需要完整 AOSP 源码树也能编（NDK 版走 `screencap` 后端），
但需要 NDK 和 **jsoncpp 1.9.4 源码**。没有仓库内的
`aosp/external/jsoncpp` 时，先用 `JSONCPP_DIR` 指定该依赖：

```bash
JSONCPP_DIR=/path/to/jsoncpp bash tools/build-ndk.sh
                                            # 默认 arm64-v8a / API 31
adb push dev/02-native-daemon/out/ndk/arm64-v8a/remote-control /data/local/tmp/
adb shell chmod 755 /data/local/tmp/remote-control
adb shell "setsid /data/local/tmp/remote-control --socket /data/local/tmp/remote-control.sock \
           --http-bind 0.0.0.0 --foreground >/dev/null 2>&1 &"
```

浏览器打开 `http://<设备IP>:8088/` 就是控制台。

要 **SurfaceFlinger 后端**（快 20 倍）才需要 AOSP 树：

```bash
bash tools/integrate-aosp.sh                 # 把 dev/ 源码 rsync 进 AOSP 树
TARGET=sdk_phone64_x86_64-userdebug bash tools/build-remote-control.sh
```

> ⚠️ `integrate-aosp.sh` 是 **rsync 复制**，不是软链。改了 `dev/` 下的代码
> 却没重新接入的话，`build-remote-control.sh` 会顺利通过并产出一个**跟改动无关的
> 旧二进制**。所以构建脚本里有源码新鲜度检查，不新鲜会直接叫停。
>
> 也**别拿 NDK 版测性能** —— 那跑的是 `screencap`。用
> `curl /api/v1/config | jq -r .runtime.capture.backend` 确认。

---

## 目录结构

```
AutoSnapshotAndroid/
├── README.md                  ← 本文件：项目入口
│
├── docs/                      ← 专题文档
│   ├── 01-selection.md           选型与结论：为什么不用内核、三种落点、硬约束
│   ├── 02-architecture.md        架构：组件、协议、并发模型、数据流
│   ├── 03-reference.md           技术参考：各 Android 版本 API 差异、触控能力矩阵
│   ├── 04-environment.md         ⭐ 本机实际环境（磁盘/镜像源/构建容器）
│   ├── 05-design-notes.md        设计取舍与踩过的坑（含速查表）
│   ├── 06-capture-performance.md 抓帧与编码性能实测、调优、测量纪律
│   ├── 07-dependencies.md        ⭐ 依赖清单：用了什么库、为什么不用手写
│   ├── 08-input-injection.md     备选注入路径：Java 系统服务 / 特权 APK
│   ├── 09-deployment-and-update.md  ⭐ 固化（开机自启/保活）+ 热替换通道【计划】
│   ├── evidence/                 实机验证证据（截图 + 自检输出）
│   └── api/                      ⭐ **接口权威文档**（HTTP / WebSocket / socket / 配置 / 错误码 / 调试）
│
├── tools/                     ← 环境与构建脚本
│   ├── setup-host.sh             环境初始化
│   ├── integrate-aosp.sh         把 dev/ 源码接进 AOSP 树（rsync）
│   ├── build-remote-control.sh            编 remote-control / rcctl（AOSP 平台后端）
│   ├── build-ndk.sh              编 NDK 版（screencap 后端，无需 AOSP 树）
│   ├── check-api-docs.py         核对文档与运行中的服务是否一致（**文档 ↔ 服务**）
│   └── check-docs.py             文档体检：链接 / 索引覆盖 / 编号 / 孤儿 /
│                                 陈旧路径 / 脚本路径是否存在（**文档 ↔ 仓库**）
│
├── aosp/                      ← AOSP 源码树
│
└── dev/                       ← 开发轨道（索引见 dev/README.md）
    ├── README.md                 ⭐ **本树的索引**：三个模块 + 编号沿革（01/03/06 去哪了）
    ├── 02-native-daemon/         ⭐ 主体代码（remote-control + rcctl）
    ├── 04-android-rom/           验证与交付轨道：自编 ROM（x86_64 桥 / arm64 原生两条线）
    │   └── docs/                 ← **16 篇专题**（评估 / 构建坑 / 交付 / 验收 / 网络 /
    │                                快照 / 实例控制 / macOS / release 打包；索引见该模块 README §6）
    └── 05-controller-app/        上位应用（设备本地的服务管理器；需「所有文件访问」权限）
```

> ⚠️ `dev/` 的编号是**历史沿革**，不是「第几个模块」：`01-ndk-prototype`／`03-java-service`／
> `04-emulator` 都已移除或改名，`04` 这个号被用过**三次**（`04-emulator` → `04-x64-android`
> → 现在的 `04-android-rom`）。逐条去向见 [`dev/README.md`](dev/README.md)。

---

## 必须先知道的三件事

**1. 不需要全量编译系统镜像。** `m remote-control` 只编模块。只有要装进 `/system/bin/`
并开机自启时才涉及镜像，而那也能用 Magisk 绕开。

**2. 唯一躲不掉的是 `repo sync`**（走 SF 后端的话）。`remote-control` 用了 `libgui`
这类平台私有库，头文件只存在于 AOSP 源码里。纯 NDK 轨道没这个约束。

**3. Android 12 上 native 进程注入不了触摸。** `IInputManager` 在 12 上是
Java-only AIDL。`remote-control` 用 `/dev/uinput` 绕开 —— 代价是会创建一个可枚举的
输入设备。完整论证见 [`docs/01-selection.md`](docs/01-selection.md)。

---

## 能力边界

**做不到**：

- **`FLAG_SECURE` 内容抓不到** —— 受保护图层不会被合成进 CPU 可读缓冲区，
  这是 SurfaceFlinger 的设计。要改必须打 framework 补丁。
- **不提供隐蔽性** —— `remote-control` 会出现在 `dumpsys` / `service list`，
  SELinux domain 和 socket 标签都可枚举。
- **非 root 真机上触控受限** —— `/dev/uinput` 通常只对 `system` / root 开放。

**已知工程限制**：

- 平台私有 API 跨版本不稳定，升级 Android 必须对着源码重新确认
  （核对清单见 [`docs/03-reference.md`](docs/03-reference.md)）
- 触控与其它有状态操作仍由 `Dispatcher` 串行化；截图与显示查询使用
  `Capture` 自身的锁，可与长手势、文件操作并行。单个截图后端的抓帧仍串行
- H.264 每个订阅者拥有独立 MediaCodec 编码器，受设备编码器并发能力限制；
  JPEG/PNG/WebP 的共享编码缓存不适用于有状态的 H.264 帧间编码
- 要传 fd 的客户端（截图帧、APK 安装）**必须在设备上运行** ——
  `SEQPACKET` 和 `SCM_RIGHTS` 过不了 `adb forward`
- 正式实例默认绑定 `0.0.0.0` 并开启鉴权，首次实例化生成随机令牌；可在
  release 的 `templates/config.ini` 预设服务配置。参见
  [`docs/api/04-config.md`](docs/api/04-config.md)

---

## 参考实现出处

- 截图：[`frameworks/base/cmds/screencap/screencap.cpp`](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/cmds/screencap/screencap.cpp)
- 触控：`frameworks/base/cmds/input/src/com/android/commands/input/Input.java` + `core/java/android/hardware/input/IInputManager.aidl`
- 版本限制：[Restricted screen reading](https://source.android.com/docs/core/permissions/restricted-screen-reading)
- GKI 模块约束：[Android 内核 ABI 监控](https://source.android.com/docs/core/architecture/kernel/abi-monitor)

---

## 许可

**MIT** —— 见 [LICENSE](LICENSE)。

内置的第三方代码只有 libwebp（BSD 3-Clause）和 libjpeg-turbo 的头文件
（BSD-style），都是宽松许可，**没有任何 copyleft**，所以 MIT 成立。
H.264 可选地运行时加载系统 `libyuv.so`（BSD 3-Clause）；缺库或缺符号时
使用内置标量转换，不新增必须安装的设备库。
逐项清单和判定依据在 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

> ⚠️ 两样东西**不在**本仓库里，别以为 MIT 覆盖了它们：
> AOSP 源码树（`.gitignore` 的 `/aosp/`，那 314 个 git 项目各自有许可），
> 以及 Google 的 arm64 翻译层载荷（`dev/04-android-rom/payload/system/`，
> 专有二进制，由 `fetch-payload.sh` 现拉、不得再分发）。
