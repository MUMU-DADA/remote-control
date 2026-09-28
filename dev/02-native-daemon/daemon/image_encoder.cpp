// image_encoder.cpp — 运行时选择编码器
//
// 原来的写法是编译期分支：
//
//     #ifdef __ANDROID__
//         // 永远用 AndroidBitmap_compress（Skia）
//     #else
//         // 永远用 png_encoder（dlopen zlib）
//     #endif
//
// 问题是 `AndroidBitmap_compress` 是 `__INTRODUCED_IN(30)` ——
// Android 11 才有。编译期分支意味着"编给 Android 就假定设备是 11+"，
// 于是整个服务的最低版本被一个编码 API 抬到了 Android 11。
//
// 改成运行时探测之后：
//
//     AndroidBitmap 可用（API 30+）→ 走它（**和原来完全一样**）
//     不可用（API < 30）          → JPEG 走 dlopen libjpeg，
//                                   PNG 走 zlib，
//                                   WebP 没有
//
// 关键设计：**能用 AndroidBitmap 就优先用它**。高版本的行为
// 一行都不变 —— 这是这个改造最重要的约束（改造老版本时把新版本
// 改坏，是这类重构最典型的翻车方式）。
//
// 为什么要 dlopen libjnigraphics 而不是直接链接：
//   直接链接的话，二进制在 Android 10 上**加载就会失败**
//   （"cannot locate symbol AndroidBitmap_compress"），
//   连"探测"的机会都没有。dlopen 才能把"这个函数不存在"
//   变成一个可以处理的返回值。

#include "image_encoder.h"

#include <dlfcn.h>
#include <stdlib.h>   // getenv
#include <string.h>

// AndroidBitmap_* 只在 Android 上存在。宿主机（编测试用）没有这些头，
// 也没有 libjnigraphics —— 那边永远走 png_encoder。
//
// ⚠️ 注意这个 __ANDROID__ 和改造前那个**不是一回事**：
//    以前它决定"用哪种编码器"，现在只决定"这个平台有没有这个头文件"。
//    真正的选择在 Android 上是**运行时探测**做的（见 ProbeJniGraphics）。
#ifdef __ANDROID__
#include <android/bitmap.h>
#include <android/data_space.h>
#define REMOTE_CONTROL_HAS_JNIGRAPHICS 1
#endif

#include "remote_control_log.h"
#include "jpeg_encoder.h"
#include "h264_encoder.h"
#include "png_encoder.h"
#include "webp_encoder.h"

