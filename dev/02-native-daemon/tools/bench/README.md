# 编码器基准

回答"换编码器会损失多少性能"用。

```bash
# 编译（NDK，x86_64 模拟器）
CXX=/opt/android/android-ndk-r26d/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android31-clang++
$CXX -std=c++20 -O2 -fno-exceptions -I ../../daemon -o bench_encoder \
     bench_encoder.cpp ../../daemon/image_encoder.cpp \
     ../../daemon/png_encoder.cpp ../../daemon/log_buffer.cpp \
     -ljnigraphics -llog -static-libstdc++

# 抓一张真实帧（raw RGBA），推到设备
curl -o real.raw 'http://<设备>:8088/api/v1/capture?format=raw'
adb push real.raw /data/local/tmp/

# 跑
adb shell '/data/local/tmp/bench_encoder 320 480 40 /data/local/tmp/real.raw'
```

## ⚠️ 一定要传真实抓帧

第一版用合成的"像屏幕"图（渐变 + 细线 + 色块），测出来
**PNG 比 JPEG 还小** —— 因为那张图太规整，PNG 的预测器吃得很爽。
而真机上 PNG 67KB / JPEG 24KB，正好相反。

**图像内容的压缩特性差异极大，合成图给的结论是失真的。**

## ⚠️ 分辨率要按真机来

本项目实测环境是 320×480（模拟器）。真机 1080×1920 是 **13 倍**的像素，
编码耗时同比例增长 —— 拿 320×480 的数字去推真机性能会严重低估。

## 实测结果（Android 12 / x86_64 模拟器，320×480 真实抓帧）

| 编码器 | ms/次 | 字节 | 相对 JPEG |
|---|---|---|---|
| zlib PNG level=1 | 5.3 | 67,122 | 4.4x 慢 / 2.8x 大 |
| zlib PNG level=6 | 13.3 | 59,211 | 11x 慢 |
| Skia PNG | 13.7 | 76,466 | 11x 慢 |
| **Skia JPEG q75** | **1.2** | **24,341** | — |
| Skia WebP q75 | 10.6 | 14,598 | 8.8x 慢 / 更小 |

两个值得注意的：

- **我们自己的 zlib PNG 比 Skia 的 PNG 又快又小**（5.3ms/67KB vs 13.7ms/76KB）。
  所以"降到 Android 8 只能用 zlib PNG"这件事，在 PNG 这个格式上
  反而是升级 —— 真正的损失是**失去 JPEG/WebP**。
- **WebP 编码比 JPEG 慢 8.8 倍**，但小 40%。它适合带宽紧张、
  CPU 富余的场景；默认不用它。

## 同目录还有

`bench_memfd.cpp` —— 测 `memfd_create` 的 bionic 包装 vs 直接 `syscall()`
的成本差（结论：29 ns/次，可忽略）。

---

# jpeg_dlopen_test —— 修正一个我下得太快的结论

早先的判断是："Android 8~10 用不了 `AndroidBitmap_compress`（API 30），
所以只能退回 PNG，失去 JPEG/WebP。"

**这个判断对 JPEG 是错的。** 当时的理由是：

> 调 libjpeg 要 `jpeglib.h` 里的 `jpeg_compress_struct` 布局 ——
> 那个结构体很大且随版本变，NDK 里没有头文件，自己声明等于赌 ABI。

太悲观了。漏掉的三件事：

1. **libjpeg 是 VNDK 库**（AOSP `Android.bp`: `vendor_available` +
   `product_available` + `vndk: { enabled: true }`）。
   **VNDK 存在的意义就是跨 Android 版本提供稳定 ABI。**
2. **头文件可以 vendor** —— AOSP 的 `jconfig.h` 把 `JPEG_LIB_VERSION`
   钉死在 `62`，所以结构体布局是确定的，不是"随版本变"。
3. **它只改名内部符号**（`jpeglibmangler.h` 把 `jpeg_make_c_derived_tbl`
   之类改成 `chromium_*`），公开 API 一个没动。

## 实测

```bash
# vendor 头文件（7 个，来自 aosp/external/libjpeg-turbo）
cp .../{jconfig,jmorecfg,jpeglib,jerror,jpegint,jpeglibmangler,jversion}.h vendor-jpeg/

$CXX -std=c++20 -O2 -I vendor-jpeg -o jpeg_test jpeg_dlopen_test.cpp -ldl
adb push jpeg_test /data/local/tmp/ && adb shell /data/local/tmp/jpeg_test /data/local/tmp/out.jpg
```

```
✓ dlopen("libjpeg.so") 成功
✓ 10 个必需符号全部找到
✓ 编码完成 → 合法 JPEG：320x480，13756 字节，SOI/EOI 齐全
✓ 编码耗时 0.5 ms/次
```

独立验证（`file`）：

```
JPEG image data, JFIF standard 1.01, baseline, precision 8, 320x480, components 3
```

段序列 APP0 DQT DQT SOF0 DHT DHT DHT DHT SOS —— 结构完整。

## 所以结论要改成

| 能力 | Android 11+ | Android 8~10（改用 dlopen 后） |
|---|---|---|
| **JPEG** | AndroidBitmap（Skia） | **dlopen libjpeg** ✅ 保留 |
| WebP | AndroidBitmap（Skia） | ⚠️ 要 vendor libwebp 源码（5.9 MB），否则失去 |
| PNG | AndroidBitmap / zlib | zlib ✅ |

**只有 WebP 有风险，而它不是默认格式。**

⚠️ 一个诚实的保留：0.5 ms 是在合成图上测的，而 Skia 的 1.2 ms 是在
真实抓帧上测的 —— **两者不可直接比较**。要比较得用同一张图。
