// bench_encoder.cpp — 编码器性能基准
//
// 目的：回答"如果降到 Android 8，PNG-only 会损失多少性能"。
//
// Android 8~10 上不能用 AndroidBitmap_compress（API 30 才有），
// 只能走 png_encoder（dlopen zlib）。而那条路现在只在非 Android
// 构建里用过 —— **它到底多快，没人测过**。
//
// 这个基准在同一台设备上跑两条路，给出可比数字。
//
// ⚠️ 图像内容对结果影响极大。
//
//    第一版用合成的"像屏幕"图（渐变 + 细线 + 色块），测出来 PNG 比 JPEG
//    **还小** —— 因为那张图太规整，PNG 的预测器吃得很爽。而真机上
//    PNG 86KB / JPEG 11KB，正好相反。
//
//    所以默认要读一张**真实抓帧**（raw RGBA）。合成图只作为退路，
//    而且它给的是失真的结论。
//
// 用法（推到设备上跑）：
//   bench_encoder [宽] [高] [轮数] [raw-RGBA 文件]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <time.h>
#include <unistd.h>

#include "image_encoder.h"
#include "png_encoder.h"

using namespace autod;

namespace {

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// 造一张"像屏幕"的图：大块纯色 + 一些细线（文字边框的样子）。
//
// 不用随机噪声：那既不像屏幕，也会让所有无损编码器都退化到
// 最差情况，测出来的数字没有参考价值。
void MakeScreenLike(std::vector<uint8_t>* buf, uint32_t w, uint32_t h) {
    buf->resize(static_cast<size_t>(w) * h * 4);
    uint8_t* p = buf->data();
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* q = p + (static_cast<size_t>(y) * w + x) * 4;
            // 背景：竖向渐变（模拟很多应用的浅色背景）
            uint8_t bg = static_cast<uint8_t>(238 - (y * 20 / h));
            q[0] = bg; q[1] = bg; q[2] = bg; q[3] = 255;
            // 顶部状态栏
            if (y < h / 16) { q[0] = 40; q[1] = 40; q[2] = 40; }
            // 每 24 行画一条细横线（模拟文字行）
            if (y % 24 == 0 && y > h / 16) { q[0] = 90; q[1] = 90; q[2] = 90; }
            // 左侧一块彩色方块
            if (x < w / 6 && y > h / 3 && y < h / 3 + h / 8) {
                q[0] = 60; q[1] = 140; q[2] = 220;
            }
        }
    }
}

struct Result {
    const char* name;
    double      msPerEncode;
    size_t      bytes;
    bool        ok;
};

