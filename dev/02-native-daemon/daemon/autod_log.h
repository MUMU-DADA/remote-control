// autod_log.h —— 日志兼容层
//
// Android 上用 liblog（日志进 logd，可用 logcat 看）。
// 主机上（单元测试、协议联调）退回 stderr。
//
// 这样 inject.cpp / capture.cpp 这类容易被平台 API 绑死的文件，
// 只要不碰平台头文件就能在开发机上直接编译测试。

#pragma once

#ifdef __ANDROID__
#include <log/log.h>
#else
#include <stdio.h>

#define ALOGI(...)                                    \
    do {                                              \
        fprintf(stderr, "I autod: ");                 \
        fprintf(stderr, __VA_ARGS__);                 \
        fprintf(stderr, "\n");                        \
    } while (0)

#define ALOGW(...)                                    \
    do {                                              \
        fprintf(stderr, "W autod: ");                 \
        fprintf(stderr, __VA_ARGS__);                 \
        fprintf(stderr, "\n");                        \
    } while (0)

#define ALOGE(...)                                    \
    do {                                              \
        fprintf(stderr, "E autod: ");                 \
        fprintf(stderr, __VA_ARGS__);                 \
        fprintf(stderr, "\n");                        \
    } while (0)

#define LOG_ALWAYS_FATAL_IF(cond, ...)                \
    do {                                              \
        if (cond) {                                   \
            fprintf(stderr, "F autod: ");             \
            fprintf(stderr, __VA_ARGS__);             \
            fprintf(stderr, "\n");                    \
            abort();                                  \
        }                                             \
    } while (0)

#endif
