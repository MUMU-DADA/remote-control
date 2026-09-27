// inject.cpp —— 触控注入的共享手势逻辑
//
// 与平台无关。真正的注入动作交给后端（inject_backend.h）。
// 这样换后端时，手势时序、插值、downTime 管理这些逻辑一行都不用改。
//
// 文件清单：
//   inject.cpp          本文件，手势逻辑（平台无关）
//   inject_uinput.cpp   /dev/uinput 后端
//   inject_binder.cpp   IInputManager 后端

#include "inject.h"

#include <time.h>
#include <unistd.h>

#include "autod_log.h"
#include "inject_backend.h"

namespace autod {
namespace {

int64_t NowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

bool IsGestureEnd(int32_t action) {
    return action == kActionUp || action == kActionCancel;
}

}  // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

Injector::Injector() = default;

// 必须定义在 .cpp 里：unique_ptr<InjectorBackend> 析构时需要完整类型
Injector::~Injector() {
    if (backend_) {
        backend_->Close();
    }
}

const char* Injector::BackendName() const {
    return backend_ ? backend_->Name() : "(未初始化)";
}

bool Injector::Init(const InjectorConfig& config, std::string* error) {
    backend_ = CreateInjectorBackend();
    if (!backend_) {
        if (error) *error = "没有可用的注入后端（Android.bp 里没选后端？）";
        return false;
    }

    if (!backend_->Open(config, error)) {
        backend_.reset();
        return false;
    }

    ALOGI("注入后端就绪: %s", backend_->Name());
    return true;
}

// ---------------------------------------------------------------------------
// 单点事件
// ---------------------------------------------------------------------------

bool Injector::SendSingle(int32_t action, const TouchPoint& point,
                          int64_t eventTimeNs, bool async, std::string* error) {
    if (!backend_) {
        if (error) *error = "Injector 未初始化";
        return false;
    }

    // downTime 在 DOWN 时建立，在 UP/CANCEL 时清零；
    // 中间所有事件共用同一个值 —— 这是 MotionEvent 的语义要求。
    if (action == kActionDown || downTimeNs_ == 0) {
        downTimeNs_ = eventTimeNs;
    }
    const int64_t downTime = downTimeNs_;

    const bool ok =
            backend_->Emit(action, point, downTime, eventTimeNs, async, error);

    if (IsGestureEnd(action)) {
        downTimeNs_ = 0;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 手势
// ---------------------------------------------------------------------------

bool Injector::Tap(const TouchPoint& point, uint32_t durationMs, bool async,
                   std::string* error) {
    const int64_t t0 = NowNs();

    if (!SendSingle(kActionDown, point, t0, async, error)) {
        return false;
    }

    if (durationMs > 0) {
        usleep(static_cast<useconds_t>(durationMs) * 1000);
    }

    return SendSingle(kActionUp, point, NowNs(), async, error);
}

bool Injector::Swipe(const TouchPoint& from, const TouchPoint& to,
                     uint32_t durationMs, uint32_t steps, bool async,
                     std::string* error) {
    if (durationMs == 0) durationMs = 300;

    // 按 ~60Hz 估算步数；步数太少会让滑动看起来是「跳」过去的
    if (steps == 0) {
        steps = durationMs * 60 / 1000;
        if (steps < 2)   steps = 2;
        if (steps > 240) steps = 240;
    }

    const int64_t t0 = NowNs();
    const int64_t perStepNs =
            static_cast<int64_t>(durationMs) * 1000000LL / steps;

    if (!SendSingle(kActionDown, from, t0, async, error)) {
        return false;
    }

    for (uint32_t i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);

        TouchPoint mid;
        mid.id = from.id;
        // 显式用 float 运算再取整，避免隐式窄化
        mid.x = static_cast<int32_t>(
                static_cast<float>(from.x) +
                static_cast<float>(to.x - from.x) * t + 0.5f);
        mid.y = static_cast<int32_t>(
                static_cast<float>(from.y) +
                static_cast<float>(to.y - from.y) * t + 0.5f);
        mid.pressure = from.pressure + (to.pressure - from.pressure) * t;
        mid.size     = from.size + (to.size - from.size) * t;

        const int64_t when = t0 + perStepNs * i;
        if (!SendSingle(kActionMove, mid, when, async, error)) {
            return false;
        }

        // 时间戳是自己造的，但实际节奏要跟上，否则事件会被 InputFlinger 归并
        const int64_t now = NowNs();
        if (when > now) {
            usleep(static_cast<useconds_t>((when - now) / 1000));
        }
    }

    return SendSingle(kActionUp, to, NowNs(), async, error);
}

// ---------------------------------------------------------------------------
// 手动多点触控
// ---------------------------------------------------------------------------

bool Injector::TouchDown(const TouchPoint& point, bool async, std::string* error) {
    return SendSingle(kActionDown, point, NowNs(), async, error);
}

bool Injector::TouchMove(const TouchPoint& point, bool async, std::string* error) {
    return SendSingle(kActionMove, point, NowNs(), async, error);
}

bool Injector::TouchUp(const TouchPoint& point, bool async, std::string* error) {
    return SendSingle(kActionUp, point, NowNs(), async, error);
}

}  // namespace autod