namespace remote_control {
namespace {

// WebP 的默认 method。
//
// 实测（320x480 真实抓帧，q75）：
//   0 →  3.3 ms / 17,192 B      最快
//   2 →  5.2 ms / 14,542 B      ← 这里
//   3 → 10.3 ms / 14,036 B      Skia 用的
//   6 → 21.8 ms / 13,222 B      最慢最好
//
// 选 2 是因为它只比最强的差 3.6% 体积，却快一倍。
// Skia 选 3 是为了跟 Chrome 对齐，不是因为它最优。
constexpr int kDefaultWebpMethod = 2;

// 环境变量：强制走回退路径。
//
// 存在的理由：**回退路径原本只能在老设备上验证**。Android 11+ 永远
// 探测得到 AndroidBitmap，所以 libjpeg / 内置 libwebp 那条路在开发机上
// 根本跑不到 —— 而"没跑过的代码"和"没有的代码"在出故障时是一样的。
//
//   REMOTE_CONTROL_FORCE_FALLBACK=1 remote-control --socket ...
//
// supervisor 启动的话变量会继承下去：
//   adb shell "REMOTE_CONTROL_FORCE_FALLBACK=1 setsid nohup
//              /data/local/tmp/remote-control-supervisord.sh > /dev/null 2>&1 &"
//
// ⚠️ 它只影响**编码器选择**，不改协议、不改截图后端。
//    /config 的 codecs.forced 会如实标出来 —— 别把强制的结果当设备真相。
bool ForcedFallback() {
    static const bool forced = []() {
        const char* v = getenv("REMOTE_CONTROL_FORCE_FALLBACK");
        if (v == nullptr || v[0] != '1') return false;
        ALOGW("REMOTE_CONTROL_FORCE_FALLBACK=1 —— 跳过 AndroidBitmap_compress，"
              "强制走回退编码器（仅用于验证老设备路径）");
        return true;
    }();
    return forced;
}

// 这种格式该用哪个编码器 —— **实测决定的，不是"哪个更原生"**。
//
// 直觉上 AndroidBitmap_compress（Skia）应该比内置的快。实测**相反**：
//
//   720p，60fps 目标     Skia                   内置              差
//   ───────────────────────────────────────────────────────────────
//   jpeg                60.3 fps / 43.9 KiB   61.0 fps / 43.4 KiB   持平
//   webp                20.5 fps / 25.0 KiB   38.3 fps / 25.5 KiB  +87%
//   png                 22.2 fps / 72.9 KiB   45.8 fps / 85.3 KiB +106%
//
//   （x86_64 模拟器，SurfaceFlinger 后端，宿主空闲时测的；
//     复现命令见 docs/06-capture-performance.md 第七节）
//
// 原因大概率在参数上：Skia 的 WebP 用 method=3（跟 Chrome 对齐），
// 我们内置的用 method=2 —— 按 webp_encoder.h 里的实测，正好差一倍。
// PNG 那边 Skia 用了较高的 zlib 级别，我们默认 1（最快）。
//
// **代价是体积**：WebP 只大 2%（可忽略），PNG 大 17%（要自己权衡）。
// 所以这不该是写死的结论，留了开关：
//
//   REMOTE_CONTROL_FORCE_FALLBACK=1   全走内置（验证老设备路径用）
//   REMOTE_CONTROL_PREFER_NATIVE=1    全走 Skia（要最小体积时用）
bool PreferBuiltin(ImageFormat f) {
    if (ForcedFallback()) return true;
    if (const char* v = getenv("REMOTE_CONTROL_PREFER_NATIVE");
        v != nullptr && v[0] == '1') {
        return false;
    }
    return f == ImageFormat::kWebp || f == ImageFormat::kPng;
}

#ifdef REMOTE_CONTROL_HAS_JNIGRAPHICS

// ⚠️ 自己声明写回调的类型，**不用头文件里的 `AndroidBitmap_CompressWriteFunc`**。
//
//    那个 typedef 被标了 `__INTRODUCED_IN(30)`，编到 API < 30 时
//    引用它就是编译错误（-Werror 直接拦下），哪怕我们只是想拿它
//    声明一个函数指针、根本不链接那个符号。
//
//    签名一字不差地抄过来即可。
using CompressWriteFn = bool (*)(void* userContext, const void* data,
                                 size_t size);

// libjnigraphics 的运行时函数表。
//
// 只探一个符号就够：`AndroidBitmap_compress` 是随 API 30 一起进来的，
// 它存在就意味着这一整套都能用。
struct JniGraphics {
    void* handle = nullptr;
    // 函数指针用真实签名存，调用处不用重复转型
    int (*compress)(const AndroidBitmapInfo*, int32_t, const void*, int32_t,
                    int32_t, void*, CompressWriteFn) = nullptr;
};

JniGraphics& Jni() {
    static JniGraphics g;
    return g;
}

// 尝试加载 libjnigraphics 并取 AndroidBitmap_compress。
//
// **只试一次**并缓存结果：在缺这个符号的设备上（Android 8~10），
// 每帧都重试一遍失败的 dlopen 是纯浪费。
bool ProbeJniGraphics() {
    if (ForcedFallback()) return false;
    static const bool ok = []() {
        JniGraphics& g = Jni();
        if (g.handle != nullptr) return g.compress != nullptr;

        g.handle = dlopen("libjnigraphics.so", RTLD_NOW | RTLD_LOCAL);
        if (g.handle == nullptr) {
            // 同样：dlerror() 只能调一次（见 jpeg_encoder.cpp 的说明）
            const char* e = dlerror();
            ALOGI("图像编码: libjnigraphics 不可用（%s）",
                  e != nullptr ? e : "未知原因");
            return false;
        }
        *(void**)(&g.compress) = dlsym(g.handle, "AndroidBitmap_compress");
        if (g.compress == nullptr) {
            // 这就是 Android 8~10 上的情况：库在，但符号没有。
            // 不是错误，是这台设备的正常状态。
            ALOGI("图像编码: AndroidBitmap_compress 不存在"
                  "（API < 30），改用 dlopen libjpeg + zlib");
            dlclose(g.handle);
            g.handle = nullptr;
            return false;
        }
        return true;
    }();
    return ok;
}

// 写回调。AndroidBitmap_compress 可能**多次**调用它
// （文档："May be called more than once for each call to this method"），
// 所以必须是追加，不能覆盖。
bool WriteToBuffer(void* ctx, const void* data, size_t size) {
    static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), size);
    return true;
}

