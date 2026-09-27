// capture.h — 屏幕捕获
//
// 走 SurfaceFlinger 的 captureDisplay 路径（与 AOSP 的 screencap 命令同源）。
// 不读 /dev/graphics/fb0 —— 现代 Android 用 HWC 硬件合成，fb0 拿不到真实画面。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace autod {

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

    // 显式指定显示 ID（0 表示自动选主显示）。
    void SetDisplayId(uint64_t id);

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
};

}  // namespace autod
