// inject_binder.cpp —— IInputManager 触控注入后端【在 Android 12 上不可用】
//
// =============================================================================
// ⚠️ 本文件在 Android 12 上无法编译，且这是有意为之。
// =============================================================================
//
// 已对照 android-12.0.0_r34 实际源码逐项核实（见 docs/06-constraints.md 约束 1）：
//
// 1. 全树只有一份 IInputManager，位于
//      frameworks/base/core/java/android/hardware/input/IInputManager.aidl
//    是纯 Java AIDL —— 没有 @VintfStability，没有 cpp / ndk backend 标注，
//    import 的是 android.view.InputEvent 这类 Java 类型。
//    → 编译器不会生成 <android/hardware/input/IInputManager.h>，include 直接失败。
//
// 2. 原生侧唯一的输入服务接口是 android.os.IInputFlinger
//      frameworks/native/libs/input/android/os/IInputFlinger.aidl
//    但它只有四个方法：setInputWindows / createInputChannel /
//    removeInputChannel / setFocusedWindow —— **没有注入能力**。
//
// 3. Android 12 的 InputFlinger 还跑在 system_server 进程内：
//      frameworks/native/services/inputflinger/Android.bp
//      "// TODO(b/23084678): Move inputflinger to its own process"
//    （拆成独立进程是后来的版本才做的）
//
// 结论：**Android 12 上不存在任何 native 的输入注入路径。** 用 inject_uinput.cpp。
//
// -----------------------------------------------------------------------------
// 什么时候可以回来启用这个文件
// -----------------------------------------------------------------------------
//
//   - 目标切到 Android 13+，且 AOSP 给 IInputManager 补了 cpp backend
//   - 或者你自行修改 framework 给 AIDL 加 backend（会牵动整个 framework，风险高）
//
// 后者的优越之处：不创建额外的输入设备，不会出现在 /proc/bus/input/devices。
//
// 启用方法：删掉下面的 #error 行，并把 Android.bp 里 autod_binder 的 srcs 指过来。
//
// -----------------------------------------------------------------------------
// Android 12 的 MotionEvent::initialize 签名（供将来参考）
// -----------------------------------------------------------------------------
//
// 来源：frameworks/native/include/input/Input.h（注意路径 —— Android 12 的
//       Input.h 在 frameworks/native/include/input/，不是 libs/input/）
//
// 与后来的版本差别很大。Android 12 只有一个面向多指事件的 initialize，
// 而且要求 hmac / classification / transform / displayWidth / displayHeight：
//
//   void initialize(int32_t id, int32_t deviceId, uint32_t source, int32_t displayId,
//                   std::array<uint8_t, 32> hmac, int32_t action, int32_t actionButton,
//                   int32_t flags, int32_t edgeFlags, int32_t metaState,
//                   int32_t buttonState, MotionClassification classification,
//                   const ui::Transform& transform,
//                   float xPrecision, float yPrecision,
//                   float rawXCursorPosition, float rawYCursorPosition,
//                   int32_t displayWidth, int32_t displayHeight,
//                   nsecs_t downTime, nsecs_t eventTime, size_t pointerCount,
//                   const PointerProperties* pointerProperties,
//                   const PointerCoords* pointerCoords);
//
// 注入模式常量（frameworks/native/libs/input/android/os/InputEventInjectionSync.aidl）：
//   NONE = 0（异步，不等待）  WAIT_FOR_RESULT = 1  WAIT_FOR_FINISHED = 2
//
// =============================================================================

#error "Android 12 没有 native 输入注入路径：IInputManager 是 Java-only AIDL，IInputFlinger 无注入方法。请使用 inject_uinput.cpp。详见 docs/06-constraints.md 约束 1。"

// ---------------------------------------------------------------------------
// 以下代码保留供参考。上面的 #error 删掉后才会参与编译。
// 注意：它按 Android 15/16 的 API 形态编写，在 Android 12 上即使去掉 #error
//       也不会编译通过 —— 需要同时按上面的签名改 Emit()。
// ---------------------------------------------------------------------------

#include <string.h>
#include <time.h>
#include <unistd.h>

#include <binder/IServiceManager.h>
#include <input/Input.h>

#include <android/hardware/input/IInputManager.h>

#include "autod_log.h"
#include "inject_backend.h"

using namespace android;

