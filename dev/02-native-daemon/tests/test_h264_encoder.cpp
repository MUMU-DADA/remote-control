#include <media/NdkMediaCodec.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "h264_encoder.h"
#include "test_util.h"

using remote_control_test::Check;

struct AMediaFormat {
    std::map<std::string, int32_t> ints;
};

struct OutputBuffer {
    std::vector<uint8_t> bytes;
    AMediaCodecBufferInfo info{};
};

struct AMediaCodec {
    AMediaFormat format;
    std::vector<uint8_t> input;
    OutputBuffer active;
};

namespace {

std::deque<OutputBuffer> g_output;
ssize_t g_inputResult = 0;
int32_t g_stride = 4, g_sliceHeight = 2;
bool g_createFails = false;
int g_released = 0;
int g_formatChanges = 0;
int g_liveCodecs = 0;
std::vector<uint8_t> g_lastInput;
int64_t g_lastInputPts = 0;

void Output(std::vector<uint8_t> bytes, uint32_t flags = 0,
            int32_t offset = 0, int32_t size = -1) {
    if (size == -1) size = static_cast<int32_t>(bytes.size());
    g_output.push_back({std::move(bytes), {offset, size, 1, flags}});
}

}  // namespace

AMediaFormat* AMediaFormat_new() { return new AMediaFormat; }
void AMediaFormat_delete(AMediaFormat* f) { delete f; }
void AMediaFormat_setString(AMediaFormat*, const char*, const char*) {}
void AMediaFormat_setInt32(AMediaFormat* f, const char* key, int32_t value) { f->ints[key] = value; }
bool AMediaFormat_getInt32(AMediaFormat* f, const char* key, int32_t* value) {
    auto found = f->ints.find(key);
    if (found == f->ints.end()) return false;
    *value = found->second;
    return true;
}

AMediaCodec* AMediaCodec_createEncoderByType(const char*) {
    if (g_createFails) return nullptr;
    ++g_liveCodecs;
    return new AMediaCodec;
}
media_status_t AMediaCodec_configure(AMediaCodec* c, const AMediaFormat* f,
                                    void*, void*, uint32_t) {
    c->format = *f;
    c->input.resize(static_cast<size_t>(g_stride) * g_sliceHeight * 3 / 2);
    return AMEDIA_OK;
}
media_status_t AMediaCodec_start(AMediaCodec*) { return AMEDIA_OK; }
media_status_t AMediaCodec_stop(AMediaCodec*) { return AMEDIA_OK; }
media_status_t AMediaCodec_delete(AMediaCodec* c) {
    --g_liveCodecs;
    delete c;
    return AMEDIA_OK;
}
AMediaFormat* AMediaCodec_getInputFormat(AMediaCodec* c) {
    auto* f = new AMediaFormat(c->format);
    f->ints["stride"] = g_stride;
    f->ints["slice-height"] = g_sliceHeight;
    return f;
}
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec*, int64_t timeout) {
    Check(timeout <= 5000, "input dequeue has a 5ms budget");
    return g_inputResult;
}
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec* c, size_t, size_t* size) {
    *size = c->input.size();
    return c->input.data();
}
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec* c, size_t, size_t,
                                          size_t size, uint64_t pts, uint32_t) {
    g_lastInput.assign(c->input.begin(), c->input.begin() + size);
    g_lastInputPts = static_cast<int64_t>(pts);
    return AMEDIA_OK;
}
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec* c, AMediaCodecBufferInfo* info,
                                       int64_t timeout) {
    Check(timeout <= 20000, "output dequeue has a 20ms budget");
    if (g_formatChanges > 0) {
        --g_formatChanges;
        return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;
    }
    if (g_output.empty()) return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    c->active = std::move(g_output.front());
    g_output.pop_front();
    *info = c->active.info;
    return 0;
}
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec* c, size_t, size_t* size) {
    *size = c->active.bytes.size();
    return c->active.bytes.data();
}
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec*, size_t, bool) {
    ++g_released;
    return AMEDIA_OK;
}
media_status_t AMediaCodec_setParameters(AMediaCodec*, const AMediaFormat*) { return AMEDIA_OK; }

