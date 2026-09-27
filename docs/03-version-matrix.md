# Android 版本 API 差异对照

`autod` 依赖的全部是**平台私有 API**，AOSP 每个大版本都可能改签名。
本文档记录已知差异，升级目标版本时**必须对着源码重新确认**，不要照抄。

> 源码位置随版本变化，用 [cs.android.com](https://cs.android.com/android/platform/superproject) 搜类名最快。

---

## 1. 屏幕捕获

### Android 15 / 16（本仓库基准）

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

### Android 12 / 13（推荐目标版本）

```cpp
#include <gui/ScreenCapture.h>
#include <gui/SurfaceComposerClient.h>

sp<IBinder> token = SurfaceComposerClient::getPhysicalDisplayToken(displayId);

ScreenCapture::CaptureArgs args;
args.displayToken = token;                // ← 注意是 token，不是 DisplayId
args.width  = 0;                          // 0 = 原始分辨率
args.height = 0;
args.dataspace  = ui::Dataspace::V0_SRGB;
args.pixelFormat = ui::PixelFormat::RGBA_8888;

ScreenCapture::CaptureResults res;
status_t err = ScreenCapture::captureDisplay(args, &res);
```

**主要差异**：
| 项 | 12/13 | 15/16 |
|---|---|---|
| 显示标识 | `sp<IBinder>` token | `DisplayId` 值类型 |
| 参数类型 | `ScreenCapture::CaptureArgs` | `gui::CaptureArgs` |
| 调用方式 | 同步返回 `status_t` | 通过 `SyncScreenCaptureListener` |
| 结果类型 | `ScreenCapture::CaptureResults` | `ScreenCaptureResults` |

### Android 9 / 10（不推荐）

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

### 全部版本通用的坑

- **必须先 `ProcessState::self()->startThreadPool()`**，否则 Binder 回调收不到，
  `SyncScreenCaptureListener` 会永久阻塞。参考 `screencap.cpp` 里的注释：
  `setThreadPoolMaxThreadCount(0)` 然后 `startThreadPool()`。
- `GraphicBuffer::lock()` 之后**必须 `unlock()`**，否则 SurfaceFlinger 会耗尽缓冲区。
- stride ≠ width。逐行拷贝时步长要用 `getStride() * bytesPerPixel(format)`。
- `getPhysicalDisplayIds()` 可能返回多个显示，**必须显式选一个**，不要依赖顺序稳定
  （`screencap.cpp` 里有明确警告）。

---

## 2. 输入注入

### 原生 Binder 路径（性能好，依赖 AOSP 树）

接口：`android.hardware.input.IInputManager::injectInputEvent(const InputEvent&, int32_t mode)`

```cpp
#include <input/Input.h>              // android::MotionEvent
#include <binder/IServiceManager.h>

sp<IServiceManager> sm = defaultServiceManager();
sp<IBinder> binder = sm->getService(String16("input"));

// AIDL 生成的 C++ 接口，命名空间随 AIDL 包名变化，务必对源码确认
auto im = android::hardware::input::IInputManager::fromBinder(binder);

android::MotionEvent event;
// event.initialize(...) 的参数很长：downTime, eventTime, action, pointerCount,
// pointerProperties[], pointerCoords[], metaState, buttonState,
// xPrecision, yPrecision, deviceId, edgeFlags, source, displayId, flags
//
// MotionEvent 定义在 frameworks/native/libs/input/input/Input.h
// Java 侧参考逻辑见 cmds/input/src/com/android/commands/input/Input.java

im->injectInputEvent(event, INJECT_INPUT_EVENT_MODE_ASYNC);
```

**权限**：`android.permission.INJECT_EVENTS`，保护级别 `signature`。
`system` UID 和 `shell` UID 都已持有，所以 `autod` 以 `system` 运行即可
（`adb shell input` 能工作就是这个原因）。

关键注意：
- `MotionEvent::initialize()` 的参数在各版本**变过多次**，`displayId` 和 `flags`
  是较晚才加的。务必对照你目标版本的 `Input.h`。
- 多点触控要正确填 `PointerProperties`（含 `id` 和 `toolType`）与 `PointerCoords`
  （含 `x, y, pressure, size, touchMajor, touchMinor`）。
- `INJECT_INPUT_EVENT_MODE_ASYNC` 不等待分发结果，延迟低；
  `WAIT_FOR_FINISH` 会阻塞到事件分发完成。

### `/dev/uinput` 路径（不依赖 AOSP 树，可用 NDK 编译）

如果暂时不想投入 AOSP 构建环境，触控这一半可以先用 `uinput` 实现：

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

**代价与差异**（务必理解，这决定了你选哪条路）：

| | `IInputManager` 路径 | `uinput` 路径 |
|---|---|---|
| 需要 AOSP 树 | ✅ 需要 | ❌ 不需要（NDK 即可） |
| 延迟 | 低（一次 Binder） | 低（一次 write） |
| 事件来源 | framework 层注入 | 走完整 evdev → InputFlinger 管线 |
| 额外输入设备 | ❌ 不产生 | ✅ 会新增一个 `/dev/input/eventX` |
| 可被枚举 | 否 | **是**：出现在 `/proc/bus/input/devices` 和 `/sys/class/input/`，带 `name`/`uniq` |
| 需要的权限 | `INJECT_EVENTS`（signature） | `/dev/uinput` 的 SELinux 访问权 + 通常需 root |

> `uinput` 会创建**可枚举的输入设备**。如果你的需求里包含"不被应用发现"，
> 这条路直接排除 —— 任何原生代码读 `/proc/bus/input/devices` 都能看到它。
> 这也是为什么长期方案应该走 `IInputManager`。

### 最低成本替代（原型期）

daemon 里直接 fork/exec `input` 命令：

```cpp
system("/system/bin/input tap 540 1200");
```

零依赖、立即可用，但每次 fork 约 30–80 ms，且无法做连续手势。
**只用于阶段 1 验证坐标是否正确**，不要进生产。

---

## 3. 快速核对清单（升级 Android 版本时）

- [ ] `getPhysicalDisplayIds()` 的返回类型是否变了
- [ ] 截图参数结构体的命名空间和字段（`attachGainmap`、`hintForSeamlessTransition` 等）
- [ ] 截图是同步返回还是走 listener
- [ ] `ScreenCaptureResults` 的字段名（`buffer` / `fenceResult` / `capturedDataspace`）
- [ ] `MotionEvent::initialize()` 的参数列表
- [ ] `IInputManager` 的 AIDL 包名与生成的 C++ 命名空间
- [ ] `INJECT_EVENTS` 的保护级别是否收紧
- [ ] 是否有新的屏幕捕获角色限制（参考 Restricted screen reading）
- [ ] `GraphicBuffer` 的 `lock`/`unlock` API 是否改成 `lockAsync`
