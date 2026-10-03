// inject_uinput.cpp —— /dev/uinput 触控注入后端
//
// 为什么需要这个后端：
//   Android 12 的 IInputManager 是 Java-only AIDL，native 进程调不了
//   （见 docs/06-constraints.md 约束 1）。uinput 是内核自带的模块，
//   用户态通过纯 syscall 就能创建虚拟触摸屏并注入事件，不依赖任何平台 API。
//
// 代价：
//   1. 会创建一个**可枚举的**输入设备（出现在 /proc/bus/input/devices 和
//      /sys/class/input/），任何原生代码都能看到
//   2. 需要 /dev/uinput 的访问权限（Android 上通常要 root 或 system UID）
//
// 实现协议：Linux input 子系统的 multitouch protocol B
//   参考 Documentation/input/multi-touch-protocol.rst
//
// 本文件不依赖任何 Android 头文件，可以在开发机上直接编译运行测试
// （见 dev/02-native-daemon/tests/test_inject_uinput.cpp）

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/uinput.h>

#include "remote_control_log.h"
#include "inject_backend.h"

namespace remote_control {
namespace {

// 协议 B 的槽位上限。Android 的 MotionEvent 最多支持 16 个指针，
// 这里取 10（大多数触控控制器的实际上限）。
constexpr int kMaxSlots = 10;

// ABS 轴的取值范围
constexpr int32_t kPressureMax = 255;
constexpr int32_t kTouchMajorMax = 255;

// 默认坐标范围。很多触控控制器用这个约定值，Android 会自动缩放到屏幕。
// 但如果调用方用的是屏幕像素坐标，必须显式传入实际分辨率，否则会缩小到左上角。
constexpr uint32_t kDefaultRange = 32767;

// 设备创建后等待系统枚举。InputReader 需要一点时间通过 inotify 收到新设备。
constexpr useconds_t kSettleUs = 200000;

std::string ErrnoString(const char* what) {
    return std::string(what) + " 失败: " + strerror(errno) +
           " (errno=" + std::to_string(errno) + ")";
}

int32_t ScaleToRange(float normalized, int32_t maxValue) {
    if (normalized <= 0.0f) return 0;
    if (normalized >= 1.0f) return maxValue;
    return static_cast<int32_t>(normalized * static_cast<float>(maxValue) + 0.5f);
}

}  // namespace

// ---------------------------------------------------------------------------

class UinputInjector final : public InjectorBackend {
  public:
    ~UinputInjector() override { Close(); }

    const char* Name() const override { return "uinput"; }

    bool Open(const InjectorConfig& config, std::string* error) override;
    bool Emit(int32_t action, const TouchPoint& point, int64_t downTimeNs,
              int64_t eventTimeNs, bool async, std::string* error) override;
    void Close() override;

  private:
    bool WriteEvent(uint16_t type, uint16_t code, int32_t value,
                    std::string* error);
    bool Sync(std::string* error);

    int AllocSlot(int32_t pointerId);
    int FindSlot(int32_t pointerId) const;
    void ReleaseSlot(int32_t pointerId);
    int ActiveSlotCount() const;

    int      fd_             = -1;
    bool     deviceCreated_  = false;
    uint32_t rangeX_         = 0;   // ABS_MT_POSITION_X 的 maximum
    uint32_t rangeY_         = 0;
    int32_t  nextTrackingId_ = 1;

    int32_t  slotOwner_[kMaxSlots];  // 每个槽位属于哪个 pointerId，-1 表示空闲
};

// ---------------------------------------------------------------------------
// 打开与创建虚拟设备
// ---------------------------------------------------------------------------

bool UinputInjector::Open(const InjectorConfig& config, std::string* error) {
    Close();
    for (int i = 0; i < kMaxSlots; ++i) slotOwner_[i] = -1;

    const uint32_t w = config.touchWidth  ? config.touchWidth  : kDefaultRange + 1;
    const uint32_t h = config.touchHeight ? config.touchHeight : kDefaultRange + 1;
    rangeX_ = w - 1;
    rangeY_ = h - 1;

    fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        if (error) {
            *error = ErrnoString("open(\"/dev/uinput\")") +
                     " —— 需要 root 或 system UID，且内核要启用 CONFIG_INPUT_UINPUT";
        }
        return false;
    }

