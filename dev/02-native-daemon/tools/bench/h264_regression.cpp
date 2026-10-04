#include <dlfcn.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "h264_encoder.h"

using remote_control::H264Encoder;

namespace {

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool HasNal(const std::vector<uint8_t>& data, uint8_t type) {
    for (size_t i = 0; i + 3 < data.size(); ++i) {
        if (data[i] != 0 || data[i + 1] != 0) continue;
        size_t offset = 0;
        if (data[i + 2] == 1) offset = i + 3;
        else if (i + 4 < data.size() && data[i + 2] == 0 && data[i + 3] == 1) offset = i + 4;
        if (offset != 0 && offset < data.size() && (data[offset] & 31) == type) return true;
    }
    return false;
}

bool EncodeOne(H264Encoder* encoder, const std::vector<uint8_t>& rgba,
               uint32_t width, uint32_t height, std::vector<uint8_t>* output,
               std::string* error) {
    const auto before = encoder->GetStats();
    const int64_t deadline = NowMs() + 2000;
    while (encoder->GetStats().framesIn == before.framesIn && NowMs() < deadline) {
        if (!encoder->EncodeRgba(rgba.data(), width, height, output, error)) return false;
    }
    if (encoder->GetStats().framesIn != before.framesIn + 1) {
        *error = "single input was never accepted";
        return false;
    }
    while (output->empty() && NowMs() < deadline) {
        if (!encoder->PollOutput(output, error, 20)) return false;
    }
    if (output->empty()) {
        *error = "single input never produced output without another input";
        return false;
    }
    return true;
}

bool TestLibyuvColors() {
    void* handle = dlopen("libyuv.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        puts("SKIP libyuv colors: library unavailable");
        return true;
    }
    using Convert = int (*)(const uint8_t*, int, uint8_t*, int, uint8_t*, int, int, int);
    auto convert = reinterpret_cast<Convert>(dlsym(handle, "ABGRToNV12"));
    if (convert == nullptr) {
        dlclose(handle);
        puts("SKIP libyuv colors: ABGRToNV12 unavailable");
        return true;
    }
    // Pure red/blue distinguish RGBA from BGRA and NV12 UV from NV21 VU.
    for (const bool red : {true, false}) {
        const uint8_t r = red ? 255 : 0;
        const uint8_t b = red ? 0 : 255;
        const std::array<uint8_t, 16> rgba = {r, 0, b, 255, r, 0, b, 255,
                                            r, 0, b, 255, r, 0, b, 255};
        std::array<uint8_t, 6> yuv{};
        if (convert(rgba.data(), 8, yuv.data(), 2, yuv.data() + 4, 2, 2, 2) != 0) {
            dlclose(handle);
            return false;
        }
        const int y = red ? 82 : 41;
        const int u = red ? 90 : 240;
        const int v = red ? 240 : 110;
        if (std::abs(static_cast<int>(yuv[0]) - y) > 2 ||
            std::abs(static_cast<int>(yuv[4]) - u) > 2 ||
            std::abs(static_cast<int>(yuv[5]) - v) > 2) {
            printf("FAIL colors: %s -> Y=%u U=%u V=%u\n", red ? "red" : "blue",
                   yuv[0], yuv[4], yuv[5]);
            dlclose(handle);
            return false;
        }
    }
    dlclose(handle);
    puts("PASS libyuv RGBA channel order, NV12 UV order, BT.601 limited range");
    return true;
}

}  // namespace

int main() {
    if (!TestLibyuvColors()) return 1;
    H264Encoder encoder;
    H264Encoder::Config cfg;
    cfg.width = 320;
    cfg.height = 480;
    cfg.fps = 30;
    cfg.iFrameIntervalSec = 60;
    std::string error;
    auto odd = cfg;
    odd.width = 319;
    if (encoder.Start(odd, &error) || H264Encoder::ActiveCount() != 0) {
        puts("FAIL odd NV12 dimensions or encoder slot leaked");
        return 1;
    }
    if (!encoder.Start(cfg, &error)) {
        printf("FAIL start: %s\n", error.c_str());
        return 1;
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(cfg.width) * cfg.height * 4, 0);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = 220;
        rgba[i + 1] = 40;
        rgba[i + 2] = 30;
        rgba[i + 3] = 255;
    }
    std::vector<uint8_t> output;
    if (!EncodeOne(&encoder, rgba, cfg.width, cfg.height, &output, &error)) {
        printf("FAIL first input: %s\n", error.c_str());
        return 1;
    }
    if (!HasNal(output, 7) || !HasNal(output, 8) || !HasNal(output, 5)) {
        puts("FAIL first IDR missing SPS/PPS");
        return 1;
    }
    const int64_t firstPts = encoder.GetStats().lastOutputPtsUs;
    puts("PASS one input emits complete SPS/PPS/IDR without a subsequent input");
    output.clear();
    encoder.RequestKeyFrame();
    if (!EncodeOne(&encoder, rgba, cfg.width, cfg.height, &output, &error)) {
        printf("FAIL second input: %s\n", error.c_str());
        return 1;
    }
    if (!HasNal(output, 7) || !HasNal(output, 8) || !HasNal(output, 5) ||
        encoder.GetStats().lastOutputPtsUs <= firstPts) {
        puts("FAIL requested IDR missing SPS/PPS or non-monotonic timestamp");
        return 1;
    }
    puts("PASS requested IDR repeats SPS/PPS with increasing timestamp");
    output.assign(3, 255);
    if (!encoder.PollOutput(&output, &error) || !output.empty()) {
        puts("FAIL empty output polling");
        return 1;
    }
    cfg.bitrate /= 2;
    if (!encoder.Start(cfg, &error) || encoder.CurrentConfig().bitrate != cfg.bitrate) {
        printf("FAIL bitrate update: %s\n", error.c_str());
        return 1;
    }
    encoder.Stop();
    if (H264Encoder::ActiveCount() != 0) {
        puts("FAIL encoder slot leaked after stop");
        return 1;
    }
    puts("PASS empty polling, bitrate update, slot accounting, odd-size rejection");
    return 0;
}
