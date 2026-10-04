#pragma once

#include <cstdint>

struct AMediaFormat;

inline constexpr const char* AMEDIAFORMAT_KEY_MIME = "mime";
inline constexpr const char* AMEDIAFORMAT_KEY_WIDTH = "width";
inline constexpr const char* AMEDIAFORMAT_KEY_HEIGHT = "height";
inline constexpr const char* AMEDIAFORMAT_KEY_BIT_RATE = "bitrate";
inline constexpr const char* AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate";
inline constexpr const char* AMEDIAFORMAT_KEY_I_FRAME_INTERVAL = "i-frame-interval";
inline constexpr const char* AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";

AMediaFormat* AMediaFormat_new();
void AMediaFormat_delete(AMediaFormat*);
void AMediaFormat_setString(AMediaFormat*, const char*, const char*);
void AMediaFormat_setInt32(AMediaFormat*, const char*, int32_t);
bool AMediaFormat_getInt32(AMediaFormat*, const char*, int32_t*);
