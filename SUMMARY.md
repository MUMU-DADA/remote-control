# 方案总结与决策记录

> 本文档记录本项目的完整技术选型过程、关键结论与待决事项。
> 最后更新：本次方案讨论结束。

---

## 一、项目目标

在 Android 系统层对外提供两种能力，长期方案：

1. **屏幕截图** —— 获取屏幕画面
2. **触摸注入** —— 模拟点击、滑动、多点触控

对外以 Unix domain socket / HTTP 接口暴露，供外部程序调用。

---

## 二、核心结论

> **「内核模块 + 隐蔽 + 绕过 FLAG_SECURE」这三件事凑不出一个可行方案；
> 但「系统内置服务 + 对外提供截图触控」完全成立，且工程上干净。**

根本原因：**这两项能力的实现本来就在 framework 层，不在内核层。**

| 能力 | 实际实现位置 | 内核方案能否替代 |
|---|---|---|
| 截图 | `SurfaceFlinger::captureDisplay()` | ❌ 不能 |
| 触控 | `InputManagerService.injectInputEvent()` | ⚠️ 能，但要绕道 |

---

## 三、方案演进

### 3.1 原始设想：内核模块（LKM）

最初的方案是写内核模块，理由是"应用层无法检测其存在"。逐条核对后的结论：

| 原始说法 | 核对结果 |
|---|---|
| 内核模块隐蔽性最高，推荐核心方案 | ❌ **技术前提错误** |
| 屏幕捕获读 `/dev/graphics/fb0` | ❌ 现代内核多数已移除 `CONFIG_FB`；HWC 硬件合成下 fb0 是黑屏/残缺 |
| 内核模块可绕过 FLAG_SECURE | ❌ FLAG_SECURE 是 SurfaceFlinger 图层属性，受保护图层**不会被合成进任何 CPU 可读缓冲区** |
| `kp7742/TouchSimulation` 是内核模块方案 | ❌ 分类错误，是用户态程序写 `/dev/uinput` |
| `minicap` 是内核模块方案 | ❌ 分类错误，是用户态 socket 服务 |
| GKI 设备可通过 KernelSU 动态加载自定义 `.ko` | ⚠️ GKI 用 `TRIM_UNUSED_KSYMS` + `UNUSED_KSYMS_WHITELIST` 限制导出符号，另有 CRC 校验与 KMI 冻结 |
| 低版本安卓可绕开 GKI | ⚠️ GKI **只约束内核模块**，用户态进程完全不受影响 |

**结论：内核路线的致命伤不是难度，而是拿不到数据。**

### 3.2 三种可行落点

| 方案 | 说明 | 评价 |
|---|---|---|
| A. 特权 APK（`priv-app` + 平台签名） | 用 SDK 层 API，不改 framework | 迭代最快，`adb install` 即可 |
| **B. 独立 native daemon** | C++ 常驻进程，直接 Binder 调 SF / IMS | ✅ **选定**，长期形态 |
| C. system_server 内新增服务 | 权限最全（同进程内绕过权限检查） | ❌ system_server 崩 = 整机重启，不选 |

### 3.3 最终架构：方案 B

```
        外部调用方
             │ Unix domain socket (SOCK_SEQPACKET)
             │ + SCM_RIGHTS 传 memfd
             ▼
   ┌──────────────────────────┐
   │  autod (native daemon)   │
   │  socket_server / capture │
   │  inject                  │
   │  SELinux domain: autod   │
   │  UID: system             │
   └────┬──────────────┬──────┘
        │ Binder       │ Binder
        ▼              ▼
  SurfaceFlinger  InputManagerService
  captureDisplay   injectInputEvent
```

---

## 四、四个关键发现

### 发现 1：GKI 与用户态方案无关

GKI 只约束**内核模块**。`autod` 是纯用户态进程，通过 Binder 调用系统服务，与 GKI 完全无关。

→ **选 Android 版本不应为了躲 GKI**，而应基于 API 成熟度、文档完善度、硬件可获得性。

### 发现 2：Binder 装不下一帧画面

Binder 单次事务上限约 1 MB，而 1080p RGBA_8888 是 8 MB。

