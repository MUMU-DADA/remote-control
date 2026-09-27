// capture.cpp — 基于 SurfaceFlinger 的屏幕捕获
//
// 【版本基准】本实现对齐 AOSP main（Android 15/16）的 API 形态，参考：
//   frameworks/base/cmds/screencap/screencap.cpp
//
// 【换版本时】Android 12/13 的 API 差异较大，见 docs/03-version-matrix.md。
// API 不匹配时编译器报错就是这个文件需要改，其它文件不受影响。

#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SyncScreenCaptureListener.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>

#include "autod_log.h"

using namespace android;

namespace autod {
namespace {

std::string ErrnoString(const char* what) {
    return std::string(what) + " 失败: " + strerror(errno) +
           " (errno=" + std::to_string(errno) + ")";
}

// 创建匿名内存文件。memfd 没有文件系统实体，可以安全地通过 SCM_RIGHTS 传递。
// 需要内核 3.17+ / Android 8+；更老的平台需要退回 ashmem。
int CreateMemFd(const char* name, uint64_t size) {
    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) {
        ALOGE("autod: memfd_create 失败: %s", strerror(errno));
        return -1;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ALOGE("autod: ftruncate(%llu) 失败: %s",
              static_cast<unsigned long long>(size), strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

// ---------------------------------------------------------------------------
// Frame
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
// Capture
// ---------------------------------------------------------------------------

bool Capture::Init(std::string* error) {
    if (!ResolveDisplay(error)) return false;
    ALOGI("autod: 使用显示 id=0x%llx",
          static_cast<unsigned long long>(activeDisplayId_));
    return true;
}

void Capture::SetDisplayId(uint64_t id) {
    requestedDisplayId_ = id;
    resolved_ = false;
}

void Capture::Shutdown() { resolved_ = false; }

bool Capture::ResolveDisplay(std::string* error) {
    // screencap 里的明确警告：多显示时顺序不保证稳定，所以必须显式选一个。
    const std::vector<PhysicalDisplayId> ids =
            SurfaceComposerClient::getPhysicalDisplayIds();

    if (ids.empty()) {
        if (error) *error = "没有找到任何物理显示";
        return false;
    }

    if (requestedDisplayId_ != 0) {
        for (const PhysicalDisplayId& candidate : ids) {
            if (candidate.value == requestedDisplayId_) {
                activeDisplayId_ = candidate.value;
                resolved_ = true;
                return true;
            }
        }
        if (error) {
            *error = "指定的显示 id 不存在: " +
                     std::to_string(requestedDisplayId_);
        }
        return false;
    }

    if (ids.size() > 1) {
        ALOGW("autod: 检测到 %zu 个显示，未指定时默认用第一个。"
              "顺序不保证稳定，生产环境请用 --display 显式指定。", ids.size());
    }
    activeDisplayId_ = ids.front().value;
    resolved_ = true;
    return true;
}

bool Capture::ListDisplays(std::vector<DisplayInfo>* out,
                           std::string* error) const {
    out->clear();
    const std::vector<PhysicalDisplayId> ids =
            SurfaceComposerClient::getPhysicalDisplayIds();
    if (ids.empty()) {
        if (error) *error = "没有找到任何物理显示";
        return false;
    }

    for (const PhysicalDisplayId& id : ids) {
        DisplayInfo info;
        info.id = id.value;

        const sp<IBinder> token = SurfaceComposerClient::getPhysicalDisplayToken(id);
        if (token != nullptr) {
            ui::DisplayMode mode;
            if (SurfaceComposerClient::getActiveDisplayMode(token, &mode) == NO_ERROR) {
                info.width     = static_cast<uint32_t>(mode.resolution.getWidth());
                info.height    = static_cast<uint32_t>(mode.resolution.getHeight());
                info.refreshHz = static_cast<uint32_t>(mode.refreshRate);
            }
        }
        out->push_back(info);
    }
    return true;
}

bool Capture::Grab(Frame* out, std::string* error) {
    if (!resolved_ && !ResolveDisplay(error)) return false;

    const DisplayId displayId = DisplayId::fromValue(activeDisplayId_);

    // gui::CaptureArgs 在 android/gui/DisplayCaptureArgs.h
    gui::CaptureArgs args;
    args.hintForSeamlessTransition = false;
    args.attachGainmap             = false;

    // 同步等待结果。注意：必须先 ProcessState::startThreadPool()，
    // 否则这里的 Binder 回调永远收不到，waitForResults() 会死等。
    sp<SyncScreenCaptureListener> listener = new SyncScreenCaptureListener();
    ScreenshotClient::captureDisplay(displayId, args, listener);

    ScreenCaptureResults result = listener->waitForResults();
    if (!result.fenceResult.ok()) {
        if (error) {
            *error = "截图失败, fence status=" +
                     std::to_string(fenceStatus(result.fenceResult));
        }
        return false;
    }

    const sp<GraphicBuffer>& buffer = result.buffer;
    if (buffer == nullptr) {
        if (error) *error = "截图返回了空 buffer";
        return false;
    }

    void* base = nullptr;
    status_t lockStatus =
            buffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN, &base);
    if (lockStatus != NO_ERROR || base == nullptr) {
        if (error) {
            *error = "GraphicBuffer::lock 失败, status=" +
                     std::to_string(lockStatus);
        }
        return false;
    }

    const uint32_t width  = buffer->getWidth();
    const uint32_t height = buffer->getHeight();
    const uint32_t stride = buffer->getStride();          // 像素
    const uint32_t format = buffer->getPixelFormat();
    const uint32_t bpp    = bytesPerPixel(format);
    const uint64_t rowBytes  = static_cast<uint64_t>(width) * bpp;
    const uint64_t totalSize = rowBytes * height;

    Frame frame;
    frame.fd = CreateMemFd("autod-frame", totalSize);
    if (frame.fd < 0) {
        // lock 之后必须 unlock，否则 SurfaceFlinger 的缓冲区会被耗尽
        buffer->unlock();
        if (error) *error = ErrnoString("memfd_create");
        return false;
    }

    void* dst = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                     frame.fd, 0);
    if (dst == MAP_FAILED) {
        buffer->unlock();
        if (error) *error = ErrnoString("mmap");
        return false;
    }

    // 逐行拷贝：必须用 stride 而不是 width 做步长，否则图像会斜切
    const uint8_t* src = static_cast<const uint8_t*>(base);
    uint8_t* dstBytes  = static_cast<uint8_t*>(dst);
    const uint64_t srcStride = static_cast<uint64_t>(stride) * bpp;
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(dstBytes + y * rowBytes, src + y * srcStride, rowBytes);
    }

    munmap(dst, totalSize);
    buffer->unlock();

    frame.size   = totalSize;
    frame.width  = width;
    frame.height = height;
    frame.stride = width;   // 我们做了紧凑拷贝，对客户端暴露的 stride 就是 width
    frame.format = format;

    *out = std::move(frame);
    return true;
}

// ---------------------------------------------------------------------------
// 后端标识
// ---------------------------------------------------------------------------

const char* Capture::BackendName() { return "surfaceflinger"; }

}  // namespace autod

