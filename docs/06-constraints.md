# 06 · 关键约束与风险

> 本项目的硬约束清单。**动手前请先读完本文。**

---

## 约束 1（最重要）· Android 12 的 `injectInputEvent` 是 Java-only

### 事实

核对 [`IInputManager.aidl` @ android12-release](https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/android12-release/core/java/android/hardware/input/IInputManager.aidl)：

```java
package android.hardware.input;

import android.view.InputEvent;        // ← 纯 Java 类
import android.graphics.Rect;
import android.os.IBinder;
import android.view.InputMonitor;
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

### 对比：截图没问题

截图用的 `ISurfaceComposer` **是** native AIDL，位于 `frameworks/native/libs/gui/`，C++ 可以直连。

### 影响

> **native daemon 能截图，但注入不了触摸。**

这是方案 B 在 Android 12 上的硬约束。

### 三个解法

#### 解法 A：触控改走 `/dev/uinput` ← 推荐先做

| 项 | 说明 |
|---|---|
| 实现 | 纯 syscall，不依赖任何平台 API |
| 结构 | `autod` 保持 native 单进程：截图走 Binder，触控走 uinput |
| 优点 | 改动最小，立即可用；不依赖 AOSP 树也能编 |
| 缺点 | **会创建一个可枚举的输入设备** |

**实现要点**（容易漏的）：

```cpp
// 除了 ABS_MT_* 和 BTN_TOUCH，还要设 INPUT_PROP_DIRECT，
// 否则 InputFlinger 不会把它识别成直接触摸屏
ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