int32_t ToAndroidFormat(ImageFormat f) {
    switch (f) {
        case ImageFormat::kJpeg: return ANDROID_BITMAP_COMPRESS_FORMAT_JPEG;
        case ImageFormat::kWebp: return ANDROID_BITMAP_COMPRESS_FORMAT_WEBP_LOSSY;
        default:                 return ANDROID_BITMAP_COMPRESS_FORMAT_PNG;
    }
}

// PNG 的 quality 语义是 zlib 级别（1-9），而 AndroidBitmap 期望 0-100。
// 不换算的话 level=1 会被当成"质量 1"编出一张糊图。
int ToAndroidQuality(ImageFormat f, int q) {
    if (f == ImageFormat::kPng) {
        if (q < 1 || q > 9) q = 1;
        int v = q * 100 / 9;
        return v < 1 ? 1 : v;
    }
    if (q <= 0 || q > 100) return (f == ImageFormat::kWebp) ? 80 : 75;
    return q;
}
#endif  // REMOTE_CONTROL_HAS_JNIGRAPHICS

// 宿主机（编测试用）没有 libjnigraphics，也没有那些头文件。
// 给一个同名函数，让 Init() 的调用点不用再包一层 #ifdef。
#ifndef REMOTE_CONTROL_HAS_JNIGRAPHICS
bool ProbeJniGraphics() { return false; }
#endif

}  // namespace

QualityRange QualityRangeFor(ImageFormat f) {
    switch (f) {
        case ImageFormat::kPng:
            // zlib 压缩级别，不是图像质量。1 最快最大，9 最小最慢。
            return QualityRange{1, 9, 1};
        case ImageFormat::kWebp:
            return QualityRange{1, 100, 80};
        case ImageFormat::kJpeg:
            return QualityRange{1, 100, 75};
        case ImageFormat::kH264:
            // 也是 1-100，但会被换算成码率（见 rest_api 里那段公式）
            return QualityRange{1, 100, 75};
        default:
            return QualityRange{1, 100, 75};
    }
}

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
    if (name == "h264")                { *out = ImageFormat::kH264; return true; }
    return false;
}

const char* ImageEncoder::Name(ImageFormat f) {
    switch (f) {
        case ImageFormat::kPng:  return "png";
        case ImageFormat::kJpeg: return "jpeg";
        case ImageFormat::kWebp: return "webp";
        case ImageFormat::kRaw:  return "raw";
        case ImageFormat::kH264: return "h264";
        default:                 return "auto";
    }
}

