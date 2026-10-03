// png_encoder.cpp — PNG 编码（zlib 运行时加载）

#include "png_encoder.h"

#include <dlfcn.h>
#include <string.h>

#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>

#include "remote_control_log.h"

namespace remote_control {
namespace {

// ── zlib 的最小 ABI ─────────────────────────────────────────────────────────
//
// 只声明用到的两个函数。zlib 的 C ABI 极稳定，硬编码是安全的。
using uLong = unsigned long;
using uLongf = unsigned long;
using Bytef = unsigned char;

constexpr int Z_OK = 0;
// 0 = 不压缩，1 = 最快，9 = 最小。6 是 zlib 默认值。
constexpr int kZDefaultCompression = -1;

struct ZlibApi {
    int   (*compress2)(Bytef* dest, uLongf* destLen, const Bytef* source,
                       uLong sourceLen, int level) = nullptr;
    uLong (*compressBound)(uLong sourceLen) = nullptr;
    uLong (*crc32)(uLong crc, const Bytef* buf, unsigned int len) = nullptr;
    const char* (*zlibVersion)() = nullptr;
};

ZlibApi g_z;
std::atomic<bool> g_loaded{false};
std::mutex g_zlib_mutex;

void PutU32(std::string* out, uint32_t v) {
    // PNG 全部是大端
    out->push_back(static_cast<char>((v >> 24) & 0xFF));
    out->push_back(static_cast<char>((v >> 16) & 0xFF));
    out->push_back(static_cast<char>((v >> 8) & 0xFF));
    out->push_back(static_cast<char>(v & 0xFF));
}

// 一个 PNG chunk：长度 + 类型 + 数据 + CRC（对类型和数据一起算）
void PutChunk(std::string* out, const char type[4], const std::string& data) {
    PutU32(out, static_cast<uint32_t>(data.size()));
    const size_t crcStart = out->size();
    out->append(type, 4);
    out->append(data);
    const uLong crc = g_z.crc32(0L, reinterpret_cast<const Bytef*>(out->data() + crcStart),
                                static_cast<unsigned int>(4 + data.size()));
    PutU32(out, static_cast<uint32_t>(crc));
}

// 选滤波类型。
//
// PNG 允许每行选不同的滤波器，选得好不好直接决定压缩率。
// 这里用的是规范推荐的启发式：对每种候选算"滤波后字节的绝对值和"，
// 取最小的那个 —— 和越小说明残差越集中，deflate 压得越好。
//
// 只试 0(None)/1(Sub)/2(Up) 三种：Paeth 收益有限而计算更贵，
// 而截图这类内容 Sub/Up 已经能拿到绝大部分收益。
uint8_t ChooseFilter(const uint8_t* cur, const uint8_t* prev, size_t rowBytes,
                     std::vector<uint8_t>* out) {
    out->resize(rowBytes);

    uint8_t bestFilter = 0;
    uint64_t bestScore = UINT64_MAX;

    for (uint8_t f = 0; f <= 2; ++f) {
        uint64_t score = 0;
        for (size_t i = 0; i < rowBytes; ++i) {
            const uint8_t a = (i >= 4) ? cur[i - 4] : 0;         // 左
            const uint8_t b = prev ? prev[i] : 0;                 // 上
            uint8_t v;
            switch (f) {
                case 1:  v = static_cast<uint8_t>(cur[i] - a); break;
                case 2:  v = static_cast<uint8_t>(cur[i] - b); break;
                default: v = cur[i]; break;
            }
            // 绝对值和：把有符号字节映射成无符号距离
            score += (v < 128) ? v : (256 - v);
        }
        if (score < bestScore) {
            bestScore = score;
            bestFilter = f;
        }
    }

    for (size_t i = 0; i < rowBytes; ++i) {
        const uint8_t a = (i >= 4) ? cur[i - 4] : 0;
        const uint8_t b = prev ? prev[i] : 0;
        switch (bestFilter) {
            case 1:  (*out)[i] = static_cast<uint8_t>(cur[i] - a); break;
            case 2:  (*out)[i] = static_cast<uint8_t>(cur[i] - b); break;
            default: (*out)[i] = cur[i]; break;
        }
    }
    return bestFilter;
}

}  // namespace

PngEncoder& PngEncoder::Instance() {
    static PngEncoder e;
    return e;
}

bool PngEncoder::Init(std::string* error) {
    if (available_.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> lock(g_zlib_mutex);
    if (g_loaded.load(std::memory_order_acquire)) {
        available_.store(true, std::memory_order_release);
        return true;
    }

    const char* candidates[] = {"libz.so", "libz.so.1", nullptr};
    void* h = nullptr;
    std::string lastErr;
    for (int i = 0; candidates[i] != nullptr; ++i) {
        h = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (h != nullptr) break;
        // ⚠️ dlerror() 只能调一次 —— 它取走错误后会清掉，第二次返回 nullptr。
        //    写成 `dlerror() ? dlerror() : "…"` 在拼接/赋值时就会拿到
        //    nullptr（std::string 会段错误，const char* 则得到一个空指针）。
        //    实测：dlopen 失败时这里直接崩。
        const char* dlErr = dlerror();
        lastErr = dlErr ? dlErr : "unknown";
    }
    if (h == nullptr) {
        if (error) *error = "设备上没有可用的 zlib（" + lastErr + "）—— 无法编码 PNG";
        return false;
    }

    ZlibApi api;
    api.compress2     = reinterpret_cast<decltype(api.compress2)>(dlsym(h, "compress2"));
    api.compressBound = reinterpret_cast<decltype(api.compressBound)>(dlsym(h, "compressBound"));
    api.crc32         = reinterpret_cast<decltype(api.crc32)>(dlsym(h, "crc32"));
    api.zlibVersion   = reinterpret_cast<decltype(api.zlibVersion)>(dlsym(h, "zlibVersion"));

    if (api.compress2 == nullptr || api.compressBound == nullptr ||
        api.crc32 == nullptr) {
        dlclose(h);
        if (error) *error = "zlib 缺少必需符号（compress2/compressBound/crc32）";
        return false;
    }

    // The shared function table remains valid until process exit.
    g_z = api;
    g_loaded.store(true, std::memory_order_release);
    available_.store(true, std::memory_order_release);
    ALOGI("zlib 已加载: %s（PNG 编码可用）",
          g_z.zlibVersion ? g_z.zlibVersion() : "unknown");
    return true;
}

std::string PngEncoder::EncodeRgba(const uint8_t* pixels, uint32_t width,
                                   uint32_t height, int compressionLevel,
                                   std::string* error) {
    if (!g_loaded.load(std::memory_order_acquire)) {
        if (error) *error = "zlib 未加载";
        return {};
    }
    if (pixels == nullptr || width == 0 || height == 0) {
        if (error) *error = "尺寸或像素为空";
        return {};
    }
    // 防御：宽高相乘可能溢出，也会让分配变得荒谬
    if (width > 65535 || height > 65535) {
        if (error) *error = "尺寸超出 PNG 支持范围";
        return {};
    }

    const size_t rowBytes = static_cast<size_t>(width) * 4;
    const uint64_t rawSize = static_cast<uint64_t>(rowBytes + 1) * height;
    if (rawSize > (256ull << 20)) {
        if (error) *error = "图像过大（原始数据超过 256MB）";
        return {};
    }

    // ── 1. 逐行滤波，拼成 deflate 的输入 ──
    std::string raw;
    raw.reserve(static_cast<size_t>(rawSize));
    std::vector<uint8_t> filtered;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* cur = pixels + static_cast<size_t>(y) * rowBytes;
        const uint8_t* prev = (y > 0) ? (pixels + static_cast<size_t>(y - 1) * rowBytes)
                                      : nullptr;
        const uint8_t f = ChooseFilter(cur, prev, rowBytes, &filtered);
        raw.push_back(static_cast<char>(f));
        raw.append(reinterpret_cast<const char*>(filtered.data()), rowBytes);
    }

    // ── 2. deflate ──
    uLongf bound = g_z.compressBound(static_cast<uLong>(raw.size()));
    std::string compressed;
    compressed.resize(bound);
    uLongf destLen = bound;
    const int rc = g_z.compress2(
            reinterpret_cast<Bytef*>(&compressed[0]), &destLen,
            reinterpret_cast<const Bytef*>(raw.data()),
            static_cast<uLong>(raw.size()),
            compressionLevel == 0 ? kZDefaultCompression : compressionLevel);
    if (rc != Z_OK) {
        if (error) *error = "zlib 压缩失败，返回码 " + std::to_string(rc);
        return {};
    }
    compressed.resize(destLen);

    // ── 3. 拼 PNG ──
    std::string png;
    png.reserve(compressed.size() + 128);

    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    png.append(reinterpret_cast<const char*>(sig), 8);

    {
        std::string ihdr;
        PutU32(&ihdr, width);
        PutU32(&ihdr, height);
        ihdr.push_back(8);    // 位深
        ihdr.push_back(6);    // 颜色类型 6 = RGBA
        ihdr.push_back(0);    // 压缩方法（只有 0 合法）
        ihdr.push_back(0);    // 滤波方法（只有 0 合法）
        ihdr.push_back(0);    // 隔行扫描：0 = 不隔行
        PutChunk(&png, "IHDR", ihdr);
    }

    PutChunk(&png, "IDAT", compressed);
    PutChunk(&png, "IEND", std::string());

    return png;
}

}  // namespace remote_control
