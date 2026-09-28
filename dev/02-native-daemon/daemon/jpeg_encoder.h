// jpeg_encoder.h — JPEG 编码（dlopen libjpeg）
//
// 为什么需要它：
//
//   JPEG/WebP 现在走的是 `AndroidBitmap_compress`（Skia），而那个 API
//   是 **`__INTRODUCED_IN(30)`** —— Android 11 才有。为了让
//   Android 8~10 也能出 JPEG，需要另一条路。
//
// 为什么是 dlopen 而不是直接链接：
//
//   NDK 不提供 libjpeg 的头文件和库。而设备上一直有
//   `/system/lib64/libjpeg.so`（VNDK 库，ABI 跨版本稳定）——
//   缺的只是头文件，vendored 在 `vendor/jpeg/`（来源见那个目录的 README）。
//
//   dlopen 还有个实际好处：**任何缺这个库的设备上只是 JPEG 不可用，
//   而不是启动就崩**。`/api/v1/describe` 会如实报告。
//
// ⚠️ 它和 Skia 是**同一个编码器**。Skia 的 `Android.bp` 里 libjpeg
//    就是依赖，`SkJpegEncoder` 直接调它的 C API。所以两条路的输出
//    在参数对齐后是一致的 —— 实测差 554 字节，正好是 Skia 多嵌的
//    一个 ICC 段（见 tools/bench/README.md 的证据链）。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace remote_control {

class JpegEncoder {
  public:
    static JpegEncoder& Instance();

    // 加载 libjpeg。失败不致命 —— 上层会退回 PNG。
    bool Init(std::string* error);

    bool Available() const { return available_; }

    // "libjpeg-turbo 2.1.0" 之类，给 /describe 用
    const char* BackendName() const { return backend_.c_str(); }

    // 编码 RGBA8888 → JPEG。
    //
    // quality 1-100。失败返回空串，原因写进 error。
    //
    // 内部**不**做 RGBA→RGB 转换：libjpeg-turbo 支持直接吃
    // `JCS_EXT_RGBA`，省掉每帧一次 460KB 的拷贝。
    std::string EncodeRgba(const uint8_t* rgba, uint32_t width, uint32_t height,
                           int quality, std::string* error);

  private:
    JpegEncoder() = default;
    ~JpegEncoder();

    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    // dlopen 出来的函数指针表。
    //
    // 用一堆具名成员而不是一个 void* 数组：dlsym 失败时能直接报出
    // **是哪个符号**缺了，而不是"libjpeg 不可用"。
    struct Api;
    Api*   api_ = nullptr;
    void*  handle_ = nullptr;
    bool   available_ = false;
    std::string backend_;
};

}  // namespace remote_control
