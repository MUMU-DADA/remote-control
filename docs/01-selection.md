# 选型与结论

> 为什么不用内核模块、三种可行落点的对比、最终选定的方案，以及动手前必须读完的硬约束清单。
>
> 第 6 节是「官方与开源实现对照」：能用现成实现的不自己写，用了哪个、为什么。

---

## 1. 需求

在 Android 系统层对外提供两种能力：

- **屏幕截图**：获取屏幕画面
- **触摸注入**：模拟点击、滑动、多点触控

对外以 HTTP/JSON、WebSocket、Unix socket 三种传输暴露，接口细节见 `api/README.md`。

---

## 2. 内核路线为什么不行

内核模块（LKM）是最初的设想，理由是"应用层无法检测其存在"。逐条核对后否定了这条路。

### 2.1 截图：内核拿不到数据

原始设想是读 `/dev/graphics/fb0`。问题有两层：

**第一层：fb0 在现代 Android 上基本不存在。** Android 已从 framebuffer 迁移到 DRM/KMS，`CONFIG_FB` 在多数新内核里已被移除。

**第二层（更致命）：即使 fb0 存在，也拿不到真实画面。** 现代 Android 用 HWC（硬件合成器）把各个图层直接叠加后推给屏幕，合成过程发生在显示控制器里，不经过 CPU 可访问的 framebuffer。要截图，必须让 SurfaceFlinger 额外做一次 **GPU 合成**到捕获缓冲区。

这就是为什么所有人都走 `SurfaceFlinger::captureDisplay()`——不是在绕远路，而是**唯一的路径**。

### 2.2 FLAG_SECURE：内核层绕不过

`FLAG_SECURE` 是 SurfaceFlinger 的**图层属性**（`setSecure(true)`）。被标记的图层：

- 不会被合成进普通合成路径
- 受保护内容走 protected buffer 路径，**CPU 不可读**
- 不会出现在任何可捕获的缓冲区里

**这是设计目标，不是权限问题。** 给 `autod` 再高的权限也拿不到。

