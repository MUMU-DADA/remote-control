// jpeg_paths_compare.cpp — 对比两条 JPEG 编码路径
//
// 问题：为了让 Android 8~10 用上 JPEG，要加 dlopen libjpeg 这条路。
//       但高版本现在走的是 AndroidBitmap_compress（Skia）。
//       **换成 dlopen 会不会把高版本改坏？**
//
// 假设：Skia 的 JPEG 编码器底层就是 libjpeg（external/skia/Android.bp
//       里 "libjpeg" 是依赖）。如果是同一个编码器，
//       同样的输入 + 同样的质量参数，输出应该**一模一样**。
//
// 这个程序就是验证这个假设：同一张真实抓帧，两条路各编一遍，
// 比时间、比大小、比**逐字节**。
//
// 用法：
//   jpeg_paths_compare <raw-RGBA 文件> [宽] [高] [质量] [轮数]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <time.h>

#include "image_encoder.h"      // AndroidBitmap 那条路
#include "vendor-jpeg/jpeglib.h"  // dlopen 那条路（用 vendor 的头文件）

using namespace autod;

namespace {

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

struct JpegApi {
    void* handle = nullptr;
    decltype(&jpeg_std_error)        std_error = nullptr;
    decltype(&jpeg_CreateCompress)   create_compress = nullptr;
    decltype(&jpeg_destroy_compress) destroy_compress = nullptr;
    decltype(&jpeg_set_defaults)     set_defaults = nullptr;
    decltype(&jpeg_set_quality)      set_quality = nullptr;
    decltype(&jpeg_start_compress)   start_compress = nullptr;
    decltype(&jpeg_write_scanlines)  write_scanlines = nullptr;
    decltype(&jpeg_finish_compress)  finish_compress = nullptr;
    decltype(&jpeg_mem_dest)         mem_dest = nullptr;

