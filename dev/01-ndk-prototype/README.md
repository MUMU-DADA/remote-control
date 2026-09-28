# 01 · NDK 构建路径（不需要 AOSP 源码树）

> **让 `autod` 本身脱离 AOSP 树编译运行** —— 不需要 `repo sync` 110 GB，
> 不需要首次编译 1 小时。只要有 NDK 和一台 root 的安卓设备。

---

## 这个轨道现在是什么

最初这里规划的是一个**独立的原型程序**（另写一份 exec `screencap` + 写 uinput 的代码）。

**那个方案已经不需要了** —— 截图和触控都做成了 `autod` 的可插拔后端，
同一个代码库、同一套协议、同一份测试，只是换个后端编译：

```
                    capture_surfaceflinger.cpp   ← 需要 AOSP 树
capture 后端  ──┬── capture_screencap.cpp       ← NDK 可用 ★
                └── capture_stub.cpp            ← 测试用

                ┌── inject_uinput.cpp            ← NDK 可用 ★
inject 后端   ──├── inject_binder.cpp           ← Android 12 上不可用
                └── inject_vtp.cpp              ← 走 AOSP 官方服务
```

**避免了两套并行代码**，也不会出现"原型验证过了、主线又踩一遍坑"的情况。

---

## 怎么用

```bash
# 1. 装 NDK（约 640 MB）
wget https://dl.google.com/android/repository/android-ndk-r26d-linux.zip
unzip -q android-ndk-r26d-linux.zip -d /opt/android/

# 2. 编译（默认 arm64-v8a + API 31）
bash tools/build-ndk.sh

# 产物
#   dev/02-native-daemon/out/ndk/arm64-v8a/autod
#   dev/02-native-daemon/out/ndk/arm64-v8a/autodctl
```

**实测产物**（已在本机编译通过）：

```
autod     545,616 字节   ELF 64-bit LSB pie executable, ARM aarch64
autodctl  436,096 字节   ELF 64-bit LSB pie executable, ARM aarch64

动态依赖：liblog.so  libc.so  libm.so  libdl.so    ← 全是 bionic 系统库
```

**没有任何平台私有库**，推到设备就能跑。

### 部署

```bash
adb push out/ndk/arm64-v8a/autod    /data/local/tmp/
adb push out/ndk/arm64-v8a/autodctl /data/local/tmp/
adb shell chmod 755 /data/local/tmp/autod /data/local/tmp/autodctl

# 触控范围必须显式指定 —— screencap 后端拿不到显示尺寸
adb shell wm size       # 先看分辨率
adb shell su -c '/data/local/tmp/autod --socket /data/local/tmp/autod.sock \
                 --touch-range 1080x2400 &'

adb shell /data/local/tmp/autodctl --socket /data/local/tmp/autod.sock info
adb shell /data/local/tmp/autodctl --socket /data/local/tmp/autod.sock capture -o /data/local/tmp/shot.png
adb shell /data/local/tmp/autodctl --socket /data/local/tmp/autod.sock tap 540 1200
```

需要 root（`/dev/uinput` 是 `0600 root:root`，进程得能写）。

---

## 代价

| 项 | NDK 路径 | AOSP 路径 |
|---|---|---|
| 编译前置 | NDK 640 MB | 源码树 110 GB + 首次编译 1 小时 |
| 截图延迟 | **~197 ms**（每次 fork+exec） | **8–12 ms**（SF 直连，720p） |
| 能拿到显示尺寸 | ✅ 启动探针自动探测 | ✅ |
| 区域截图 / 降采样 | ❌ | ✅（`sourceCrop`） |
| 触控 | ✅ uinput | ✅ uinput 或官方 vtp 服务 |

**定位：让服务尽早在真机上跑起来、验证整条链路。最终形态还是 AOSP 构建。**

---

## 参考资料：`screencap` 的输出格式

`capture_screencap.cpp` 解析的就是这个格式，来自 AOSP 的
`frameworks/base/cmds/screencap/screencap.cpp`（`saveImage()` 的分支）：

```
偏移 0    uint32  width
偏移 4    uint32  height
偏移 8    uint32  pixelFormat
偏移 12   uint32  colorSpace
偏移 16   像素数据，逐行紧密排列，每行 width * bytesPerPixel 字节
```

`pixelFormat` 的取值（`android PixelFormat`）：

| 值 | 格式 | 每像素字节 |
|---|---|---|
| 1 | RGBA_8888 | 4 |
| 2 | RGBX_8888 | 4 |
| 3 | RGB_888 | 3 |
| 4 | RGB_565 | 2 |
| 5 | BGRA_8888 | 4 |
| 22 | RGBA_FP16 | 8 |
| 43 | RGBA_1010102 | 4 |

加 `-p` 参数则输出 PNG，但那样就没法直接内存映射，而且编码很贵。

---

## 参考资料：uinput 设备配置

`inject_uinput.cpp` 用的初始化序列（`dev/02-native-daemon/daemon/inject_uinput.cpp`）：

```c
ioctl(fd, UI_SET_EVBIT,  EV_KEY);
ioctl(fd, UI_SET_EVBIT,  EV_ABS);
ioctl(fd, UI_SET_EVBIT,  EV_SYN);

ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);

ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

// ⚠️ 最容易被忽略的一步：不设这个，InputReader 不会把它当触摸屏
ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
```

事件按 **multitouch protocol B** 顺序写，最后 `input_sync()` / `SYN_REPORT`。

> AOSP 官方也有等价实现（`frameworks/native/services/vr/virtual_touchpad/`），
> 还带一份 `.idc` 声明 `touch.deviceType = touchScreen`。
> 详见 [`docs/01-selection.md`](../../docs/01-selection.md) 第 6 节「官方与开源实现对照」。

---

## 相关文档

- 官方/开源实现对照 → [`docs/01-selection.md`](../../docs/01-selection.md) 第 6 节
- Android 12 的触控约束 → [`docs/01-selection.md`](../../docs/01-selection.md) 第 7 节
- 抓帧性能实测 → [`docs/06-capture-performance.md`](../../docs/06-capture-performance.md)
- AOSP 主线 → [`../02-native-daemon/README.md`](../02-native-daemon/README.md)
