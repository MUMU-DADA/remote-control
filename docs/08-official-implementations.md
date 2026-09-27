# 08 · 官方与开源实现对照

> 原则：**能用官方或成熟开源实现的，不自己写。**
>
> 本文逐组件记录：存在什么现成实现、我们用了哪个、为什么。

---

## 汇总

| 组件 | 现成实现 | 我们的选择 | 状态 |
|---|---|---|---|
| 截图（设备命令） | AOSP `screencap` | **直接用** | ✅ |
| 截图（SF 直连） | AOSP `screencap.cpp` | **对照实现** | ✅ |
| 触控（Binder 路径） | AOSP `virtual_touchpad` | **直接用** | ✅ 新增 |
| 触控（uinput 路径） | AOSP `EvdevInjector` | 自己写（有原因，见下） | ⚠️ |
| SELinux 策略 | AOSP `virtual_touchpad.te` | **照抄模板** | ✅ |
| init 服务框架 | AOSP `init.rc` / `android_get_control_socket` | **直接用** | ✅ |
| 日志 | AOSP `liblog` / NDK `android/log.h` | **直接用** | ✅ |
| Binder 传输 | AOSP `libbinder` | **直接用** | ✅ |
| 帧数据通道 | Linux `memfd` + `SCM_RIGHTS` | **直接用** | ✅ |
| socket 协议 | 无官方对应 | 自定义 | — |

---

## 1. 截图：AOSP `screencap`

### 现成实现

AOSP 自带 `frameworks/base/cmds/screencap/`，命令 `/system/bin/screencap`。

### 我们的选择

| 路径 | 做法 |
|---|---|
| `capture_screencap.cpp` | **直接 exec `/system/bin/screencap`**，解析它的 stdout 原始像素流 |
| `capture_surfaceflinger.cpp` | **对照 `screencap.cpp` 实现**，走 `ScreenshotClient::captureDisplay` |

### 为什么 SF 路径不直接复用 `screencap` 命令

- exec 一次 100–300 ms，SF 直连 20–35 ms
- 但 SF 路径必须编进 AOSP 树（依赖 `libgui`），
  所以 NDK 路径保留了 exec 方案

**两条路径都源自官方实现，没有自己发明抓屏方式。**

---

## 2. 触控：AOSP `virtual_touchpad` ★

### 现成实现

```
frameworks/native/services/vr/virtual_touchpad/
├── EvdevInjector.{h,cpp}            通用 uinput 封装
├── VirtualTouchpadEvdev.{h,cpp}     造设备 + 事件合成
├── VirtualTouchpadService.cpp       Binder 服务
├── VirtualTouchpadClient.cpp        Binder 客户端
├── main.cpp                         /system/bin/virtual_touchpad
├── idc/vr-virtual-touchpad-0.idc    声明 touch.deviceType = touchScreen
├── virtual_touchpad.rc              service（user system / group system input uhid）
├── include/VirtualTouchpad.h        ★ 导出
├── include/VirtualTouchpadClient.h  ★ 导出
└── aidl/android/dvr/IVirtualTouchpadService.aidl
```

**这不是触摸板，是触摸屏。** `.idc` 里明确写着：

```
device.internal = 1
touch.deviceType = touchScreen
```

设备能力：`INPUT_PROP_DIRECT` + `BTN_TOUCH` + 多指 `ABS_MT_*`，坐标范围 `0x10000`。

### 我们的选择：`inject_vtp.cpp`（新增）

走官方 Binder 服务：

```cpp
#include <VirtualTouchpadClient.h>       // 从 export_include_dirs 拿
auto tp = android::dvr::VirtualTouchpadClient::Create();
tp->Attach();
tp->Touch(slot, x_norm, y_norm, pressure);   // 坐标 [0.0, 1.0)
```

### 最大的好处：权限模型简化

| | 自己写 uinput | 走官方服务 |
|---|---|---|
| `autod` 需要的 POSIX 权限 | `uhid` 组（`/dev/uinput` 是 `0660 uhid:uhid`） | 无 |
| `autod` 需要的 SELinux 权限 | `uhid_device:chr_file { w_file_perms ioctl }` | `virtual_touchpad_service:service_manager find` |
| 需维护的 uinput 代码 | ~350 行 | 0 |

### 代价

