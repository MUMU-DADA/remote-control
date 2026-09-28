# vendor/webp —— 内置的 libwebp（编码器部分）

来源：`aosp/external/webp`（Android 12 的 `android-12.0.0_r34`）

## 为什么内置

**设备上没有 `libwebp.so`。** 实测 Android 12 上找不到，Skia 里也
没有 —— 而 `AndroidBitmap_compress` 是 `__INTRODUCED_IN(30)`。

所以不内置的话，**Android 8~10 完全没有 WebP 编码能力**。

这和 JPEG 的处理方式不同：

| | JPEG | WebP |
|---|---|---|
| 系统库 | ✅ 一直有 `/system/lib64/libjpeg.so`（VNDK 稳定 ABI） | ❌ 没有 |
| 做法 | dlopen 系统库 + vendor 头文件 | **源码整个编进来** |
| 体积代价 | ~0（7 个头文件） | **+578 KB 二进制** |

## 内容

```
src/enc/    23 个 .c   编码主流程
src/dsp/    63 个 .c   SIMD 加速
src/utils/  12 个 .c   公共工具
src/dec/     0 个 .c   只要头文件 ⚠️
src/webp/    0 个 .c   公开头（encode.h / types.h / …）
```

共 **98 个 .c**、42 个 .h，约 2.5 MB 源码。

### ⚠️ `src/dec/` 的 `.c` 不要，但头文件必须留

编码器的头文件里 `#include "src/dec/common_dec.h"`（`vp8i_enc.h` 第 18 行）。
删掉整个 `src/dec/` 会有 34 个文件编不过 —— 报的是
`'src/dec/common_dec.h' file not found`，看起来像缺了编码器的东西，
其实缺的是解码器的**头**。

## 编译开关

从 AOSP 的 `libwebp-encode` 抄的：

```
-O2
-DANDROID              走 Android 的内存/日志适配
-DWEBP_SWAP_16BIT_CSP  Android 的 16 位通道顺序
-DWEBP_USE_THREAD      允许多线程（我们固定 thread_level=0，但代码要在）
```

### ⚠️ 不要自己加 `-DWEBP_USE_SSE2` / `-DWEBP_USE_NEON`

`src/dsp/dsp.h:71` 会按编译器的预定义宏自己判断：

```c
#if defined(__SSE2__) || defined(WEBP_MSC_SSE2) || defined(WEBP_HAVE_SSE2)
#define WEBP_USE_SSE2
#endif
```

重复定义会报 `macro redefined`。

### `HAVE_CONFIG_H` 不要定义

8 处 `#include "src/webp/config.h"` 都在 `#ifdef HAVE_CONFIG_H` 里。
那个 config.h 是 autotools 生成的，我们不用 —— 不定义就编不到。

## 参数选择（实测，见 `tools/bench/webp_bench.cpp`）

| method | 320×480 | 1080p | 相对 Skia 默认(m=3) |
|---|---|---|---|
| 0 | 3.3 ms | 24.8 ms | 快 3.1x，体积 +22% |
| **2** | **5.2 ms** | 38.8 ms | **快 2.0x，体积 +3.6%** ← 默认 |
| 3（Skia） | 10.3 ms | 78.5 ms | — |
| 6 | 21.8 ms | 126.5 ms | 慢 2.1x，体积 -5.8% |

`method=2` 是甜点：只比最强的差 3.6% 体积，却快一倍。
**Skia 用 3 是为了跟 Chrome 对齐，不是因为它最优。**

`thread_level=1` 实测只快 **5%** —— libwebp 的线程只并行熵编码那一小段。
画面流这种尺寸不值得为它引入线程池，所以固定单线程。

## 更新方法

升 AOSP 版本时重新拷一遍：

```bash
A=aosp/external/webp
V=dev/02-native-daemon/daemon/vendor/webp
rm -rf $V/src
for d in enc dsp utils; do mkdir -p $V/src/$d; cp $A/src/$d/*.c $V/src/$d/; done
mkdir -p $V/src/dec $V/src/webp
cp $A/src/dec/*.h  $V/src/dec/      # 只要头
cp $A/src/webp/*.h $V/src/webp/
find $A/src -name '*.h' | while read h; do
  rel="${h#$A/src/}"; mkdir -p "$V/src/$(dirname $rel)"; cp "$h" "$V/src/$rel"
done
```

## 协议

libwebp 用 BSD 3-Clause（见 `aosp/external/webp/COPYING`）。
内置到本项目时**要把许可声明一起带上** —— 见仓库根目录的
`THIRD-PARTY-NOTICES.md`。