    // --- 1. 声明支持的事件类型 ---
    if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd_, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd_, UI_SET_EVBIT, EV_SYN) < 0) {
        if (error) *error = ErrnoString("UI_SET_EVBIT");
        return false;
    }

    // --- 2. 声明 ABS 轴（protocol B 必需的几条）---
    const int absAxes[] = {
        ABS_MT_SLOT, ABS_MT_TRACKING_ID,
        ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
        ABS_MT_PRESSURE, ABS_MT_TOUCH_MAJOR, ABS_MT_TOUCH_MINOR,
    };
    for (int axis : absAxes) {
        if (ioctl(fd_, UI_SET_ABSBIT, axis) < 0) {
            if (error) *error = ErrnoString("UI_SET_ABSBIT");
            return false;
        }
    }

    // --- 3. 按键 ---
    if (ioctl(fd_, UI_SET_KEYBIT, BTN_TOUCH) < 0) {
        if (error) *error = ErrnoString("UI_SET_KEYBIT(BTN_TOUCH)");
        return false;
    }

    // --- 4. ⚠️ 关键且最容易漏的一步 ---
    // 不设 INPUT_PROP_DIRECT，Android 的 InputReader 不会把这个设备
    // 识别成「直接触摸屏」，事件不会被当成触摸处理。
    if (ioctl(fd_, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) {
        ALOGW("UI_SET_PROPBIT(INPUT_PROP_DIRECT) 失败: %s —— "
              "设备可能不会被识别为触摸屏", strerror(errno));
        // 不致命，继续
    }

    // --- 5. 设置各轴的取值范围 ---
    auto setupAbs = [&](uint16_t code, int32_t min, int32_t max,
                        int32_t fuzz, int32_t flat) -> bool {
        uinput_abs_setup abs{};
        abs.code             = code;
        abs.absinfo.minimum  = min;
        abs.absinfo.maximum  = max;
        abs.absinfo.fuzz     = fuzz;
        abs.absinfo.flat     = flat;
        return ioctl(fd_, UI_ABS_SETUP, &abs) >= 0;
    };

    if (!setupAbs(ABS_MT_SLOT, 0, kMaxSlots - 1, 0, 0) ||
        !setupAbs(ABS_MT_POSITION_X, 0, static_cast<int32_t>(rangeX_), 0, 0) ||
        !setupAbs(ABS_MT_POSITION_Y, 0, static_cast<int32_t>(rangeY_), 0, 0) ||
        !setupAbs(ABS_MT_PRESSURE, 0, kPressureMax, 0, 0) ||
        !setupAbs(ABS_MT_TOUCH_MAJOR, 0, kTouchMajorMax, 0, 0) ||
        !setupAbs(ABS_MT_TOUCH_MINOR, 0, kTouchMajorMax, 0, 0)) {
        if (error) *error = ErrnoString("UI_ABS_SETUP");
        return false;
    }

    // --- 6. 注册设备 ---
    uinput_setup setup{};
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "%s", config.deviceName);
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x1;
    setup.id.product = 0x1;
    setup.id.version = 1;

    if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0) {
        if (error) *error = ErrnoString("UI_DEV_SETUP");
        return false;
    }
    if (ioctl(fd_, UI_DEV_CREATE) < 0) {
        if (error) *error = ErrnoString("UI_DEV_CREATE");
        return false;
    }
    deviceCreated_ = true;

    // 等待系统枚举新设备
    usleep(kSettleUs);

    ALOGI("已创建虚拟触摸屏 \"%s\"，坐标范围 %ux%u，%d 个槽位",
          config.deviceName, rangeX_ + 1, rangeY_ + 1, kMaxSlots);
    return true;
}

void UinputInjector::Close() {
    if (fd_ >= 0) {
        if (deviceCreated_) ioctl(fd_, UI_DEV_DESTROY);
        close(fd_);
        fd_ = -1;
    }
    deviceCreated_ = false;
    for (int i = 0; i < kMaxSlots; ++i) slotOwner_[i] = -1;
}

// ---------------------------------------------------------------------------
// 底层写事件
// ---------------------------------------------------------------------------

bool UinputInjector::WriteEvent(uint16_t type, uint16_t code, int32_t value,
                                std::string* error) {
    input_event ev{};
    ev.type  = type;
    ev.code  = code;
    ev.value = value;

    const ssize_t n = write(fd_, &ev, sizeof(ev));
    if (n != static_cast<ssize_t>(sizeof(ev))) {
        if (error) *error = ErrnoString("write(input_event)");
        return false;
    }
    return true;
}

bool UinputInjector::Sync(std::string* error) {
    // SYN_REPORT 是事件批的终止符。少了它，内核不会把这批事件交给 InputReader。
    return WriteEvent(EV_SYN, SYN_REPORT, 0, error);
}

// ---------------------------------------------------------------------------
// 槽位管理
// ---------------------------------------------------------------------------

int UinputInjector::FindSlot(int32_t pointerId) const {
    for (int i = 0; i < kMaxSlots; ++i) {
        if (slotOwner_[i] == pointerId) return i;
    }
    return -1;
}

int UinputInjector::AllocSlot(int32_t pointerId) {
    const int existing = FindSlot(pointerId);
    if (existing >= 0) return existing;

    for (int i = 0; i < kMaxSlots; ++i) {
        if (slotOwner_[i] < 0) {
            slotOwner_[i] = pointerId;
            return i;
        }
    }
    return -1;   // 槽位耗尽
}

void UinputInjector::ReleaseSlot(int32_t pointerId) {
    const int slot = FindSlot(pointerId);
    if (slot >= 0) slotOwner_[slot] = -1;
}