int main() {
    setenv("REMOTE_CONTROL_H264_LIBYUV", "0", 1);
    remote_control::H264Encoder encoder;
    remote_control::H264Encoder::Config cfg;
    cfg.width = 4;
    cfg.height = 2;
    std::string error;
    std::vector<uint8_t> out{9};
    std::vector<uint8_t> rgba(32, 255);

    g_createFails = true;
    Check(!encoder.Start(cfg, &error) && encoder.ActiveCount() == 0,
          "failed creation releases reserved slot");
    g_createFails = false;
    g_stride = 5;
    Check(!encoder.Start(cfg, &error) && g_liveCodecs == 0 && encoder.ActiveCount() == 0,
          "odd NV12 stride rejects and releases codec");
    g_stride = 4;
    g_sliceHeight = 3;
    Check(!encoder.Start(cfg, &error) && g_liveCodecs == 0 && encoder.ActiveCount() == 0,
          "odd NV12 slice-height rejects and releases codec");
    g_sliceHeight = 2;
    Check(encoder.Start(cfg, &error), "start valid codec");
    Check(encoder.PollOutput(&out, &error) && out.empty(), "empty poll succeeds and clears output");

    g_inputResult = -123;
    Check(!encoder.EncodeRgba(rgba.data(), 4, 2, &out, &error) &&
          encoder.GetStats().framesDropped == 0, "fatal input error is not treated as a drop");
    g_inputResult = AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    Check(encoder.EncodeRgba(rgba.data(), 4, 2, &out, &error) &&
          encoder.GetStats().framesDropped == 1, "busy input is a normal drop");

    Output({1, 2, 3}, 0, -1, 2);
    const int releasedBefore = g_released;
    Check(!encoder.PollOutput(&out, &error) && g_released == releasedBefore + 1,
          "negative offset rejected and output released");
    Output({1, 2, 3}, 0, 4, 1);
    Check(!encoder.PollOutput(&out, &error), "offset beyond capacity rejected");
    Output({1, 2, 3}, 0, 2, 2);
    Check(!encoder.PollOutput(&out, &error), "size beyond remaining capacity rejected");
    Output({1, 2, 3}, 0, -1, 2);
    Check(!encoder.EncodeRgba(rgba.data(), 4, 2, &out, &error),
          "input backpressure propagates drain error");
    g_inputResult = 0;

    const std::vector<uint8_t> config{0, 0, 0, 1, 0x68, 5,
                                      0, 0, 0, 1, 0x67, 0x42, 0xC0, 0x29};
    const std::vector<uint8_t> idr{0, 0, 0, 1, 0x65, 8};
    g_formatChanges = 1;
    Output(config, AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG);
    Output(idr);
    Check(encoder.PollOutput(&out, &error) && out.size() == config.size() + idr.size(),
          "codec config combines with first access unit");
    Check(encoder.CodecString() == "avc1.42C029", "SPS found after a preceding PPS");
    Output(idr);
    Check(encoder.PollOutput(&out, &error) && out.size() == config.size() + idr.size(),
          "each IDR repeats stored parameter sets");

    Output({0, 0, 0, 1, 0x61}, AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME);
    Check(encoder.PollOutput(&out, &error) && out.empty(), "partial access unit stays buffered");
    Output({8});
    Output({0, 0, 0, 1, 0x61, 9});
    Check(encoder.PollOutput(&out, &error) && out.size() == 6 && out.back() == 8,
          "partial access unit joins before emission");
    Check(encoder.PollOutput(&out, &error) && out.size() == 6 && out.back() == 9,
          "next access unit retains a separate boundary");

    encoder.Stop();
    g_stride = 8;
    g_sliceHeight = 4;
    Check(encoder.Start(cfg, &error), "padded even NV12 layout accepted");
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = 255;
        rgba[i + 1] = rgba[i + 2] = 0;
    }
    Check(encoder.EncodeRgba(rgba.data(), 4, 2, &out, &error), "scalar conversion into padded layout");
    Check(g_lastInput.size() == 48 && g_lastInput[0] == 82 && g_lastInput[4] == 16 &&
          g_lastInput[16] == 16 && g_lastInput[32] == 90 && g_lastInput[33] == 240 &&
          g_lastInput[36] == 128 && g_lastInput[40] == 128,
          "RGBA red, NV12 UV order, stride/slice offsets, neutral padding");
    const int64_t firstPts = g_lastInputPts;
    encoder.EncodeRgba(rgba.data(), 4, 2, &out, &error);
    Check(g_lastInputPts > firstPts, "input timestamps increase");
    encoder.Stop();
    Check(g_liveCodecs == 0 && encoder.ActiveCount() == 0, "all codecs and slots released");
    return remote_control_test::Summary("H.264 MediaCodec 回归");
}
