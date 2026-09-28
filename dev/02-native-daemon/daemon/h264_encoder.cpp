// h264_encoder.cpp — 用设备端 MediaCodec 编码 H.264

#include "h264_encoder.h"

#include <stdlib.h>   // getenv/atoi
#include <string.h>
#include <time.h>

#include <atomic>

#include "remote_control_log.h"

// 编译期开关：没有 libmediandk 的构建（宿主机测试）就整个降级成"不可用"。
//
// 这里用 __ANDROID__ 而不是"运行时探测" —— AMediaCodec 是**链接期**
// 依赖（-lmediandk），宿主机根本没有这个库。而 Android 侧它从
// API 21 就有，不存在"设备上没有"的情况（真没有的话
// AMediaCodec_createEncoderByType 返回 NULL，那是运行时的另一回事）。
#ifdef __ANDROID__
#define REMOTE_CONTROL_HAS_MEDIANDK 1
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
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
#endif

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

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
}

bool H264Encoder::Start(const Config& cfg, std::string* error) {
#ifdef REMOTE_CONTROL_HAS_MEDIANDK
    if (cfg.width == 0 || cfg.height == 0) {
        if (error) *error = "尺寸不能为 0";
        return false;
    }
    if (running_ && cfg_.width == cfg.width && cfg_.height == cfg.height) {
        return true;                    // 已经在跑，尺寸也没变
    }
    Stop();

    AMediaCodec* c = AMediaCodec_createEncoderByType("video/avc");
    if (c == nullptr) {
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
        return false;
    }

    st = AMediaCodec_start(c);
    if (st != AMEDIA_OK) {
        if (error) *error = "AMediaCodec_start 失败: " + std::to_string((int)st);
        AMediaCodec_delete(c);
        return false;
    }

    codec_ = c;
    cfg_ = cfg;
    running_ = true;
    g_activeEncoders.fetch_add(1);
    wantKeyFrame_ = true;      // 第一帧必须是关键帧，否则客户端开头是黑的
    codecString_.clear();
    codecConfig_.clear();
    stats_ = Stats{};
    ALOGI("H.264 编码器就绪: %ux%u @%u bps, %u fps, I 帧间隔 %us",
          cfg.width, cfg.height, cfg.bitrate, cfg.fps, cfg.iFrameIntervalSec);
    return true;
#else
    if (error) *error = "这个构建没有 libmediandk（H.264 需要 Android 构建）";
    return false;
#endif
}

std::string H264Encoder::CodecString() const { return codecString_; }

void H264Encoder::RequestKeyFrame() { wantKeyFrame_ = true; }

