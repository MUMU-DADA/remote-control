# vendor/jpeg —— 从 AOSP 拿来的 libjpeg 头文件

来源：`aosp/external/libjpeg-turbo/`（7 个文件）

## 为什么需要 vendor

NDK **不提供** libjpeg 的头文件和库。而设备上一直有
`/system/lib64/libjpeg.so` —— 它是 VNDK 库，ABI 跨版本稳定。
缺的只是头文件。

## ⚠️ 必须从 AOSP 拿，不能从上游 libjpeg-turbo 拿

差别在 `jconfig.h` 的 `JPEG_LIB_VERSION`：

```
AOSP:            #define JPEG_LIB_VERSION  62      ← v6b ABI
上游 turbo 默认:  #define JPEG_LIB_VERSION  80      ← v8 ABI
```

**结构体布局在 v6b 和 v8 之间是不同的。** 拿上游的头文件去调
Android 上的库，`jpeg_compress_struct` 的字段会错位 —— 而且不会
报错，只会编出垃圾或者崩溃。

`aosp/external/libjpeg-turbo/jconfig.h` 是 Android 实际编译这个库时
用的那份，所以它是对的。

## 符号改名是无关的

`jpeglibmangler.h` 把 `jpeg_make_c_derived_tbl` 之类改成 `chromium_*` ——
那些是**内部**符号。公开 API（`jpeg_CreateCompress` 等）原名不动，
所以 `dlsym` 能正常找到。

## 更新方法

升 AOSP 版本时重新拷一遍（`jconfig.h` 尤其重要）：

```bash
cp aosp/external/libjpeg-turbo/{jconfig,jmorecfg,jpeglib,jerror,jpegint,jpeglibmangler,jversion}.h \
   dev/02-native-daemon/daemon/vendor/jpeg/
```
