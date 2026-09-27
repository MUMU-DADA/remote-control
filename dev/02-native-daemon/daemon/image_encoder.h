// image_encoder.h — 画面编码（JPEG / WebP / PNG）
//
// 为什么需要它，而不是继续只用 PNG：
//
//   PNG 是**无损**的，一帧 320x480 要 100+ KB。对"看画面、点坐标"
//   这个用途完全不划算 —— 实测：
//       PNG  quality=1   116 KB/帧   3.4 MB/s @30fps
//       PNG  quality=6    91 KB/帧   2.5 MB/s @30fps
//   压缩级别从 1 提到 6 只省 22%，却把帧率从 30 拖到 27.5。
//   PNG 是给"存一张图"设计的，不是给"传一路流"设计的。
//
//   JPEG/WebP 有损，但小一个数量级，浏览器解码也快得多。
//
// 实现走 NDK 的 AndroidBitmap_compress（Skia 内部实现）：
//   - 不用带 jpeglib.h —— 那个结构体布局很大且随版本变，
//     NDK 里没有头文件，自己声明等于赌 ABI
//   - 写回调直接收进 std::string，连 memfd 都省了
//   - 同一套代码顺带支持 WebP，比 JPEG 还小
//
// 主机上没有 AndroidBitmap_compress，退回 png_encoder（dlopen zlib）。

#pragma once

#include <cstdint>
#include <string>

namespace autod {

enum class ImageFormat {
    kAuto,   // 选当前平台最好的（设备上 JPEG，主机上 PNG）
    kPng,
    kJpeg,
    kWebp,
    kRaw,    // 不编码，直接给原始像素（调用方自己处理）
};

class ImageEncoder {
  public:
    static ImageEncoder& Instance();

    // 探测可用的编码器。失败不致命 —— 会退回 PNG。
    bool Init(std::string* error);

    // 设备上是否能用 AndroidBitmap_compress（能用就有 JPEG/WebP）
    bool hasNativeCodecs() const { return native_; }

    // 编码。rgba 必须是 RGBA8888（和 capture 的输出一致）。
    //
    // quality 含义随格式变：JPEG/WebP 是 0-100（默认 75/80），
    // PNG 是 zlib 级别 1-9（默认 1，快优先）。
    // 失败返回空字符串，原因写进 error。
    std::string Encode(const uint8_t* rgba, uint32_t width, uint32_t height,
                       ImageFormat format, int quality, std::string* error);

    // 解析 "jpeg" / "jpg" / "webp" / "png" / "auto"
    static bool ParseFormat(const std::string& name, ImageFormat* out);
    static const char* Name(ImageFormat f);
    static const char* MimeType(ImageFormat f);

    // 自动选格式时实际会用的那个
    ImageFormat BestFormat() const;

  private:
    ImageEncoder() = default;

    bool native_ = false;
};

}  // namespace autod
