// h264_encoder.h — H.264 编码（设备端 MediaCodec）
//
// 为什么值得做：JPEG 每帧 11 KB，H.264 在同样的画质下能到几百字节 ——
// 差一个数量级。代价是消费端要能解（见下）。
//
// ⚠️ **消费端有限制**：WebCodecs 的 `VideoDecoder` 需要
//    Chrome/WebView **94+**。设备上的 WebView 是 91，解不了。
//    所以 H.264 必须**带自动回退** —— 探测不到 WebCodecs 就用 JPEG。
//    桌面浏览器（正常用法）没问题。
//
// 前置验证：tools/bench/h264_feasibility.c
// 它确认了原生进程能创建 AVC 编码器、configure/start 能过、
// 能编出 Annex-B 的 SPS。写这个模块之前先跑过它。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace autod {

class H264Encoder {
  public:
    static H264Encoder& Instance();

    struct Config {
        uint32_t width  = 0;
        uint32_t height = 0;
        uint32_t bitrate = 2'000'000;   // bps
        uint32_t fps     = 30;
        uint32_t iFrameIntervalSec = 2;
    };

    // 创建并启动编码器。尺寸变了要 Stop 再 Start。
    bool Start(const Config& cfg, std::string* error);
    void Stop();
    bool Running() const;

    // 当前配置（Running() 为假时无意义）
    const Config& CurrentConfig() const { return cfg_; }

    // SPS 里的 profile/level，拼成 WebCodecs 要的 codec 串。
    //
    // 形如 "avc1.42C029"。它来自 SPS 的第 1~3 字节 —— 各设备的
    // profile/level 不同，**不能写死**，否则浏览器的
    // VideoDecoder.isConfigSupported 会拒掉。
    // 还没拿到 SPS 时返回空串。
    std::string CodecString() const;

    // 送一帧 RGBA，把**这次能取到的** NAL 数据追加到 out。
    //
    // 返回 false 表示编码器出错（调用方应 Stop）。
    // out 为空是**正常**的 —— 编码器有内部缓冲，不是每送一帧
    // 就立刻出一帧。
    bool EncodeRgba(const uint8_t* rgba, uint32_t width, uint32_t height,
                    std::vector<uint8_t>* out, std::string* error);

    // 要求下一个输出帧是关键帧。
    //
    // 新客户端接入时必须调 —— 否则它要等到下一个 I 帧（默认 2 秒）
    // 才能解出画面，这期间是黑的。
    void RequestKeyFrame();

    struct Stats {
        uint64_t framesIn  = 0;
        uint64_t framesOut = 0;
        uint64_t bytesOut  = 0;
        int64_t  lastEncodeMs = 0;
    };
    Stats GetStats() const;

  private:
    H264Encoder() = default;
    ~H264Encoder();
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    bool DrainOutput(std::vector<uint8_t>* out, bool block, std::string* error);

    void*  codec_ = nullptr;          // AMediaCodec*
    Config cfg_;
    bool   running_ = false;
    bool   wantKeyFrame_ = false;
    std::string codecString_;
    Stats  stats_;
    std::vector<uint8_t> yuv_;        // RGBA→YUV 的转换缓冲，复用避免每帧分配
};

}  // namespace autod
