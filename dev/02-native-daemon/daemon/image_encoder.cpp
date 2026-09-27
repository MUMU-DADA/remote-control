// image_encoder.cpp

#include "image_encoder.h"

#include <string.h>

#include "autod_log.h"
#include "png_encoder.h"

#ifdef __ANDROID__
#include <android/bitmap.h>
#include <android/data_space.h>
#endif

namespace autod {
namespace {

#ifdef __ANDROID__

// AndroidBitmap_compress 可能会**多次**调用写回调
// （"May be called more than once for each call to this method"），
// 所以回调里必须是追加，不能覆盖。
struct Sink {
    std::string* out;
    bool         failed = false;
};

bool WriteCb(void* ctx, const void* data, size_t size) {
    auto* sink = static_cast<Sink*>(ctx);
    // 不包 try/catch：AOSP 是 -fno-exceptions，写了也编不过
    // （实测被 -Werror 拦下）。这里唯一可能"抛"的是 append 的
    // bad_alloc，而 -fno-exceptions 下它本来就是 abort，
    // catch 也救不回来。真失败靠返回值表达。
    sink->out->append(static_cast<const char*>(data), size);
    return true;
}

int32_t ToAndroidFormat(ImageFormat f) {
    switch (f) {
        case ImageFormat::kJpeg: return ANDROID_BITMAP_COMPRESS_FORMAT_JPEG;
        case ImageFormat::kWebp: return ANDROID_BITMAP_COMPRESS_FORMAT_WEBP_LOSSY;
        default:                 return ANDROID_BITMAP_COMPRESS_FORMAT_PNG;
    }
}

#endif  // __ANDROID__

}  // namespace

ImageEncoder& ImageEncoder::Instance() {
    static ImageEncoder enc;
    return enc;
}

bool ImageEncoder::ParseFormat(const std::string& name, ImageFormat* out) {
    if (name == "auto" || name.empty()) { *out = ImageFormat::kAuto; return true; }
    if (name == "png")                 { *out = ImageFormat::kPng;  return true; }
    if (name == "jpeg" || name == "jpg") { *out = ImageFormat::kJpeg; return true; }
    if (name == "webp")                { *out = ImageFormat::kWebp; return true; }
    if (name == "raw")                 { *out = ImageFormat::kRaw;  return true; }
    return false;
}

const char* ImageEncoder::Name(ImageFormat f) {
    switch (f) {
        case ImageFormat::kPng:  return "png";
        case ImageFormat::kJpeg: return "jpeg";
        case ImageFormat::kWebp: return "webp";
        case ImageFormat::kRaw:  return "raw";
        default:                 return "auto";
    }
}

const char* ImageEncoder::MimeType(ImageFormat f) {
    switch (f) {
        case ImageFormat::kPng:  return "image/png";
        case ImageFormat::kJpeg: return "image/jpeg";
        case ImageFormat::kWebp: return "image/webp";
        default:                 return "application/octet-stream";
    }
}

ImageFormat ImageEncoder::BestFormat() const {
    // 设备上有 Skia 的编码器就用 JPEG —— 对画面流来说，
    // "小一个数量级"比"无损"重要得多。
    return native_ ? ImageFormat::kJpeg : ImageFormat::kPng;
}

bool ImageEncoder::Init(std::string* error) {
#ifdef __ANDROID__
    native_ = true;
    ALOGI("图像编码器: AndroidBitmap_compress（JPEG / WebP / PNG）");
    return true;
#else
    // 主机上没有 AndroidBitmap_compress。退回 dlopen zlib 的 PNG ——
    // 只为让主机上的测试能跑，设备上永远走上面那条。
    native_ = false;
    if (!PngEncoder::Instance().Init(error)) return false;
    ALOGI("图像编码器: PNG(dlopen zlib) —— 主机回退路径，无 JPEG/WebP");
    return true;
#endif
}

std::string ImageEncoder::Encode(const uint8_t* rgba, uint32_t width,
                                 uint32_t height, ImageFormat format,
                                 int quality, std::string* error) {
    if (rgba == nullptr || width == 0 || height == 0) {
        if (error) *error = "空图像";
        return {};
    }
    if (format == ImageFormat::kAuto) format = BestFormat();

#ifdef __ANDROID__
    if (format == ImageFormat::kJpeg || format == ImageFormat::kWebp ||
        format == ImageFormat::kPng) {
        AndroidBitmapInfo info;
        memset(&info, 0, sizeof(info));
        info.width  = width;
        info.height = height;
        info.stride = width * 4;
        info.format = ANDROID_BITMAP_FORMAT_RGBA_8888;
        // 屏幕截图是不透明的，但帧里 alpha 恒为 255。
        // 标成 PREMUL 而不是 UNPREMUL：JPEG 会丢掉 alpha，
        // 而 PNG 路径下 PREMUL 对 alpha=255 的结果和不透明一致。
        info.flags  = ANDROID_BITMAP_FLAGS_ALPHA_PREMUL;

        Sink sink;
        std::string out;
        sink.out = &out;

        // PNG 的 quality 语义是 zlib 级别，而 AndroidBitmap 期望 0-100。
        // 不换算的话 level=1 会被当成"质量 1"编出一张糊图。
        int q = quality;
        if (format == ImageFormat::kPng) {
            if (q < 1 || q > 9) q = 1;
            // zlib 1..9 → Skia 质量。Skia 的 PNG 编码器把 quality 当
            // "压缩努力程度"，0-100 线性映射到 zlib 级别就够了。
            q = q * 100 / 9;
            if (q < 1) q = 1;
        } else if (q <= 0 || q > 100) {
            q = (format == ImageFormat::kWebp) ? 80 : 75;
        }

        const int rc = AndroidBitmap_compress(
                &info, ADATASPACE_SRGB, rgba, ToAndroidFormat(format), q,
                &sink, &WriteCb);

        if (rc != ANDROID_BITMAP_RESULT_SUCCESS || sink.failed) {
            if (error) {
                *error = std::string("AndroidBitmap_compress 失败: rc=") +
                         std::to_string(rc);
            }
            return {};
        }
        if (out.empty()) {
            if (error) *error = "编码器返回了空数据";
            return {};
        }
        return out;
    }
#else
    (void)quality;
#endif

    // PNG 兜底：主机上走 dlopen zlib 的那条路
    int level = quality;
    if (level < 1 || level > 9) level = 1;
    return PngEncoder::Instance().EncodeRgba(rgba, width, height, level, error);
}

}  // namespace autod
