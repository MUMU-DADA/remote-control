// h264_encoder.cpp — 用设备端 MediaCodec 编码 H.264

#include "h264_encoder.h"

#include <stdlib.h>   // getenv/atoi
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <limits>

#include "remote_control_log.h"

// 编译期开关：没有 libmediandk 的构建（宿主机测试）就整个降级成"不可用"。
//
// 这里用 __ANDROID__ 而不是"运行时探测" —— AMediaCodec 是**链接期**
// 依赖（-lmediandk），宿主机根本没有这个库。而 Android 侧它从
// API 21 就有，不存在"设备上没有"的情况（真没有的话
// AMediaCodec_createEncoderByType 返回 NULL，那是运行时的另一回事）。
#if defined(__ANDROID__) || defined(REMOTE_CONTROL_TEST_MEDIANDK)
#define REMOTE_CONTROL_HAS_MEDIANDK 1
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <dlfcn.h>
#endif

namespace remote_control {

namespace {

#ifdef REMOTE_CONTROL_HAS_MEDIANDK
// NDK 的 media 头里**没有** COLOR_Format* 这些常量 —— 它们是 Java 侧
// MediaCodecInfo.CodecCapabilities 的字段。值本身是平台稳定的，
// 所以在这里自己定义。
//
//   19 = COLOR_FormatYUV420Planar     (I420)
//   21 = COLOR_FormatYUV420SemiPlanar (NV12)
constexpr int32_t kColorFormatYuv420SemiPlanar = 21;

int64_t NowUs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

// libyuv 的名字按 32 位整数命名：ABGR 在内存中正好是 R,G,B,A。
using AbgrToNv12 = int (*)(const uint8_t*, int, uint8_t*, int,
                           uint8_t*, int, int, int);

AbgrToNv12 LibyuvConverter() {
    static const AbgrToNv12 convert = []() -> AbgrToNv12 {
        const char* disabled = getenv("REMOTE_CONTROL_H264_LIBYUV");
        if (disabled != nullptr && strcmp(disabled, "0") == 0) return nullptr;
        void* handle = dlopen("libyuv.so", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) return nullptr;
        auto fn = reinterpret_cast<AbgrToNv12>(dlsym(handle, "ABGRToNV12"));
        if (fn == nullptr) dlclose(handle);
        // 成功时保留句柄：所有编码器共享函数，进程退出时由系统回收。
        return fn;
    }();
    return convert;
}
#endif

}  // namespace

namespace {
// 全局并发计数。用原子而不是锁 —— 只增只减，没必要为它引入一把锁。
std::atomic<int> g_activeEncoders{0};
}  // namespace

int H264Encoder::MaxConcurrent() {
    // 2 是保守值：真机硬件编码器通常 1~2 路。
    // 用环境变量可以调，调试时有用。
    static const int n = []() {
        const char* v = getenv("REMOTE_CONTROL_H264_MAX");
        if (v != nullptr) {
            const int x = atoi(v);
            if (x > 0 && x <= 16) return x;
        }
        return 2;
    }();
    return n;
}

int H264Encoder::ActiveCount() { return g_activeEncoders.load(); }

bool H264Encoder::SlotAvailable() {
    return g_activeEncoders.load() < MaxConcurrent();
}

bool H264Encoder::Supported() {
#ifdef REMOTE_CONTROL_HAS_MEDIANDK
    return true;
#else
    return false;
#endif
}

H264Encoder::~H264Encoder() { Stop(); }

bool H264Encoder::Running() const { return running_; }

void H264Encoder::Stop() {
#ifdef REMOTE_CONTROL_HAS_MEDIANDK
    if (codec_ != nullptr) {
        auto* c = static_cast<AMediaCodec*>(codec_);
        if (running_) AMediaCodec_stop(c);
        AMediaCodec_delete(c);
        codec_ = nullptr;
        g_activeEncoders.fetch_sub(1);
    }
#endif
    running_ = false;
    wantKeyFrame_ = false;
    codecString_.clear();
    codecConfig_.clear();
    partialOutput_.clear();
    inputStride_ = inputSliceHeight_ = 0;
    lastInputPtsUs_ = 0;
}

bool H264Encoder::Start(const Config& cfg, std::string* error) {
#ifdef REMOTE_CONTROL_HAS_MEDIANDK
    if (cfg.width == 0 || cfg.height == 0 || (cfg.width & 1) != 0 ||
        (cfg.height & 1) != 0 ||
        cfg.width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max() / 4) ||
        cfg.height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        static_cast<uint64_t>(cfg.width) * cfg.height >
                std::numeric_limits<size_t>::max() / 4 ||
        cfg.bitrate == 0 || cfg.bitrate > static_cast<uint32_t>(INT32_MAX) ||
        cfg.fps == 0 || cfg.fps > static_cast<uint32_t>(INT32_MAX) ||
        cfg.iFrameIntervalSec > static_cast<uint32_t>(INT32_MAX)) {
        if (error) *error = "H.264 需要正数偶数尺寸及有效的码率/帧率";
        return false;
    }
    if (running_ && cfg_.width == cfg.width && cfg_.height == cfg.height &&
        cfg_.fps == cfg.fps && cfg_.iFrameIntervalSec == cfg.iFrameIntervalSec) {
        if (cfg_.bitrate == cfg.bitrate) return true;
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 26
        AMediaFormat* params = AMediaFormat_new();
        AMediaFormat_setInt32(params, "video-bitrate", static_cast<int32_t>(cfg.bitrate));
        const media_status_t updated = AMediaCodec_setParameters(
                static_cast<AMediaCodec*>(codec_), params);
        AMediaFormat_delete(params);
        if (updated == AMEDIA_OK) {
            cfg_ = cfg;
            return true;
        }
#endif
    }
    Stop();

