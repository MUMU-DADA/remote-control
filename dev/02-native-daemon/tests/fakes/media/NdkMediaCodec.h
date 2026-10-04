#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

#include "NdkMediaFormat.h"

using media_status_t = int;
struct AMediaCodec;
struct AMediaCodecBufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
};

enum {
    AMEDIA_OK = 0,
    AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG = 2,
    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4,
    AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME = 8,
    AMEDIACODEC_CONFIGURE_FLAG_ENCODE = 1,
    AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3,
    AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2,
    AMEDIACODEC_INFO_TRY_AGAIN_LATER = -1,
};

AMediaCodec* AMediaCodec_createEncoderByType(const char*);
media_status_t AMediaCodec_configure(AMediaCodec*, const AMediaFormat*, void*, void*, uint32_t);
media_status_t AMediaCodec_start(AMediaCodec*);
media_status_t AMediaCodec_stop(AMediaCodec*);
media_status_t AMediaCodec_delete(AMediaCodec*);
AMediaFormat* AMediaCodec_getInputFormat(AMediaCodec*);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec*, int64_t);
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec*, size_t, size_t*);
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec*, size_t, size_t, size_t, uint64_t, uint32_t);
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec*, AMediaCodecBufferInfo*, int64_t);
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec*, size_t, size_t*);
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec*, size_t, bool);
media_status_t AMediaCodec_setParameters(AMediaCodec*, const AMediaFormat*);
