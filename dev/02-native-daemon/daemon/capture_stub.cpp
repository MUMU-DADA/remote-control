// capture_stub.cpp —— 截图后端：桩实现（仅供测试）
//
// 生成合成图像而不是真的抓屏。用途：
//   1. 在开发机上跑完整 daemon 的集成测试，不需要 AOSP 环境
//   2. 验证帧通道（memfd + SCM_RIGHTS + mmap）的正确性
//   3. 在没有显示设备的 CI 上冒烟测试
//
// 与其他截图后端实现同一个 Capture 接口，由 Android.bp / Makefile 选择。
// **不要编进产品镜像。**

#include "memfd_util.h"
#include "capture.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>

#include "autod_log.h"

namespace autod {
namespace {

constexpr uint32_t kStubWidth  = 1080;
constexpr uint32_t kStubHeight = 1920;
constexpr uint32_t kStubFormat = 1;   // PIXEL_FORMAT_RGBA_8888

}  // namespace

// ---------------------------------------------------------------------------
// Frame（与真实后端共用同一份实现语义）
// ---------------------------------------------------------------------------

Frame::~Frame() { Reset(); }

Frame::Frame(Frame&& other) noexcept { *this = std::move(other); }

Frame& Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        Reset();
        fd     = other.fd;
        size   = other.size;
        width  = other.width;
        height = other.height;
        stride = other.stride;
        format = other.format;
        other.fd = -1;
    }
    return *this;
}

void Frame::Reset() {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    size = 0;
    width = height = stride = format = 0;
}

// ---------------------------------------------------------------------------
// Capture（桩）
// ---------------------------------------------------------------------------

bool Capture::Init(std::string* error) {
    (void)error;
    ALOGW("使用桩截图后端 —— 生成合成图像，不是真实屏幕内容");
    resolved_ = true;
    return true;
}

void Capture::SetDisplayId(uint64_t id) { requestedDisplayId_ = id; }

void Capture::Shutdown() { resolved_ = false; }

bool Capture::ListDisplays(std::vector<DisplayInfo>* out,
                           std::string* error) const {
    (void)error;
    out->clear();
    DisplayInfo info;
    info.id        = requestedDisplayId_ ? requestedDisplayId_ : 1;
    info.width     = kStubWidth;
    info.height    = kStubHeight;
    info.refreshHz = 60;
    out->push_back(info);
    return true;
}

bool Capture::ResolveDisplay(std::string* error) {
    (void)error;
    activeDisplayId_ = requestedDisplayId_ ? requestedDisplayId_ : 1;
    resolved_ = true;
    return true;
}

bool Capture::Grab(Frame* out, std::string* error) {
    if (!resolved_ && !ResolveDisplay(error)) return false;

    const uint32_t width  = kStubWidth;
    const uint32_t height = kStubHeight;
    const uint64_t size   = static_cast<uint64_t>(width) * height * 4;

    Frame frame;
    frame.fd = MakeMemfd("autod-stub-frame");
    if (frame.fd < 0) {
        if (error) {
            *error = std::string("memfd 创建失败: ") + strerror(errno);
        }
        return false;
    }
    if (ftruncate(frame.fd, static_cast<off_t>(size)) != 0) {
        if (error) {
            *error = std::string("ftruncate 失败: ") + strerror(errno);
        }
        return false;
    }

    void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      frame.fd, 0);
    if (base == MAP_FAILED) {
        if (error) *error = std::string("mmap 失败: ") + strerror(errno);
        return false;
    }

    // 画一张可识别的合成图：
    //   R 通道 = x 方向渐变，G 通道 = y 方向渐变，B 固定
    // 这样测试可以从像素值反推坐标，验证数据没被破坏。
    auto* px = static_cast<uint8_t*>(base);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint64_t o = (static_cast<uint64_t>(y) * width + x) * 4;
            px[o + 0] = static_cast<uint8_t>(x * 255 / (width  - 1));
            px[o + 1] = static_cast<uint8_t>(y * 255 / (height - 1));
            px[o + 2] = 0x40;
            px[o + 3] = 0xFF;
        }
    }
    munmap(base, size);

    frame.size   = size;
    frame.width  = width;
    frame.height = height;
    frame.stride = width;
    frame.format = kStubFormat;

    *out = std::move(frame);
    return true;
}

// ---------------------------------------------------------------------------
// 后端标识
// ---------------------------------------------------------------------------

const char* Capture::BackendName() { return "stub(测试用，非真实屏幕)"; }

}  // namespace autod