    int active = g_activeEncoders.load();
    do {
        if (active >= MaxConcurrent()) {
            if (error) *error = "H.264 并发编码器已达上限";
            return false;
        }
    } while (!g_activeEncoders.compare_exchange_weak(active, active + 1));

    AMediaCodec* c = AMediaCodec_createEncoderByType("video/avc");
    if (c == nullptr) {
        g_activeEncoders.fetch_sub(1);
        if (error) {
            *error = "设备上没有 AVC 编码器（AMediaCodec_createEncoderByType "
                     "返回 NULL）";
        }
        return false;
    }

    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, static_cast<int32_t>(cfg.width));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, static_cast<int32_t>(cfg.height));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, static_cast<int32_t>(cfg.bitrate));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, static_cast<int32_t>(cfg.fps));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL,
                          static_cast<int32_t>(cfg.iFrameIntervalSec));
    // NV12（Y 平面 + 交错的 UV）。用**明确的**格式而不是 Flexible：
    // Flexible 的实际布局要另外查 codec 的 format，多一处失败点，
    // 而我们只需要一种。
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                          kColorFormatYuv420SemiPlanar);
    // 字符串键兼容旧 NDK API；不支持这些提示的编码器会忽略它们。
    AMediaFormat_setInt32(f, "max-bframes", 0);
    AMediaFormat_setInt32(f, "latency", 0);
    AMediaFormat_setInt32(f, "color-standard", 4);   // BT.601 PAL
    AMediaFormat_setInt32(f, "color-range", 2);      // limited range
    AMediaFormat_setInt32(f, "color-transfer", 3);   // SDR video

    media_status_t st = AMediaCodec_configure(c, f, nullptr, nullptr,
                                              AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK) {
        if (error) {
            *error = "AMediaCodec_configure 失败: " + std::to_string((int)st) +
                     "（尺寸 " + std::to_string(cfg.width) + "x" +
                     std::to_string(cfg.height) + " 可能超出编码器能力）";
        }
        AMediaCodec_delete(c);
        g_activeEncoders.fetch_sub(1);
        return false;
    }

    st = AMediaCodec_start(c);
    if (st != AMEDIA_OK) {
        if (error) *error = "AMediaCodec_start 失败: " + std::to_string((int)st);
        AMediaCodec_delete(c);
        g_activeEncoders.fetch_sub(1);
        return false;
    }

    codec_ = c;
    cfg_ = cfg;
    running_ = true;
    inputStride_ = cfg.width;
    inputSliceHeight_ = cfg.height;
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 28
    AMediaFormat* input = AMediaCodec_getInputFormat(c);
    if (input != nullptr) {
        int32_t stride = 0, sliceHeight = 0;
        if (AMediaFormat_getInt32(input, "stride", &stride) &&
            stride >= static_cast<int32_t>(cfg.width)) {
            inputStride_ = static_cast<uint32_t>(stride);
        }
        if (AMediaFormat_getInt32(input, "slice-height", &sliceHeight) &&
            sliceHeight >= static_cast<int32_t>(cfg.height)) {
            inputSliceHeight_ = static_cast<uint32_t>(sliceHeight);
        }
        AMediaFormat_delete(input);
    }
