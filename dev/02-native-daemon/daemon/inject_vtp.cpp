// inject_vtp.cpp —— 基于 AOSP 官方 virtual_touchpad 的注入后端
//
// =============================================================================
// 为什么用这个后端（而不是我们自己写 uinput）
// =============================================================================
//
// AOSP 里已经有官方的「虚拟触摸屏」实现：
//
//   frameworks/native/services/vr/virtual_touchpad/
//   ├── EvdevInjector.{h,cpp}         通用 uinput 封装
//   ├── VirtualTouchpadEvdev.{h,cpp}  造设备 + 事件合成
//   ├── VirtualTouchpadService.cpp    Binder 服务
//   ├── VirtualTouchpadClient.cpp     Binder 客户端 ★ 本文件用它
//   ├── idc/vr-virtual-touchpad-0.idc 声明 touch.deviceType = touchScreen
//   └── virtual_touchpad.rc           service，user system / group system input uhid
//
// 它的设备配置和我们自己写的那套是等价的（INPUT_PROP_DIRECT + BTN_TOUCH +
// 多指 ABS_MT_*），而且多了一份 .idc 让 Android 明确按 touchScreen 分类。
//
// -----------------------------------------------------------------------------
// 用它的两个实质好处
// -----------------------------------------------------------------------------
//
// 1. **autod 不再需要 /dev/uinput 权限**
//    自己写 uinput 时，autod 需要 uhid 组 + SELinux 的 uhid_device 访问权。
//    走官方服务的话，autod 只需要能 find 到 virtual_touchpad 服务：
//        binder_call(autod, virtual_touchpad)
//        allow autod virtual_touchpad_service:service_manager find
//
// 2. **少维护 ~350 行 uinput 代码**（设备配置、槽位管理、协议 B 时序）
//
// -----------------------------------------------------------------------------
// 代价与限制
// -----------------------------------------------------------------------------
//
// - 设备只有 **2 个触控槽位**（kTouchpads = 2），做不了 3 指以上手势。
//   单指点击/滑动/双指缩放都够用。
// - 坐标是归一化浮点 [0.0, 1.0)，需要自己换算。
// - 需要 virtual_touchpad 服务在跑：
//      PRODUCT_PACKAGES += virtual_touchpad
//   它默认**不在** aosp_arm64 的产品包里（是 VR 专用），要显式加。
// - 多一次 Binder 往返（约 0.3-1 ms），仍远快于 adb。
//
// -----------------------------------------------------------------------------
// 启用方式
// -----------------------------------------------------------------------------
//
// 本文件默认不参与编译。要启用：
//   1. daemon/Android.bp 里把 autod 的 inject_uinput.cpp 换成 inject_vtp.cpp
//   2. shared_libs 加 "libvirtualtouchpadclient"，去掉不需要的
//   3. 产品配置里加 PRODUCT_PACKAGES += virtual_touchpad
//   4. sepolicy 按上面第 1 条改（autod.te 里有说明）
//
// =============================================================================

#include <cmath>
#include <cstring>

// 官方导出头（libvirtualtouchpadclient 的 export_include_dirs = ["include"]）
#include <VirtualTouchpad.h>
#include <VirtualTouchpadClient.h>

#include <utils/Errors.h>

#include "autod_log.h"
#include "inject_backend.h"

using android::dvr::VirtualTouchpad;
using android::dvr::VirtualTouchpadClient;

namespace autod {
namespace {

// 官方实现固定 2 个 touchpad（VirtualTouchpadEvdev::kTouchpads）
constexpr int kMaxPointers = 2;

}  // namespace

class VirtualTouchpadInjector final : public InjectorBackend {
  public:
    const char* Name() const override { return "virtual_touchpad(AOSP 官方)"; }

    bool Open(const InjectorConfig& config, std::string* error) override;
    bool Emit(int32_t action, const TouchPoint& point, int64_t downTimeNs,
              int64_t eventTimeNs, bool async, std::string* error) override;
    void Close() override;

  private:
    // 把像素坐标换算成官方要的归一化 [0,1)
    void ToNormalized(const TouchPoint& point, float* x, float* y) const;

    std::unique_ptr<VirtualTouchpad> touchpad_;
    uint32_t rangeW_ = 0;
    uint32_t rangeH_ = 0;

