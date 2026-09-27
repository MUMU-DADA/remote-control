// inject_binder.cpp —— IInputManager 触控注入后端
//
// ⚠️ 本后端在 Android 12 上**不可用**。
//
// 原因：Android 12 的 IInputManager 是纯 Java AIDL（位于
// frameworks/base/core/java/），没有 cpp/ndk backend 标注，且 import 的是
// android.view.InputEvent 这类 Java 类型。native 进程无法直接调用。
// 完整论证见 docs/06-constraints.md 约束 1。
//
// 保留这个后端的原因：
//   1. Android 13+ 的 AOSP 已往 native 方向演进，将来可能可用
//   2. 如果自行为 AIDL 加 cpp backend（需要改 framework），这个后端就用得上
//   3. 它比 uinput 优越的地方：**不创建可枚举的输入设备**
//
// 默认构建走 inject_uinput.cpp。切到本后端需要在 Android.bp 里改 srcs。
//
// 【版本敏感】MotionEvent::initialize() 的参数列表跨版本改动过多次。
// 编译失败时对照目标版本的 frameworks/native/libs/input/input/Input.h 调整。

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

// 把 autod 的动作常量翻译成 Android 的 AMOTION_EVENT_ACTION_*
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

// ---------------------------------------------------------------------------

class BinderInjector final : public InjectorBackend {
  public:
    const char* Name() const override { return "binder(IInputManager)"; }

    bool Open(const InjectorConfig& config, std::string* error) override;
    bool Emit(int32_t action, const TouchPoint& point, int64_t downTimeNs,
              int64_t eventTimeNs, bool async, std::string* error) override;

  private:
    sp<hardware::input::IInputManager> inputManager_;
    int32_t displayId_ = 0;
};

bool BinderInjector::Open(const InjectorConfig& config, std::string* error) {
    displayId_ = config.displayId;

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
            *error = "IInputManager::fromBinder 失败 —— "
                     "Android 12 上这个 AIDL 没有 cpp backend，属于预期行为；"
                     "请改用 uinput 后端（见 docs/06-constraints.md）";
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
    // ↓↓↓ 版本敏感区域：对照 frameworks/native/libs/input/input/Input.h ↓↓↓
    event.initialize(
            /* deviceId      */ 0,                        // 0 = 虚拟设备
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

    const int32_t mode = async ? INJECT_INPUT_EVENT_MODE_ASYNC
                               : INJECT_INPUT_EVENT_MODE_WAIT_FOR_FINISH;

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

// 后端工厂：链接哪个后端 .cpp，这里就返回哪个实例
std::unique_ptr<InjectorBackend> CreateInjectorBackend() {
    return std::make_unique<BinderInjector>();
}

}  // namespace autod
