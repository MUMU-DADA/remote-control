// h264_bench.cpp — 用真实抓帧验证 H264Encoder
//
// 回答两个问题：
//   1. 编得出来吗（前几帧有没有 SPS/PPS/IDR）
//   2. 编一帧要多久、多少字节（和 JPEG 比）
//
// 用法（推到设备上跑）：
//   h264_bench <raw-RGBA 文件> [宽] [高] [质量?] [帧数]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <time.h>
#include <unistd.h>

#include "h264_encoder.h"

using namespace remote_control;

namespace {

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// 把 Annex-B 流里的 NAL 类型列出来（只列前几个）
std::string NalSummary(const std::vector<uint8_t>& d, size_t maxNals = 6) {
    std::string out;
    size_t i = 0;
    int n = 0;
    while (i + 4 <= d.size() && n < static_cast<int>(maxNals)) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
            const size_t off = (d[i + 2] == 1) ? i + 3 : i + 4;
            if (off < d.size()) {
                const int t = d[off] & 0x1F;
                const char* name = (t == 1) ? "非IDR" : (t == 5) ? "IDR" :
                                   (t == 7) ? "SPS" : (t == 8) ? "PPS" :
                                   (t == 6) ? "SEI" : "?";
                if (!out.empty()) out += " ";
                out += name;
                ++n;
            }
            i = off + 1;
        } else {
            ++i;
        }
    }
    return out.empty() ? "（没找到起始码）" : out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("用法: %s <raw-RGBA> [宽] [高] [帧数]\n", argv[0]);
        return 1;
    }
    const uint32_t w = argc > 2 ? static_cast<uint32_t>(atoi(argv[2])) : 320;
    const uint32_t h = argc > 3 ? static_cast<uint32_t>(atoi(argv[3])) : 480;
    const int rounds = argc > 4 ? atoi(argv[4]) : 30;

    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    FILE* f = fopen(argv[1], "rb");
    if (f == nullptr || fread(rgba.data(), 1, rgba.size(), f) != rgba.size()) {
        printf("读不了 %s（需要 %zu 字节）\n", argv[1], rgba.size());
        if (f) fclose(f);
        return 1;
    }
    fclose(f);

    printf("\n=== H264Encoder 验证 ===\n");
    printf("  %ux%u，%d 帧，真实抓帧\n\n", w, h, rounds);

    H264Encoder::Config cfg;
    cfg.width  = w;
    cfg.height = h;
    cfg.bitrate = 2'000'000;
    cfg.fps = 30;
    cfg.iFrameIntervalSec = 2;

    std::string err;
    if (!H264Encoder::Instance().Start(cfg, &err)) {
        printf("  x Start 失败: %s\n", err.c_str());
        return 1;
    }
    printf("  + 编码器已启动\n");

    size_t totalBytes = 0;
    int    withData = 0;
    int64_t totalMs = 0;
    std::vector<uint8_t> firstChunk;

    for (int i = 0; i < rounds; ++i) {
        // 每帧稍微改一点，否则编码器可能把内容完全相同的帧压到极小
        rgba[(static_cast<size_t>(i) * 997) % (rgba.size() - 4)] =
                static_cast<uint8_t>(i * 7);

        std::vector<uint8_t> out;
        const int64_t t0 = NowMs();
        const bool ok = H264Encoder::Instance().EncodeRgba(
                rgba.data(), w, h, &out, &err);
        const int64_t dt = NowMs() - t0;
        totalMs += dt;

        if (!ok) {
            printf("  x 第 %d 帧失败: %s\n", i, err.c_str());
            H264Encoder::Instance().Stop();
            return 1;
        }
        totalBytes += out.size();
        if (!out.empty()) {
            ++withData;
            if (firstChunk.empty()) firstChunk = out;
            if (i < 6) {
                printf("  第 %d 帧: %5zu 字节  %6lld ms  NAL: %s\n",
                       i, out.size(), (long long)dt, NalSummary(out).c_str());
            }
        }
    }

    const auto st = H264Encoder::Instance().GetStats();
    printf("\n  ── 汇总 ──\n");
    printf("  送帧 %llu，出帧 %llu，有数据的轮次 %d/%d\n",
           (unsigned long long)st.framesIn, (unsigned long long)st.framesOut,
           withData, rounds);
    printf("  总输出 %zu 字节，平均 %.0f 字节/轮\n",
           totalBytes, static_cast<double>(totalBytes) / rounds);
    printf("  平均耗时 %.2f ms/轮\n", static_cast<double>(totalMs) / rounds);
    printf("  codec 串: %s\n",
           H264Encoder::Instance().CodecString().empty()
                   ? "（还没解析出 SPS）"
                   : H264Encoder::Instance().CodecString().c_str());
    if (!firstChunk.empty()) {
        printf("  首块 NAL: %s\n", NalSummary(firstChunk).c_str());
    }

    printf("\n  参考: JPEG q75 在 320x480 上约 11 KB / 1.1 ms\n");
    printf("  H.264 的对比要注意：只有 I 帧才该和 JPEG 比，P 帧小得多\n\n");

    // 落一份出来，方便 ffmpeg 之类的工具验证
    if (argc > 5) {
        FILE* o = fopen(argv[5], "wb");
        if (o != nullptr) {
            // 再跑一遍，把所有输出拼起来
            H264Encoder::Instance().Stop();
            H264Encoder::Instance().Start(cfg, &err);
            for (int i = 0; i < rounds; ++i) {
                std::vector<uint8_t> out;
                H264Encoder::Instance().EncodeRgba(rgba.data(), w, h, &out, &err);
                if (!out.empty()) fwrite(out.data(), 1, out.size(), o);
            }
            fclose(o);
            printf("  已写出 %s（Annex-B，可用 ffplay 直接播）\n\n", argv[5]);
        }
    }

    H264Encoder::Instance().Stop();
    return 0;
}