→ 截图数据走 `memfd` + `SCM_RIGHTS` 传 fd，客户端 `mmap` 零拷贝读取。

### 发现 3：`SCM_RIGHTS` 过不了 `adb forward`

`adb forward` 是 TCP 通道，文件描述符无法传递。

→ **客户端必须在设备上运行**，因此提供 C++ 版 `autodctl`，Python 版仅用于协议文档与主机侧 mock 联调。

### 发现 4（最重要）：Android 12 的 `injectInputEvent` 是 Java-only

核对 [`IInputManager.aidl` @ android12-release](https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/android12-release/core/java/android/hardware/input/IInputManager.aidl)：

```java
package android.hardware.input;
import android.view.InputEvent;        // ← 纯 Java 类
import android.graphics.Rect;
...
boolean injectInputEvent(in InputEvent ev, int mode);
```

- 没有 `@VintfStability`，没有 cpp/ndk backend 标注
- import 的全是 Java 类型
- 它位于 `frameworks/base/core/java/`，是纯 Java AIDL

对比：截图用的 `ISurfaceComposer` **是** native AIDL（在 `frameworks/native/libs/gui/`），C++ 可直连。

→ **native daemon 能截图，但注入不了触摸。** 这是方案 B 在 Android 12 上的硬约束，必须分拆解决。详见 `docs/01-selection.md` 的约束部分。

---

## 五、交付物与验证状态

### 已交付

- `dev/02-native-daemon/` —— native daemon 主体（截图 3 后端 / 触控 3 后端，均编译期可选）
- `docs/` —— 8 份专题文档
- `dev/01-ndk-prototype/` `dev/03-java-service/` —— 另外两条轨道的方案
- `tools/setup-host.sh` `tools/repo-sync.sh` —— 环境与源码同步脚本

### 已实际验证（真实运行，非 mock）

| 项目 | 结果 |
|---|---|
| `Request` / `Reply` 结构体布局（C++ ↔ Python） | 44 / 40 字节，两侧一致 ✓ |
| `info` / `capture` / `tap` / `swipe` 协议往返 | 全部通过 ✓ |
| `memfd` + `SCM_RIGHTS` 传 fd | 成功 ✓ |
| PNG 输出 | 320×240，全部 chunk CRC 校验通过 ✓ |
| **uinput 触控注入** | **31 项检查通过** ✓ |
| **端到端集成（真实 Dispatcher）** | **32 项检查通过** ✓ |

**测试都是真设备验证**：真的创建虚拟触摸屏、注入事件、从 `/dev/input/eventN`
读回逐条校验，走完整内核 input 子系统路径。集成测试验证的链路：

```
客户端 ──SOCK_SEQPACKET──> SocketServer ──> Dispatcher ──┬──> Capture(桩)
                                                          │      └─ memfd ──SCM_RIGHTS──> mmap 校验像素
                                                          └──> Injector ──> /dev/uinput ──> 内核
```

其中帧通道校验了 8,294,400 字节的完整帧经 `SCM_RIGHTS` 到达客户端后可 `mmap`
且像素值正确。

```bash
cd dev/02-native-daemon/tests && sudo make run
```

### 开发过程中修掉的两个真实 bug

**1. `SocketServer` 缺移动构造。** 禁用了拷贝构造但没提供移动构造，
工厂函数里的 `return s;` 编译不过。这段代码在 AOSP 构建时也会失败——
是集成测试提前暴露了它。

**2. 测试自身的求值顺序缺陷。** `Check(LastValueOf(...,&x) && x == 540, "...", x)`
—— C++ 实参求值顺序未指定，打印的是旧值。会造成「断言通过但诊断信息骗人」，
掩盖真实故障。

**过程中修掉一个真实 bug**：`Reply` 里 `uint64_t dataSize` 紧跟 `uint32_t` 序列时，编译器会插入 4 字节隐式填充（C++ 侧 40 字节 vs Python 侧 36 字节）。已改为显式 `reserved` 字段并用 `static_assert` / `assert` 在两侧锁死。

### 尚未验证

