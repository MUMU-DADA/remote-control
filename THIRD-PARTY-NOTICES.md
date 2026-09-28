# 第三方代码声明

本项目**内置**（源码随仓库分发）了以下第三方代码。
它们各自的完整许可原文在对应目录里。

---

## libwebp

| | |
|---|---|
| 位置 | `dev/02-native-daemon/daemon/vendor/webp/` |
| 来源 | AOSP `external/webp`（android-12.0.0_r34） |
| 上游 | https://chromium.googlesource.com/webm/libwebp |
| 许可 | **BSD 3-Clause**（见同目录 `LICENSE`） |
| 版权 | Copyright (c) 2010, Google Inc. All rights reserved. |
| 范围 | `src/enc` + `src/dsp` + `src/utils` 的 98 个 `.c` 及相应头文件 |

**为什么内置**：设备上没有 `libwebp.so`（实测 Android 12 没有，
Skia 里也没找到），而 `AndroidBitmap_compress` 是 API 30 才有的 ——
不内置的话 Android 8~10 完全没有 WebP 编码能力。

BSD 3-Clause 要求分发时保留版权声明和许可原文，因此：
- `vendor/webp/LICENSE` 是许可原文
- 本节保留版权归属

---

## libjpeg-turbo

| | |
|---|---|
| 位置 | `dev/02-native-daemon/daemon/vendor/jpeg/` |
| 来源 | AOSP `external/libjpeg-turbo`（android-12.0.0_r34） |
| 上游 | https://github.com/libjpeg-turbo/libjpeg-turbo |
| 许可 | **BSD-style**（三套兼容许可，见同目录 `LICENSE.md`） |
| 范围 | **仅头文件**（7 个），没有内置任何 `.c` |

**注意**：这里**只 vendor 了头文件**。实际的编码器是运行时
`dlopen("libjpeg.so")` 用系统自带的那个 —— 设备上一直有，
且是 VNDK 库（ABI 跨版本稳定）。

头文件本身是接口声明，但既然随仓库分发，许可声明一并保留。

> ⚠️ 头文件**必须**从 AOSP 拿，不能用上游的 —— AOSP 的 `jconfig.h`
> 把 `JPEG_LIB_VERSION` 钉死在 `62`，而上游默认是 `80`，
> 两版的结构体布局不同。用错了会静默产出垃圾或崩溃。
> 详见 `vendor/jpeg/README.md`。

---

## 链接但未内置的

以下依赖由**设备系统**提供，本项目不分发它们的代码：

| 依赖 | 来源 | 用途 |
|---|---|---|
| `libjpeg.so` | 设备 `/system/lib64/`（VNDK） | JPEG 编码（dlopen） |
| `libz.so` | 设备 `/system/lib64/` | PNG 编码（dlopen） |
| `libcurl.so` | 设备 `/system/lib64/`（VNDK） | 下载（dlopen） |
| `libjnigraphics.so` | 设备 `/system/lib64/` | `AndroidBitmap_compress`（dlopen，API 30+） |
| `liblog.so` | NDK | 日志 |

运行时 `dlopen`，**没有静态链接，也没有随本项目分发**。

---

## 更新第三方代码时

重新拷源码之后要确认：

1. 对应的 `LICENSE` 文件还在（`vendor/*/LICENSE*`）
2. 本文件的「来源」一栏的版本号对得上
3. 如果新增了别的第三方代码，在这里加一节