const char* ImageEncoder::MimeType(ImageFormat f) {
    switch (f) {
        case ImageFormat::kPng:  return "image/png";
        case ImageFormat::kJpeg: return "image/jpeg";
        case ImageFormat::kWebp: return "image/webp";
        case ImageFormat::kH264: return "video/h264";
        default:                 return "application/octet-stream";
    }
}

bool ImageEncoder::Supports(ImageFormat f) const {
    if (f == ImageFormat::kRaw) return true;
    // 只读查询 —— 初始化在 Init() 里做过一次了。
    // 这里再 Init 会在"报告能力"时产生副作用（加载库、占 fd）。
    if (f == ImageFormat::kPng) return native_ || PngEncoder::Instance().Available();
    if (native_) return true;                      // AndroidBitmap：JPEG / WebP 都有
    if (f == ImageFormat::kJpeg) return JpegEncoder::Instance().Available();
    // WebP 现在**总是**可用 —— libwebp 已经内置进来了。
    // 以前这里返回 false，因为 WebP 只有 AndroidBitmap 能出（API 30+）。
    if (f == ImageFormat::kWebp) return WebpEncoder::Instance().Available();
    // H.264 走设备端 MediaCodec，和上面的软件编码器不是一回事
    if (f == ImageFormat::kH264) return H264Encoder::Supported();
    return false;
}

ImageFormat ImageEncoder::BestFormat() const {
    // 有 Skia 就用 JPEG —— 对画面流来说，"小一个数量级"比"无损"重要得多。
    // 没有的话，有 libjpeg 也优先 JPEG（同样理由），最后才退回 PNG。
    if (native_) return ImageFormat::kJpeg;
    if (JpegEncoder::Instance().Available()) return ImageFormat::kJpeg;
    return ImageFormat::kPng;
}

bool ImageEncoder::Init(std::string* error) {
    native_ = ProbeJniGraphics();

    // PNG 兜底（dlopen zlib）在任何版本上都要备着 ——
    // 即使有 Skia，某些格式组合也可能要它。
    std::string perr;
    const bool haveZlib = PngEncoder::Instance().Init(&perr);

    // libjpeg 只在没有 Skia 时才需要。有 Skia 就不去 dlopen 了 ——
    // 少一个运行时依赖，也少一条可能的失败路径。
    if (!native_) {
        std::string jerr;
        if (!JpegEncoder::Instance().Init(&jerr)) {
            ALOGW("图像编码: %s", jerr.c_str());
        }
    }

    if (!native_ && !haveZlib &&
        !JpegEncoder::Instance().Available()) {
        if (error) {
            *error = "没有可用的图像编码器（Skia 不可用、zlib 不可用、"
                     "libjpeg 不可用）";
        }
        return false;
    }

    ALOGI("图像编码器: %s%s", BackendSummary().c_str(),
          native_ ? "（Skia）"
                  : (ForcedFallback() ? "（回退路径，强制）" : "（回退路径）"));
    return true;
}

bool ImageEncoder::FallbackForced() const { return ForcedFallback(); }

std::string ImageEncoder::BackendSummary() const {
    // 现在是**分格式**选的（WebP/PNG 走内置，JPEG 走 Skia —— 见 PreferBuiltin
    // 上面的实测表），所以不能再笼统报一个后端名。逐个列出来，
    // 否则排查"为什么 WebP 这个设备上快那个设备上慢"时没有依据。
    std::string s;
    auto add = [&s](const char* fmt, const std::string& who) {
        if (!s.empty()) s += " + ";
        s += fmt;
        s += "=";
        s += who;
    };

    const bool jniOk = native_;
    auto forFormat = [&](ImageFormat f, const char* name) {
        if (jniOk && !PreferBuiltin(f)) {
            add(name, "AndroidBitmap_compress");
            return;
        }
        switch (f) {
            case ImageFormat::kWebp:
                if (WebpEncoder::Instance().Available()) {
                    add(name, "libwebp（内置）");
                    return;
                }
                break;
            case ImageFormat::kPng:
                if (PngEncoder::Instance().Available()) {
                    add(name, "zlib PNG");
                    return;
                }
                break;
            default:
                if (JpegEncoder::Instance().Available()) {
                    add(name, JpegEncoder::Instance().BackendName());
                    return;
                }
                break;
        }
        if (jniOk) add(name, "AndroidBitmap_compress");
    };

    forFormat(ImageFormat::kJpeg, "jpeg");
    forFormat(ImageFormat::kWebp, "webp");
    forFormat(ImageFormat::kPng,  "png");

    if (s.empty()) s = "（无）";
    return s;
}