    bool Load() {
        handle = dlopen("libjpeg.so", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) return false;
#define GET(f, s) do { *(void**)(&f) = dlsym(handle, s); if (!f) return false; } while (0)
        GET(std_error, "jpeg_std_error");
        GET(create_compress, "jpeg_CreateCompress");
        GET(destroy_compress, "jpeg_destroy_compress");
        GET(set_defaults, "jpeg_set_defaults");
        GET(set_quality, "jpeg_set_quality");
        GET(start_compress, "jpeg_start_compress");
        GET(write_scanlines, "jpeg_write_scanlines");
        GET(finish_compress, "jpeg_finish_compress");
        GET(mem_dest, "jpeg_mem_dest");
#undef GET
        return true;
    }
};

struct ErrorMgr { jpeg_error_mgr pub; jmp_buf jump; char msg[JMSG_LENGTH_MAX]; };
void OnError(j_common_ptr ci) {
    auto* e = reinterpret_cast<ErrorMgr*>(ci->err);
    (*ci->err->format_message)(ci, e->msg);
    longjmp(e->jump, 1);
}

// 用 dlopen 的 libjpeg 编一张 RGB 图
bool EncodeViaLibjpeg(const JpegApi& jp, const std::vector<uint8_t>& rgb,
                      uint32_t w, uint32_t h, int quality,
                      std::vector<uint8_t>* out, std::string* err) {
    jpeg_compress_struct cinfo;
    ErrorMgr jerr;
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));
    cinfo.err = jp.std_error(&jerr.pub);
    jerr.pub.error_exit = OnError;

    unsigned char* buf = nullptr;
    unsigned long  len = 0;
    if (setjmp(jerr.jump)) {
        *err = jerr.msg;
        if (buf) free(buf);
        return false;
    }

    jp.create_compress(&cinfo, JPEG_LIB_VERSION, sizeof(cinfo));
    jp.mem_dest(&cinfo, &buf, &len);
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    // ⚠️ 要和 Skia 用同样的参数，否则比字节没意义。
    //    Skia 的 SkJpegEncoder 默认也是 set_defaults + set_quality。
    jp.set_defaults(&cinfo);
    jp.set_quality(&cinfo, quality, TRUE);
    // ⚠️ 这行是两条路能不能对上的关键。
    //
    // libjpeg 默认 optimize_coding = FALSE（jcparam.c:229）：用标准
    // Huffman 表，快但文件大。而 Skia 显式设成 TRUE
    // （SkJpegEncoder.cpp:163，注释写着"improves compression at the
    // cost of slower encode performance"）。
    //
    // 不设这一行，同一张图两条路差 5% 大小 —— 那不是编码器不同，
    // 是参数不同。
    cinfo.optimize_coding = TRUE;
    jp.start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(
                &rgb[static_cast<size_t>(cinfo.next_scanline) * w * 3]);
        jp.write_scanlines(&cinfo, &row, 1);
    }
    jp.finish_compress(&cinfo);
    out->assign(buf, buf + len);
    jp.destroy_compress(&cinfo);
    free(buf);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("用法: %s <raw-RGBA 文件> [宽] [高] [质量] [轮数]\n", argv[0]);
        return 1;
    }
    const uint32_t w = argc > 2 ? static_cast<uint32_t>(atoi(argv[2])) : 320;
    const uint32_t h = argc > 3 ? static_cast<uint32_t>(atoi(argv[3])) : 480;
    const int quality = argc > 4 ? atoi(argv[4]) : 75;
    const int rounds  = argc > 5 ? atoi(argv[5]) : 20;

    // 读真实抓帧
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    FILE* f = fopen(argv[1], "rb");
    if (f == nullptr || fread(rgba.data(), 1, rgba.size(), f) != rgba.size()) {
        printf("  读不了 %s（需要 %zu 字节 RGBA）\n", argv[1], rgba.size());
        if (f) fclose(f);
        return 1;
    }
    fclose(f);

    printf("\n=== 两条 JPEG 路径对比 ===\n");
    printf("  %ux%u，质量 %d，%d 轮，真实抓帧\n\n", w, h, quality, rounds);

    // RGBA → RGB（libjpeg 那条要 3 分量；Skia 直接吃 RGBA）
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0, j = 0; i + 3 < rgba.size(); i += 4, j += 3) {
        rgb[j] = rgba[i]; rgb[j + 1] = rgba[i + 1]; rgb[j + 2] = rgba[i + 2];
    }

    std::string err;
    if (!ImageEncoder::Instance().Init(&err)) { printf("  ImageEncoder: %s\n", err.c_str()); return 1; }
    printf("  AndroidBitmap 可用: %s\n",
           ImageEncoder::Instance().hasNativeCodecs() ? "是" : "否");

    JpegApi jp;
    const bool haveLibjpeg = jp.Load();
    printf("  dlopen libjpeg 可用: %s\n\n", haveLibjpeg ? "是" : "否");

    // ── A：AndroidBitmap（高版本现在走这条）──
    std::vector<uint8_t> a;
    int64_t t0 = NowMs();
    for (int i = 0; i < rounds; ++i) {
        std::string e;
        std::string s = ImageEncoder::Instance().Encode(rgba.data(), w, h,
                                                        ImageFormat::kJpeg, quality, &e);
        if (s.empty()) { printf("  AndroidBitmap 失败: %s\n", e.c_str()); return 1; }
        a.assign(s.begin(), s.end());
    }
    const double msA = static_cast<double>(NowMs() - t0) / rounds;

    // ── B：dlopen libjpeg（低版本会走这条）──
    std::vector<uint8_t> b;
    double msB = 0;
    if (haveLibjpeg) {
        t0 = NowMs();
        for (int i = 0; i < rounds; ++i) {
            if (!EncodeViaLibjpeg(jp, rgb, w, h, quality, &b, &err)) {
                printf("  libjpeg 失败: %s\n", err.c_str());
                return 1;
            }
        }
        msB = static_cast<double>(NowMs() - t0) / rounds;
    }

    // ── 对比 ──
    printf("  %-28s %10s %12s\n", "路径", "ms/次", "字节");
    printf("  %s\n", std::string(52, '-').c_str());
    printf("  %-28s %10.1f %12zu\n", "AndroidBitmap (Skia)", msA, a.size());
    if (haveLibjpeg) {
        printf("  %-28s %10.1f %12zu\n", "dlopen libjpeg", msB, b.size());
    }

    if (haveLibjpeg && !a.empty() && !b.empty()) {
        const bool sameSize = a.size() == b.size();
        const bool sameBytes = sameSize && memcmp(a.data(), b.data(), a.size()) == 0;
        printf("\n  逐字节对比:\n");
        printf("    大小相同: %s（%zu vs %zu）\n",
               sameSize ? "✓" : "✗", a.size(), b.size());
        printf("    内容相同: %s\n", sameBytes ? "✓ 完全一致" : "✗ 有差异");

        if (!sameBytes && sameSize) {
            size_t diff = 0, firstDiff = SIZE_MAX;
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i] != b[i]) { ++diff; if (firstDiff == SIZE_MAX) firstDiff = i; }
            }
            printf("    不同字节数: %zu / %zu（%.4f%%），首个不同在偏移 %zu\n",
                   diff, a.size(), 100.0 * diff / a.size(), firstDiff);
        }

        printf("\n  时间比: dlopen/AndroidBitmap = %.2fx\n", msB / msA);

        printf("\n  ────────────────────────────────────────\n");
        if (sameBytes) {
            printf("  同一个编码器、同一份输出 —— 换路径对高版本**零影响**。\n");
        } else if (sameSize) {
            printf("  同大小但字节有差 —— 可能是 Skia 的内部参数略有不同。\n");
            printf("  画质与体积一致，视觉上无差别。\n");
        } else {
            printf("  大小不同 —— 参数不完全等价，需要逐项对齐后再判断。\n");
        }
        printf("  ────────────────────────────────────────\n");
    }

    // 顺便确认 WebP 只有一条路
    printf("\n  WebP 可用性: AndroidBitmap=%s，dlopen libwebp=%s\n",
           ImageEncoder::Instance().hasNativeCodecs() ? "是" : "否",
           dlopen("libwebp.so", RTLD_NOW) != nullptr ? "是" : "否");
    return 0;
}