    // 记录哪些 pointer 当前是按下的 —— 抬起时要补发 pressure=0
    bool down_[kMaxPointers] = {false, false};
};

// ---------------------------------------------------------------------------
// 打开
// ---------------------------------------------------------------------------

bool VirtualTouchpadInjector::Open(const InjectorConfig& config,
                                   std::string* error) {
    rangeW_ = config.touchWidth;
    rangeH_ = config.touchHeight;

    if (rangeW_ == 0 || rangeH_ == 0) {
        if (error) {
            *error = "virtual_touchpad 后端需要显式指定触控范围"
                     "（--touch-range WxH）—— 坐标要归一化，没有范围就没法换算";
        }
        return false;
    }

    // Create() 只是构造客户端对象；Attach() 才去连服务
    touchpad_ = VirtualTouchpadClient::Create();
    if (!touchpad_) {
        if (error) {
            *error = "VirtualTouchpadClient::Create() 返回空 "
                     "—— 检查是否链接了 libvirtualtouchpadclient";
        }
        return false;
    }

    const android::status_t st = touchpad_->Attach();
    if (st != android::OK) {
        if (error) {
            *error = "attach() 失败 (status=" + std::to_string(st) + ") "
                     "—— 确认 virtual_touchpad 服务在跑："
                     "ps -A | grep virtual_touchpad";
        }
        touchpad_.reset();
        return false;
    }

    ALOGI("已连接 virtual_touchpad 服务，坐标范围 %ux%u，最多 %d 个指针",
          rangeW_, rangeH_, kMaxPointers);
    return true;
}

void VirtualTouchpadInjector::Close() {
    if (touchpad_) {
        touchpad_->Detach();
        touchpad_.reset();
    }
}

// ---------------------------------------------------------------------------
// 事件发射
// ---------------------------------------------------------------------------

void VirtualTouchpadInjector::ToNormalized(const TouchPoint& point,
                                           float* x, float* y) const {
    // 官方要求 [0.0, 1.0)，所以上界要减一，并且夹紧防止越界
    float nx = static_cast<float>(point.x) / static_cast<float>(rangeW_);
    float ny = static_cast<float>(point.y) / static_cast<float>(rangeH_);

    if (nx < 0.0f) nx = 0.0f;
    if (ny < 0.0f) ny = 0.0f;
    if (nx >= 1.0f) nx = 0.9999f;
    if (ny >= 1.0f) ny = 0.9999f;

    *x = nx;
    *y = ny;
}

bool VirtualTouchpadInjector::Emit(int32_t action, const TouchPoint& point,
                                   int64_t /*downTimeNs*/,
                                   int64_t /*eventTimeNs*/, bool /*async*/,
                                   std::string* error) {
    if (!touchpad_) {
        if (error) *error = "virtual_touchpad 后端未初始化";
        return false;
    }

    // 官方只有 2 个 touchpad，用 pointerId 取模映射
    const int slot = point.id % kMaxPointers;

    float x = 0.0f;
    float y = 0.0f;
    ToNormalized(point, &x, &y);

    // pressure 语义：> 0 表示接触，0 表示离开。
    // 官方的实现里这个字段同时承担 touch down/up 的语义，
    // 所以抬起动作必须发 pressure = 0。
    float pressure = point.pressure;
    if (pressure <= 0.0f) pressure = 1.0f;   // 调用方没给就用 1.0

    android::status_t st = android::OK;

    switch (action) {
        case kActionDown:
        case kActionPointerDown:
            down_[slot] = true;
            st = touchpad_->Touch(slot, x, y, pressure);
            break;

        case kActionMove:
            // 没有配对的 DOWN 时，官方实现会返回 ERROR_SEQUENCING。
            // 这里补一次 down，避免整条手势失效。
            if (!down_[slot]) {
                ALOGW("pointerId=%d 收到 MOVE 但没有配对的 DOWN，按 DOWN 处理",
                      point.id);
                down_[slot] = true;
            }
            st = touchpad_->Touch(slot, x, y, pressure);
            break;

        case kActionUp:
        case kActionPointerUp:
            down_[slot] = false;
            st = touchpad_->Touch(slot, x, y, 0.0f);   // 0 = 抬起
            break;

        case kActionCancel:
            // 抬起所有还在按下的指针
            for (int i = 0; i < kMaxPointers; ++i) {
                if (!down_[i]) continue;
                down_[i] = false;
                const android::status_t s = touchpad_->Touch(i, x, y, 0.0f);
                if (s != android::OK && st == android::OK) st = s;
            }
            break;

        default:
            if (error) *error = "不支持的动作: " + std::to_string(action);
            return false;
    }

    if (st != android::OK) {
        if (error) {
            *error = "VirtualTouchpad::Touch 失败 (status=" +
                     std::to_string(st) + ")";
        }
        return false;
    }
    return true;
}

// 后端工厂：链接哪个后端 .cpp，这里就返回哪个实例
std::unique_ptr<InjectorBackend> CreateInjectorBackend() {
    return std::make_unique<VirtualTouchpadInjector>();
}

}  // namespace autod