业界常见的绕过方式是框架层 hook（如 [LSPosed/DisableFlagSecure](https://github.com/LSPosed/DisableFlagSecure)）——这本身就说明它在 framework 层而非 kernel 层。真要绕过得改 system 分区里的 SurfaceFlinger 或 hook composer HAL。**本项目不做这件事。**

### 2.3 触控：内核能做，但是绕道

触控这条路内核**确实能做**——`uinput` 是内核自带的模块，写 `/dev/uinput` 就能创建虚拟触摸屏。AOSP 甚至规定了虚拟触摸设备要导出虚拟按键映射文件（[Touch devices](https://source.android.google.cn/docs/core/interaction/input/touch-devices)）。

但这是**用户态接口**，不需要写内核模块。绕内核反而更简单。

### 2.4 GKI：真正的技术障碍

Android 11+ 的 GKI 项目对内核模块有严格约束：

- `TRIM_UNUSED_KSYMS=y` + `UNUSED_KSYMS_WHITELIST` 限制导出符号（[Android 内核 ABI 监控](https://source.android.google.cn/docs/core/architecture/kernel/abi-monitor)）
- `CONFIG_MODVERSIONS` 的 CRC 校验
- KMI 冻结
- vendor 模块需签入 `system_dlkm`，过 dm-verity / AVB

KernelSU 的 LKM 模式能工作，是因为它**针对具体内核版本专门编译**。新写一个 hook 图形栈的模块，需要完全匹配的源码 + config + 工具链。

### 2.5 结论

| 原始说法 | 核对结果 |
|---|---|
| 内核模块隐蔽性最高，推荐核心方案 | ❌ 技术前提错误 |
| 屏幕捕获读 `/dev/graphics/fb0` | ❌ 内核多数已移除 `CONFIG_FB`；HWC 合成下拿不到画面 |
| 内核模块可绕过 FLAG_SECURE | ❌ 受保护图层不进 CPU 可读缓冲区 |
| `kp7742/TouchSimulation` 是内核模块 | ❌ 是用户态程序写 `/dev/uinput` |
| `minicap` 是内核模块 | ❌ 是用户态 socket 服务 |
| GKI 设备可 insmod 自定义 `.ko` | ⚠️ 符号白名单 + CRC 校验 + KMI 冻结 |
| 低版本安卓可绕开 GKI | ⚠️ GKI 只约束内核模块，与用户态方案无关 |

> **内核路线的致命伤不是难度，而是拿不到数据。**

---

## 3. 一个常见误判：低版本安卓

有人会想"用 Android 9/10 绕开 GKI"。这个推理有漏洞：

**GKI 只约束内核模块。** 用户态进程通过 Binder 调用系统服务，完全不受 GKI 影响。

所以选低版本唯一重新打开的是**内核模块**路径——而内核路径的数据问题（拿不到真实合成画面）依然存在。

**选 Android 版本应该基于：API 成熟度、文档完善度、硬件可获得性、安全补丁状态。**

本项目选定 **Android 12**：

| 版本 | 评价 |
|---|---|
| Android 11 | 可用，截图 API 已有，个别参数更老 |
| **Android 12** | ✅ **选定**。截图 API 成熟，SELinux 文档齐全 |
| Android 13 | 同样合适，`IInputManager` 情况未变 |
| Android 14 | 截图 API 有一次重构 |
| Android 15+ | 引入 [Restricted screen reading](https://source.android.com/docs/core/permissions/restricted-screen-reading)，屏幕捕获收敛到特定角色 |
| Android 9 / 10 | 截图 API 形态不同，需多写适配层 |

---

## 4. 三种可行落点

### A. 特权 APK（`priv-app` + 平台签名）

| 项 | 说明 |
|---|---|
| 实现 | Java，用 SDK 层 API |
| 权限 | `INJECT_EVENTS`（signature）、`CAPTURE_VIDEO_OUTPUT`（signature\|privileged） |
| 改动 | **不需要改 framework 一行代码** |
| 部署 | 编进 `/system/priv-app/`，用平台密钥签名 |
| 优点 | 迭代最快，`adb install` 即可试<br>不受 GKI 影响<br>不触发 AVB 校验失败 |
| 缺点 | 受 SDK API 限制，拿不到底层控制<br>性能不如 native |

### B. 独立 native daemon ← **本项目选定**

| 项 | 说明 |
|---|---|
| 实现 | C++ 常驻进程，`init.rc` 拉起 |
| 接口 | 截图直连 `ISurfaceComposer`；触控走 `/dev/uinput` |
| 需要 | 自己的 SELinux domain、`seclabel`、allow 规则、`file_contexts` 标签 |
| 优点 | **隔离**：崩了不带走 system_server<br>**可独立更新**<br>**性能最好**：可走 gralloc/dma-buf 零拷贝 |
| 缺点 | 需要 AOSP 树编译（平台私有头文件不在 NDK 里）<br>需要写 sepolicy<br>**Android 12 上注入只能走 `/dev/uinput`**（见约束 1） |

本质上是 `screencap` 和 `input` 两个命令的"常驻服务化"版本。

### C. system_server 内新增 Binder 服务

| 项 | 说明 |
|---|---|
| 实现 | 写 `IAutoService.aidl`，实现后 `ServiceManager.addService()` |
| 优点 | 权限最全（同进程内，无需权限检查）<br>可直接调内部 API |
| 缺点 | **system_server 崩 = 整机重启**<br>每次改动要重编 system image<br>启动顺序有讲究 |
| 评价 | ❌ 除非确实需要 hook WMS 的截图流程，否则不选 |

---

## 5. 决策

**选方案 B（native daemon），理由：**

1. 截图和触控的实现都在 framework 层，native 能直连 Binder
2. 隔离性好，不会因服务崩溃影响整机
3. 可独立更新，不必每次重刷系统
4. 是 `screencap` / `input` 命令的自然服务化形态，有权威参考实现
5. 长期形态，不需要推倒重来

**已知代价**：需要 AOSP 源码树；需要写 sepolicy；Android 12 上触控需绕道走 `/dev/uinput`。

---

## 6. 官方与开源实现对照

> 原则：**能用官方或成熟开源实现的，不自己写。**

| 组件 | 现成实现 | 我们的选择 | 状态 |
|---|---|---|---|
| 截图（设备命令） | AOSP `screencap` | **直接用** | ✅ |
| 截图（SF 直连） | AOSP `screencap.cpp` | **对照实现** | ✅ |
| 触控（uinput 路径） | AOSP `EvdevInjector` | 自己写（有原因，见 6.3） | ⚠️ |
| 触控（Binder 路径） | AOSP `virtual_touchpad` | **直接用**（`autod_vtp` 目标） | ✅ 需加进产品包 |
| SELinux 策略 | AOSP `virtual_touchpad.te` | **照抄模板** | ✅ |
| init 服务框架 | AOSP `init.rc` / `android_get_control_socket` | **直接用** | ✅ |
| 日志 | AOSP `liblog` / NDK `android/log.h` | **直接用** | ✅ |
| Binder 传输 | AOSP `libbinder` | **直接用** | ✅ |
| 帧数据通道 | Linux `memfd` + `SCM_RIGHTS` | **直接用** | ✅ |
| socket 协议 | 无官方对应 | 自定义 | — |

### 6.1 截图：AOSP `screencap`

AOSP 自带 `frameworks/base/cmds/screencap/`，命令 `/system/bin/screencap`。我们两条后端都源自它：

| 路径 | 做法 |
|---|---|
| `capture_screencap.cpp` | **直接 exec `/system/bin/screencap`**，解析它的 stdout 原始像素流 |
| `capture_surfaceflinger.cpp` | **对照 `screencap.cpp` 实现**，走 `ScreenshotClient::captureDisplay` |

为什么 SF 路径不直接复用 `screencap` 命令：exec 一次 100–300 ms，SF 直连 20–35 ms（实测 120 ms vs 23 ms，见 `03-reference.md`）；但 SF 路径必须编进 AOSP 树（依赖 `libgui`），所以 NDK 路径保留了 exec 方案。

**两条路径都源自官方实现，没有自己发明抓屏方式。**

### 6.2 触控：AOSP `virtual_touchpad` ★

```
frameworks/native/services/vr/virtual_touchpad/
├── EvdevInjector.{h,cpp}                通用 uinput 封装
├── VirtualTouchpadEvdev.{h,cpp}         造设备 + 事件合成
├── VirtualTouchpad{Service,Client}.cpp  Binder 服务 / 客户端
├── main.cpp + virtual_touchpad.rc       /system/bin/virtual_touchpad
├── idc/vr-virtual-touchpad-0.idc        声明 touch.deviceType = touchScreen
└── include/VirtualTouchpad{,Client}.h   ★ 导出
```

**这不是触摸板，是触摸屏。** `.idc` 里明确写着：

```
device.internal = 1
touch.deviceType = touchScreen
```

设备能力：`INPUT_PROP_DIRECT` + `BTN_TOUCH` + 多指 `ABS_MT_*`，坐标范围 `0x10000`。

我们的 `inject_vtp.cpp` 走官方 Binder 服务：

```cpp
#include <VirtualTouchpadClient.h>       // 从 export_include_dirs 拿
auto tp = android::dvr::VirtualTouchpadClient::Create();
tp->Attach();
tp->Touch(slot, x_norm, y_norm, pressure);   // 坐标 [0.0, 1.0)
```

最大的好处是权限模型简化：

| | 自己写 uinput | 走官方服务 |
|---|---|---|
| `autod` 需要的 POSIX 权限 | `uhid` 组（`/dev/uinput` 是 `0660 uhid:uhid`） | 无 |
| `autod` 需要的 SELinux 权限 | `uhid_device:chr_file { w_file_perms ioctl }` | `virtual_touchpad_service:service_manager find` |
| 需维护的 uinput 代码 | ~350 行 | 0 |

代价：

| 限制 | 说明 |
|---|---|
| **只有 2 个触控槽位** | `VirtualTouchpadEvdev::kTouchpads = 2`，做不了 3 指以上手势 |
| 不在默认产品包里 | 是 VR 专用，需要 `PRODUCT_PACKAGES += virtual_touchpad`；不加的话 `attach()` 会失败（`status=-2`） |
| 多一次 Binder 往返 | 约 0.3–1 ms，仍远快于 adb 的 30–80 ms |

**三个注入后端并存**：

```
inject_uinput.cpp   自己写：10 槽位、完整协议 B、主机可测、NDK 可用   ← 默认
inject_vtp.cpp      官方：2 槽位、无需 uinput 权限
inject_binder.cpp   IInputManager：不可枚举，但 Android 12 上加载会失败
```

由 `Android.bp` 选一个链接（同名工厂函数不能共存）：

| 目标 | 触控后端 | 适用 |
|---|---|---|
| `m autod` | uinput | 默认；功能最全 |
| `m autod_vtp` | 官方服务 | 想要最小权限面时 |
| `m autod_binder` | IInputManager | Android 13+ 或自改 AIDL |

**后端抽象在这里体现价值：切换时上层代码一行没改。**

### 6.3 触控：为什么没有直接用 `EvdevInjector`

`EvdevInjector` 是 AOSP 的通用 uinput 封装（配 property/key/abs/rel + 发事件），我们的 `inject_uinput.cpp` 实质上是它的重新实现。**没有直接用它有两个具体原因：**

1. **头文件没导出。** `libvirtualtouchpad` 的 `export_include_dirs` 只有 `include/`，而 `EvdevInjector.h` 在模块根目录。外部模块 include 不到，要用就得往 include path 里塞 `..`——而 Soong 明确禁止路径逃逸（`init_rc` 上实测报 `Path is outside directory`）。

2. **会丢掉主机可测性。** `EvdevInjector` 依赖 `android-base/unique_fd.h` 和 `utils/String8.h`，只能在 AOSP 树内编译。而我们的版本是纯 Linux syscall，在开发机上能跑 35 项真实设备测试（真的创建虚拟触摸屏、注入、读回校验）。

**结论：这里自己写是有依据的取舍，不是重复造轮子。** 但官方版的能力（2 槽位）覆盖不了我们的需求（多点触控），这也是保留自研版本的理由之一。

### 6.4 评估过但没采用的方案

**scrcpy** —— 开源、成熟，做「投屏 + 控制」的事实标准。没采用：它以 `shell` 身份跑（adb 推 server 到 `/data/local/tmp`）、生命周期绑定 adb 连接、对外是需配套客户端的私有协议、注入优先走 Java `InputManager`——部署模型和「开机自启的系统级常驻服务」不匹配。它的**原理**我们采用了（VirtualDisplay + MediaCodec 那条路没走，因为要的是单帧截图而非视频流）。

**minicap / minitouch** —— STF 的设备端组件。没采用：minicap 读 SurfaceFlinger，原理与我们的 SF 后端一致；minitouch 直接写 `/dev/input/eventX`（**已存在的**设备节点），而我们要创建**新的**虚拟设备——两者不是一回事；两者都需要 root 且部署方式与 scrcpy 类似。

**Kbox-patches** —— 华为鲲鹏的云手机方案（Apache-2.0，含内核补丁）。没采用：它的补丁用于让 AOSP 在容器里启动（SELinux/vintf/seccomp/gralloc），不是做截图触控的，而且要配套鲲鹏硬件。

### 6.5 明确自己写的部分

| 组件 | 为什么自己写 |
|---|---|
| socket 协议（`protocol.h`） | 无官方对应；需求明确且小（44/40 字节定长结构） |
| `socket_server.cpp` | AOSP 无现成的 SEQPACKET + `SCM_RIGHTS` 服务端 |
| `dispatch.cpp` | 业务逻辑 |
| `inject_uinput.cpp` | 见 6.3（导出限制 + 主机可测性 + 槽位数） |
| `http_server.cpp` / `rest_api.cpp` / `websocket.cpp` | 只用一个 HTTP/1.1 子集，不值得引第三方库 |
| `capture_stub.cpp` | 测试专用，无对应物 |

---

## 7. 硬约束与风险

### 约束 1（最重要）· Android 12 的 `injectInputEvent` 是 Java-only

**已在本地 synced 的 Android 12 源码树中实地核实**（不再是网上的推断）：

```bash
# 全树只有这一个 IInputManager 定义，且没有任何 backend / vintfstability / ndk 标注
find aosp/frameworks -name "IInputManager*"
# → aosp/frameworks/base/core/java/android/hardware/input/IInputManager.aidl
grep -inE "backend|vintfstability|ndk" IInputManager.aidl      # → 无输出
```

```java
package android.hardware.input;

import android.view.InputEvent;        // ← 纯 Java 类
import android.graphics.Rect;
import android.os.IBinder;
...
interface IInputManager {
    // Injects an input event into the system. To inject into windows owned by other
    // applications, the caller must have the INJECT_EVENTS permission.
    @UnsupportedAppUsage
    boolean injectInputEvent(in InputEvent ev, int mode);
    ...
}
```

三个关键特征：

1. 位于 `frameworks/base/core/java/`，是**纯 Java AIDL**
2. **没有 `@VintfStability`，没有 cpp / ndk backend 标注**
3. `import` 的全是 Java 类型（`android.view.InputEvent`、`android.graphics.Rect`）

**原生侧也没有替代路径**（已核实）。有人会想"那走 `IInputFlinger` 呢？它是原生 AIDL"。**不行**：

```bash
$ cat frameworks/native/libs/input/android/os/IInputFlinger.aidl
interface IInputFlinger {
    oneway void setInputWindows(in InputWindowInfo[] inputHandles,
            in @nullable ISetInputWindowsListener setInputWindowsListener);
    InputChannel createInputChannel(in @utf8InCpp String name);
    void removeInputChannel(in IBinder connectionToken);
    oneway void setFocusedWindow(in FocusRequest request);
}
```

只有四个方法，**没有任何注入能力**。而且 Android 12 的 InputFlinger 还跑在 system_server 进程内：

```
frameworks/native/services/inputflinger/Android.bp
// TODO(b/23084678): Move inputflinger to its own process and mark it hidden
```

（拆成独立进程是后来的版本才做的。）

**结论：Android 12 上不存在任何 native 的输入注入路径。**

> - `IInputManager` 是 Java-only AIDL
> - `IInputFlinger` 是原生 AIDL，但没有注入方法
> - 全树搜索只有一份 `IInputManager` 定义

因此 `inject_uinput.cpp`（写 `/dev/uinput`）是 Android 12 上**唯一可行**的方案。按键注入同理——`keyboard.cpp` 也是用 uinput 建虚拟键盘设备。

**对比：截图没问题。** 截图用的 `ISurfaceComposer` **是** native AIDL，位于 `frameworks/native/libs/gui/`，C++ 可以直连。

**影响：native daemon 能截图，但注入不了触摸。**

三个解法：

#### 解法 A：触控改走 `/dev/uinput` ← 已采用

| 项 | 说明 |
|---|---|
| 实现 | 纯 syscall，不依赖任何平台 API |
| 结构 | `autod` 保持 native 单进程：截图走 Binder，触控走 uinput |
| 优点 | 改动最小，立即可用；不依赖 AOSP 树也能编 |
| 缺点 | **会创建一个可枚举的输入设备** |

**实现要点**（完整 ioctl 序列见 `03-reference.md` 第 5.1 节）——最容易漏的一条：

```cpp
// 除了 ABS_MT_* 和 BTN_TOUCH，还要设 INPUT_PROP_DIRECT，
// 否则 InputFlinger 不会把它识别成直接触摸屏
ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
```

事件注入按 **multitouch protocol B** 顺序写，最后 `input_sync()`。

**权限要求**：`/dev/uinput` 在 Android 上是 `0660 uhid:uhid`，且在 SELinux 里只有少数 domain 能访问。**通常需要 root 或 `system` UID + `uhid` 组。**

#### 解法 B：拆一个 Java 系统服务 ← 长期形态

```
autod (C++)          →  截图（SurfaceFlinger Binder）✅
AutodInputService    →  注入（InputManager.injectInputEvent）✅
     (Java)
两者用 Binder 或 socket 通信
```

| 优点 | 缺点 |
|---|---|
| 架构干净，各用各自能用的 API | 多一个进程 |
| 不用创建额外的输入设备 | Java 服务要进 `priv-app` 或 system_server |
| | 需要系统签名 |

详见 `../dev/03-java-service/`。

#### 解法 C：改 AOSP 给 AIDL 加 cpp backend

理论上可行，但 `InputEvent` 是 Java 类，要换成 `libinput` 里的 native parcelable，会牵动整个 framework。**风险高，不建议。**

**当前状态：A 已落地并实测通过，B 保留为长期形态。**

### 约束 2 · `FLAG_SECURE` 内容抓不到

见 2.2 节的原理。**这是设计目标，不是权限问题——无解。** 给 `autod` 再高的权限也拿不到，本项目不改 framework。

### 约束 3 · 本方案不提供隐蔽性

必须明确：**系统服务方案给不了隐蔽性，反而更显眼。**

| 可枚举的东西 | 方式 |
|---|---|
| 服务本身 | `dumpsys`、`service list`、`ps` |
| socket | `/dev/socket/autod`（或 `/data/local/tmp/autod.sock`）的文件权限和 SELinux 标签 |
| HTTP 端口 | 默认 8088，`netstat` 可见 |
| SELinux domain | `sesearch`、`dmesg` 里的 avc 记录 |
| 二进制 | `/system/bin/autod` 或 `/data/local/tmp/autod` |

**触控和按键走 `/dev/uinput`，会额外多两个可枚举的输入设备（触摸屏 + 键盘）：**

```
/proc/bus/input/devices          带 name / uniq / phys
/sys/class/input/eventX/device/name
```

任何原生代码读这两个文件就能看到它。**这也是长期方案应该走 `IInputManager` 的原因。**

### 约束 4 · 必须有一棵 AOSP 源码树

`autod` 用的平台私有头文件（`gui/SurfaceComposerClient.h`、`gui/SyncScreenCaptureListener.h`、`input/Input.h` 等）**不在 NDK 里**。

**唯一绕开的方式**是纯 NDK 方案（截图 exec `screencap`，触控 uinput），代价是截图慢一个数量级（120ms vs 23ms）。头文件清单与源码树体积见 `04-environment.md`。

### 约束 5 · SELinux domain 是主要工作量

Android 的 `init` 只会启动有 SELinux 标签的二进制。`autod` 需要：

1. `autod.rc` 里有 `seclabel u:r:autod:s0`
2. `system/sepolicy/private/autod.te` 定义 domain 与 allow 规则
3. `file_contexts` 给二进制打标签
4. 能 `binder_call` 到 `surfaceflinger` 和 `system_server`
5. 编 `neverallow` 会**直接编译失败**，需逐条加规则

**正确工作流**：

```
写初始规则
    ↓
编译报 neverallow 违反  →  按报错逐条加 allow
    ↓
运行时 dmesg 出现 avc: denied  →  audit2allow 生成规则
```

`audit2allow` 用法：

```bash
adb shell dmesg | grep avc > avc.log
./prebuilts/build-tools/linux-x86/bin/audit2allow -i avc.log
```

**临时定位技巧**：在 `.te` 里加 `permissive autod;`，会打印所有会被拒绝的操作而不真拦，方便一次性收集全部 avc 报错。**定位完必须删除**——permissive domain 会导致 CTS 失败。

**绕开方式**：阶段 1 原型以 root 身份跑，跳过整个 sepolicy。见 `../dev/02-native-daemon/README.md`。

### 约束 6 · 客户端位置与对外暴露

`SCM_RIGHTS`（传 fd）和 `SOCK_SEQPACKET` 都**过不了 `adb forward`**——那是 TCP 通道。所以：

- **要传 fd（截图帧、APK 安装）的客户端必须在设备上运行**——提供 C++ 版 `autodctl`，编进系统或推到设备上跑；Python 版仅用于协议文档和主机侧 mock 联调
- **网络客户端走 HTTP/WebSocket**：`autod` 自带这两条传输，不需要再架网关

> ⚠️ **默认不鉴权**，且内置默认只绑 `127.0.0.1`。要对外必须显式把 `bind` 写成 `0.0.0.0`——**那是需要主动做的决定**：同一个网络里任何人都能看屏幕、点屏幕、按键、读剪贴板、装应用。开启鉴权见 `api/04-config.md`。

**仍然不要把 Unix socket 直接暴露到 TCP**：fd 传递会失效，无鉴权的 TCP 服务等于把设备完全交出去。

### 约束 7 · 平台 API 跨版本不稳定

`autod` 依赖的全是**平台私有 API**，AOSP 每个大版本都可能改签名（显示标识、截图参数结构体、结果字段……）。

**升级 Android 版本时必须重对源码**，逐项对照清单见 `03-reference.md`。

### 约束 8 · 非 root 真机的权限

| 能力 | `shell` UID（`adb shell` 身份） | 说明 |
|---|---|---|
| **截图** | ✅ 可以 | `adb exec-out screencap` 就是 shell 身份 |
| **触控（Java 路径）** | ✅ 可以 | `adb shell input tap` 就是 shell 身份，有 `INJECT_EVENTS` |
| **触控（native 路径）** | ❌ 不行 | Android 12 的 `IInputManager` 是 Java-only |
| **触控（uinput）** | ❌ 不行 | `/dev/uinput` 通常只对 `uhid` 组 / root 开放 |

**结论：非 root 真机上，native `autod` 能截图，但注入不了触摸。**

三条出路：

1. **Magisk root**（推荐，最省事）→ `autod` 以 root 跑，uinput 和截图都能用。**不需要编系统镜像。**
2. **刷 userdebug AOSP 镜像** → `adb root` 可用，但要编完整镜像 + 拉 vendor blobs，是最重的路
3. **长期形态**：截图走 native daemon，触控拆 Java 服务

### 风险汇总

| # | 风险 | 影响 | 缓解 |
|---|---|---|---|
| 1 | Android 12 注入 Java-only | **native 单进程注入不了触摸** | 已走 uinput 落地；长期可拆 Java 服务 |
| 2 | SELinux 规则调不通 | 服务起不来 | `permissive` 定位 + `audit2allow` |
| 3 | 平台 API 版本差异 | 编译失败 | 集中改 `capture_*.cpp` / `inject_*.cpp` |
| 4 | 构建环境缺失 | 无法编译 | 见 `04-environment.md` |
| 5 | 帧数据过大 | Binder 事务失败 | 已用 memfd 规避 |
| 6 | 结构体布局错位 | 读到垃圾数据 | 已用 `static_assert` + `assert` 锁死 |
| 7 | `GraphicBuffer` 忘记 unlock | SurfaceFlinger 缓冲耗尽 | 代码里已配对 |
| 8 | 忘记 `startThreadPool()` | `waitForResults()` 死等 | 已在 `main.cpp` 处理 |
| 9 | `FLAG_SECURE` 抓不到 | 功能缺失 | **无解**，需改 framework |
| 10 | 无隐蔽性 | 可被枚举 | **无解**，是本方案的固有属性 |

---

## 8. 相关文档

- 架构与关键设计决策 → `02-architecture.md`
- Android 版本 API 差异、延迟数据、触控能力矩阵 → `03-reference.md`
- 硬件、磁盘预算、构建环境 → `04-environment.md`
- 接口的权威说明 → `api/README.md`
- 设计与踩坑记录 → `05-design-notes.md`
