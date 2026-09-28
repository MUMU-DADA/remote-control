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

namespace remote_control {

enum class ImageFormat {
    kAuto,   // 选当前平台最好的（设备上 JPEG，主机上 PNG）
    kPng,
    kJpeg,
    kWebp,
    kRaw,    // 不编码，直接给原始像素（调用方自己处理）

    // H.264（走设备端 MediaCodec）。
    //
    // 和上面几个有本质区别：H.264 是**有状态**的（SPS/PPS 只发一次、
    // 帧间参考），所以不能用这个类来编 —— 它每次调用都是独立的。
    // 流式路径自己持有 H264Encoder（每个流一个）。
    // 这个枚举值只是让"格式解析/能力上报"有个统一的说法。
    //
    // 也**不能走 MJPEG** —— multipart 里装不下带帧间依赖的流。
    // 只支持 WebSocket。
    kH264,
};

// 每种格式的 quality 取值范围。
//
// ⚠️ **唯一的来源**。三个地方都用它：
//     - `/api/v1/params` 上报给客户端（网页的拖动条按它设范围）
//     - 请求解析时钳位（`?quality=`）
//     - WebSocket 的 {"t":"quality"} 命令
//
// 各写一份的话迟早会出现"接口说支持 1-100、实际只认到 9"这种不一致 ——
// 而用户看到的是"拖了没反应"。实测踩过：h264 被当成 PNG 钳到 9，
// 拖动条拖到 75 实际按 9 走。
//
// 量纲本来就不同：PNG 是 zlib 压缩级别（1-9），JPEG/WebP 是图像质量
// （1-100），H.264 会被换算成码率（也是 1-100 的量纲）。
struct QualityRange {
    int min = 1;
    int max = 100;
    int def = 75;
};

QualityRange QualityRangeFor(ImageFormat f);

class ImageEncoder {
  public:
    static ImageEncoder& Instance();

    // 探测可用的编码器。失败不致命 —— 会退回 PNG。
    bool Init(std::string* error);

    // 设备上是否能用 AndroidBitmap_compress（能用就有 JPEG/WebP）
    //
    // ⚠️ 这是**运行时探测**的结果，不是编译期常量。
    //    Android 11+ 为 true，Android 8~10 为 false。
    bool hasNativeCodecs() const { return native_; }

    // 某个格式在这台设备上能不能编。
    //
    // 客户端应该用它来决定"要不要提供这个选项"，而不是假设 ——
    // Android 8~10 上没有 WebP，硬发 format=webp 只会拿到一个错误。
    bool Supports(ImageFormat f) const;

    // 当前实际在用的编码器，给 /describe 和日志展示用
    std::string BackendSummary() const;

    // 是否被 REMOTE_CONTROL_FORCE_FALLBACK=1 强制走了回退路径。
    //
    // 暴露出来是为了**别把强制的结果当成设备真相** ——
    // 一个开着这个变量的实例，它的 codecs 和能力都不代表这台设备。
    bool FallbackForced() const;

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

}  // namespace remote_control