#endif
    if ((inputStride_ & 1) != 0 || (inputSliceHeight_ & 1) != 0 ||
        static_cast<uint64_t>(inputStride_) * inputSliceHeight_ >
                std::numeric_limits<size_t>::max() / 2) {
        if (error) *error = "编码器返回了无效的 NV12 stride/slice-height";
        Stop();
        return false;
    }
    wantKeyFrame_ = true;      // 第一帧必须是关键帧，否则客户端开头是黑的
    codecString_.clear();
    codecConfig_.clear();
    stats_ = Stats{};
    stats_.libyuv = LibyuvConverter() != nullptr;
    ALOGI("H.264 编码器就绪: %ux%u @%u bps, %u fps, I 帧间隔 %us",
          cfg.width, cfg.height, cfg.bitrate, cfg.fps, cfg.iFrameIntervalSec);
    ALOGI("H.264 RGBA 转换: %s，stride=%u，slice-height=%u",
          stats_.libyuv ? "libyuv SIMD" : "scalar", inputStride_, inputSliceHeight_);
    return true;
#else
    (void)cfg;
    if (error) *error = "这个构建没有 libmediandk（H.264 需要 Android 构建）";
    return false;
#endif
}

std::string H264Encoder::CodecString() const { return codecString_; }

void H264Encoder::RequestKeyFrame() { wantKeyFrame_ = true; }

H264Encoder::Stats H264Encoder::GetStats() const { return stats_; }

bool H264Encoder::PollOutput(std::vector<uint8_t>* out, std::string* error,
                             int timeoutMs) {
    if (!running_ || codec_ == nullptr || out == nullptr) {
        if (error) *error = "编码器没在跑或输出缓冲为空";
        return false;
    }
    return DrainOutput(out, std::clamp(timeoutMs, 0, 20) * int64_t{1000}, error);
}

#ifdef REMOTE_CONTROL_HAS_MEDIANDK

