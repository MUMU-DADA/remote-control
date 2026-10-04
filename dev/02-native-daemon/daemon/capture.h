// capture.h — 屏幕捕获
//
// 走 SurfaceFlinger 的 captureDisplay 路径（与 AOSP 的 screencap 命令同源）。
// 不读 /dev/graphics/fb0 —— 现代 Android 用 HWC 硬件合成，fb0 拿不到真实画面。

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace remote_control {

// 一帧截图。像素数据放在 memfd 里，可以直接把 fd 传给客户端做零拷贝读取。
struct Frame {
    int      fd     = -1;   // memfd；析构时自动 close
    uint64_t size   = 0;    // 有效字节数
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t stride = 0;    // 单位：像素（注意 stride != width）
    uint32_t format = 0;    // android PixelFormat

    Frame() = default;
    ~Frame();
    Frame(Frame&& other) noexcept;
    Frame& operator=(Frame&& other) noexcept;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    void Reset();
};

struct DisplayInfo {
    uint64_t id     = 0;
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t refreshHz = 0;
};

class Capture {
  public:
    bool Init(std::string* error);

    // 枚举所有物理显示。
    bool ListDisplays(std::vector<DisplayInfo>* out, std::string* error) const;

    // 抓取当前选定的显示。
    bool Grab(Frame* out, std::string* error);

    // 以一次请求的目标宽度抓帧，并在返回前恢复共享配置。这样调用方不需要
    // 把 SetTargetWidth() 和 Grab() 分成两个有竞态的临界区。
    bool Grab(Frame* out, std::string* error, uint32_t targetWidth);

    // 显式指定显示 ID（0 表示自动选主显示）。
    void SetDisplayId(uint64_t id);

    // 目标宽度（0 = 原始分辨率）。
    //
    // ⚠️ 这是**源头降采样**，不是事后缩放 —— SurfaceFlinger 的
    //    DisplayCaptureArgs.width/height 直接决定合成时的渲染尺寸
    //    （`reqSize = ui::Size(args.width, args.height)`，见
    //    SurfaceFlinger.cpp:5986）。所以设了它，抓帧本身就只处理
    //    目标尺寸的像素，memfd、mmap、后续编码全都跟着变小。
    //
    //    对 1440x2960 的屏幕传 720，数据量直接少 4 倍。
    //
    // 只有 SurfaceFlinger 后端认这个。screencap(exec) 后端没有尺寸
    // 参数，会忽略它（那台路上只能在事后降采样）。
    // 内联实现：这是个平凡 setter，而 Capture 的方法是按后端各实现
    // 一份的（SetDisplayId 就有三份）—— 为三行代码改三个文件不划算。
    void SetTargetWidth(uint32_t width) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        targetWidth_ = width;
    }
    uint32_t TargetWidth() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return targetWidth_;
    }

    // 立刻解析当前 displayId 并缓存。
    //
    // 公开出来是给 API 用的：客户端热改 display 之后要能**当场**知道
    // 这个 id 能不能用，而不是等下一次抓帧才失败。
    bool ResolveDisplay(std::string* error);

    // 释放缓存的 Binder 资源。析构时自动调用。
    void Shutdown();

    // 后端名，用于日志与诊断
    static const char* BackendName();

  private:
    uint64_t requestedDisplayId_ = 0;
    uint64_t activeDisplayId_    = 0;
    bool     resolved_           = false;

    // 最近一次成功抓帧的分辨率。
    //
    // screencap 这类"exec 外部命令"的后端拿不到显示枚举，
    // 只能靠抓一帧来知道真实分辨率。mutable 是因为 ListDisplays()
    // 声明成了 const，但需要读缓存。
    mutable uint32_t lastWidth_  = 0;
    mutable uint32_t lastHeight_ = 0;

    // 目标抓帧宽度（0 = 原始分辨率）。见 SetTargetWidth。
    uint32_t targetWidth_ = 0;

    // Capture 由抓帧线程、配置请求线程和显示状态请求线程共同访问。
    // recursive_mutex 允许 Grab() 在需要时调用 ResolveDisplay()，同时保持
    // 每个后端的公开入口都遵循同一把锁。
    mutable std::recursive_mutex mutex_;
};

}  // namespace remote_control
