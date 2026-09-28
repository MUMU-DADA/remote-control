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
| 位置 | `dev/02-native-daemon/daemon/vendor/jpeg/`（设备侧）<br>`dev/02-native-daemon/tools/bench/vendor-jpeg/`（主机侧基准工具） |
| 来源 | AOSP `external/libjpeg-turbo`（android-12.0.0_r34） |
| 上游 | https://github.com/libjpeg-turbo/libjpeg-turbo |
| 许可 | **BSD-style**（三套兼容许可，见同目录 `LICENSE.md`） |
| 范围 | **仅头文件**（7 个），没有内置任何 `.c` |

> ⚠️ 头文件有**两份**（设备侧 `daemon/vendor/jpeg/` 与主机侧
> `tools/bench/vendor-jpeg/`，内容相同）。本节原来只提了前者 ——
> 后者随仓库分发、许可声明却没写，属漏项，已补。

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

## 本项目的许可

**MIT**，见根目录 [`LICENSE`](LICENSE)。

选 MIT 而不是 GPL 的依据是**上面这份清单里没有任何 copyleft 代码**：

| 内置的 | 许可 | 是否传染 |
|---|---|---|
| libwebp | BSD 3-Clause | 否 |
| libjpeg-turbo（仅头文件 ×2 份） | BSD-style（IJG + BSD-3 + zlib） | 否 |

其余依赖是运行时 `dlopen` 设备上的系统库（libjpeg / libz / libcurl /
libjnigraphics），**不随本项目分发**。AOSP 源码树与 Google 的翻译层载荷
都在 `.gitignore` 里，不在本仓库中。

BSD 系是宽松许可，可以并入 MIT 作品 —— 代价是要**保留它们的版权声明**，
这正是本文件存在的意义。反过来，如果哪天内置了 GPL/LGPL 的代码，
本项目就不能再以 MIT 分发，得整体换成 GPL（这也是 LICENSE 里那条
"更新第三方代码时"要一起检查的原因）。

---

## 更新第三方代码时

重新拷源码之后要确认：

1. 对应的 `LICENSE` 文件还在（`vendor/*/LICENSE*`）
2. 本文件的「来源」一栏的版本号对得上
3. 如果新增了别的第三方代码，在这里加一节
4. **确认新依赖不是 GPL/LGPL/AGPL** —— 是的话本项目的 MIT 就失效了，
   要么换依赖，要么整体改 GPL（见上一节）
