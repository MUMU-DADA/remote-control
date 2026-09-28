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

---

# jpeg_paths_compare —— 改造会不会把高版本改坏

问题：为了让 Android 8~10 用上 JPEG 要加 dlopen 这条路，
但高版本现在走 AndroidBitmap（Skia）。**换过去会不会降级？**

答案：**不会。两条路是同一个编码器。**

## 证据链

**1. 同一个库**
`external/skia/Android.bp` 里 `"libjpeg"` 是依赖 —— Skia 的 JPEG
编码器底层就是 libjpeg-turbo。设备上那个 `libjpeg.so` 就是它。

**2. 第一版差 5%，原因是参数没对齐**

```
AndroidBitmap (Skia)    1.3 ms    24,341 B
dlopen libjpeg          0.5 ms    25,583 B   ← 又快又大
```

快而大 = 没开 `optimize_coding`。查 Skia 源码：

```c
// SkJpegEncoder.cpp:160-163
// Tells libjpeg-turbo to compute optimal Huffman coding tables
// for the image. This improves compression at the cost of slower encode.
fCInfo.optimize_coding = TRUE;      ← Skia 开了
// 而 libjpeg 默认是 FALSE（jcparam.c:229）
```

**3. 补上之后，时间和大小都对上了**

```
AndroidBitmap (Skia)    1.1 ms    24,341 B
dlopen libjpeg          1.1 ms    23,787 B
```

**4. 剩下 554 字节的差 = Skia 嵌的 ICC 色彩配置文件**

```
Skia 段序列:  APP0  APP2(ICC_PROFILE, 550 字节)  DQT DQT SOF0 DHT×4 SOS
我们的:       无 ICC 段
差 554 = ICC 载荷 550 + 段头 4        ✓
```

截图是 sRGB 的，这个 ICC 是冗余的。**我们的输出反而小 550 字节。**

## 结论

| | 高版本用 AndroidBitmap | 换成 dlopen libjpeg |
|---|---|---|
| 编码器 | libjpeg-turbo | **同一个** |
| 耗时（同参数） | 1.1 ms | 1.1 ms |
| 体积 | 24,341 B | 23,787 B（少一个冗余 ICC） |
| 画面 | 一致 | 一致 |

**换过去不是降级。** 但也没有必要强行换 —— 见下面"零风险设计"。

## 零风险设计

改造应该做成**运行时探测 + 保持现状优先**：

```
API ≥ 30  →  AndroidBitmap（JPEG / WebP / PNG）  ← 一行代码都不变
API < 30  →  dlopen libjpeg（JPEG） / zlib（PNG）
```

高版本走的还是原来那条路，**行为完全不变**；探测只在启动时做一次。

代价：多一条代码路径要维护。收益：老设备也有 JPEG。

## 附带确认

`dlopen("libwebp.so")` 失败 —— 设备上没有这个库。
**WebP 只有 AndroidBitmap 能出**，所以：
- 高版本：WebP 保留 ✅
- 老版本：没有 WebP（可选格式，不是默认）

---

# h264_feasibility —— H.264 这条路走不走得通

写任何 H.264 代码之前先跑这个。它回答三个前置问题：
原生守护进程能不能创建 AVC 编码器、configure/start 过不过、能不能
真的编出一帧。

```bash
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android21-clang \
    -o mc h264_feasibility.c -lmediandk
adb push mc /data/local/tmp/ && adb shell /data/local/tmp/mc
```

## 实测（Android 12 / x86_64 模拟器）

```
+ 编码器已创建
configure: 0 +
start: 0 +
dequeueInputBuffer: 0 +
输入缓冲: 230400 字节（正好是 320x480 YUV420 的大小）
queueInputBuffer: 0 +
dequeueOutputBuffer: -2          ← INFO_OUTPUT_FORMAT_CHANGED，正常
第二次 dequeueOutputBuffer: 0
+ 编出了一帧: 31 字节, flags=0x2 (CODEC_CONFIG)
  前 8 字节: 00 00 00 01 67 42 C0 29
  起始码: Annex-B
  首个 NAL 类型: 7（SPS）
```

