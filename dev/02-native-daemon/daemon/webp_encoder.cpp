// webp_encoder.cpp — 用内置的 libwebp 编码

#include "webp_encoder.h"

#include <vector>

#include "autod_log.h"

// 内置的 libwebp（源码在 vendor/webp/，来源与取舍见那个目录的 README）。
//
// 源码统一写 `#include "src/webp/…"`，所以 -I 要指到 vendor/webp/ ——
// 和 AOSP 的 local_include_dirs: ["."] 一致。
#include "src/webp/encode.h"

namespace autod {

WebpEncoder& WebpEncoder::Instance() {
    static WebpEncoder enc;
    return enc;
}

bool WebpEncoder::Init(std::string* /*error*/) {
    // 内置的，没有可失败的地方。
    //
    // libwebp 会为 SIMD 分发做一次 CPU 特性探测（VP8GetCPUInfo），
    // 那是惰性的，第一次编码时自己做，不用在这里初始化。
    return true;
}

std::string WebpEncoder::EncodeRgba(const uint8_t* rgba, uint32_t width,
                                    uint32_t height, int quality, int method,
                                    std::string* error) {
    if (rgba == nullptr || width == 0 || height == 0) {
        if (error) *error = "空图像";
        return {};
    }
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    if (method < 0) method = 0;
    if (method > 6) method = 6;

    // 用完整的 WebPConfig/WebPPicture 流程，而不是一句话的
    // WebPEncodeRGBA —— 后者不给机会设 method，而 method 正是
    // 让内置这件事划得来的关键（默认 4，比 2 慢一倍还只小 0.1%）。
    WebPConfig cfg;
    if (!WebPConfigInit(&cfg)) {
        if (error) *error = "WebPConfigInit 失败";
        return {};
    }
    if (!WebPConfigPreset(&cfg, WEBP_PRESET_DEFAULT, static_cast<float>(quality))) {
        if (error) *error = "WebPConfigPreset 失败";
        return {};
    }
    cfg.lossless     = 0;          // 有损：画面流要的是带宽，不是无损
    cfg.quality      = static_cast<float>(quality);
    cfg.method       = method;
    cfg.thread_level = 0;          // 实测多线程只快 5%，不值得引入线程池
    if (!WebPValidateConfig(&cfg)) {
        if (error) {
            *error = "WebP 参数不合法（quality=" + std::to_string(quality) +
                     " method=" + std::to_string(method) + "）";
        }
        return {};
    }

    WebPPicture pic;
    if (!WebPPictureInit(&pic)) {
        if (error) *error = "WebPPictureInit 失败";
        return {};
    }
    pic.use_argb = 1;              // 保留 alpha 通道信息（截图恒为 255）
    pic.width    = static_cast<int>(width);
    pic.height   = static_cast<int>(height);

    // 这里**必须**检查返回值：ImportRGBA 会因内存不足失败，
    // 而失败时 pic 里是没数据的 —— 继续编会掉进断言或产出垃圾。
    if (!WebPPictureImportRGBA(&pic, rgba, static_cast<int>(width) * 4)) {
        if (error) *error = "WebPPictureImportRGBA 失败（内存不足？）";
        WebPPictureFree(&pic);
        return {};
    }

    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    pic.writer     = WebPMemoryWrite;
    pic.custom_ptr = &writer;

    const int ok = WebPEncode(&cfg, &pic);

    std::string out;
    if (ok) {
        out.assign(reinterpret_cast<const char*>(writer.mem), writer.size);
    } else if (error) {
        // WebPPicture 的 error_code 比"编码失败"有用得多
        *error = "WebP 编码失败（错误码 " + std::to_string(pic.error_code) + "）";
    }

    // 顺序不能反：writer 的缓冲要等 pic 不再用它才能释放
    WebPPictureFree(&pic);
    WebPMemoryWriterClear(&writer);
    return out;
}

}  // namespace autod
