# 技术参考

> 三部分：**Android 版本 API 差异**、**延迟与吞吐**、**触控能力矩阵**。
>
> `autod` 依赖的全部是**平台私有 API**，AOSP 每个大版本都可能改签名。升级目标版本时**必须对着源码重新确认，不要照抄**。源码位置随版本变化，用 [cs.android.com](https://cs.android.com/android/platform/superproject) 搜类名最快。
>
> 第二部分的数字除标注"实测"外均按管线结构估算，不是实测值；估算方法与实测打点见该部分末尾。

---

# 第一部分 · Android 版本 API 差异

## 1. Android 12（本项目目标，已对照 `android-12.0.0_r34` 源码逐项核实）

**核实来源**：

| 文件 | 内容 |
|---|---|
| `frameworks/native/libs/gui/include/gui/LayerState.h` | `CaptureArgs` / `DisplayCaptureArgs` |
| `frameworks/native/libs/gui/include/gui/SurfaceComposerClient.h` | `ScreenshotClient` / `SurfaceComposerClient` |
| `frameworks/native/libs/gui/include/gui/SyncScreenCaptureListener.h` | 同步 listener |
| `frameworks/native/libs/gui/include/gui/ScreenCaptureResults.h` | 结果结构体 |
| `frameworks/native/libs/ui/include/ui/DisplayId.h` | `PhysicalDisplayId` |
| `frameworks/native/libs/ui/include/ui/DisplayMode.h` | `ui::DisplayMode` |

**注意：Android 12 没有 `gui/ScreenCapture.h`。** 那是后来的版本才有的。

**省流提示**：`sourceCrop` 和 `frameScaleX/Y` 可以直接做**区域截图**和**降采样**——这是延迟优化里收益最大的两项（见第 10 节），不需要改协议就能先验证。

### 1.1 截图调用形态

```cpp
#include <gui/LayerState.h>              // DisplayCaptureArgs
#include <gui/ScreenCaptureResults.h>    // gui::ScreenCaptureResults
#include <gui/SurfaceComposerClient.h>   // SurfaceComposerClient / ScreenshotClient
#include <gui/SyncScreenCaptureListener.h>
#include <ui/DisplayId.h>                // PhysicalDisplayId

// --- 枚举显示 ---
std::vector<PhysicalDisplayId> ids = SurfaceComposerClient::getPhysicalDisplayIds();
// 按值收，不是引用
sp<IBinder> token = SurfaceComposerClient::getPhysicalDisplayToken(
        PhysicalDisplayId{ids.front().value});
// 从 uint64 构造：explicit PhysicalDisplayId(uint64_t id)

// --- 查询显示模式（参数是 token，不是 DisplayId）---
ui::DisplayMode mode;
SurfaceComposerClient::getActiveDisplayMode(token, &mode);
// mode.resolution.getWidth() / getHeight(), mode.refreshRate

// --- 截图 ---
DisplayCaptureArgs args;                 // 继承 CaptureArgs
args.displayToken = token;
args.width        = 0;                   // 0 = 原始分辨率
args.height       = 0;
args.pixelFormat  = ui::PixelFormat::RGBA_8888;
args.dataspace    = ui::Dataspace::UNKNOWN;   // 用显示自身色彩空间

sp<SyncScreenCaptureListener> listener = new SyncScreenCaptureListener();
status_t st = ScreenshotClient::captureDisplay(args, listener);
if (st != NO_ERROR) { /* 提交失败 */ }

gui::ScreenCaptureResults result = listener->waitForResults();
// waitForResults() 内部已调 fence->waitForever()，返回时数据一定可读

if (result.result != OK) { /* Android 12 用 status_t result */ }
sp<GraphicBuffer> buffer = result.buffer;
```

**Android 12 的结果结构体：**

```cpp
namespace android::gui {
struct ScreenCaptureResults : public Parcelable {
    sp<GraphicBuffer> buffer;
    sp<Fence> fence = Fence::NO_FENCE;
    bool capturedSecureLayers{false};
    ui::Dataspace capturedDataspace{ui::Dataspace::V0_SRGB};
    status_t result = OK;          // ← 注意是 status_t
};
}
```

**`DisplayCaptureArgs` 的定义：**

```cpp
struct CaptureArgs {
    ui::PixelFormat pixelFormat{ui::PixelFormat::RGBA_8888};
    Rect sourceCrop;
    float frameScaleX{1}, frameScaleY{1};
    bool captureSecureLayers{false};
    int32_t uid{UNSET_UID};
    ui::Dataspace dataspace = ui::Dataspace::UNKNOWN;
    bool allowProtected = false;
    bool grayscale = false;
};

struct DisplayCaptureArgs : CaptureArgs {
    sp<IBinder> displayToken;
    uint32_t width{0};
    uint32_t height{0};
    bool useIdentityTransform{false};
};
```

## 2. Android 15 / 16（对照版本，**不是**本仓库目标）

参考：[`cmds/screencap/screencap.cpp` @ main](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/cmds/screencap/screencap.cpp)

```cpp
#include <android/gui/DisplayCaptureArgs.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SyncScreenCaptureListener.h>

std::vector<PhysicalDisplayId> ids = SurfaceComposerClient::getPhysicalDisplayIds();
DisplayId id = ids.front();

gui::CaptureArgs args;                    // 注意命名空间是 gui::
args.hintForSeamlessTransition = false;
args.attachGainmap = false;

sp<SyncScreenCaptureListener> listener = new SyncScreenCaptureListener();
ScreenshotClient::captureDisplay(id, args, listener);
ScreenCaptureResults res = listener->waitForResults();

if (!res.fenceResult.ok()) { /* fenceStatus(res.fenceResult) 取错误码 */ }
sp<GraphicBuffer> buf = res.buffer;
```

关键点：

- `getPhysicalDisplayIds()` 返回 `std::vector<PhysicalDisplayId>`
- 参数结构是 `gui::CaptureArgs`（在 `android/gui/DisplayCaptureArgs.h`）
- 结果里的 `buffer` 是 `sp<GraphicBuffer>`，要 `lock(USAGE_SW_READ_OFTEN, &base)` 才能读
- `res.fenceResult` 是 `ftl::Expected`，用 `.ok()` 判成功

## 3. Android 12 vs 15/16 关键差异

| 项 | Android 12 | Android 15 / 16 |
|---|---|---|
| 截图头文件 | `gui/LayerState.h` | `android/gui/DisplayCaptureArgs.h` |
| 参数类型 | `DisplayCaptureArgs` | `gui::CaptureArgs` |
| 显示标识 | `sp<IBinder>` token | `DisplayId` 值类型 |
| 调用 | `ScreenshotClient::captureDisplay(args, listener)` | `ScreenshotClient::captureDisplay(displayId, args, listener)` |
| 结果字段 | `status_t result` | `ftl::Expected fenceResult` |
| `SyncScreenCaptureListener` | ✅ 有 | ✅ 有 |
| `gui/ScreenCapture.h` | ❌ **不存在** | ✅ 有 |

## 4. Android 9 / 10（不推荐）

```cpp
DisplayCaptureArgs args;
args.displayToken = displayToken;
args.width = 0; args.height = 0;
args.pixelFormat = PIXEL_FORMAT_RGBA_8888;

sp<ISurfaceComposer> sf = ComposerService::getComposerService();
sp<ScreenCaptureResults> res = sp<ScreenCaptureResults>::make();
sf->captureDisplay(args, res.get());
```

差异更大，且需要自己处理 `ISurfaceComposer` 的 Binder 调用。

## 5. 输入注入

### 5.1 `/dev/uinput` 路径 ← **Android 12 上唯一可用**

```cpp
int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
ioctl(fd, UI_SET_EVBIT,  EV_ABS);
ioctl(fd, UI_SET_EVBIT,  EV_KEY);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

struct uinput_setup usetup = {};
strncpy(usetup.name, "autod-touch", UINPUT_MAX_NAME_SIZE);
usetup.id.bustype = BUS_VIRTUAL;
usetup.id.vendor  = 0x1;
usetup.id.product = 0x1;
ioctl(fd, UI_DEV_SETUP, &usetup);
ioctl(fd, UI_DEV_CREATE);

// 注入：按 multitouch protocol B 顺序写事件，最后 input_sync
```

按键注入同理：`keyboard.cpp` 用 uinput 建一个 `autod-keyboard` 虚拟键盘设备。

**代价与差异**（务必理解，这决定了选哪条路）：

| | `IInputManager` 路径 | `uinput` 路径 |
|---|---|---|
| 需要 AOSP 树 | ✅ 需要 | ❌ 不需要（NDK 即可） |
| 单次开销 | 0.3–1 ms（一次 Binder 往返） | 2–10 µs（一次 write） |
| 吞吐 | 1000–3000 事件/秒 | 数万事件/秒 |
| 事件来源 | framework 层注入 | 走完整 evdev → InputFlinger 管线 |
| 额外输入设备 | ❌ 不产生 | ✅ 会新增 `/dev/input/eventX` |
| 可被枚举 | 否 | **是**：出现在 `/proc/bus/input/devices` 和 `/sys/class/input/`，带 `name`/`uniq` |
| 需要的权限 | `INJECT_EVENTS`（signature） | `/dev/uinput` 的 SELinux 访问权 + `uhid` 组 / root |
| Android 12 可用 | ❌（Java-only AIDL） | ✅ |

> `uinput` 会创建**可枚举的输入设备**。如果需求里包含"不被应用发现"，这条路直接排除——任何原生代码读 `/proc/bus/input/devices` 都能看到它。这也是为什么长期方案应该走 `IInputManager`。

### 5.2 原生 Binder 路径（Android 13+ 参考，Android 12 上不存在）

> ⚠️ **Android 12 上此路径不存在。** 已全树核实：只有一份 Java 版 `IInputManager`，`IInputFlinger` 也没有注入方法。详见 `01-selection.md` 约束 1。以下内容供 Android 13+ 参考。

接口：`android.hardware.input.IInputManager::injectInputEvent(const InputEvent&, int32_t mode)`

```cpp
#include <input/Input.h>              // android::MotionEvent
#include <binder/IServiceManager.h>

sp<IServiceManager> sm = defaultServiceManager();
sp<IBinder> binder = sm->getService(String16("input"));

// AIDL 生成的 C++ 接口，命名空间随 AIDL 包名变化，务必对源码确认
auto im = android::hardware::input::IInputManager::fromBinder(binder);
```

**注入模式常量**（Android 12 已核实，来自 `frameworks/native/libs/input/android/os/InputEventInjectionSync.aidl`）：

| 常量 | 值 | 含义 |
|---|---|---|
| `NONE` | 0 | 异步，假定总是成功 |
| `WAIT_FOR_RESULT` | 1 | 等前面的分发完成，以便判定是否允许注入 |
| `WAIT_FOR_FINISHED` | 2 | 等事件被完全处理 |

**权限**：`android.permission.INJECT_EVENTS`，保护级别 `signature`。`system` UID 和 `shell` UID 都已持有。

#### Android 12 的 `MotionEvent::initialize` 签名（已核实）

**注意头文件路径**：Android 12 的 `Input.h` 在 `frameworks/native/include/input/Input.h`（**不是** `libs/input/input/`）。

```cpp
void initialize(int32_t id, int32_t deviceId, uint32_t source, int32_t displayId,
                std::array<uint8_t, 32> hmac, int32_t action, int32_t actionButton,
                int32_t flags, int32_t edgeFlags, int32_t metaState,
                int32_t buttonState, MotionClassification classification,
                const ui::Transform& transform,
                float xPrecision, float yPrecision,
                float rawXCursorPosition, float rawYCursorPosition,
                int32_t displayWidth, int32_t displayHeight,
                nsecs_t downTime, nsecs_t eventTime, size_t pointerCount,
                const PointerProperties* pointerProperties,
                const PointerCoords* pointerCoords);
```

Android 12 **只有这一个**面向多指事件的 `initialize`，且要求填 `hmac`、`classification`、`ui::Transform`、`displayWidth/Height`——它是为 InputReader 填充事件设计的，不是为注入设计的。后来的版本（15/16）简化成了不需要 hmac/transform 的形态。

其他构造函数：

```cpp
InputEvent::initialize(int32_t id, int32_t deviceId, uint32_t source,
                       int32_t displayId, int32_t action, ...);   // 基类
KeyEvent::initialize  /  FocusEvent::initialize  /  ...
```

`MotionEvent` 定义在 `frameworks/native/include/input/Input.h`，实现在 `frameworks/native/libs/input/Input.cpp`。Java 侧参考逻辑见 `frameworks/base/cmds/input/src/com/android/commands/input/Input.java`。

关键注意：

- 多点触控要正确填 `PointerProperties`（`id` + `toolType`）与 `PointerCoords`
- `client` 侧一般用 `0`（NONE）拿到最低延迟

### 5.3 最低成本替代（原型期）

daemon 里直接 fork/exec `input` 命令：

```cpp
system("/system/bin/input tap 540 1200");
```

零依赖、立即可用，但每次 fork 约 30–80 ms，且无法做连续手势。**只用于阶段 1 验证坐标是否正确**，不要进生产。

## 6. 全部版本通用的坑

- **必须先 `ProcessState::self()->startThreadPool()`**，否则 Binder 回调收不到，`SyncScreenCaptureListener` 会永久阻塞。参考 `screencap.cpp` 里的注释：`setThreadPoolMaxThreadCount(0)` 然后 `startThreadPool()`。
- `GraphicBuffer::lock()` 之后**必须 `unlock()`**，否则 SurfaceFlinger 会耗尽缓冲区。
- stride ≠ width。逐行拷贝时步长要用 `getStride() * bytesPerPixel(format)`。
- `getPhysicalDisplayIds()` 可能返回多个显示，**必须显式选一个**，不要依赖顺序稳定（`screencap.cpp` 里有明确警告）。

## 7. 快速核对清单（升级 Android 版本时）

- [ ] `getPhysicalDisplayIds()` 的返回类型是否变了
- [ ] 截图参数结构体的命名空间和字段（`attachGainmap`、`hintForSeamlessTransition` 等）
- [ ] 截图是同步返回还是走 listener
- [ ] `ScreenCaptureResults` 的字段名（`buffer` / `fenceResult` / `capturedDataspace`）
- [ ] `MotionEvent::initialize()` 的参数列表
- [ ] `IInputManager` 的 AIDL 包名与生成的 C++ 命名空间
- [ ] `INJECT_EVENTS` 的保护级别是否收紧
- [ ] 是否有新的屏幕捕获角色限制（参考 Restricted screen reading）
- [ ] `GraphicBuffer` 的 `lock`/`unlock` API 是否改成 `lockAsync`

---

# 第二部分 · 延迟与吞吐

> ⚠️ 本部分的数字是按管线结构**估算**的。实测数据见 `05-design-notes.md`：单次抓帧 SurfaceFlinger 后端 **23 ms**、`screencap` 后端 **120 ms**；流帧率 **29.8 fps** vs **9.6 fps**。两者量级一致，估算值可用于设计，实测值用于验收。

## 8. 截图延迟

按管线拆解（1080p RGBA_8888）：

| 环节 | 耗时 | 说明 |
|---|---|---|
| 客户端 → socket → autod | 0.1 ms | Unix socket，无网络栈 |
| Binder 到 SurfaceFlinger | 0.3–1 ms | 一次 Binder 往返 |
| **GPU 合成一帧到捕获缓冲** | **8–20 ms** | ← **主要瓶颈** |
| 等待 fence | 含在上面 | |
| `memcpy` 8 MB 到 memfd | 1–3 ms | 移动端内存带宽约 5–10 GB/s |
| 客户端 `mmap` | ~0 | 零拷贝 |

**合计**：

| 刷新率 | 端到端延迟 |
|---|---|
| 60 Hz | **20–35 ms** |
| 120 Hz | **10–20 ms** |

**为什么合成是瓶颈**：平时 HWC（硬件合成器）把各图层直接叠加后推给屏幕，不经过 CPU。一旦要**捕获**，SurfaceFlinger 必须额外做一次 GPU 合成，把结果渲染到一块 CPU 可读的缓冲区。这一步**绕不掉**——这正是为什么所有人都走 `captureDisplay()` 而不是读 framebuffer。

**吞吐**：

| 实现方式 | 帧率 |
|---|---|
| 同步 `waitForResults()` 串行 | **15–30 fps** |
| 改流水线（不等每帧结果） | **30–60 fps** |

流水线的代价是持续占用 GPU，会拖慢前台应用。

**对比 adb 方案**：

| 方式 | 单次耗时 |
|---|---|
| `adb exec-out screencap` | 100–300 ms |
| **daemon 方案（SF 后端）** | **20–35 ms** |

**快 5–10 倍。** `screencap` exec 后端没有这个优势（实测 120 ms），它的价值是"任何 root 设备都能跑、不需要编进 AOSP 树"。

## 9. 触控延迟

| 环节 | 耗时 |
|---|---|
| 客户端 → socket → autod | 0.1 ms |
| 构造事件 | ~0.05 ms |
| 注入（uinput write / Binder `injectInputEvent`） | 2–10 µs / 0.3–1 ms |
| InputDispatcher → 应用 | 下一帧，8–17 ms |
| **应用响应** | **取决于应用主线程** |

| 指标 | 数值 |
|---|---|
| 事件进入输入管线 | **1–2 ms** |
| 到应用真正响应 | 再加 1 帧 |
| **注入速率** | **1000–3000 事件/秒**（Binder 路径上限）；uinput 路径为数万/秒 |

Binder 路径的注入速率上限来自每次 `injectInputEvent` 一次往返（约 0.3–1 ms），够 60–120 Hz 的滑动采样。

**对比 adb 方案**：

| 方式 | 单次耗时 |
|---|---|
| `adb shell input tap` | 30–80 ms（fork + exec） |
| **daemon 方案** | **1–2 ms** |

**快 50 倍以上。**

## 10. 优化空间

| 优化 | 收益 | 代价 |
|---|---|---|
| **只捕获需要的区域**（`sourceCrop`） | 合成和 memcpy 按面积等比下降，**收益最大** | 需改协议支持区域参数 |
| 跳过 memcpy，直接导出 dmabuf fd | 省 1–3 ms | 客户端要懂 gralloc 布局 |
| 流水线捕获 | 吞吐翻倍 | 持续占 GPU |
| 服务端不编 PNG，发原始 RGBA | 省 15–40 ms | 带宽增大 |
| 降采样（如输出 540p） | memcpy 省 4 倍 | 精度下降 |

**PNG 编码很贵**：1080p 在设备上编码 PNG 要 15–40 ms。如果调用方能接受原始像素，**不要编 PNG**（流的场景改走 JPEG，见 `05-design-notes.md`）。

## 11. 怎么实测

上面的数字需要实测校验。在 `autod` 里加三处 `CLOCK_MONOTONIC` 打点：

```cpp
// capture.cpp 的 Grab() 里
const int64_t t0 = NowNs();                    // 收到请求
// ... ScreenshotClient::captureDisplay() ...
const int64_t t1 = NowNs();                    // 捕获返回
// ... memcpy 完成 ...
const int64_t t2 = NowNs();                    // 数据拷完

ALOGI("capture: 合成=%lldus 拷贝=%lldus",
      (t1 - t0) / 1000, (t2 - t1) / 1000);
```

`inject.cpp` 的 `SendSingle()` 里同理，测注入的往返。跑一次就能拿到目标设备的真实数字。**不同 SoC 差异很大**，尤其是 GPU 合成那一步。

---

# 第三部分 · 触控能力矩阵

## 12. 已实现 / 待补

| 能力 | 状态 |
|---|---|
| 单击 | ✅ 可配持续时间 |
| 长按 | ✅ 单击 + 长 duration，**中途不能发 MOVE**（否则系统判成拖拽） |
| 双击 | ✅ 两次单击，间隔默认 120ms（系统阈值约 300ms） |
| 拖拽 | ✅ 起点先停顿 120ms 再移动，否则被判成滑动（fling） |
| 滑动 | ✅ 直线插值，可配时长和步数 |
| 压感 / 接触面积 | ⚠️ 每个手势可设常量值，尚不支持逐点曲线 |
| **真多点触控** | ✅ **uinput 后端最多 10 个指针**（`kMaxSlots = 10`，`pointerId` → slot 映射）；⚠️ Binder 后端固定 `pointerCount = 1`；⚠️ 官方 `virtual_touchpad` 后端只有 2 槽位 |
| 按键注入 | ✅ `KeyEvent` 已实现（uinput 虚拟键盘） |
| 文本输入 | ⚠️ 靠逐个键码，未做输入法接口 |
| 鼠标 / 滚轮 | ❌ 未实现 |

**多点触控（Binder 路径）要做什么**：

1. `pointerCount = N`
2. 平行的 `PointerProperties[]` 和 `PointerCoords[]` 数组
3. 正确的动作编码：

   ```
   ACTION_POINTER_DOWN | (index << ACTION_POINTER_INDEX_SHIFT)
   ACTION_POINTER_UP   | (index << ACTION_POINTER_INDEX_SHIFT)
   ```

4. 维护指针 ID 的生命周期

**框架层面的上限**：

- `MotionEvent` 最多 **16 个指针**
- 工具类型可指 finger / stylus / mouse
- 支持 hover（鼠标悬停）

## 13. 拿不到的

**真实硬件指纹**。具体包括：

- 电容屏原始采样数据
- 与加速度计 / 陀螺仪的物理相关性（真实触摸时设备会有微小震动）
- 与触控采样率的相位关系
- 真实的接触面积变化曲线

**这些在 framework 层注入解决不了。** 如果应用做了行为分析，注入事件和真实触摸在这些维度上是有差异的。

---

## 相关文档

- 为什么合成是瓶颈、为什么必须走 uinput → `01-selection.md`
- Android 12 触控约束的完整核实记录 → `01-selection.md` 约束 1
- 实测性能数据与踩坑记录 → `05-design-notes.md`
- 接口的权威说明 → `api/README.md`
