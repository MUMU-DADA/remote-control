// jpeg_dlopen_test.cpp — 验证"用 dlopen + vendor 头文件调 libjpeg"这条路可行
//
// 背景：早先的判断是"Android 8~10 用不了 AndroidBitmap_compress，
// 所以只能退回 PNG，失去 JPEG 能力"。这个判断**可能过头了** ——
// 设备上一直有 /system/lib64/libjpeg.so，而且是 VNDK 库
// （vndk: enabled 意味着跨版本的稳定 ABI）。缺的只是头文件，
// 而头文件是可以 vendor 进来的（AOSP 里有，jconfig.h 定死了 ABI 版本 62）。
//
// 这个程序就是为了把"可能"变成"确定"：
//   1. dlopen("libjpeg.so")
//   2. 用 vendor 的头文件按正常 libjpeg 流程编码一张图
//   3. 检查产物是不是合法 JPEG
//
// 如果跑通，说明 ABI 对得上 —— 那 Android 8~10 就不必失去 JPEG。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>

// vendor 进来的头文件（来自 AOSP external/libjpeg-turbo）
#include "jpeglib.h"

namespace {

// ── libjpeg 的函数指针 ──
//
// 全部从 dlopen 拿，不链接 libjpeg.so —— 这样在任何缺这个库的
// 设备上只是功能不可用，而不是启动就崩。
struct JpegApi {
    void*                    handle = nullptr;
    decltype(&jpeg_std_error)             std_error = nullptr;
    decltype(&jpeg_CreateCompress)        create_compress = nullptr;
    decltype(&jpeg_destroy_compress)      destroy_compress = nullptr;
    decltype(&jpeg_set_defaults)          set_defaults = nullptr;
    decltype(&jpeg_set_quality)           set_quality = nullptr;
    decltype(&jpeg_start_compress)        start_compress = nullptr;
    decltype(&jpeg_write_scanlines)       write_scanlines = nullptr;
    decltype(&jpeg_finish_compress)       finish_compress = nullptr;
    decltype(&jpeg_mem_dest)              mem_dest = nullptr;
    decltype(&jpeg_abort_compress)        abort_compress = nullptr;

    bool Load(const char** missing) {
        handle = dlopen("libjpeg.so", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) { *missing = "dlopen(libjpeg.so)"; return false; }

#define GET(field, sym)                                                    \
        do {                                                               \
            *(void**)(&field) = dlsym(handle, sym);                        \
            if (field == nullptr) { *missing = sym; return false; }        \
        } while (0)

        GET(std_error,        "jpeg_std_error");
        GET(create_compress,  "jpeg_CreateCompress");
        GET(destroy_compress, "jpeg_destroy_compress");
        GET(set_defaults,     "jpeg_set_defaults");
        GET(set_quality,      "jpeg_set_quality");
        GET(start_compress,   "jpeg_start_compress");
        GET(write_scanlines,  "jpeg_write_scanlines");
        GET(finish_compress,  "jpeg_finish_compress");
        GET(mem_dest,         "jpeg_mem_dest");
        GET(abort_compress,   "jpeg_abort_compress");
#undef GET
        return true;
    }
};

// libjpeg 的错误处理是 setjmp/longjmp。这里只记错误信息，
// 因为测试程序不关心恢复。
struct ErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf        jump;
    char           msg[JMSG_LENGTH_MAX];
};

void OnError(j_common_ptr cinfo) {
    auto* e = reinterpret_cast<ErrorMgr*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, e->msg);
    longjmp(e->jump, 1);
}

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