std::string ImageEncoder::Encode(const uint8_t* rgba, uint32_t width,
                                 uint32_t height, ImageFormat format,
                                 int quality, std::string* error) {
    if (rgba == nullptr || width == 0 || height == 0) {
        if (error) *error = "空图像";
        return {};
    }
    if (format == ImageFormat::kAuto) format = BestFormat();

    // ── 走 Skia（AndroidBitmap_compress）──
    //
    // ⚠️ 不是"能用就用" —— WebP/PNG 上它比内置编码器慢一倍，
    //    见 PreferBuiltin 上面的实测表。
#ifdef REMOTE_CONTROL_HAS_JNIGRAPHICS
    if (native_ && format != ImageFormat::kRaw && !PreferBuiltin(format)) {
        AndroidBitmapInfo info;
        memset(&info, 0, sizeof(info));
        info.width  = width;
        info.height = height;
        info.stride = width * 4;
        info.format = ANDROID_BITMAP_FORMAT_RGBA_8888;
        // 屏幕截图是不透明的，但帧里 alpha 恒为 255。
        // 标 PREMUL：JPEG 会丢掉 alpha，而 PNG 路径下 PREMUL 对
        // alpha=255 的结果和不透明一致。
        info.flags  = ANDROID_BITMAP_FLAGS_ALPHA_PREMUL;

        std::string out;
        const int rc = Jni().compress(&info, ADATASPACE_SRGB, rgba,
                                      ToAndroidFormat(format),
                                      ToAndroidQuality(format, quality),
                                      &out, &WriteToBuffer);
        if (rc != ANDROID_BITMAP_RESULT_SUCCESS) {
            // Skia 失败了也不该整条流断掉 —— 往下走回退路径。
            // 但要说清楚，不然会变成"偶尔糊一张图而没人知道为什么"。
            ALOGW("AndroidBitmap_compress 失败 rc=%d，本帧改用回退路径", rc);
        } else if (!out.empty()) {
            return out;
        }
    }
#endif  // REMOTE_CONTROL_HAS_JNIGRAPHICS

    // ── 回退：JPEG 走 libjpeg ──
    if (format == ImageFormat::kJpeg) {
        if (JpegEncoder::Instance().Available()) {
            return JpegEncoder::Instance().EncodeRgba(rgba, width, height,
                                                      quality > 0 ? quality : 75,
                                                      error);
        }
        if (error) {
            *error = "这台设备没有可用的 JPEG 编码器"
                     "（AndroidBitmap_compress 需要 API 30+，"
                     "libjpeg.so 也没找到）";
        }
        return {};
    }

    // ── 回退：WebP 走内置的 libwebp ──
    //
    // 以前这里直接报错（"WebP 只有 AndroidBitmap 能出"），
    // 因为设备上没有 libwebp.so。现在源码内置了，任何版本都能编。
    if (format == ImageFormat::kWebp) {
        return WebpEncoder::Instance().EncodeRgba(rgba, width, height,
                                                  quality > 0 ? quality : 75,
                                                  kDefaultWebpMethod, error);
    }

    // ── 回退：PNG 走 zlib ──
    int level = quality;
    if (level < 1 || level > 9) level = 1;
    return PngEncoder::Instance().EncodeRgba(rgba, width, height, level, error);
}

}  // namespace remote_control
