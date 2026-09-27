# AutoSnapshotAndroid

在 Android 系统层对**外提供屏幕截图与触摸注入**能力的长期方案。

---

## 现状速览

| 项 | 状态 |
|---|---|
| 目标 Android | **12** |
| 目标架构 | **ARM64**（构建主机 x86_64，标准交叉编译） |
| 架构方案 | **B：native daemon**（`autod` 常驻进程） |
| 协议层 | ✅ 已实现并在主机验证通过 |
| 平台 API 层 | ⚠️ 未在真实 AOSP 环境编译验证 |
| 触控实现 | ⚠️ **待决**：Android 12 的 `injectInputEvent` 是 Java-only |

**先读 [`SUMMARY.md`](SUMMARY.md)** —— 完整的选型过程、四个关键发现与待决事项。

---

## 核心结论

> 「内核模块 + 隐蔽 + 绕过 FLAG_SECURE」凑不出可行方案；
> 「系统内置服务 + 对外提供截图触控」完全成立。

因为截图（`SurfaceFlinger::captureDisplay`）和触控（`InputManagerService.injectInputEvent`）
的实现本来就在 framework 层，不在内核层。内核方案等于在拿不到数据的地方重新实现一遍。

---

## 目录结构

```
AutoSnapshotAndroid/
├── README.md                  ← 本文件：项目索引
├── SUMMARY.md                 ← 方案总结与决策记录（先读这个）
│
├── docs/                      ← 专题文档
│   ├── 01-selection.md           方案选型
│   ├── 02-architecture.md        架构设计
│   ├── 03-version-matrix.md      各版本 API 差异
│   ├── 04-hardware.md            硬件与构建环境
│   ├── 05-latency-and-touch.md   延迟与触控能力
│   ├── 06-constraints.md         约束与风险
│   ├── 07-environment.md         ⭐ 本机实际环境（磁盘/镜像源/容器）
│   └── 08-official-implementations.md  官方/开源实现对照 ★
│
├── tools/                     ← 环境与构建脚本
│   ├── setup-host.sh             环境初始化
│   └── repo-sync.sh              AOSP 源码同步
│
├── aosp/                      ← AOSP 源码树（~85 GB，已同步中）
│
└── dev/                       ← 四条轨道（01–03 是开发轨道，04 是验证环境）
    ├── 01-ndk-prototype/         阶段 0：纯 NDK 快速验证（当天可跑）
    ├── 02-native-daemon/         阶段 1–2：AOSP native daemon（主体代码）
    ├── 03-java-service/          阶段 3：Java 系统服务（长期形态）
    └── 04-emulator/              QEMU 验证环境：开发机跑 arm64 原版、Windows 跑自制 ROM
```

---

## 环境已就绪

| 项 | 配置 |
|---|---|
| 数据盘 | 458 GiB 挂载在项目目录（`/dev/sda1`） |
| 内存 / Swap | 31 GiB / 35 GiB |
| 构建环境 | Docker 容器 `autod-builder`（Ubuntu 22.04 + JDK 11） |
| 镜像源 | **全部国内**（清华 TUNA + daocloud） |

详见 [`docs/07-environment.md`](docs/07-environment.md)。

```bash
# 进入构建环境
docker exec -it autod-builder bash

# 查看源码同步进度
tail -f /var/log/aosp-sync.log
```

---

## 四条轨道怎么选

```
                    ┌─────────────────────────────┐
                    │  想最快看到效果？            │
                    └──────────┬──────────────────┘
                               │
              ┌────────────────┴────────────────┐
              ▼                                 ▼
    01-ndk-prototype                    02-native-daemon
    纯 NDK，不需要 AOSP 树               需要 repo sync (~85 GB)
    截图 exec screencap                 截图走 SurfaceFlinger Binder
    触控 /dev/uinput                    触控待定
    当天可跑，截图慢                     性能好，是长期主线
              │                                 │
              └────────────────┬────────────────┘
                               ▼
                      03-java-service
                  Android 12 上触控只能走 Java
                  长期形态：native 截图 + Java 注入
```

| 轨道 | 前置条件 | 出结果时间 | 定位 |
|---|---|---|---|
| `01-ndk-prototype` | NDK + 一台 root 安卓机 | **当天** | 验证思路可行性 |
| `02-native-daemon` | AOSP 源码树（~85 GB） | 首次 1 小时，之后分钟级 | 长期主线 |
| `03-java-service` | AOSP 源码树 + 系统签名 | 数天 | 补 Android 12 的触控缺口 |
| `04-emulator` | 上面两个 + `out/` ~100 GB | 首次编译数小时，之后分钟级 | **免真机**验证 02：开发机跑 arm64 原版安卓，Windows 跑编好的 ROM |

> `04-emulator` 不是产品形态，是 02 的**验证基础设施**：`02` 的「等一台 root 的 ARM64 真机」
> 这个前置条件，用它可以先绕过去。详见 [`dev/04-emulator/README.md`](dev/04-emulator/README.md)。

---

## 快速开始

### 只想先看看能不能跑通

```bash
cd dev/01-ndk-prototype
cat README.md
```

不需要 AOSP，不需要大硬盘，当天出结果。

### 直接上主线

```bash
# 一次性（~85 GB）
repo init -u https://android.googlesource.com/platform/manifest -b android-12.0.0_r34
repo sync -c --depth=1 --no-tags -j8

# 每次改代码（首次 30–90 分钟，之后几分钟）
source build/envsetup.sh
lunch aosp_arm64-userdebug
m autod

# 部署
adb push $ANDROID_PRODUCT_OUT/system/bin/autod /data/local/tmp/
```

详见 `dev/02-native-daemon/README.md`。

---

## 必须先知道的三件事

**1. 不需要全量编译系统镜像。** `m autod` 只编模块。只有要装进 `/system/bin/` 并开机自启时才涉及镜像，而那也能用 Magisk 绕开。

**2. 唯一躲不掉的是 `repo sync`。** 因为 `autod` 用了 `libgui` 这类平台私有库，头文件只存在于 AOSP 源码里。除非走纯 NDK 轨道。

**3. Android 12 上 native 进程注入不了触摸。** `IInputManager` 在 12 上是 Java-only AIDL。必须改用 `/dev/uinput` 或拆 Java 服务。详见 `docs/06-constraints.md`。

---

## 能力边界

**做不到**：

- **`FLAG_SECURE` 内容抓不到** —— 受保护图层不会被合成进 CPU 可读缓冲区，这是 SurfaceFlinger 的设计。要改必须打 framework 补丁。
- **本方案不提供隐蔽性** —— `autod` 会出现在 `dumpsys` / `service list`，SELinux domain 和 socket 标签都可枚举。
- **非 root 真机上触控受限** —— `/dev/uinput` 通常只对 `system` / root 开放。

**已知工程限制**：

- 平台 API 跨版本不稳定，升级 Android 必须重对源码
- `autod` 目前串行处理请求，高并发需线程池
- 客户端必须在设备上运行（`SEQPACKET` 和 `SCM_RIGHTS` 过不了 `adb forward`）

---

## 参考实现出处

- 截图：[`frameworks/base/cmds/screencap/screencap.cpp`](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/cmds/screencap/screencap.cpp)
- 触控：`frameworks/base/cmds/input/src/com/android/commands/input/Input.java` + `core/java/android/hardware/input/IInputManager.aidl`
- 版本限制：[Restricted screen reading](https://source.android.com/docs/core/permissions/restricted-screen-reading)
- GKI 模块约束：[Android 内核 ABI 监控](https://source.android.com/docs/core/architecture/kernel/abi-monitor)
