// capture_surfaceflinger.cpp —— 基于 SurfaceFlinger 的屏幕捕获
//
// 【版本基准】本实现**对齐 Android 12**（android-12.0.0_r34），API 形态已逐项
// 对照以下 AOSP 源码核实过：
//
//   frameworks/native/libs/gui/include/gui/LayerState.h            DisplayCaptureArgs
//   frameworks/native/libs/gui/include/gui/SurfaceComposerClient.h ScreenshotClient
//   frameworks/native/libs/gui/include/gui/SyncScreenCaptureListener.h
//   frameworks/native/libs/gui/include/gui/ScreenCaptureResults.h
//   frameworks/native/libs/ui/include/ui/DisplayId.h               PhysicalDisplayId
//   frameworks/native/libs/ui/include/ui/DisplayMode.h             ui::DisplayMode
//
// Android 12 与 15/16 的差异很大，换版本时见 docs/03-version-matrix.md。

#include "memfd_util.h"
#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gui/ISurfaceComposer.h>
#include <gui/LayerState.h>              // DisplayCaptureArgs / CaptureArgs
#include <gui/ScreenCaptureResults.h>    // gui::ScreenCaptureResults
#include <gui/SurfaceComposerClient.h>   // SurfaceComposerClient / ScreenshotClient
#include <gui/SyncScreenCaptureListener.h>
#include <ui/DisplayId.h>                // PhysicalDisplayId
#include <ui/DisplayState.h>             // ui::DisplayState（逻辑尺寸）
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>

#include "remote_control_log.h"

using namespace android;