| 限制 | 说明 |
|---|---|
| **只有 2 个触控槽位** | `VirtualTouchpadEvdev::kTouchpads = 2`，做不了 3 指以上手势 |
| 不在默认产品包里 | 是 VR 专用，需要 `PRODUCT_PACKAGES += virtual_touchpad` |
| 多一次 Binder 往返 | 约 0.3–1 ms，仍远快于 adb 的 30–80 ms |

### 两个后端并存

```
inject_uinput.cpp   自己写：10 槽位、完整协议 B、主机可测、NDK 可用
inject_vtp.cpp      官方：2 槽位、无需 uinput 权限
```

由 `Android.bp` 选一个链接（同名工厂函数不能共存）：

| 目标 | 触控后端 | 适用 |
|---|---|---|
| `m autod` | uinput | 默认；功能最全 |
| `m autod_vtp` | 官方服务 | 想要最小权限面时 |

**后端抽象在这里体现价值：切换时上层代码一行没改。**

---

## 3. 触控：为什么没有直接用 `EvdevInjector`

`EvdevInjector` 是 AOSP 的通用 uinput 封装（配 property/key/abs/rel + 发事件），
我们的 `inject_uinput.cpp` 实质上是它的重新实现。**没有直接用它有两个具体原因：**

1. **头文件没导出。** `libvirtualtouchpad` 的 `export_include_dirs` 只有 `include/`，
   而 `EvdevInjector.h` 在模块根目录。外部模块 include 不到，
   要用就得往 include path 里塞 `..`——而 Soong 明确禁止路径逃逸
   （`init_rc` 上实测报 `Path is outside directory`）。

2. **会丢掉主机可测性。** `EvdevInjector` 依赖 `android-base/unique_fd.h` 和
   `utils/String8.h`，只能在 AOSP 树内编译。而我们的版本是纯 Linux syscall，
   在开发机上能跑 31 项真实设备测试（真的创建虚拟触摸屏、注入、读回校验）。

**结论：这里自己写是有依据的取舍，不是重复造轮子。** 但官方版的能力
（2 槽位）覆盖不了我们的需求（多点触控），这也是保留自研版本的理由之一。

---

## 4. 评估过但没采用的方案

### scrcpy

开源、成熟，做「投屏 + 控制」的事实标准。**没采用的原因：**

| 项 | scrcpy | 我们的目标 |
|---|---|---|
| 运行身份 | `shell`（通过 adb 推 server 到 `/data/local/tmp`） | 系统服务，`system` UID |
| 生命周期 | 绑定 adb 连接 | 开机自启，常驻 |
| 对外接口 | 私有协议，需配套客户端 | 明确的 socket API |
| 输入注入 | 优先走 Java `InputManager` | uinput / 官方 vtp 服务 |

scrcpy 的**原理**我们采用了（VirtualDisplay + MediaCodec 那条路我们没走，
因为要的是单帧截图而非视频流），但它的部署模型和「系统级常驻服务」不匹配。

### minicap / minitouch

STF 的设备端组件。**没采用的原因：**
- minicap 读 SurfaceFlinger，原理与我们的 SF 后端一致
- minitouch 直接写 `/dev/input/eventX`（**已存在的**设备节点），
  而我们要创建**新的**虚拟设备——两者不是一回事
- 两者都需要 root 且部署方式和 scrcpy 类似

### Kbox-patches

华为鲲鹏的云手机方案（Apache-2.0，含内核补丁）。**没采用的原因：**
它的补丁用于让 AOSP 在容器里启动（SELinux/vintf/seccomp/gralloc），
不是做截图触控的；而且要配套鲲鹏硬件。

---

## 5. 明确自己写的部分

| 组件 | 为什么自己写 |
|---|---|
| socket 协议（`protocol.h`） | 无官方对应；需求明确且小（44/40 字节定长结构） |
| `socket_server.cpp` | AOSP 无现成的 SEQPACKET + `SCM_RIGHTS` 服务端 |
| `dispatch.cpp` | 业务逻辑 |
| `inject_uinput.cpp` | 见第 3 节（导出限制 + 主机可测性 + 槽位数） |
| `capture_stub.cpp` | 测试专用，无对应物 |

---

## 6. 相关文档

- 方案选型 → `01-selection.md`
- Android 12 的 API 核实记录 → `03-version-matrix.md`
- 约束与风险 → `06-constraints.md`