int main(int argc, char** argv) {
    printf("\n=== dlopen libjpeg 可行性验证 ===\n\n");

    const char* missing = "";
    JpegApi jp;
    if (!jp.Load(&missing)) {
        printf("  ✗ 加载失败: %s\n", missing);
        return 1;
    }
    printf("  ✓ dlopen(\"libjpeg.so\") 成功\n");
    printf("  ✓ 10 个必需符号全部找到\n");

    // 造一张 320x480 的测试图（含渐变与细线，别用纯色 —— 纯色测不出
    // 编码器是否真的在压缩）
    const uint32_t w = 320, h = 480;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = rgb.data() + (static_cast<size_t>(y) * w + x) * 3;
            uint8_t bg = static_cast<uint8_t>(238 - (y * 20 / h));
            p[0] = bg; p[1] = bg; p[2] = bg;
            if (y < h / 16) { p[0] = p[1] = p[2] = 40; }
            if (y % 24 == 0 && y > h / 16) { p[0] = p[1] = p[2] = 90; }
            if (x < w / 6 && y > h / 3 && y < h / 3 + h / 8) {
                p[0] = 60; p[1] = 140; p[2] = 220;
            }
        }
    }

    // ── 用 vendor 的头文件走标准 libjpeg 流程 ──
    //
    // 每一轮都要**重建 compress 对象**：libjpeg 的状态机不允许
    // 连续两次 start_compress（报 "Improper call to JPEG library
    // in state 101"）。第一版就是栽在这 —— 以为 start 可以重入。
    const int ROUNDS = 20;
    unsigned long outSize = 0;
    std::vector<unsigned char> outVec;
    int64_t dt = 0;

    for (int round = 0; round < ROUNDS; ++round) {
        jpeg_compress_struct cinfo;
        ErrorMgr jerr;
        memset(&cinfo, 0, sizeof(cinfo));
        memset(&jerr, 0, sizeof(jerr));

        cinfo.err = jp.std_error(&jerr.pub);
        jerr.pub.error_exit = OnError;

        unsigned char* outBuf = nullptr;
        unsigned long  outLen = 0;

        if (setjmp(jerr.jump)) {
            printf("  ✗ libjpeg 报错: %s\n", jerr.msg);
            if (outBuf != nullptr) free(outBuf);
            return 1;
        }

        jp.create_compress(&cinfo, JPEG_LIB_VERSION, sizeof(cinfo));
        jp.mem_dest(&cinfo, &outBuf, &outLen);
        cinfo.image_width      = w;
        cinfo.image_height     = h;
        cinfo.input_components = 3;
        cinfo.in_color_space   = JCS_RGB;
        jp.set_defaults(&cinfo);
        jp.set_quality(&cinfo, 75, TRUE);

        const int64_t t0 = NowMs();
        jp.start_compress(&cinfo, TRUE);
        while (cinfo.next_scanline < cinfo.image_height) {
            JSAMPROW row = &rgb[static_cast<size_t>(cinfo.next_scanline) * w * 3];
            jp.write_scanlines(&cinfo, &row, 1);
        }
        jp.finish_compress(&cinfo);
        dt += NowMs() - t0;

        jp.destroy_compress(&cinfo);

        outSize = outLen;
        outVec.assign(outBuf, outBuf + outLen);
        free(outBuf);
    }

    printf("  ✓ 编码完成\n\n");

    // ── 检查产物是不是合法 JPEG ──
    const unsigned char* outBuf = outVec.data();
    bool ok = true;
    if (outSize < 4) { printf("  ✗ 输出太小: %lu\n", outSize); ok = false; }
    else if (outBuf[0] != 0xFF || outBuf[1] != 0xD8) {
        printf("  ✗ 不是 JPEG（缺 SOI 标记）: %02X %02X\n", outBuf[0], outBuf[1]);
        ok = false;
    } else if (outBuf[outSize - 2] != 0xFF || outBuf[outSize - 1] != 0xD9) {
        printf("  ✗ 不是完整 JPEG（缺 EOI 标记）: %02X %02X\n",
               outBuf[outSize - 2], outBuf[outSize - 1]);
        ok = false;
    }

    if (ok) {
        printf("  ✓ 产物是合法 JPEG：%ux%u，%lu 字节，SOI/EOI 标记齐全\n",
               w, h, outSize);
        printf("  ✓ 编码耗时 %.1f ms/次（%d 轮）\n",
               static_cast<double>(dt) / ROUNDS, ROUNDS);
        printf("\n  ────────────────────────────────────────\n");
        printf("  结论：dlopen + vendor 头文件这条路**可行**。\n");
        printf("        Android 8~10 不必失去 JPEG。\n");
        printf("  ────────────────────────────────────────\n");
    }

    // 落一份出来，方便用真实解码器验证
    const char* dumpPath = argc > 1 ? argv[1] : nullptr;
    if (dumpPath != nullptr && ok) {
        FILE* f = fopen(dumpPath, "wb");
        if (f != nullptr) {
            fwrite(outBuf, 1, outSize, f);
            fclose(f);
            printf("\n  已写出 %s，可用图片查看器打开验证\n", dumpPath);
        }
    }

    return ok ? 0 : 1;
}