`capture.cpp` / `inject.cpp` 里的平台 API 调用**未在真实 AOSP 环境编译或运行过**。需要 AOSP 源码树就绪后才能验证。

---

## 六、阻塞项

| 阻塞 | 说明 | 绕法 |
|---|---|---|
| **需要 AOSP 源码树** | 平台私有头文件不在 NDK 里 | `repo sync --depth=1`，~85 GB，一次性 |
| **需要 SELinux domain** | 新服务无 domain 则 init 拒绝启动 | 阶段 1 跳过，以 root 身份跑；阶段 2 用 `audit2allow` 补 |
| **Android 12 注入是 Java-only** | native 调不了 `IInputManager` | 改用 `/dev/uinput`，或拆 Java 系统服务 |
| **当前工作机编不了 AOSP** | 3.8 GB 内存 / 17 GB 磁盘 | 需另配构建机 |

**注意：不需要全量编译系统镜像。** `m autod` 只编模块，日常迭代几分钟。只有要装进 `/system/bin/` 并开机自启时才涉及系统镜像——而那也可以用 Magisk 绕开。

---

## 七、已确认的决策

| 项目 | 决定 |
|---|---|
| 目标 Android 版本 | **Android 12** |
| 目标架构 | **ARM64**（构建主机仍是 x86_64，标准交叉编译） |
| 架构方案 | 方案 B：native daemon |
| 触控实现 | 待定：`/dev/uinput`  vs  Java 系统服务 |
| 截图实现 | SurfaceFlinger `captureDisplay` |

---

## 八、待决事项

1. **触控路径二选一**（见 `dev/01-ndk-prototype/` 与 `dev/03-java-service/`）
2. **是否先做纯 NDK 原型**快速验证思路
3. **构建机采购**（见 `docs/04-environment.md`）
4. **测试设备**：Magisk root 的 ARM64 真机 vs Cuttlefish

---

## 九、文档索引

**接口以 `docs/api/` 为准** —— 那是唯一权威版本。

### 接口文档

| 文档 | 内容 |
|---|---|
| `docs/api/README.md` | 索引、5 分钟上手、三条传输的取舍、坐标约定 |
| `docs/api/01-http.md` | HTTP/JSON API —— 29 个端点逐个的参数与响应 |
| `docs/api/02-websocket.md` | 画面流 / 触控流 / 日志流 |
| `docs/api/03-socket.md` | Unix socket 二进制协议（32 条命令） |
| `docs/api/04-config.md` | 配置文件、命令行、鉴权、supervisor、部署 |
| `docs/api/05-errors.md` | 状态码与错误处理 |
| `docs/api/06-debugging.md` | 环境变量、强制回退路径、验证方法 |

### 设计文档

| 文档 | 内容 |
|---|---|
| `docs/01-selection.md` | **选型与结论**：为什么不用内核、三种落点对比、关键约束、官方实现对照 |
| `docs/02-architecture.md` | **架构**：组件、协议、数据流 |
| `docs/03-reference.md` | **技术参考**：各 Android 版本 API 差异、延迟拆解、触控能力矩阵 |
| `docs/04-environment.md` | **环境与构建**：硬件预算、镜像源、构建流程、本机环境 |
| `docs/05-design-notes.md` | **设计记录**：为什么这么做、踩过的坑（含 20 条速查表）、实测数据 |

### 代码

| 路径 | 内容 |
|---|---|
| `dev/01-ndk-prototype/` | 阶段 0：纯 NDK 快速验证 |
| `dev/02-native-daemon/` | 阶段 1–2：AOSP native daemon（主体代码） |
| `dev/03-java-service/` | 阶段 3：Java 系统服务（长期形态） |
| `dev/04-emulator/` | 模拟器方向（**独立线，见该目录的 README**） |
| `dev/05-controller-app/` | 上位应用（只做服务管理） |

### 工具

| 路径 | 内容 |
|---|---|
| `tools/check-api-docs.py` | 文档一致性检查 —— 改了协议跑一遍 |
| `tools/lan-up.sh` | 一键部署到模拟器并暴露到局域网 |
| `tools/build-ndk.sh` | NDK 构建（不需要 AOSP 源码树） |