namespace {

// RGBA8888 → NV12（Y 平面 + 交错 UV）。
//
// 用整数近似做 BT.601 的 YUV 转换。浮点版本更准，但这里是每帧
// 几十万像素的热路径，而屏幕内容对色度精度的要求不高。
//
//   Y  = ( 66R + 129G +  25B + 128) >> 8) + 16
//   U  = ((-38R -  74G + 112B + 128) >> 8) + 128
//   V  = ((112R -  94G -  18B + 128) >> 8) + 128
//
// ⚠️ 注意是**全范围**还是**有限范围**：上面的偏移（+16 / +128）是
//    有限范围（video range）。编码器一般期望这个。
void RgbaToNv12(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* out,
                uint32_t stride, uint32_t sliceHeight) {
    uint8_t* yPlane = out;
    uint8_t* uvPlane = out + static_cast<size_t>(stride) * sliceHeight;

    for (uint32_t j = 0; j < h; ++j) {
        for (uint32_t i = 0; i < w; ++i) {
            const uint8_t* p = rgba + (static_cast<size_t>(j) * w + i) * 4;
            const int r = p[0], g = p[1], b = p[2];
            yPlane[static_cast<size_t>(j) * stride + i] = static_cast<uint8_t>(
                    ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    // 色度是 2x2 下采样。逐 2x2 取平均再算，比只取左上角一个像素
    // 好得多 —— 后者在细线条上会出现彩色锯齿。
    for (uint32_t j = 0; j + 1 < h; j += 2) {
        for (uint32_t i = 0; i + 1 < w; i += 2) {
            int rs = 0, gs = 0, bs = 0;
            for (uint32_t dy = 0; dy < 2; ++dy) {
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    const uint8_t* p = rgba + (static_cast<size_t>(j + dy) * w + (i + dx)) * 4;
                    rs += p[0]; gs += p[1]; bs += p[2];
                }
            }
            const int r = rs / 4, g = gs / 4, b = bs / 4;
            const int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            const int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            uint8_t* q = uvPlane + (static_cast<size_t>(j) / 2) * stride + i;
            q[0] = static_cast<uint8_t>(u < 0 ? 0 : (u > 255 ? 255 : u));
            q[1] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

// 从 SPS 拼出 WebCodecs 要的 codec 串（"avc1.42C029"）。
//
// SPS 的 NAL 头之后三个字节就是 profile_idc / constraint_flags /
// level_idc。**不能写死** —— 各设备的 profile/level 不同，
// 写死了浏览器的 isConfigSupported 会拒。
std::string CodecStringFromSps(const uint8_t* nal, size_t len) {
    // nal 指向 NAL 头（0x67 = SPS），后面至少还要 3 字节
    if (len < 4) return {};
    char buf[24];
    snprintf(buf, sizeof(buf), "avc1.%02X%02X%02X", nal[1], nal[2], nal[3]);
    return buf;
}

const uint8_t* FindNal(const uint8_t* data, size_t size, uint8_t type) {
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0) continue;
        size_t off = 0;
        if (data[i + 2] == 1) off = i + 3;
        else if (i + 4 < size && data[i + 2] == 0 && data[i + 3] == 1) off = i + 4;
        if (off != 0 && off < size && (data[off] & 0x1F) == type) {
            return data + off;
        }
    }
    return nullptr;
}

}  // namespace

bool H264Encoder::DrainOutput(std::vector<uint8_t>* out, int64_t timeoutUs,
                              std::string* error) {
    AMediaCodec* c = static_cast<AMediaCodec*>(codec_);
    if (out != nullptr) out->clear();
    const int64_t deadline = NowUs() + timeoutUs;

    for (int guard = 0; guard < 16; ++guard) {   // 一轮最多取 16 个
        AMediaCodecBufferInfo info;
        memset(&info, 0, sizeof(info));
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(
                c, &info, std::max<int64_t>(0, deadline - NowUs()));

        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return true;   // 正常，没数据
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            // 正常：出第一帧之前会先报一次。如果这次就带着 codec config，
            // 从 SPS 里把 codec 串补上。
            continue;
        }
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (idx < 0) {
            if (error) *error = "dequeueOutputBuffer: " + std::to_string((long)idx);
            return false;
        }

        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getOutputBuffer(c, static_cast<size_t>(idx), &cap);
        if (info.offset < 0 || info.size < 0 ||
            static_cast<size_t>(info.offset) > cap ||
            static_cast<size_t>(info.size) > cap - static_cast<size_t>(info.offset)) {
            AMediaCodec_releaseOutputBuffer(c, static_cast<size_t>(idx), false);
            if (error) *error = "编码器输出 buffer offset/size 超出容量";
            return false;
        }
        if (buf == nullptr || info.size <= 0) {
            AMediaCodec_releaseOutputBuffer(c, static_cast<size_t>(idx), false);
            continue;
        }
        if (buf != nullptr && info.size > 0) {
            const uint8_t* p = buf + info.offset;

            // CODEC_CONFIG（SPS/PPS）单独存起来，**不作为一帧发出**。
            //
            // 它不是一个完整的访问单元。单独发出去的话：
            //   - WebCodecs 会拿到一个只有参数集、没有图像的 chunk
            //   - 客户端的解码器可能直接报错
            // 拼到下一个真正的帧（IDR）前面才是对的。
            if ((info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0) {
                codecConfig_.assign(p, p + info.size);
                if (codecString_.empty()) {
                    if (const uint8_t* sps = FindNal(p, info.size, 7)) {
                        codecString_ = CodecStringFromSps(
                                sps, static_cast<size_t>(info.size) - (sps - p));
                    }
                }
                AMediaCodec_releaseOutputBuffer(c, static_cast<size_t>(idx), false);
                continue;
            }

            // 某些编码器会用 PARTIAL_FRAME 把同一访问单元分成多个 buffer。
            // 先合并，只有访问单元结束时才交给上层，防止客户端收到半帧。
            const bool partial = (info.flags & AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME) != 0;
            const bool assembled = partial || !partialOutput_.empty();
            if (assembled) {
                partialOutput_.insert(partialOutput_.end(), p, p + info.size);
                AMediaCodec_releaseOutputBuffer(c, static_cast<size_t>(idx), false);
                if (partial) continue;
            }
            const uint8_t* frame = assembled ? partialOutput_.data() : p;
            const size_t frameSize = assembled ? partialOutput_.size() : info.size;
            const bool keyFrame = FindNal(frame, frameSize, 5) != nullptr;
            const uint8_t* sps = FindNal(frame, frameSize, 7);
            // 每个 IDR 都带参数集，客户端重建解码器后可直接重新加入。
            if (!codecConfig_.empty() && keyFrame && sps == nullptr) {
                out->insert(out->end(), codecConfig_.begin(), codecConfig_.end());
            }
            if (codecString_.empty() && sps != nullptr) {
                codecString_ = CodecStringFromSps(sps, frameSize - (sps - frame));
            }
            out->insert(out->end(), frame, frame + frameSize);
            partialOutput_.clear();
            if (!assembled) {
                AMediaCodec_releaseOutputBuffer(c, static_cast<size_t>(idx), false);
            }
            stats_.bytesOut += static_cast<uint64_t>(out->size());
            ++stats_.framesOut;
            stats_.lastOutputPtsUs = info.presentationTimeUs;
            stats_.lastOutputLatencyUs = std::max<int64_t>(
                    0, NowUs() - info.presentationTimeUs);
            return true;
        }
    }
    return true;
}

bool H264Encoder::EncodeRgba(const uint8_t* rgba, uint32_t width, uint32_t height,
                             std::vector<uint8_t>* out, std::string* error) {
    if (!running_ || codec_ == nullptr || out == nullptr) {
        if (error) *error = "编码器没在跑或输出缓冲为空";
        return false;
    }
    if (rgba == nullptr || width == 0 || height == 0) {
        if (error) *error = "空图像";
        return false;
    }
    // 尺寸变了必须重建 —— 编码器一旦 configure 就不能改尺寸
    if (width != cfg_.width || height != cfg_.height) {
        if (error) {
            *error = "尺寸从 " + std::to_string(cfg_.width) + "x" +
                     std::to_string(cfg_.height) + " 变成 " +
                     std::to_string(width) + "x" + std::to_string(height) +
                     "，需要重建编码器";
        }
        return false;
    }

    AMediaCodec* c = static_cast<AMediaCodec*>(codec_);
    const int64_t t0 = NowUs();

    // 只等待一个很小的预算：启动/编码器瞬时忙时仍允许拿到输入槽，
    // 但不会像旧实现一样把整条连接阻塞 200ms。
    ssize_t idx = AMediaCodec_dequeueInputBuffer(c, 5000 /* 5ms */);
    if (idx < 0) {
        if (idx != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            if (error) *error = "dequeueInputBuffer: " + std::to_string((long)idx);
            return false;
        }
        // 输入队列满 = 编码器跟不上。**不算错误** —— 丢掉这一帧，
        // 下一帧再来。对画面流来说丢帧远好过卡住。
        ++stats_.framesDropped;
        stats_.lastEncodeMs = (NowUs() - t0) / 1000;
        return DrainOutput(out, 0, error);
    }

    size_t cap = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(c, static_cast<size_t>(idx), &cap);
    const size_t lumaBytes = static_cast<size_t>(inputStride_) * inputSliceHeight_;
    const size_t need = lumaBytes + lumaBytes / 2;
    if (buf == nullptr || cap < need) {
        AMediaCodec_queueInputBuffer(c, static_cast<size_t>(idx), 0, 0, 0,
                                     AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG);
        if (error) {
            *error = "输入缓冲太小: " + std::to_string(cap) + " < " +
                     std::to_string(need);
        }
        return false;
    }

    const int64_t convertStart = NowUs();
    if (inputStride_ != width || inputSliceHeight_ != height) {
        memset(buf, 16, lumaBytes);
        memset(buf + lumaBytes, 128, lumaBytes / 2);
    }
    const AbgrToNv12 convert = LibyuvConverter();
    if (convert != nullptr) {
        const int result = convert(rgba, static_cast<int>(width * 4),
                                   buf, static_cast<int>(inputStride_),
                                   buf + static_cast<size_t>(inputStride_) * inputSliceHeight_,
                                   static_cast<int>(inputStride_), static_cast<int>(width),
                                   static_cast<int>(height));
        if (result != 0) {
            if (error) *error = "libyuv ABGRToNV12 失败: " + std::to_string(result);
            AMediaCodec_queueInputBuffer(c, static_cast<size_t>(idx), 0, 0, 0,
                                         AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG);
            return false;
        }
        stats_.libyuv = true;
    } else {
        RgbaToNv12(rgba, width, height, buf, inputStride_, inputSliceHeight_);
        stats_.libyuv = false;
    }
    stats_.lastConvertUs = NowUs() - convertStart;

    // 关键帧请求：NDK 的 queueInputBuffer **没有**关键帧标志
    // （AMEDIACODEC_BUFFER_FLAG_* 只有 CODEC_CONFIG / EOS / PARTIAL）。
    // 原生侧的正确做法是 setParameters("request-sync", 1)。
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 26
    if (wantKeyFrame_) {
        AMediaFormat* p = AMediaFormat_new();
        AMediaFormat_setInt32(p, "request-sync", 1);
        AMediaCodec_setParameters(c, p);   // 失败也不致命：下一个 I 帧照样会来
        AMediaFormat_delete(p);
        wantKeyFrame_ = false;
    }
#else
    wantKeyFrame_ = false;   // API < 26 没有 setParameters，只能等下一个 I 帧
#endif

    const int64_t ptsUs = std::max<int64_t>(NowUs(), lastInputPtsUs_ + 1);
    lastInputPtsUs_ = ptsUs;
    const media_status_t st = AMediaCodec_queueInputBuffer(
            c, static_cast<size_t>(idx), 0, need, ptsUs, 0);
    if (st != AMEDIA_OK) {
        if (error) *error = "queueInputBuffer: " + std::to_string((int)st);
        return false;
    }
    ++stats_.framesIn;

    if (!DrainOutput(out, 0, error)) return false;

    stats_.lastEncodeMs = (NowUs() - t0) / 1000;
    return true;
}

#else   // !REMOTE_CONTROL_HAS_MEDIANDK

bool H264Encoder::EncodeRgba(const uint8_t*, uint32_t, uint32_t,
                             std::vector<uint8_t>*, std::string* error) {
    if (error) *error = "这个构建没有 libmediandk（H.264 需要 Android 构建）";
    return false;
}

bool H264Encoder::DrainOutput(std::vector<uint8_t>*, int64_t, std::string*) {
    return false;
}

#endif  // REMOTE_CONTROL_HAS_MEDIANDK

}  // namespace remote_control
