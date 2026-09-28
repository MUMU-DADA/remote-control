// webp_encoder.h — WebP 编码（内置 libwebp）
//
// 为什么需要内置：
//
//   设备上没有 `libwebp.so`（实测 Android 12 上就没有，Skia 里也没找到），
//   而 `AndroidBitmap_compress` 是 **API 30** 才有的。所以 Android 8~10
//   完全没有 WebP 编码能力 —— 除非把 libwebp 编进来。
//
// 和 jpeg_encoder 的区别：
//
//   JPEG 那边是 dlopen **系统已有的** libjpeg（设备上一直有，VNDK 稳定 ABI），
//   只 vendor 了头文件。WebP 这边没有系统库可用，所以是**完整的源码内置**。
//
// 参数选择（实测，见 tools/bench/webp_bench.cpp）：
//
//   method       320x480     1080p      相对 Skia 默认(m=3)
//   ─────────────────────────────────────────────────────
//   0             3.3 ms     24.8 ms     快 3.1x，体积 +22%
//   2             5.2 ms     38.8 ms     快 2.0x，体积 +3.6%   ← 默认
//   3 (Skia)     10.3 ms     78.5 ms     —
//   6            21.8 ms    126.5 ms     慢 2.1x，体积 -5.8%
//
//   **method=2 是甜点**：只比最强的差 3.6% 体积，却快一倍。
//   Skia 用 3 是为了跟 Chrome 对齐，不是因为它最优。
//
//   `thread_level=1` 实测只快 5% —— libwebp 的线程只并行熵编码那一小段，
//   画面流这种尺寸不值得为它引入线程池。所以固定单线程。

#pragma once

#include <cstdint>
#include <string>

namespace autod {

class WebpEncoder {
  public:
    static WebpEncoder& Instance();

    // 纯内置，没有"加载失败"这回事 —— 只要编进来了就能用。
    // 保留这个签名是为了和 JpegEncoder/PngEncoder 一致。
    bool Init(std::string* error);

    bool Available() const { return true; }

    const char* BackendName() const { return "libwebp（内置）"; }

    // 编码 RGBA8888 → WebP（有损）。
    //
    // quality 1-100。method 0-6，越高质量越好、越慢；默认 2。
    // 失败返回空串。
    std::string EncodeRgba(const uint8_t* rgba, uint32_t width, uint32_t height,
                           int quality, int method, std::string* error);

  private:
    WebpEncoder() = default;
    WebpEncoder(const WebpEncoder&) = delete;
    WebpEncoder& operator=(const WebpEncoder&) = delete;
};

}  // namespace autod