// 必须实现的 ABS 轴
UI_SET_ABSBIT, ABS_MT_SLOT
UI_SET_ABSBIT, ABS_MT_TRACKING_ID
UI_SET_ABSBIT, ABS_MT_POSITION_X
UI_SET_ABSBIT, ABS_MT_POSITION_Y
UI_SET_ABSBIT, ABS_MT_PRESSURE
UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR
UI_SET_KEYBIT, BTN_TOUCH
```

事件注入按 **multitouch protocol B** 顺序写，最后 `input_sync()`。

**权限要求**：`/dev/uinput` 在 Android 上通常是 `0660 system:input`，且在 SELinux 里只有少数 domain 能访问。**通常需要 root 或 `system` UID。**

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

理论上可行，但 `InputEvent` 是 Java 类，要换成 `libinput` 里的 native parcelable，会牵动整个 framework。

**风险高，不建议。**

### 建议

**A 先落地验证整条链路，B 作为长期形态。**

---

## 约束 2 · `FLAG_SECURE` 内容抓不到

`FLAG_SECURE` 是 SurfaceFlinger 的**图层属性**（`setSecure(true)`）。被标记的图层：

- 不会被合成进普通合成路径
- 受保护内容走 protected buffer 路径，**CPU 不可读**
- 不会出现在任何可捕获的缓冲区

**这是设计目标，不是权限问题。** 给 `autod` 再高的权限也拿不到。

业界常见的绕过方式是框架层 hook（如 [LSPosed/DisableFlagSecure](https://github.com/LSPosed/DisableFlagSecure)）——这本身就说明它在 framework 层而非 kernel 层。

**真要绕过得改 system 分区里的 SurfaceFlinger 或 hook composer HAL。本项目不做这件事。**

---

## 约束 3 · 本方案不提供隐蔽性

必须明确：**系统服务方案给不了隐蔽性，反而更显眼。**

| 可枚举的东西 | 方式 |
|---|---|
| 服务本身 | `dumpsys`、`service list`、`ps` |
| socket | `/dev/socket/autod` 的文件权限和 SELinux 标签 |
| SELinux domain | `sesearch`、`dmesg` 里的 avc 记录 |
| 二进制 | `/system/bin/autod` |

**如果触控走 `/dev/uinput`，会额外多一个可枚举的输入设备：**

```
/proc/bus/input/devices          带 name / uniq / phys
/sys/class/input/eventX/device/name
```

任何原生代码读这两个文件就能看到它。**这也是长期方案应该走 `IInputManager` 的原因。**

---

## 约束 4 · 必须有一棵 AOSP 源码树

`autod` 用的平台私有头文件**不在 NDK 里**：

| 头文件 | 位置 |
|---|---|
| `gui/SurfaceComposerClient.h` | `frameworks/native/libs/gui/` |
| `gui/SyncScreenCaptureListener.h` | 同上 |
| `android/gui/DisplayCaptureArgs.h` | 同上 |
| `input/Input.h` | `frameworks/native/libs/input/` |
| `android/hardware/input/IInputManager.h` | AIDL 生成 |

**唯一绕开的方式**是纯 NDK 方案（截图 exec `screencap`，触控 uinput），代价是截图慢一个数量级。详见 `../dev/01-ndk-prototype/`。

---

## 约束 5 · SELinux domain 是主要工作量

Android 的 `init` 只会启动有 SELinux 标签的二进制。`autod` 需要：

1. `autod.rc` 里有 `seclabel u:r:autod:s0`
2. `system/sepolicy/private/autod.te` 定义 domain 与 allow 规则
3. `file_contexts` 给二进制打标签
4. 能 `binder_call` 到 `surfaceflinger` 和 `system_server`
5. 编 `neverallow` 会**直接编译失败**，需逐条加规则

### 正确工作流

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

### 绕开方式

阶段 1 原型以 root 身份跑，跳过整个 sepolicy。见 `../dev/02-native-daemon/README.md`。

---

## 约束 6 · 客户端必须在设备上运行

`SCM_RIGHTS`（传 fd）和 `SOCK_SEQPACKET` 都**过不了 `adb forward`**——那是 TCP 通道。

所以：

- 提供 **C++ 版 `autodctl`**，编进系统或推到设备上跑
- Python 版仅用于协议文档和主机侧 mock 联调

**如果要对外暴露网络接口**，需要两级结构：

```
网络客户端 ──HTTP/WS──> 网关（设备外）──Unix socket──> autod
```

网关负责：鉴权、把 memfd 里的帧编码成 JPEG/WebP 或 H.264、限流。

**不要直接把 socket 暴露到 TCP**：
- fd 传递失效
- 无鉴权的 TCP 服务等于把设备完全交出去

---

## 约束 7 · 平台 API 跨版本不稳定

`autod` 依赖的全是**平台私有 API**，AOSP 每个大版本都可能改签名。

已知 Android 12 与 Android 15/16 的差异：

| 项 | Android 12/13 | Android 15/16 |
|---|---|---|
| 显示标识 | `sp<IBinder>` token | `DisplayId` 值类型 |
| 截图参数 | `ScreenCapture::CaptureArgs` | `gui::CaptureArgs` |
| 调用方式 | 同步返回 `status_t` | `SyncScreenCaptureListener` |
| 结果类型 | `ScreenCapture::CaptureResults` | `ScreenCaptureResults` |

**升级 Android 版本时必须重对源码。** 对照清单见 `03-version-matrix.md`。

---

## 约束 8 · 非 root 真机的权限

| 能力 | `shell` UID（`adb shell` 身份） | 说明 |
|---|---|---|
| **截图** | ✅ 可以 | `adb exec-out screencap` 就是 shell 身份 |
| **触控（Java 路径）** | ✅ 可以 | `adb shell input tap` 就是 shell 身份，有 `INJECT_EVENTS` |
| **触控（native 路径）** | ❌ 不行 | Android 12 的 `IInputManager` 是 Java-only |
| **触控（uinput）** | ❌ 不行 | `/dev/uinput` 通常只对 `system` / root 开放 |

**结论：非 root 真机上，native `autod` 能截图，但注入不了触摸。**

三条出路：

1. **Magisk root**（推荐，最省事）→ `autod` 以 root 跑，uinput 和截图都能用。**不需要编系统镜像。**
2. **刷 userdebug AOSP 镜像** → `adb root` 可用，但要编完整镜像 + 拉 vendor blobs，是最重的路
3. **长期形态**：截图走 native daemon，触控拆 Java 服务

---

## 风险汇总

| # | 风险 | 影响 | 缓解 |
|---|---|---|---|
| 1 | Android 12 注入 Java-only | **方案 B 无法单进程完成** | 走 uinput（短期）或拆 Java 服务（长期） |
| 2 | SELinux 规则调不通 | 服务起不来 | `permissive` 定位 + `audit2allow` |
| 3 | 平台 API 版本差异 | 编译失败 | 集中改 `capture.cpp` / `inject.cpp` |
| 4 | 构建环境缺失 | 无法编译 | 见 `04-hardware.md` |
| 5 | 帧数据过大 | Binder 事务失败 | 已用 memfd 规避 |
| 6 | 结构体布局错位 | 读到垃圾数据 | 已用 `static_assert` + `assert` 锁死 |
| 7 | `GraphicBuffer` 忘记 unlock | SurfaceFlinger 缓冲耗尽 | 代码里已配对 |
| 8 | 忘记 `startThreadPool()` | `waitForResults()` 死等 | 已在 `main.cpp` 处理 |
| 9 | `FLAG_SECURE` 抓不到 | 功能缺失 | **无解**，需改 framework |
| 10 | 无隐蔽性 | 可被枚举 | **无解**，是本方案的固有属性 |

---

## 相关文档

- 方案选型依据 → `01-selection.md`
- 版本 API 差异 → `03-version-matrix.md`
- 各轨道的应对策略 → `../dev/`
