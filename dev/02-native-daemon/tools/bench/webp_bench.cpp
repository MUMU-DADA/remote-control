// webp_bench.cpp — 决定内置 libwebp 后该怎么配参数
//
// 背景：Android 8~10 上没有 WebP 编码能力，要内置 libwebp 才补得上。
//       但 WebP 编码比 JPEG 慢约 10 倍（Skia method=3，单线程）。
//
// 这个基准回答：**换成内置的 libwebp 之后，能不能通过参数把它调快？**
//
// 两个可调的：
//   method       0(最快) - 6(最慢最好)。Skia 用的是 3（跟 Chrome 对齐）
//   thread_level 0(单线程) - 1(多线程)。Skia 没设，默认 0
//
// 还要看文件大小 —— 调快通常会让文件变大，优势就被吃掉了。
//
// 用法：
//   webp_bench <raw-RGBA 文件> [宽] [高] [质量] [轮数]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <time.h>
#include <unistd.h>

// 路径跟着 vendor 布局走：源码统一用 "src/webp/..." 前缀，
// 所以 -I 指到 daemon/vendor/webp/ 即可（和 AOSP 的
// local_include_dirs: ["."] 一致）。
#include "src/webp/encode.h"

namespace {

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

struct Row {
    int    method;
    int    threads;
    double ms;
    size_t bytes;
    bool   ok;
};

// 编一张 RGBA 图，返回结果。
//
// 用完整的 WebPConfig/WebPPicture 流程而不是 WebPEncodeRGBA ——
// 后者不给机会设 method 和 thread_level，而这两个正是要测的。
Row Encode(const uint8_t* rgba, uint32_t w, uint32_t h, int quality,
           int method, int threads, int rounds) {
    Row r{method, threads, 0, 0, false};
    std::vector<uint8_t> out;

    for (int i = 0; i < rounds + 1; ++i) {   // 第一轮预热
        WebPConfig cfg;
        if (!WebPConfigInit(&cfg)) return r;
        if (!WebPConfigPreset(&cfg, WEBP_PRESET_DEFAULT, quality)) return r;
        cfg.lossless     = 0;                // 有损
        cfg.method       = method;
        cfg.thread_level = threads;
        cfg.quality      = quality;
        // 画面流不需要 alpha 通道的精细处理 —— 截图 alpha 恒为 255
        if (!WebPValidateConfig(&cfg)) return r;

        WebPPicture pic;
        if (!WebPPictureInit(&pic)) return r;
        pic.use_argb = 1;
        pic.width  = w;
        pic.height = h;
        if (!WebPPictureImportRGBA(&pic, rgba, w * 4)) {
            WebPPictureFree(&pic);
            return r;
        }

        WebPMemoryWriter writer;
        WebPMemoryWriterInit(&writer);
        pic.writer = WebPMemoryWrite;
        pic.custom_ptr = &writer;

        const int64_t t0 = NowMs();
        const int ok = WebPEncode(&cfg, &pic);
        const int64_t dt = NowMs() - t0;

        if (i > 0) {                          // 预热那轮不计
            r.ms += static_cast<double>(dt);
            r.bytes = writer.size;
        }
        out.assign(writer.mem, writer.mem + writer.size);
        WebPMemoryWriterClear(&writer);
        WebPPictureFree(&pic);
        if (!ok) return r;
    }

    r.ms /= rounds;
    r.ok = true;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("用法: %s <raw-RGBA> [宽] [高] [质量] [轮数]\n", argv[0]);
        return 1;
    }
    const uint32_t w = argc > 2 ? static_cast<uint32_t>(atoi(argv[2])) : 320;
    const uint32_t h = argc > 3 ? static_cast<uint32_t>(atoi(argv[3])) : 480;
    const int quality = argc > 4 ? atoi(argv[4]) : 75;
    const int rounds  = argc > 5 ? atoi(argv[5]) : 12;

    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    // 文件可选：尺寸对不上就用合成图。**但合成图的压缩特性失真** ——
    // 尤其 WebP 的预测器对规整图案吃得很爽，出来的体积会偏小。
    // 结论只信真实抓帧那些行。
    bool realFrame = false;
    FILE* f = fopen(argv[1], "rb");
    if (f != nullptr && fread(rgba.data(), 1, rgba.size(), f) == rgba.size()) {
        realFrame = true;
    } else {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                uint8_t b = static_cast<uint8_t>(238 - (y * 20 / h));
                p[0] = b; p[1] = b; p[2] = b; p[3] = 255;
                if (y < h / 16) { p[0] = p[1] = p[2] = 40; }
                if (y % 24 == 0 && y > h / 16) { p[0] = p[1] = p[2] = 90; }
                if (x < w / 6 && y > h / 3 && y < h / 3 + h / 8) {
                    p[0] = 60; p[1] = 140; p[2] = 220;
                }
            }
    }
    if (f != nullptr) fclose(f);

    printf("\n=== 内置 libwebp 参数基准 ===\n");
    printf("  %ux%u，质量 %d，%d 轮，%s\n", w, h, quality, rounds,
           realFrame ? "真实抓帧" : "合成图（体积数字会失真）");
    printf("  CPU 核数: %ld\n\n", sysconf(_SC_NPROCESSORS_ONLN));

    printf("  %-8s %-10s %10s %10s\n", "method", "thread", "ms/次", "字节");
    printf("  %s\n", std::string(42, '-').c_str());

    double baseMs = 0;
    size_t baseBytes = 0;
    for (int m : {0, 1, 2, 3, 4, 6}) {
        for (int t : {0, 1}) {
            // thread_level=1 只在多核上有意义，核数=1 时不重复测
            if (t == 1 && sysconf(_SC_NPROCESSORS_ONLN) < 2) continue;
            Row r = Encode(rgba.data(), w, h, quality, m, t, rounds);
            if (!r.ok) { printf("  %-8d %-10d ✗ 失败\n", m, t); continue; }
            if (m == 3 && t == 0) { baseMs = r.ms; baseBytes = r.bytes; }
            printf("  %-8d %-10d %10.1f %10zu", m, t, r.ms, r.bytes);
            if (baseMs > 0 && !(m == 3 && t == 0)) {
                printf("   %5.2fx 时间  %+5.1f%% 大小",
                       r.ms / baseMs, 100.0 * (static_cast<double>(r.bytes) -
                                                static_cast<double>(baseBytes)) /
                                          static_cast<double>(baseBytes));
            } else if (m == 3 && t == 0) {
                printf("   ← Skia 的配置（基准）");
            }
            printf("\n");
        }
    }

    printf("\n  参考：JPEG q75 编码约 1.1 ms，体积约 11 KB（同分辨率）\n\n");
    return 0;
}