**结论：可行。** `AMediaCodec` 是 `__INTRODUCED_IN(21)`，
设备上有 `c2.android.avc.encoder`（模拟器是软编，真机通常是硬编）。

几个容易踩的点：
- 第一次 `dequeueOutputBuffer` 返回 **-2（格式变化）** 是正常的，
  不是错误 —— 要再取一次
- 输出是 **Annex-B**（`00 00 00 01` 起始码），不是 AVCC 长度前缀
- 第一帧的 flags 是 `CODEC_CONFIG`（SPS/PPS），不是图像数据
- `COLOR_FormatYUV420Flexible`(21) 下输入缓冲大小由编码器决定，
  不能假设是 `w*h*3/2`（这次正好相等，但不能依赖）

## ⚠️ 消费端的限制

设备上的 WebView 是 **91.0.4472.114**，而 **WebCodecs
（`VideoDecoder`）需要 Chrome/WebView 94+**。

所以：
- **桌面浏览器**（正常用法：`http://<本机IP>:8088/`）→ 能解 H.264 ✓
- **设备内 WebView**（我现在用来截图验证的那个）→ 不能 ✗

这意味着 H.264 必须**带自动回退**：探测不到 WebCodecs 就退回
JPEG/MJPEG。不能假设客户端都能解。


---

# h264_bench —— H264Encoder 的实测

```bash
$CXX -std=c++20 -O2 -fno-exceptions -I daemon -o h264b h264_bench.cpp \
     daemon/h264_encoder.cpp daemon/log_buffer.cpp -lmediandk -llog -static-libstdc++
curl -o real.raw 'http://<设备>:8088/api/v1/capture?format=raw'
adb push h264b real.raw /data/local/tmp/
adb shell '/data/local/tmp/h264b /data/local/tmp/real.raw 320 480 30 /data/local/tmp/out.h264'
```

## 实测（Android 12 / x86_64 模拟器，320x480 真实抓帧）

```
+ 编码器已启动
第 0 帧:   31 字节   8 ms  NAL: SPS PPS     ← codec config
第 2 帧: 7200 字节   7 ms  NAL: IDR         ← 关键帧
第 3 帧:  109 字节   7 ms  NAL: 非IDR       ← P 帧
第 4 帧:   70 字节   1 ms  NAL: 非IDR
第 5 帧:   90 字节   1 ms  NAL: 非IDR

送帧 30，出帧 27，有数据的轮次 27/30
平均 4292 字节/轮，平均 2.43 ms/轮
codec 串: avc1.42C029      ← 从 SPS 正确解析
```

产物用独立脚本解析 NAL 序列确认：

```
共 28 个 NAL:  类型 1（非IDR）×25  类型 5（IDR）×1
               类型 7（SPS）×1     类型 8（PPS）×1
✓ SPS / PPS / IDR / 非IDR 齐全 —— 是完整的可解码流
```

**P 帧 70~110 字节 vs JPEG 的 11,000 字节 —— 小两个数量级。**
（平均 4292 被 IDR 拉高：这个测试每帧改一个字节，对静止画面
会触发较多帧内刷新。真实画面流里 I 帧间隔 2 秒，平均会低得多。）

## 实现里几个不显然的点

**`COLOR_Format*` 常量 NDK 头里没有** —— 它们是 Java 侧
`MediaCodecInfo.CodecCapabilities` 的字段。值本身平台稳定，
在 h264_encoder.cpp 里自己定义（19=I420，21=NV12）。

**`queueInputBuffer` 没有关键帧标志** —— `AMEDIACODEC_BUFFER_FLAG_*`
只有 CODEC_CONFIG / EOS / PARTIAL。原生侧的正确做法是
`AMediaCodec_setParameters("request-sync", 1)`，而那是 **API 26+**
（正好是我们的下限）。

**codec 串不能写死** —— WebCodecs 的 `isConfigSupported` 会按
profile/level 校验。必须从 SPS 的第 1~3 字节解析（这次拿到
`avc1.42C029`）。
