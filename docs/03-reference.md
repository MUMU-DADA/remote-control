# 技术参考

> 三部分：**Android 版本 API 差异**、**延迟与吞吐**、**触控能力矩阵**。
>
> `autod` 依赖的全部是**平台私有 API**，AOSP 每个大版本都可能改签名。升级目标版本时**必须对着源码重新确认，不要照抄**。源码位置随版本变化，用 [cs.android.com](https://cs.android.com/android/platform/superproject) 搜类名最快。

---

## 最低支持的 Android 版本

**编译期：Android 11（API 30）。运行时：Android 8 起。**

代码已经改造成**运行时探测**，所以下限由编译期决定 —— 而编译期
下限现在是 API 30，因为用的是 NDK 默认的 `minSdk`。要真正支持
Android 8，编译时传 `API=26` 即可（见下面"怎么把下限压下去"）。

### 原来的硬下限（改造前）

**Android 11（API 30）。实测出来的，不是估计。**

```bash
$ API=30 bash tools/build-ndk.sh     # ✓ 编译通过
$ API=29 bash tools/build-ndk.sh     # ✗ 两处硬阻断
  dispatch.cpp:69:  use of undeclared identifier 'memfd_create'
  image_encoder.cpp:144: 'AndroidBitmap_compress' is unavailable:
                         introduced in Android 30
```

API 28 / 26 报的是同样两条。拦路的都是 `__INTRODUCED_IN(30)`：

| API | 用在哪 | 为什么拦得住 |
|---|---|---|
| `memfd_create` | 所有截图路径、JSON 应答回传 | **bionic 从 API 30 才导出**这个包装。不是内核问题 —— 系统调用 Linux 3.17+ 就有，但动态链接器在 Android 10 上找不到这个符号 |
| `AndroidBitmap_compress` | `image_encoder`（JPEG/WebP 编码） | `__INTRODUCED_IN(30)`，在 `libjnigraphics.so` 里 |

其余用到的 API 都远低于这道线：`accept4` = API 21，
设备命令行的 `cmd` = Android 7，`/dev/uinput` 是内核特性。

### ✅ 这道线已经压下去了（已完成）

三处改动，都验证过：

| 改动 | 效果 | 对高版本的影响 |
|---|---|---|
| `memfd_create()` → `syscall(SYS_memfd_create, …)` | 内核 3.17+ 都支持，不受 bionic 包装的版本限制 | **+29 ns/帧**（实测） |
| `AndroidBitmap_compress` → **dlopen + dlsym** 运行时探测 | API < 30 时自动不用它 | **+0.07 ms（0.45%）**（实测，见下） |
| 新增 `jpeg_encoder`（dlopen libjpeg + vendor 头文件） | **JPEG 保住了**，不再退回 PNG | 高版本不走这条路 |

**⚠️ 「高版本行为不变」这条后来被推翻了 —— 见下面「编码器现在按格式选」。**
当初的假设是"能用 Skia 就用 Skia"，后来实测发现 Skia 在 WebP/PNG 上
比内置编码器慢一倍。

### 实测：改造对高版本的影响

```
直接调用 &AndroidBitmap_compress    14.80 ms   76,466 字节
dlsym 指针调用（改造后）            14.87 ms   76,466 字节
差异: +0.07 ms（0.45%），输出完全一致
```

`/api/v1/stream` 端到端：改造前 JPEG q75 = 29.9 fps，改造后 **30.0 fps**
（帧大小 11,345 → 11,168 字节，内容差异）。

### JPEG 为什么没有失去

原来的判断是"老版本只能退回 PNG，失去 JPEG/WebP"—— **这对 JPEG 是错的**。

漏掉的三件事：
1. `libjpeg` 是 **VNDK 库**（`vendor_available` + `vndk: enabled`），
   ABI 跨版本稳定
2. 头文件可以 vendor：AOSP 的 `jconfig.h` 把 `JPEG_LIB_VERSION`
   钉死在 `62`，布局确定
3. `jpeglibmangler.h` 只改名内部符号，公开 API 没动

实测（`tools/bench/jpeg_dlopen_test.cpp`）：dlopen 成功、10 个符号齐全、
编出合法 JPEG、`file(1)` 独立确认。

而且**它和 Skia 是同一个编码器** —— `external/skia/Android.bp` 里
`libjpeg` 就是依赖。两条路输出差 554 字节，正好是 Skia 多嵌的一个
ICC 段（完整证据链见 `tools/bench/README.md`）。

### ✅ WebP 也内置了（全版本可用）

设备上没有 `libwebp.so`（Skia 里也没找到），所以**把源码编了进来**：

| 格式 | 老版本（API < 30） | 新版本（API 30+） |
|---|---|---|
| JPEG | dlopen libjpeg ✅ | AndroidBitmap（Skia） |
| **WebP** | **内置 libwebp** ✅ | **内置 libwebp** ✅ ← 见下面「按格式选」 |
| PNG | zlib ✅ | **zlib** ✅ ← 同上 |

代价：二进制 **+578 KB**（1.62 MB → 2.20 MB），
源码 `daemon/vendor/webp/`（98 个 `.c`，2.5 MB）。

**参数选择**（实测，`tools/bench/webp_bench.cpp`）：

| method | 320×480 | 1080p | 相对 Skia 默认(m=3) |
|---|---|---|---|
| 0 | 3.3 ms | 24.8 ms | 快 3.1x，体积 +22% |
| **2** | **5.2 ms** | 38.8 ms | **快 2.0x，体积 +3.6%** ← 用的这个 |
| 3（Skia） | 10.3 ms | 78.5 ms | — |

### ⚠️ 编码器现在**按格式选**（2024 实测推翻原设计）

原先的逻辑是"`AndroidBitmap_compress` 可用就用它"。**实测发现它在
WebP/PNG 上比内置编码器慢一倍** —— 所谓"原生快路径"其实是慢路径：

720p，60fps 目标，宿主空闲（详测见 [`06-capture-performance.md`](06-capture-performance.md) 第七节）：

| 格式 | Skia `AndroidBitmap_compress` | 内置编码器 | 提升 |
|---|---|---|---|
| jpeg | 60.3 fps | 61.0 fps | 持平 |
| **webp** | 20.5 fps | **38.3 fps** | **+87%** |
| **png** | 22.2 fps | **45.8 fps** | **+106%** |

原因在参数上：Skia 的 WebP 用 `method=3`（跟 Chrome 对齐），内置的用
`method=2` —— 正好差一倍。也就是说上面那张 method 表**在 API 30+ 上
一直是死代码**，直到改成按格式选才生效。

现在的选择：

| 格式 | 走哪条 | 理由 |
|---|---|---|
| jpeg | Skia | 持平，没必要动 |
| webp | 内置 libwebp | 快 87%，只大 2% |
| png | 内置 zlib | 快 106%，大 17% |

两个开关：`AUTOD_FORCE_FALLBACK=1` 全走内置；`AUTOD_PREFER_NATIVE=1` 全走 Skia。
`/params` 的 `codecs.backend` 逐格式报出当前选择。
| 6 | 21.8 ms | 126.5 ms | 慢 2.1x，体积 -5.8% |

Skia 用 3 是为了跟 Chrome 对齐，不是因为它最优。
`thread_level=1` 只快 5%（libwebp 的线程只并行熵编码），没采用。

许可：libwebp 是 BSD 3-Clause，见 `THIRD-PARTY-NOTICES.md`。

`GET /api/v1/describe` 和 `/params` 的 `codecs` 字段仍然如实报告，
客户端照旧不该假设 —— 只是现在三个格式在任何版本上都是 `true`。

### 怎么真正编出 Android 8 版本

```bash
API=26 ABI=arm64-v8a bash tools/build-ndk.sh
```

⚠️ **低于 Android 11 的设备仍然没有实测过。** 上面的分析基于编译期
报错、源码 `__INTRODUCED_IN` 标注，以及在 Android 12 上对
各条回退路径的独立验证（dlopen libjpeg 跑通、syscall 跑通）。
"能不能在真 Android 8 上跑起来"还需要真机。

再往下会遇到别的：`cmd`（Android 7）、`dumpsys activity lru` 的输出格式、
`/dev/uinput` 的属主（Android 11 起是 `0660 uhid:uhid`，更早是 `system:input`）。

### SurfaceFlinger 直连那条路更低

上面说的是 **NDK 构建**（走 `screencap` exec，任何 root 设备都能跑）。
AOSP 构建走 SF 直连，它依赖平台私有 API，签名逐版本变化 ——
那个的版本下限是**编译目标**决定的，不是运行时探测。见下面的逐版本差异。

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

> **本部分是「为什么慢」的结构模型，不是实测数据。**
>
> **实测数字一律以 [`06-capture-performance.md`](06-capture-performance.md) 为准** ——
> 那里有测量方法、测量纪律（宿主编译会污染结果）和可复现的命令。
>
> 当前口径（720p，SurfaceFlinger 后端）：
>
> | 项 | 实测 |
> |---|---|
> | 抓一帧 | **8–12 ms** |
> | JPEG 端到端 | **61 fps** |
> | PNG / WebP 端到端 | **45.8 / 38.3 fps** |
>
> 下面那些"20–35 ms""15–30 fps"是**按管线结构估的**，量级对得上，
> 但不要拿它当验收标准。

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

| 实现方式 | 估算帧率 |
|---|---|
| 同步 `waitForResults()` 串行 | **15–30 fps** |
| 共享抓帧（现在的做法，见 `06-capture-performance.md`） | 实测 **61 fps**（jpeg） |

> 早期这里是"改流水线（不等每帧结果）"，代价是持续占用 GPU。
> 现在的做法不同：**一个抓帧线程按所有订阅者的最高需求定时抓**，
> 消费者直接拿最新帧 —— 既没有请求-应答的延迟，也不需要持续占着 GPU。
> 详见 `05-design-notes.md` 的「共享抓帧」一节。

**对比 adb 方案**：

| 方式 | 单次耗时 |
|---|---|
| `adb exec-out screencap` | 100–300 ms |
| **daemon 方案（SF 后端）** | **20–35 ms** |

**快 5–10 倍。** `screencap` exec 后端没有这个优势（NDK 构建实测 **197 ms/帧**），
它的价值是"任何 root 设备都能跑、不需要编进 AOSP 树"。

> ⚠️ 这个对比有个坑：**别拿 NDK 构建（screencap 后端）去测 SF 的性能**。
> 两份二进制长得一样，看 `/api/v1/config` 的 `runtime.capture.backend`
> 才能确认跑的是哪条路。

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

- 为什么合成是瓶颈、为什么必须走 uinput → [`01-selection.md`](01-selection.md)
- Android 12 触控约束的完整核实记录 → [`01-selection.md`](01-selection.md) 第 7 节
- **实测性能数据** → [`06-capture-performance.md`](06-capture-performance.md)
- 设计与踩坑记录 → [`05-design-notes.md`](05-design-notes.md)
- 接口的权威说明 → [`api/README.md`](api/README.md)

## 附：`screencap` 的输出格式

`screencap` 后端（NDK 构建走的那条）解析的就是这个格式，来自 AOSP 的
`frameworks/base/cmds/screencap/screencap.cpp`（`saveImage()` 的分支）：

```
偏移 0    uint32  width
偏移 4    uint32  height
偏移 8    uint32  pixelFormat
偏移 12   uint32  colorSpace
偏移 16   像素数据，逐行紧密排列，每行 width * bytesPerPixel 字节
```

`pixelFormat` 取值（`android PixelFormat`）：

| 值 | 格式 | 每像素字节 |
|---|---|---|
| 1 | RGBA_8888 | 4 |
| 2 | RGBX_8888 | 4 |
| 3 | RGB_888 | 3 |
| 4 | RGB_565 | 2 |
| 5 | BGRA_8888 | 4 |
| 22 | RGBA_FP16 | 8 |
| 43 | RGBA_1010102 | 4 |

> 加 `-p` 会输出 PNG，但那样没法直接内存映射，而且编码很贵 —— 所以后端要的是原始格式。

## 附：uinput 触控设备的初始化序列

`inject_uinput.cpp` 用的（少一步 InputReader 就不认它是触摸屏）：

```c
ioctl(fd, UI_SET_EVBIT,  EV_KEY);
ioctl(fd, UI_SET_EVBIT,  EV_ABS);
ioctl(fd, UI_SET_EVBIT,  EV_SYN);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
// ⚠️ 最容易被忽略的一步：不设这个，InputReader 不会把它当触摸屏
ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
```

事件按 **multitouch protocol B** 顺序写，最后 `input_sync()` / `SYN_REPORT`。

> AOSP 官方也有等价实现（`frameworks/native/services/vr/virtual_touchpad/`），
> 还带一份 `.idc` 声明 `touch.deviceType = touchScreen`。

