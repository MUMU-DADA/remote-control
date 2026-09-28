# AutoSnapshotAndroid

在 Android 系统层对外提供**屏幕截图、触摸注入、设备管理**服务。
产物是一个 native daemon（`autod`），通过 HTTP / WebSocket / Unix socket 三条传输对外。

**先读 [`docs/01-selection.md`](docs/01-selection.md)** —— 为什么不用内核方案、三种落点怎么比、硬约束在哪。

---

## 现状

| 项 | 状态 |
|---|---|
| 目标 Android | **12**（编译期 API 30；运行时下限 Android 8，见 [`docs/03-reference.md`](docs/03-reference.md)） |
| 架构 | native daemon（`autod`） |
| 协议 | **v7，33 条命令**，三条传输走同一个 `Dispatcher` |
| 截图 | ✅ SurfaceFlinger 直连（8–12 ms/帧）／screencap exec（~197 ms） |
| 触控 | ✅ `/dev/uinput`（Android 12 的 `IInputManager` 是 Java-only，native 调不了） |
| 应用 / 文件管理 | ✅ 列表、启动、停止、安装、下载、浏览 |
| 画面流 | ✅ WebSocket + MJPEG，jpeg/webp/png/h264，可中途改帧率/画质/分辨率 |
| 屏幕方向 | ✅ `0/90/180/270`（设备转不动时退到换显示尺寸，如实回报） |
| 鉴权 / 服务开关 | ✅ 令牌鉴权、软开关（不真停进程） |
| 验证 | 单元/集成 **223 项检查**、文档一致性 **66 项**、真浏览器端到端 4 组 |

> ⚠️ **性能结论要连着分辨率一起看。** 上面是 720p 的口径。
> 完整实测、测量方法与测量纪律见 [`docs/06-capture-performance.md`](docs/06-capture-performance.md)。

---

## 三分钟跑起来

不需要 AOSP 源码树也能编（NDK 版走 `screencap` 后端）：

```bash
bash tools/build-ndk.sh                      # 出 out/ndk/autod
adb push out/ndk/autod /data/local/tmp/
adb shell chmod 755 /data/local/tmp/autod
adb shell "setsid /data/local/tmp/autod --socket /data/local/tmp/autod.sock \
           --http-bind 0.0.0.0 --foreground >/dev/null 2>&1 &"
```

浏览器打开 `http://<设备IP>:8088/` 就是控制台。

要 **SurfaceFlinger 后端**（快 20 倍）才需要 AOSP 树：

```bash
bash tools/integrate-aosp.sh                 # 把 dev/ 源码 rsync 进 AOSP 树
TARGET=sdk_phone64_x86_64-userdebug bash tools/build-autod.sh
```

> ⚠️ `integrate-aosp.sh` 是 **rsync 复制**，不是软链。改了 `dev/` 下的代码
> 却没重新接入的话，`build-autod.sh` 会顺利通过并产出一个**跟改动无关的
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
│   ├── evidence/                 实机验证证据（截图 + 自检输出）
│   └── api/                      ⭐ **接口权威文档**（HTTP / WebSocket / socket / 配置 / 错误码 / 调试）
│
├── tools/                     ← 环境与构建脚本
│   ├── setup-host.sh             环境初始化
│   ├── integrate-aosp.sh         把 dev/ 源码接进 AOSP 树（rsync）
│   ├── build-autod.sh            编 autod / autodctl（AOSP 平台后端）
│   ├── build-ndk.sh              编 NDK 版（screencap 后端，无需 AOSP 树）
│   └── check-api-docs.py         核对文档与运行中的服务是否一致
│
├── aosp/                      ← AOSP 源码树
│
└── dev/                       ← 开发轨道
    ├── 01-ndk-prototype/         已并入 02：截图/触控都做成了 autod 的可插拔后端
    ├── 02-native-daemon/         ⭐ 主体代码（autod + autodctl）
    ├── 03-java-service/          长期形态：Android 12 上 Java 侧注入的备选
    ├── 04-x64-android/           验证环境：自编 x86_64 ROM + ARM 用户态翻译层（独立线）
    └── 05-controller-app/        上位应用（只做服务管理，不申请任何权限）
```

---

## 必须先知道的三件事

**1. 不需要全量编译系统镜像。** `m autod` 只编模块。只有要装进 `/system/bin/`
并开机自启时才涉及镜像，而那也能用 Magisk 绕开。

**2. 唯一躲不掉的是 `repo sync`**（走 SF 后端的话）。`autod` 用了 `libgui`
这类平台私有库，头文件只存在于 AOSP 源码里。纯 NDK 轨道没这个约束。

**3. Android 12 上 native 进程注入不了触摸。** `IInputManager` 在 12 上是
Java-only AIDL。`autod` 用 `/dev/uinput` 绕开 —— 代价是会创建一个可枚举的
输入设备。完整论证见 [`docs/01-selection.md`](docs/01-selection.md)。

---

## 能力边界

**做不到**：

- **`FLAG_SECURE` 内容抓不到** —— 受保护图层不会被合成进 CPU 可读缓冲区，
  这是 SurfaceFlinger 的设计。要改必须打 framework 补丁。
- **不提供隐蔽性** —— `autod` 会出现在 `dumpsys` / `service list`，
  SELinux domain 和 socket 标签都可枚举。
- **非 root 真机上触控受限** —— `/dev/uinput` 通常只对 `system` / root 开放。

**已知工程限制**：

- 平台私有 API 跨版本不稳定，升级 Android 必须对着源码重新确认
  （核对清单见 [`docs/03-reference.md`](docs/03-reference.md)）
- 截图与触控**操作串行**（锁在 `Dispatcher::Handle()`）——
  `Injector` 是有状态的，并发注入会互相破坏手势
- 要传 fd 的客户端（截图帧、APK 安装）**必须在设备上运行** ——
  `SEQPACKET` 和 `SCM_RIGHTS` 过不了 `adb forward`
- 默认**不鉴权**且只绑 `127.0.0.1`；改成 `0.0.0.0` 前请先看
  [`docs/api/04-config.md`](docs/api/04-config.md)

---

## 参考实现出处

- 截图：[`frameworks/base/cmds/screencap/screencap.cpp`](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/cmds/screencap/screencap.cpp)
- 触控：`frameworks/base/cmds/input/src/com/android/commands/input/Input.java` + `core/java/android/hardware/input/IInputManager.aidl`
- 版本限制：[Restricted screen reading](https://source.android.com/docs/core/permissions/restricted-screen-reading)
- GKI 模块约束：[Android 内核 ABI 监控](https://source.android.com/docs/core/architecture/kernel/abi-monitor)
