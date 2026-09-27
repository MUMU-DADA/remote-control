# 01 · 纯 NDK 快速验证原型

> **目标：当天就能在任意 root 安卓机上跑通截图 + 触控，不需要 AOSP 源码树。**

---

## 为什么需要这条轨道

主线的 `autod` 依赖 `libgui` 等平台私有库，需要 `repo sync` 拉 ~85 GB 源码才能编译。

但在投入那些成本之前，应该先验证**思路本身可行**：

- 截图和触控这两条链路，在你手上的设备上到底能不能通？
- 延迟能不能接受？
- 有没有意料之外的权限问题？

这条轨道用**纯 NDK** 实现同样的功能，代价是截图慢一些。

---

## 技术方案

| 能力 | 做法 | 依赖 |
|---|---|---|
| **截图** | `exec` 调用设备上已有的 `screencap`，读它的 stdout | 无（`screencap` 是 Android 自带命令） |
| **触控** | 写 `/dev/uinput` 创建虚拟触摸屏 | 内核自带 `uinput` 模块 |
| **对外接口** | 与主线相同的 Unix socket 协议 | 无 |

**两个都不需要 AOSP 树，NDK 就能编。**

---

## 截图：`screencap` 的输出格式

关键信息：`screencap` 不带 `-p` 参数时，输出的是**裸像素流**，格式是：

```
偏移 0:   uint32  width
偏移 4:   uint32  height
偏移 8:   uint32  pixelFormat
偏移 12:  uint32  colorSpace
偏移 16:  像素数据，逐行紧密排列，每行 width * bytesPerPixel 字节
```

这是从 AOSP 的 `screencap.cpp` 源码里读出来的（`saveImage()` 函数的分支）。

所以原型里可以这样读：

```c
FILE* p = popen("/system/bin/screencap", "r");
uint32_t header[4];
fread(header, 4, 4, p);          // w, h, format, colorspace
size_t bpp = bytes_per_pixel(header[2]);
size_t size = header[0] * header[1] * bpp;
uint8_t* pixels = malloc(size);
fread(pixels, 1, size, p);
pclose(p);
```

**加上 `-p` 参数**则直接输出 PNG，实现更简单但更慢。

### 代价

每次截图都要 fork + exec 一个进程，耗时 **100–300 ms**。主线方案的 `captureDisplay` 只要 20–35 ms。

**适合验证可行性，不适合生产。**

---

## 触控：uinput 完整初始化序列

```c
int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

// 1. 声明支持的事件类型
ioctl(fd, UI_SET_EVBIT, EV_KEY);
ioctl(fd, UI_SET_EVBIT, EV_ABS);
ioctl(fd, UI_SET_EVBIT, EV_SYN);

// 2. 声明 ABS 轴（multitouch protocol B）
ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);
ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);

// 3. 按键
ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

// 4. ⚠️ 关键且容易漏：标记为直接触摸设备
//    不设这个，InputFlinger 不会把它识别成触摸屏
ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

// 5. 创建虚拟设备
struct uinput_setup setup = {0};
strncpy(setup.name, "ndk-prototype-touch", UINPUT_MAX_NAME_SIZE);
setup.id.bustype = BUS_VIRTUAL;
setup.id.vendor  = 0x1;
setup.id.product = 0x1;
ioctl(fd, UI_DEV_SETUP, &setup);
ioctl(fd, UI_DEV_CREATE);
```

### 设置坐标范围

设备的触摸范围必须和屏幕分辨率对应，否则坐标会错：

```c
struct uinput_abs_setup abs = {0};
abs.code = ABS_MT_POSITION_X;
abs.absinfo.minimum = 0;
abs.absinfo.maximum = screen_width - 1;
ioctl(fd, UI_ABS_SETUP, &abs);
// Y 轴同理
```

屏幕分辨率从 `wm size` 读，或直接硬编码。

### 注入一个单点触摸

```c
void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev = {0};
    ev.type = type; ev.code = code; ev.value = value;
    write(fd, &ev, sizeof(ev));
}

// 按下
emit(fd, EV_ABS, ABS_MT_SLOT, 0);
emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 1);      // 非负值 = 新触点
emit(fd, EV_ABS, ABS_MT_POSITION_X, x);
emit(fd, EV_ABS, ABS_MT_POSITION_Y, y);
emit(fd, EV_ABS, ABS_MT_PRESSURE, 128);
emit(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 8);
emit(fd, EV_KEY, BTN_TOUCH, 1);
emit(fd, EV_SYN, SYN_REPORT, 0);

// 抬起
emit(fd, EV_ABS, ABS_MT_SLOT, 0);
emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);     // -1 = 抬起
emit(fd, EV_KEY, BTN_TOUCH, 0);
emit(fd, EV_SYN, SYN_REPORT, 0);
```

**多点触控**：用 `ABS_MT_SLOT` 切换槽位，每个触点一个 `TRACKING_ID`。最多 10 个（协议上限）。

---

## 权限要求

| 资源 | 要求 |
|---|---|
| `/dev/uinput` | 通常 `0660 system:input`，**需要 root 或 `system` UID** |
| `screencap` | `shell` UID 就有权限（`adb exec-out screencap` 就是这么跑的） |

**所以：截图不需要 root，触控需要 root。**

测试机建议用 **Magisk root 的 Pixel 6 / 6a**。

---

## 编译

用 NDK 就行，不需要 AOSP：

```bash
# 设置 NDK 路径
export NDK=$HOME/android-ndk-r26

# 编译
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang \
    -O2 -Wall -static \
    -o autod-ndk \
    main.c uinput_touch.c screencap_capture.c socket_server.c
```

`-static` 可以避免依赖设备上的 libc++ 版本。

**API level 选 31（Android 12）**，与目标一致。

---

## 复用主线的协议

`socket_server.c` 和 `protocol.h` 可以直接从主线复用——它们不依赖任何平台 API。

```
dev/02-native-daemon/daemon/protocol.h        ← 直接 #include
dev/02-native-daemon/daemon/socket_server.*   ← 改写成 C 或直接用 C++ 版
```

这样练熟之后切到主线，**上层接口一行不用改**。

---

## 验证清单

跑通后应该能确认：

- [ ] `screencap` 的输出格式解析正确，能存出有效 PNG
- [ ] uinput 虚拟触摸屏被系统识别（`getevent -pl` 能看到）
- [ ] 点击能命中 UI
- [ ] 滑动能被应用正确响应
- [ ] 实测单次截图耗时（预期 100–300 ms）
- [ ] 实测单次点击耗时（预期 <10 ms）
- [ ] 确认 `/proc/bus/input/devices` 里能看到虚拟设备（**这是 uinput 的固有代价**）

---

## 这条轨道不解决的

- **`FLAG_SECURE`**：`screencap` 同样抓不到受保护内容
- **隐蔽性**：uinput 设备可枚举，见上
- **性能**：截图慢一个数量级

它只回答一个问题：**思路能不能跑通。**

---

## 升级路径

验证通过后：

```
01-ndk-prototype  →  02-native-daemon
   exec screencap       SurfaceFlinger Binder（快 5-10 倍）
   uinput               uinput（保留）或 Java 服务
   纯 NDK               AOSP 树
```

协议和 socket 层可以原样带走。

---

## 相关文档

- 主线方案 → `../02-native-daemon/README.md`
- Android 12 的触控约束 → `../../docs/06-constraints.md`
- 延迟对比 → `../../docs/05-latency-and-touch.md`