int UinputInjector::ActiveSlotCount() const {
    int n = 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (slotOwner_[i] >= 0) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// 事件发射
// ---------------------------------------------------------------------------

bool UinputInjector::Emit(int32_t action, const TouchPoint& point,
                          int64_t /*downTimeNs*/, int64_t /*eventTimeNs*/,
                          bool /*async*/, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "uinput 设备未打开";
        return false;
    }

    // 归一化后的压力与接触面积 —— 这两个值构成事件的「行为指纹」，
    // 真实触摸的它们不是常量，这里给的是简化模型。
    const int32_t pressure   = ScaleToRange(point.pressure, kPressureMax);
    const int32_t touchMajor = ScaleToRange(point.size, kTouchMajorMax);

    switch (action) {
        case kActionDown:
        case kActionPointerDown: {
            const int slot = AllocSlot(point.id);
            if (slot < 0) {
                if (error) *error = "触摸槽位耗尽（最多 " +
                                    std::to_string(kMaxSlots) + " 个指针）";
                return false;
            }

            // 第一个手指按下时才置 BTN_TOUCH
            if (ActiveSlotCount() == 1) {
                if (!WriteEvent(EV_KEY, BTN_TOUCH, 1, error)) return false;
            }

            if (!WriteEvent(EV_ABS, ABS_MT_SLOT, slot, error)) return false;
            if (!WriteEvent(EV_ABS, ABS_MT_TRACKING_ID, nextTrackingId_++, error))
                return false;
            if (!WriteEvent(EV_ABS, ABS_MT_POSITION_X, point.x, error)) return false;
            if (!WriteEvent(EV_ABS, ABS_MT_POSITION_Y, point.y, error)) return false;
            if (!WriteEvent(EV_ABS, ABS_MT_PRESSURE, pressure, error)) return false;
            if (!WriteEvent(EV_ABS, ABS_MT_TOUCH_MAJOR, touchMajor, error))
                return false;
            if (!WriteEvent(EV_ABS, ABS_MT_TOUCH_MINOR, touchMajor, error))
                return false;
            return Sync(error);
        }

        case kActionMove: {
            int slot = FindSlot(point.id);
            if (slot < 0) {
                // 没有配对的 DOWN（调用方用法有误）。退化成一次 DOWN，
                // 否则这个事件会被内核静默丢弃。
                ALOGW("pointerId=%d 收到 MOVE 但没有配对的 DOWN，按 DOWN 处理",
                      point.id);
                return Emit(kActionDown, point, 0, 0, false, error);
            }

            if (!WriteEvent(EV_ABS, ABS_MT_SLOT, slot, error)) return false;
            if (!WriteEvent(EV_ABS, ABS_MT_POSITION_X, point.x, error)) return false;
            if (!WriteEvent(EV_ABS, ABS_MT_POSITION_Y, point.y, error)) return false;
            if (!WriteEvent(EV_ABS, ABS_MT_PRESSURE, pressure, error)) return false;
            if (!WriteEvent(EV_ABS, ABS_MT_TOUCH_MAJOR, touchMajor, error))
                return false;
            return Sync(error);
        }

        case kActionUp:
        case kActionPointerUp:
        case kActionCancel: {
            // CANCEL 要抬起所有指针，UP/POINTER_UP 只抬指定的那个
            const bool releaseAll = (action == kActionCancel);

            if (releaseAll) {
                for (int i = 0; i < kMaxSlots; ++i) {
                    if (slotOwner_[i] < 0) continue;
                    if (!WriteEvent(EV_ABS, ABS_MT_SLOT, i, error)) return false;
                    if (!WriteEvent(EV_ABS, ABS_MT_TRACKING_ID, -1, error))
                        return false;
                    slotOwner_[i] = -1;
                }
            } else {
                const int slot = FindSlot(point.id);
                if (slot < 0) {
                    ALOGW("pointerId=%d 收到 UP 但没有配对的 DOWN，忽略",
                          point.id);
                    return true;
                }
                if (!WriteEvent(EV_ABS, ABS_MT_SLOT, slot, error)) return false;
                // TRACKING_ID = -1 是协议 B 里「这个槽位抬起」的表示
                if (!WriteEvent(EV_ABS, ABS_MT_TRACKING_ID, -1, error))
                    return false;
                slotOwner_[slot] = -1;
            }

            // 所有手指都抬起后才清 BTN_TOUCH
            if (ActiveSlotCount() == 0) {
                if (!WriteEvent(EV_KEY, BTN_TOUCH, 0, error)) return false;
            }
            return Sync(error);
        }

        default:
            if (error) *error = "不支持的动作: " + std::to_string(action);
            return false;
    }
}

// 后端工厂：链接哪个后端 .cpp，这里就返回哪个实例
std::unique_ptr<InjectorBackend> CreateInjectorBackend() {
    return std::make_unique<UinputInjector>();
}

}  // namespace remote_control
