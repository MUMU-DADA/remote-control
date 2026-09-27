// autod_log.h —— 日志兼容层
//
// 三种构建形态：
//
//   1. AOSP 平台构建（cc_binary，链 liblog）
//      → <android/log.h>，日志进 logd，logcat -s autod 可见
//
//   2. NDK 构建（不需要 AOSP 树）
//      → 同样用 <android/log.h>，NDK 里有这个头，链 -llog
//      → 这样 autod 可以用一套源码同时支持两种构建
//
//   3. 主机构建（开发机上跑单元/集成测试）
//      → 回退到 stderr
//
// 注意：用 <android/log.h> 而不是 <log/log.h>，因为后者不在 NDK 里。
//       AOSP 的 liblog 同时提供这两个头，所以用前者两边都能编。

#pragma once

#if defined(__ANDROID__)

#include <android/log.h>

// 平台构建时，libgui / libutils 的头会先把 <log/log.h> 带进来，而它已经定义过
// ALOGI / ALOGW / ALOGE / LOG_ALWAYS_FATAL_IF（用 LOG_TAG 当 tag）。
// 直接重定义会撞 -Werror,-Wmacro-redefined（实测只有 capture_surfaceflinger.cpp
// 中招，因为只有它先引了 libgui 的头）。
//
// 这里先 undef 再用自己的 tag 定义 —— 而不是用 #ifndef 跳过，
// 因为跳过会让日志 tag 变成别人设的 LOG_TAG，logcat -s autod 就过滤不到了。
#undef ALOGI
#undef ALOGW
#undef ALOGE
#undef LOG_ALWAYS_FATAL_IF

#define AUTOD_LOG_TAG "autod"

#define ALOGI(...) \
    __android_log_print(ANDROID_LOG_INFO,  AUTOD_LOG_TAG, __VA_ARGS__)
#define ALOGW(...) \
    __android_log_print(ANDROID_LOG_WARN,  AUTOD_LOG_TAG, __VA_ARGS__)
#define ALOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, AUTOD_LOG_TAG, __VA_ARGS__)

#define LOG_ALWAYS_FATAL_IF(cond, ...)                     \
    do {                                                   \
        if (cond) {                                        \
            __android_log_assert(#cond, AUTOD_LOG_TAG,     \
                                 __VA_ARGS__);             \
        }                                                  \
    } while (0)

#else   // 主机

#include <stdio.h>
#include <stdlib.h>

#define ALOGI(...)                        \
    do {                                  \
        fprintf(stderr, "I autod: ");     \
        fprintf(stderr, __VA_ARGS__);     \
        fprintf(stderr, "\n");            \
    } while (0)

#define ALOGW(...)                        \
    do {                                  \
        fprintf(stderr, "W autod: ");     \
        fprintf(stderr, __VA_ARGS__);     \
        fprintf(stderr, "\n");            \
    } while (0)

#define ALOGE(...)                        \
    do {                                  \
        fprintf(stderr, "E autod: ");     \
        fprintf(stderr, __VA_ARGS__);     \
        fprintf(stderr, "\n");            \
    } while (0)

#define LOG_ALWAYS_FATAL_IF(cond, ...)    \
    do {                                  \
        if (cond) {                       \
            fprintf(stderr, "F autod: "); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n");        \
            abort();                      \
        }                                 \
    } while (0)

#endif
