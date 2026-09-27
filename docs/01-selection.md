# 01 · 方案选型

> 为什么不用内核模块，以及三种可行落点的对比。

---

## 1. 需求

在 Android 系统层对外提供两种能力，长期方案：

- **屏幕截图**：获取屏幕画面
- **触摸注入**：模拟点击、滑动、多点触控

对外以 socket / HTTP 接口暴露。

---

## 2. 内核路线为什么不行

内核模块（LKM）是最初的设想，理由是"应用层无法检测其存在"。逐条核对后否定了这条路。

### 2.1 截图：内核拿不到数据

原始设想是读 `/dev/graphics/fb0`。问题有两层：

**第一层：fb0 在现代 Android 上基本不存在。**

Android 已从 framebuffer 迁移到 DRM/KMS，`CONFIG_FB` 在多数新内核里已被移除。

**第二层（更致命）：即使 fb0 存在，也拿不到真实画面。**

现代 Android 用 HWC（硬件合成器）把各个图层直接叠加后推给屏幕，合成过程发生在显示控制器里，不经过 CPU 可访问的 framebuffer。要截图，必须让 SurfaceFlinger 额外做一次 **GPU 合成**到捕获缓冲区。

这就是为什么所有人都走 `SurfaceFlinger::captureDisplay()`——不是在绕远路，而是**唯一的路径**。

### 2.2 FLAG_SECURE：内核层绕不过

`FLAG_SECURE` 是 SurfaceFlinger 的**图层属性**（`setSecure(true)`）。被标记的图层：

- 不会被合成进普通合成路径
- 受保护内容走 protected buffer 路径，**CPU 不可读**
- 不会出现在任何可捕获的缓冲区里

这是设计目标，不是权限问题。

佐证：业界常见的绕过方式是框架层 hook（如 [LSPosed/DisableFlagSecure](https://github.com/LSPosed/DisableFlagSecure)），**这本身就说明它在 framework 层而非 kernel 层**。真要绕过得改 system 分区里的 SurfaceFlinger 或 hook composer HAL。

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
| 接口 | Binder 直连 `ISurfaceComposer` + `IInputManager` |
| 需要 | 自己的 SELinux domain、`seclabel`、allow 规则、`file_contexts` 标签 |
| 优点 | **隔离**：崩了不带走 system_server<br>**可独立更新**<br>**性能最好**：可走 gralloc/dma-buf 零拷贝 |
| 缺点 | 需要 AOSP 树编译（平台私有头文件不在 NDK 里）<br>需要写 sepolicy<br>**Android 12 上注入只能走 `/dev/uinput`**（见 `06-constraints.md`） |

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

**已知代价**：需要 AOSP 源码树；需要写 sepolicy；Android 12 上触控需绕道。

---

## 6. 相关文档

- 架构细节 → `02-architecture.md`
- Android 12 的触控约束 → `06-constraints.md`
- 版本 API 差异 → `03-version-matrix.md`
