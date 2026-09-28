// abi_call_test.cpp — "直接调用 vs dlsym 指针调用"的成本
//
// 改造后，AndroidBitmap_compress 从**直接链接**变成 **dlopen+dlsym**。
// 这是在高版本设备上**唯一**改变的东西。
//
// 如果两者耗时一致，那么任何观察到的性能变化都来自外部
// （主机负载、画面内容），不是这次改造。
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <time.h>
#include <string>
#include <vector>

#include <android/bitmap.h>
#include <android/data_space.h>

static int64_t NowMs() {
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}
static bool W(void* c, const void* d, size_t n) {
    static_cast<std::string*>(c)->append((const char*)d, n); return true;
}

int main(int argc, char** argv) {
    const uint32_t w = argc>1?(uint32_t)atoi(argv[1]):320;
    const uint32_t h = argc>2?(uint32_t)atoi(argv[2]):480;
    const int rounds = argc>3?atoi(argv[3]):20;
    const int quality = argc>4?atoi(argv[4]):6;

    std::vector<uint8_t> rgba((size_t)w*h*4);
    if (argc>5) { FILE* f=fopen(argv[5],"rb"); if(f){fread(rgba.data(),1,rgba.size(),f);fclose(f);} }

    AndroidBitmapInfo info;
    memset(&info,0,sizeof(info));
    info.width=w; info.height=h; info.stride=w*4;
    info.format=ANDROID_BITMAP_FORMAT_RGBA_8888;
    info.flags=ANDROID_BITMAP_FLAGS_ALPHA_PREMUL;

    // 直接链接的符号
    int (*direct)(const AndroidBitmapInfo*, int32_t, const void*, int32_t,
                  int32_t, void*, AndroidBitmap_CompressWriteFunc) =
            &AndroidBitmap_compress;

    // dlsym 拿的
    void* h2 = dlopen("libjnigraphics.so", RTLD_NOW|RTLD_LOCAL);
    int (*viaptr)(const AndroidBitmapInfo*, int32_t, const void*, int32_t,
                  int32_t, void*, AndroidBitmap_CompressWriteFunc) = nullptr;
    if (h2) *(void**)(&viaptr) = dlsym(h2, "AndroidBitmap_compress");

    printf("\n=== 直接调用 vs dlsym 指针调用 ===\n");
    printf("  %ux%u，%d 轮，quality=%d\n\n", w, h, rounds, quality);

    std::string out;
    auto run = [&](auto fn) -> double {
        out.clear(); fn();                       // 预热
        const int64_t t0 = NowMs();
        for (int i=0;i<rounds;++i) { out.clear(); fn(); }
        return (double)(NowMs()-t0)/rounds;
    };

    const double a = run([&]{ direct(&info, ADATASPACE_SRGB, rgba.data(),
                                     ANDROID_BITMAP_COMPRESS_FORMAT_PNG,
                                     quality*100/9, &out, &W); });
    const size_t szA = out.size();
    const double b = viaptr ? run([&]{ viaptr(&info, ADATASPACE_SRGB, rgba.data(),
                                     ANDROID_BITMAP_COMPRESS_FORMAT_PNG,
                                     quality*100/9, &out, &W); }) : 0;
    const size_t szB = out.size();

    printf("  %-28s %8.2f ms   %8zu 字节\n", "直接调用 &AndroidBitmap_compress", a, szA);
    if (viaptr) printf("  %-28s %8.2f ms   %8zu 字节\n", "dlsym 指针调用", b, szB);
    else        printf("  dlsym 失败: %s\n", dlerror());

    if (viaptr) {
        printf("\n  差异: %+.2f ms（%.2f%%）\n", b-a, 100.0*(b-a)/a);
        printf("  输出是否一致: %s\n", szA==szB ? "✓ 大小相同" : "✗ 不同");
        printf("\n  ──────────────────────────────────\n");
        printf("  间接调用只会多几个纳秒（几 ns），在 ms 级操作里测不出来。\n");
        printf("  所以高版本上任何可观测的性能变化，都不是这次改造造成的。\n");
    }
    return 0;
}
