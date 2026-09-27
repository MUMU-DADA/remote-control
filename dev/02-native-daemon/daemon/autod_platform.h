// autod_platform.h —— 构建形态判定
//
// autod 有三种构建形态，能力不同：
//
//   ┌─────────────────────┬──────────┬──────────┬──────────┐
//   │                     │ AOSP 平台 │ NDK      │ 主机     │
//   ├─────────────────────┼──────────┼──────────┼──────────┤
//   │ 宏                  │ 全定义   │ 都不定义 │ 都不定义 │
//   │ ProcessState/Binder │ ✅       │ ❌       │ ❌       │
//   │ init socket 接管    │ ✅       │ ❌       │ ❌       │
//   │ 截图后端            │ SF/桩    │ screencap│ 桩       │
//   │ 触控后端            │ uinput   │ uinput   │ uinput   │
//   │ liblog              │ ✅       │ ✅(-llog)│ stderr   │
//   └─────────────────────┴──────────┴──────────┴──────────┘
//
// ⚠️ 不要用 __ANDROID__ 来区分「AOSP 平台构建」和「NDK 构建」——
//    NDK 构建时 __ANDROID__ 同样是定义的。这是之前埋过的一个坑。

#pragma once

// AOSP 平台构建：链 libbinder / libgui / libcutils，能调系统服务。
// 由 Android.bp 里的 cflags 定义。
// #define AUTOD_FULL_PLATFORM

// NDK 构建：只有 bionic + liblog，没有平台私有库。
// 由 build/ndk 的构建脚本定义。
// #define AUTOD_NDK_BUILD

#if defined(AUTOD_FULL_PLATFORM)
    // AOSP 树里编，能力最全
    #define AUTOD_HAS_BINDER_PLATFORM 1
    #define AUTOD_HAS_INIT_SOCKET     1
#else
    // NDK 或主机：没有平台私有库
    #define AUTOD_HAS_BINDER_PLATFORM 0
    #define AUTOD_HAS_INIT_SOCKET     0
#endif

#if defined(__ANDROID__)
    #define AUTOD_IS_ANDROID 1
#else
    #define AUTOD_IS_ANDROID 0
#endif