template <typename F>
Result Bench(const char* name, int rounds, F&& fn) {
    Result r{name, 0, 0, false};
    std::string err;
    // 先跑一次预热（第一轮包含 dlopen、内存分配等一次性开销）
    std::string out = fn(&err);
    if (out.empty()) {
        printf("  %-22s ✗ %s\n", name, err.c_str());
        return r;
    }
    const int64_t t0 = NowMs();
    for (int i = 0; i < rounds; ++i) {
        out = fn(&err);
        if (out.empty()) { printf("  %-22s ✗ 第 %d 轮失败\n", name, i); return r; }
    }
    const int64_t dt = NowMs() - t0;
    r.msPerEncode = static_cast<double>(dt) / rounds;
    r.bytes = out.size();
    r.ok = true;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t w = argc > 1 ? static_cast<uint32_t>(atoi(argv[1])) : 320;
    uint32_t h = argc > 2 ? static_cast<uint32_t>(atoi(argv[2])) : 480;
    int rounds = argc > 3 ? atoi(argv[3]) : 30;

    printf("\n=== 编码器基准  %ux%u，%d 轮 ===\n", w, h, rounds);
    printf("  （原始 RGBA = %llu 字节）\n", static_cast<unsigned long long>(w) * h * 4);

    std::vector<uint8_t> rgba;
    const char* rawPath = argc > 4 ? argv[4] : nullptr;
    if (rawPath != nullptr) {
        FILE* f = fopen(rawPath, "rb");
        if (f == nullptr) {
            printf("  打不开 %s，退回合成图\n", rawPath);
            MakeScreenLike(&rgba, w, h);
        } else {
            rgba.resize(static_cast<size_t>(w) * h * 4);
            const size_t n = fread(rgba.data(), 1, rgba.size(), f);
            fclose(f);
            if (n != rgba.size()) {
                printf("  %s 只有 %zu 字节（需要 %zu），退回合成图\n",
                       rawPath, n, rgba.size());
                MakeScreenLike(&rgba, w, h);
            } else {
                printf("  图像来源: %s（真实抓帧）\n", rawPath);
            }
        }
    } else {
        printf("  ⚠️ 图像来源: 合成图 —— 结论会失真，真机请传真实抓帧\n");
        MakeScreenLike(&rgba, w, h);
    }

    std::string err;
    if (!ImageEncoder::Instance().Init(&err)) {
        printf("  ImageEncoder 初始化失败: %s\n", err.c_str());
        return 1;
    }
    printf("  ImageEncoder: nativeCodecs=%s\n\n",
           ImageEncoder::Instance().hasNativeCodecs() ? "是" : "否");
    if (!PngEncoder::Instance().Init(&err)) {
        printf("  PngEncoder 初始化失败: %s\n", err.c_str());
        return 1;
    }

    std::vector<Result> rs;

    // ── 我们自己的 zlib PNG（Android 8~10 会走这条）──
    // ⚠️ 名字必须是**生命周期足够长**的字符串。
    //
    //    这里踩了两次：先是 `char nm[48]` + snprintf（出了作用域就悬空），
    //    改成 vector<string> 之后仍然悬空 —— **vector 在 push_back 时
    //    会重分配**，之前元素的 c_str() 全部失效。
    //    reserve 到够用的容量才是真的稳。
    static std::vector<std::string> names;
    names.reserve(8);
    for (int lv : {1, 6}) {
        names.push_back("zlib PNG level=" + std::to_string(lv));
        rs.push_back(Bench(names.back().c_str(), rounds, [&](std::string* e) {
            return PngEncoder::Instance().EncodeRgba(rgba.data(), w, h, lv, e);
        }));
    }

    // ── Skia 的（Android 11+ 走这条）──
    if (ImageEncoder::Instance().hasNativeCodecs()) {
        names.push_back("Skia PNG (默认级)");
        rs.push_back(Bench(names.back().c_str(), rounds, [&](std::string* e) {
            return ImageEncoder::Instance().Encode(rgba.data(), w, h,
                                                   ImageFormat::kPng, 6, e);
        }));
        names.push_back("Skia JPEG q75");
        rs.push_back(Bench(names.back().c_str(), rounds, [&](std::string* e) {
            return ImageEncoder::Instance().Encode(rgba.data(), w, h,
                                                   ImageFormat::kJpeg, 75, e);
        }));
        names.push_back("Skia WebP q75");
        rs.push_back(Bench(names.back().c_str(), rounds, [&](std::string* e) {
            return ImageEncoder::Instance().Encode(rgba.data(), w, h,
                                                   ImageFormat::kWebp, 75, e);
        }));
    }

    printf("\n  %-22s %10s %10s %10s\n", "编码器", "ms/次", "字节", "相对");
    printf("  %s\n", std::string(56, '-').c_str());

    double base = 0;
    for (const auto& r : rs) if (r.ok) { base = r.msPerEncode; break; }

    for (const auto& r : rs) {
        if (!r.ok) continue;
        printf("  %-22s %10.1f %10zu %9.1fx\n", r.name, r.msPerEncode, r.bytes,
               base > 0 ? r.msPerEncode / base : 0.0);
    }

    printf("\n  参考：抓一帧（SurfaceFlinger 直连）约 23 ms\n");
    printf("  每帧总耗时 ≈ 23ms + 编码；30fps 的预算是 33.3ms\n\n");
    return 0;
}