H264Encoder::Stats H264Encoder::GetStats() const { return stats_; }

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
void RgbaToNv12(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* out) {
    uint8_t* yPlane = out;
    uint8_t* uvPlane = out + static_cast<size_t>(w) * h;

    for (uint32_t j = 0; j < h; ++j) {
        for (uint32_t i = 0; i < w; ++i) {
            const uint8_t* p = rgba + (static_cast<size_t>(j) * w + i) * 4;
            const int r = p[0], g = p[1], b = p[2];
            yPlane[static_cast<size_t>(j) * w + i] = static_cast<uint8_t>(
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
            uint8_t* q = uvPlane + (static_cast<size_t>(j) / 2) * w + i;
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

}  // namespace

bool H264Encoder::DrainOutput(std::vector<uint8_t>* out, bool block,
                              std::string* error) {
    AMediaCodec* c = static_cast<AMediaCodec*>(codec_);
    const int64_t timeoutUs = block ? 2000000 : 0;

    for (int guard = 0; guard < 16; ++guard) {   // 一轮最多取 16 个
        AMediaCodecBufferInfo info;
        memset(&info, 0, sizeof(info));
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(c, &info, timeoutUs);

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
                    for (int k = 0; k + 4 < info.size; ++k) {
                        if (p[k] == 0 && p[k + 1] == 0 &&
                            (p[k + 2] == 1 || (p[k + 2] == 0 && p[k + 3] == 1))) {
                            const size_t o = (p[k + 2] == 1) ? k + 3 : k + 4;
                            if (o < static_cast<size_t>(info.size) &&
                                (p[o] & 0x1F) == 7) {
                                codecString_ = CodecStringFromSps(
                                        p + o, static_cast<size_t>(info.size) - o);
                            }
                            break;
                        }
                    }
                }
                AMediaCodec_releaseOutputBuffer(c, static_cast<size_t>(idx), false);
                continue;
            }

            // 真正的帧：先把存着的 SPS/PPS 拼上（只拼一次）
            if (!codecConfig_.empty()) {
                out->insert(out->end(), codecConfig_.begin(), codecConfig_.end());
                codecConfig_.clear();
            }
            if (codecString_.empty()) {
                // Annex-B 起始码之后是 NAL 头，0x67 就是 SPS
                for (int k = 0; k + 4 < info.size; ++k) {
                    if (p[k] == 0 && p[k + 1] == 0 &&
                        (p[k + 2] == 1 || (p[k + 2] == 0 && p[k + 3] == 1))) {
                        const size_t off = (p[k + 2] == 1) ? k + 3 : k + 4;
                        if (off < static_cast<size_t>(info.size) &&
                            (p[off] & 0x1F) == 7) {          // 7 = SPS
                            codecString_ = CodecStringFromSps(
                                    p + off, static_cast<size_t>(info.size) - off);
                        }
                        break;
                    }
                }
            }
            // 追加（不是覆盖）—— 一次 dequeue 可能只有一个 NAL，
            // 但 SPS/PPS/IDR 可能分成多个 buffer
            out->insert(out->end(), p, p + info.size);
            stats_.bytesOut += static_cast<uint64_t>(info.size);
            ++stats_.framesOut;
        }
        AMediaCodec_releaseOutputBuffer(c, static_cast<size_t>(idx), false);
        if (!block) break;      // 非阻塞模式取一个就走
    }
    return true;
}

bool H264Encoder::EncodeRgba(const uint8_t* rgba, uint32_t width, uint32_t height,
                             std::vector<uint8_t>* out, std::string* error) {
    if (!running_ || codec_ == nullptr) {
        if (error) *error = "编码器没在跑";
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
    const int64_t t0 = NowMs();

    ssize_t idx = AMediaCodec_dequeueInputBuffer(c, 200000 /* 200ms */);
    if (idx < 0) {
        // 输入队列满 = 编码器跟不上。**不算错误** —— 丢掉这一帧，
        // 下一帧再来。对画面流来说丢帧远好过卡住。
        DrainOutput(out, /*block=*/false, error);
        return true;
    }

    size_t cap = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(c, static_cast<size_t>(idx), &cap);
    const size_t need = static_cast<size_t>(width) * height * 3 / 2;
    if (buf == nullptr || cap < need) {
        AMediaCodec_queueInputBuffer(c, static_cast<size_t>(idx), 0, 0, 0,
                                     AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG);
        if (error) {
            *error = "输入缓冲太小: " + std::to_string(cap) + " < " +
                     std::to_string(need);
        }
        return false;
    }

    RgbaToNv12(rgba, width, height, buf);

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

    const media_status_t st = AMediaCodec_queueInputBuffer(
            c, static_cast<size_t>(idx), 0, need, 0, 0);
    if (st != AMEDIA_OK) {
        if (error) *error = "queueInputBuffer: " + std::to_string((int)st);
        return false;
    }
    ++stats_.framesIn;

    if (!DrainOutput(out, /*block=*/false, error)) return false;

    stats_.lastEncodeMs = NowMs() - t0;
    return true;
}

#else   // !REMOTE_CONTROL_HAS_MEDIANDK

bool H264Encoder::EncodeRgba(const uint8_t*, uint32_t, uint32_t,
                             std::vector<uint8_t>*, std::string* error) {
    if (error) *error = "这个构建没有 libmediandk（H.264 需要 Android 构建）";
    return false;
}

bool H264Encoder::DrainOutput(std::vector<uint8_t>*, bool, std::string*) {
    return false;
}

#endif  // REMOTE_CONTROL_HAS_MEDIANDK

}  // namespace remote_control
