// capture.h — 屏幕捕获
//
// 走 SurfaceFlinger 的 captureDisplay 路径（与 AOSP 的 screencap 命令同源）。
// 不读 /dev/graphics/fb0 —— 现代 Android 用 HWC 硬件合成，fb0 拿不到真实画面。

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "protocol.h"   // Request：抓帧选项借它的字段传递，见 EncodeOptions

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

// 抓帧选项：抓多大、抓哪一块。
//
// ⚠️ 这两个选项**必须在源头生效**，不能"全屏抓下来再裁"。
//    SurfaceFlinger 的 DisplayCaptureArgs 同时有 sourceCrop 与 width/height，
//    意思正是"只合成这一块、并按这个尺寸合成"：
//      · sourceCrop 决定 SF 要合成多少像素
//      · width/height 决定合成时的渲染尺寸
//    两者都设了的话是**先裁后缩**。只做后者（事后软件裁）等于白抓一大堆
//    用不上的像素，还要多一次内存拷贝。
//
//    对 1440x2960 的屏幕取中间 720x720 的一块：源头做是 0.5M 像素，
//    事后裁是 4.3M 像素抓完再扔掉 88%。
struct CaptureOptions {
    // 输出宽度（0 = 不缩放，用源尺寸）
    uint32_t targetWidth = 0;

    // 裁剪区域，**显示坐标**（layer stack 坐标系，左上角为原点）。
    // cropW/cropH 为 0 表示整屏 —— 不用另设一个 bool，
    // 因为"宽或高为 0 的区域"本来就没有意义。
    int32_t  cropX = 0;
    int32_t  cropY = 0;
    uint32_t cropW = 0;
    uint32_t cropH = 0;

    bool hasCrop() const { return cropW > 0 && cropH > 0; }

    // 校验选项。screenW/screenH 传 0 表示**屏幕尺寸未知**
    // （screencap 这类后端要抓一次才知道），此时只做语法校验，
    // 越界留给后端在真抓的时候报。
    //
    // 为什么要单独一步：越界是**调用方写错了**（该 400），
    // 而抓帧失败是运行时问题（该 500）。两者混在一起的话，
    // 客户端拿到一个 500 根本不知道该改什么。
    static bool ValidateOptions(const CaptureOptions& opt, uint32_t screenW,
                                uint32_t screenH, std::string* why) {
        // 上限与两个后端里的合理性上限保持一致，防止拿一个天文数字
        // 去申请内存（那是 500 甚至 OOM，不是 400）。
        constexpr uint32_t kMaxDimension = 16384;

        if (opt.hasCrop()) {
            if (opt.cropX < 0 || opt.cropY < 0) {
                if (why) *why = "裁剪原点不能为负（x=" + std::to_string(opt.cropX) +
                                ", y=" + std::to_string(opt.cropY) + "）";
                return false;
            }
            if (opt.cropW > kMaxDimension || opt.cropH > kMaxDimension) {
                if (why) *why = "裁剪尺寸过大（上限 " +
                                std::to_string(kMaxDimension) + "）";
                return false;
            }
            if (screenW > 0 && screenH > 0 &&
                (static_cast<uint64_t>(opt.cropX) + opt.cropW > screenW ||
                 static_cast<uint64_t>(opt.cropY) + opt.cropH > screenH)) {
                if (why) {
                    *why = "裁剪区域 " + std::to_string(opt.cropX) + "," +
                           std::to_string(opt.cropY) + " " +
                           std::to_string(opt.cropW) + "x" +
                           std::to_string(opt.cropH) + " 超出屏幕 " +
                           std::to_string(screenW) + "x" + std::to_string(screenH);
                }
                return false;
            }
        }
        if (opt.targetWidth > kMaxDimension) {
            if (why) *why = "目标宽度过大（上限 " + std::to_string(kMaxDimension) + "）";
            return false;
        }
        return true;
    }

    bool operator==(const CaptureOptions& o) const {
        return targetWidth == o.targetWidth && cropX == o.cropX &&
               cropY == o.cropY && cropW == o.cropW && cropH == o.cropH;
    }
    bool operator!=(const CaptureOptions& o) const { return !(*this == o); }
};

class Capture {
  public:
    bool Init(std::string* error);

    // 枚举所有物理显示。
    bool ListDisplays(std::vector<DisplayInfo>* out, std::string* error) const;

    // 抓取当前选定的显示。
    bool Grab(Frame* out, std::string* error);

    // 按完整选项抓帧（尺寸 + 裁剪区域）。**推荐入口** ——
    // 裁剪与缩放都在源头生效，见 CaptureOptions 的说明。
    // 每个后端各实现一份。
    bool Grab(Frame* out, std::string* error, const CaptureOptions& opt);

    // 只要目标宽度的便捷入口。
    //
    // 原来这也是一个"每后端各实现一份"的方法，但三个实现做的事情一模一样
    // （设宽度、抓、恢复），差别全在 Grab(out,error,opt) 里。收成一个转发，
    // 少三处要同步维护的代码。
    bool Grab(Frame* out, std::string* error, uint32_t targetWidth) {
        CaptureOptions opt;
        opt.targetWidth = targetWidth;
        return Grab(out, error, opt);
    }

    // ── 抓帧选项如何穿过 Request 传给 Dispatcher ──────────────────────────
    //
    // Dispatcher 与 FrameHub 之间传的是一条二进制 Request，没有地方放结构体。
    // Capture 命令用不到手势那些字段，所以借用它们：
    //
    //     x          → targetWidth
    //     y          → cropX
    //     x2         → cropY
    //     y2         → cropW
    //     durationMs → cropH
    //
    // 借字段本身没问题（字段本来就是共用的），**散落各处才是问题** ——
    // 谁少改一处就会把裁剪区域当成手势坐标。所以编码/解码只留这两个函数，
    // 其它文件一律不直接读 req.x/y/x2/y2/durationMs 的抓帧含义。
    //
    // 没有用位打包（把 w/h 塞进一个 int32）：那样能省一个字段，
    // 但会引入"超了 16 位就悄悄回绕"的失败模式，而这里是能干净避开的。
    static void EncodeOptions(const CaptureOptions& opt, Request* req) {
        req->x          = static_cast<int32_t>(opt.targetWidth);
        req->y          = opt.cropX;
        req->x2         = opt.cropY;
        req->y2         = static_cast<int32_t>(opt.cropW);
        req->durationMs = opt.cropH;
    }
    static CaptureOptions DecodeOptions(const Request& req) {
        CaptureOptions opt;
        opt.targetWidth = req.x > 0 ? static_cast<uint32_t>(req.x) : 0;
        opt.cropX       = req.y;
        opt.cropY       = req.x2;
        opt.cropW       = req.y2 > 0 ? static_cast<uint32_t>(req.y2) : 0;
        opt.cropH       = req.durationMs;
        return opt;
    }

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