namespace remote_control {
namespace {

std::string ErrnoString(const char* what) {
    return std::string(what) + " 失败: " + strerror(errno) +
           " (errno=" + std::to_string(errno) + ")";
}

// 创建匿名内存文件。memfd 没有文件系统实体，可以安全地通过 SCM_RIGHTS 传递。
int CreateMemFd(const char* name, uint64_t size) {
    int fd = MakeMemfd(name);
    if (fd < 0) {
        ALOGE("memfd 创建失败: %s", strerror(errno));
        return -1;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ALOGE("ftruncate(%llu) 失败: %s",
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
    ALOGI("使用显示 id=0x%llx",
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
        ALOGW("检测到 %zu 个显示，未指定时默认用第一个。"
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
        // 枚举不到显示（比如 SF 还没准备好）：用上一次抓帧缓存的尺寸兜底。
        // 这比直接报错好 —— 调用方（Info 请求、触控范围计算）需要的是
        // "屏幕多大"，而不是"SF 现在枚举不出来"。
        if (lastWidth_ != 0 && lastHeight_ != 0) {
            DisplayInfo info;
            info.id     = requestedDisplayId_ ? requestedDisplayId_ : 1;
            info.width  = lastWidth_;
            info.height = lastHeight_;
            out->push_back(info);
            return true;
        }
        if (error) *error = "没有找到任何物理显示";
        return false;
    }

    for (const PhysicalDisplayId& id : ids) {
        DisplayInfo info;
        info.id = id.value;

        // Android 12：getPhysicalDisplayToken 按值收 PhysicalDisplayId
        const sp<IBinder> token =
                SurfaceComposerClient::getPhysicalDisplayToken(id);
        if (token != nullptr) {
            ui::DisplayMode mode;
            if (SurfaceComposerClient::getActiveDisplayMode(token, &mode) ==
                NO_ERROR) {
                info.refreshHz = static_cast<uint32_t>(mode.refreshRate);
            }
            // 报**逻辑**尺寸，跟 Grab 真正抓到的一致；面板物理模式在
            // wm size 覆盖生效时并不等于抓帧尺寸。
            ui::DisplayState state;
            if (SurfaceComposerClient::getDisplayState(token, &state) ==
                NO_ERROR) {
                info.width =
                        static_cast<uint32_t>(state.layerStackSpaceRect.getWidth());
                info.height =
                        static_cast<uint32_t>(state.layerStackSpaceRect.getHeight());
            }
        }
        out->push_back(info);
    }
    return true;
}

bool Capture::Grab(Frame* out, std::string* error) {
    if (!resolved_ && !ResolveDisplay(error)) return false;

    const PhysicalDisplayId displayId{activeDisplayId_};
    const sp<IBinder> token =
            SurfaceComposerClient::getPhysicalDisplayToken(displayId);
    if (token == nullptr) {
        if (error) *error = "拿不到显示 token";
        return false;
    }

    // Android 12：DisplayCaptureArgs 继承 CaptureArgs，定义在 gui/LayerState.h
    DisplayCaptureArgs args;
    args.displayToken = token;
    // 源头降采样：SF 会按这个尺寸**合成**，而不是合成完再缩。
    // 见 SurfaceFlinger.cpp:5986 `ui::Size reqSize(args.width, args.height)`。
    //
    // 先问出显示的**逻辑**尺寸：按宽高比算目标高度要用它，而且
    // 只有"确实变小了"才值得让 SF 换渲染尺寸。
    //
    // ⚠️ 这里要的是 getDisplayState().layerStackSpaceRect，不是
    // getActiveDisplayMode().resolution。前者才是 SF 在
    // `args.width/height == 0` 时用的默认尺寸：
    //
    //     reqSize = display->getLayerStackSpaceRect().getSize();
    //     —— SurfaceFlinger.cpp:5987
    //
    // 两者在 `wm size` / `wm density` 覆盖生效时会不一致（物理模式仍是
    // 面板原生分辨率，layer stack 跟随覆盖后的逻辑尺寸）。用错的话
    // 高宽比会算错，画面会被拉变形。
    uint32_t srcW = 0, srcH = 0;
    {
        ui::DisplayState state;
        if (SurfaceComposerClient::getDisplayState(token, &state) == NO_ERROR) {
            srcW = static_cast<uint32_t>(state.layerStackSpaceRect.getWidth());
            srcH = static_cast<uint32_t>(state.layerStackSpaceRect.getHeight());
        }
    }

    // 宽度给了就按宽高比算高度（不能只给一个 —— SF 只在两个都为 0 时
    // 才用原始分辨率，否则按给的来，只给 width 会让高度为 0）。
    if (targetWidth_ > 0 && srcW > 0 && targetWidth_ < srcW) {
        args.width  = targetWidth_;
        args.height = static_cast<uint32_t>(
                static_cast<uint64_t>(srcH) * targetWidth_ / srcW);
        if (args.height == 0) args.height = 1;
    } else {
        args.width  = 0;      // 0 = 用显示原始分辨率
        args.height = 0;
    }
    args.pixelFormat      = ui::PixelFormat::RGBA_8888;
    args.captureSecureLayers = false;
    args.allowProtected   = false;
    args.grayscale        = false;
    // UNKNOWN = 用显示自身的色彩空间（默认行为，也是 screencap 的做法）
    args.dataspace        = ui::Dataspace::UNKNOWN;

    // 同步等待。注意 waitForResults() 内部已经调了 fence->waitForever()，
    // 返回时数据一定可读。但前提是 Binder 线程池已启动 —— 见 main.cpp。
    sp<SyncScreenCaptureListener> listener = new SyncScreenCaptureListener();

    const status_t captureStatus =
            ScreenshotClient::captureDisplay(args, listener);
    if (captureStatus != NO_ERROR) {
        if (error) {
            *error = "ScreenshotClient::captureDisplay 失败, status=" +
                     std::to_string(captureStatus);
        }
        return false;
    }

    const gui::ScreenCaptureResults result = listener->waitForResults();

    // Android 12 用 status_t result；Android 15+ 改成了 ftl::Expected fenceResult
    if (result.result != OK) {
        if (error) {
            *error = "截图失败, result=" + std::to_string(result.result);
        }
        return false;
    }

    const sp<GraphicBuffer>& buffer = result.buffer;
    if (buffer == nullptr) {
        if (error) *error = "截图返回了空 buffer";
        return false;
    }

    void* base = nullptr;
    const status_t lockStatus =
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
    frame.fd = CreateMemFd("remote-control-frame", totalSize);
    if (frame.fd < 0) {
        // lock 之后必须 unlock，否则 SurfaceFlinger 的缓冲区会被耗尽
        buffer->unlock();
        if (error) *error = ErrnoString("memfd 创建");
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
    const auto* src      = static_cast<const uint8_t*>(base);
    auto*       dstBytes = static_cast<uint8_t*>(dst);
    const uint64_t srcStride = static_cast<uint64_t>(stride) * bpp;
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(dstBytes + y * rowBytes, src + y * srcStride, rowBytes);
    }

    munmap(dst, totalSize);
    buffer->unlock();

    frame.size   = totalSize;
    frame.width  = width;
    frame.height = height;
    // 同步更新缓存，和 screencap 后端保持一致。
    // 这不只是"让 -Werror 闭嘴"：ListDisplays() 拿不到实时信息时会用它兜底，
    // 两个后端行为一致才不会出现"换个后端尺寸就变 0"这种怪事。
    lastWidth_   = width;
    lastHeight_  = height;
    frame.stride = width;   // 我们做了紧凑拷贝，对客户端暴露的 stride 就是 width
    frame.format = format;

    *out = std::move(frame);
    return true;
}

// ---------------------------------------------------------------------------
// 后端标识
// ---------------------------------------------------------------------------

const char* Capture::BackendName() { return "surfaceflinger"; }

}  // namespace remote_control
