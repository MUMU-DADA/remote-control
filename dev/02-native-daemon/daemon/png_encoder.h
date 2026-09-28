// png_encoder.h — 把原始 RGBA 编成 PNG
//
// 为什么在服务端做：HTTP API 要能直接被浏览器打开。返回原始像素的接口
// 只有写代码的人能用，"对外提供 api"的意义就少了一半。
//
// 依赖：zlib（dlopen，和 libcurl 同样的理由 —— NDK 里没有它的头文件，
// 而设备上 /system/lib64/libz.so 是有的）。zlib 提供 deflate 和 crc32
// 两块，恰好是 PNG 需要的全部。

#pragma once

#include <cstdint>
#include <string>

namespace remote_control {

class PngEncoder {
  public:
    static PngEncoder& Instance();

    // 尝试加载 zlib。失败时 Available() 为 false。
    bool Init(std::string* error);
    bool Available() const { return handle_ != nullptr; }

    // 把 RGBA_8888 像素编成 PNG。
    //
    // 只支持每像素 4 字节的格式 —— 调用方负责先转成 RGBA。
    // 失败时返回空串并写 error（不抛异常，本项目全程不用异常）。
    std::string EncodeRgba(const uint8_t* pixels, uint32_t width, uint32_t height,
                           int compressionLevel, std::string* error);

  private:
    PngEncoder() = default;

    void* handle_ = nullptr;
};

}  // namespace remote_control