namespace autod {
namespace {

int32_t ToPlatformAction(int32_t action) {
    switch (action) {
        case kActionDown:        return AMOTION_EVENT_ACTION_DOWN;
        case kActionUp:          return AMOTION_EVENT_ACTION_UP;
        case kActionMove:        return AMOTION_EVENT_ACTION_MOVE;
        case kActionCancel:      return AMOTION_EVENT_ACTION_CANCEL;
        case kActionPointerDown: return AMOTION_EVENT_ACTION_POINTER_DOWN;
        case kActionPointerUp:   return AMOTION_EVENT_ACTION_POINTER_UP;
        default:                 return AMOTION_EVENT_ACTION_MOVE;
    }
}

void FillCoords(PointerCoords* coords, const TouchPoint& p) {
    coords->clear();
    coords->setAxisValue(AMOTION_EVENT_AXIS_X, static_cast<float>(p.x));
    coords->setAxisValue(AMOTION_EVENT_AXIS_Y, static_cast<float>(p.y));
    coords->setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, p.pressure);
    coords->setAxisValue(AMOTION_EVENT_AXIS_SIZE, p.size);
    coords->setAxisValue(AMOTION_EVENT_AXIS_TOUCH_MAJOR, p.size);
    coords->setAxisValue(AMOTION_EVENT_AXIS_TOUCH_MINOR, p.size);
}

void FillProperties(PointerProperties* props, const TouchPoint& p) {
    props->clear();
    props->id       = p.id;
    props->toolType = AMOTION_EVENT_TOOL_TYPE_FINGER;
}

}  // namespace

class BinderInjector final : public InjectorBackend {
  public:
    const char* Name() const override { return "binder(IInputManager)"; }

    bool Open(const InjectorConfig& config, std::string* error) override;
    bool Emit(int32_t action, const TouchPoint& point, int64_t downTimeNs,
              int64_t eventTimeNs, bool async, std::string* error) override;

  private:
    sp<hardware::input::IInputManager> inputManager_;
    int32_t displayId_     = 0;
    int32_t displayWidth_  = 0;
    int32_t displayHeight_ = 0;
};

bool BinderInjector::Open(const InjectorConfig& config, std::string* error) {
    displayId_     = config.displayId;
    displayWidth_  = static_cast<int32_t>(config.touchWidth);
    displayHeight_ = static_cast<int32_t>(config.touchHeight);

    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == nullptr) {
        if (error) *error = "拿不到 ServiceManager";
        return false;
    }

    sp<IBinder> binder = sm->getService(String16("input"));
    if (binder == nullptr) {
        if (error) *error = "找不到 input 服务（InputManagerService 未启动？）";
        return false;
    }

    inputManager_ = hardware::input::IInputManager::fromBinder(binder);
    if (inputManager_ == nullptr) {
        if (error) {
            *error = "IInputManager::fromBinder 失败 —— Android 12 上这个 AIDL "
                     "没有 cpp backend，属于预期行为；请改用 uinput 后端";
        }
        return false;
    }

    ALOGI("已连接 InputManagerService");
    return true;
}

bool BinderInjector::Emit(int32_t action, const TouchPoint& point,
                          int64_t downTimeNs, int64_t eventTimeNs,
                          bool async, std::string* error) {
    if (inputManager_ == nullptr) {
        if (error) *error = "BinderInjector 未初始化";
        return false;
    }

    PointerProperties props[1];
    PointerCoords     coords[1];
    FillProperties(&props[0], point);
    FillCoords(&coords[0], point);

    MotionEvent event;
    // ↓↓↓ 版本敏感区域 —— 这是 Android 15/16 的形态，Android 12 不适用 ↓↓↓
    event.initialize(
            /* deviceId      */ 0,
            /* source        */ AINPUT_SOURCE_TOUCHSCREEN,
            /* displayId     */ displayId_,
            /* action        */ ToPlatformAction(action),
            /* actionButton  */ 0,
            /* flags         */ 0,
            /* edgeFlags     */ 0,
            /* metaState     */ 0,
            /* buttonState   */ 0,
            /* xOffset       */ 0.0f,
            /* yOffset       */ 0.0f,
            /* xPrecision    */ 1.0f,
            /* yPrecision    */ 1.0f,
            /* rawXOffset    */ 0.0f,
            /* rawYOffset    */ 0.0f,
            /* downTime      */ downTimeNs,
            /* eventTime     */ eventTimeNs,
            /* pointerCount  */ 1,
            /* pointerProps  */ props,
            /* pointerCoords */ coords);
    // ↑↑↑ 版本敏感区域 ↑↑↑

    const int32_t mode = async ? 0 /* NONE */ : 2 /* WAIT_FOR_FINISHED */;

    const binder::Status status = inputManager_->injectInputEvent(event, mode);
    if (!status.isOk()) {
        if (error) {
            *error = "injectInputEvent 被拒绝: " + status.toString8().string() +
                     " —— 通常是缺少 INJECT_EVENTS 权限"
                     "（确认以 system 或 shell UID 运行）";
        }
        return false;
    }
    return true;
}

std::unique_ptr<InjectorBackend> CreateInjectorBackend() {
    return std::make_unique<BinderInjector>();
}

}  // namespace autod
