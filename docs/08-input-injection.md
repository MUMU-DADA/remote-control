# 备选注入路径：Java 系统服务 / 特权 APK

> 补上 Android 12 的触控缺口：native 负责截图，Java 负责注入。

> 本文是**备选方案的设计**，不是正在开发的轨道。
> 当前实现走 `/dev/uinput`（见 `dev/02-native-daemon/`），能用但有代价：
> **会创建一个可枚举的输入设备**（`/proc/bus/input/devices` 里看得见）。
> 本文这条路走 `injectInputEvent`，不创建设备 —— 代价是要平台签名、要进镜像。

---

## 为什么会有这条路

**Android 12 的 `injectInputEvent` 是 Java-only AIDL，native 进程调不了。**

完整论证见 [`01-selection.md`](01-selection.md) 第 7 节「硬约束与风险」。

对比之下，截图用的 `ISurfaceComposer` 是 native AIDL，C++ 可以直连。

于是方案 B 在 Android 12 上必须拆成两半。

---

## 架构

```
        外部调用方
             │ Unix domain socket
             ▼
   ┌──────────────────────────┐
   │  remote-control (native daemon)   │  ← 对外统一入口
   │  截图：SurfaceFlinger ✅  │
   │  触控：转发给 Java 服务  │
   └────────────┬─────────────┘
                │ 内部 Unix socket（或 Binder）
                ▼
   ┌──────────────────────────┐
   │  RemoteControlInputService (Java)│
   │  InputManager            │
   │    .injectInputEvent() ✅│
   └──────────────────────────┘
```

**对外接口不变。** 调用方仍然只连 `remote-control`，不需要知道触控是转发的。

---

## 两种 Java 落点

### 方案 1：特权 APK（`priv-app` + 平台签名）← 推荐

| 项 | 说明 |
|---|---|
| 形式 | 普通 APK，装进 `/system/priv-app/` |
| 权限 | `INJECT_EVENTS`（`signature`），由平台签名获得 |
| 编译 | AOSP 树里用 `android_app` 模块，`certificate: "platform"` |
| 启动 | init 或 `BOOT_COMPLETED` 广播 |
| 优点 | 隔离好，崩了不影响系统 |
| 缺点 | 需要平台签名密钥 |

### 方案 2：system_server 内部服务

| 项 | 说明 |
|---|---|
| 形式 | 改 `SystemServer.java`，在启动流程里 `addService()` |
| 权限 | 同进程内，无需权限检查 |
| 优点 | 权限最全，延迟最低 |
| 缺点 | **system_server 崩 = 整机重启**<br>每次改动要重编 system image<br>启动顺序有讲究 |
| 评价 | ❌ 除非确实需要 hook WMS，否则不选 |

---

## 通信方式

`remote-control`（C++）→ `RemoteControlInputService`（Java），两种选择：

| 方式 | 优点 | 缺点 |
|---|---|---|
| **Unix domain socket** ← 推荐 | 简单，无 AIDL backend 问题<br>复用已有的 `socket_server` 代码 | 多一次进程间拷贝 |
| **Binder（自定义 AIDL）** | 更「Android 原生」<br>延迟略低 | 需要给 AIDL 配 cpp backend<br>Soong 配置更复杂 |

### 为什么推荐 socket

我们**自己定义**的 AIDL 是可以加 `backend: cpp` 的（这正是 AOSP 自带的 `IInputManager` 做不到的）。但配置 `aidl_interface` 模块、处理版本兼容，工作量明显大于直接开一个 socket。

而且 **`socket_server.cpp` 已经写好并验证过了**，Java 侧用 `LocalServerSocket` 对接即可。

**触控请求量不大（每秒几十到几百次），socket 的开销可以忽略。**

---

## 代码骨架

### Java 服务

以下代码是结构骨架，不是可直接编译的完整实现。Java 的 `LocalServerSocket`
使用抽象命名空间的 `SOCK_STREAM`；native 客户端必须使用相同类型。由于 stream
不保留消息边界，实际实现还需要为触控请求和响应定义一致的长度或固定大小 framing。

```java
package com.remotecontrol.input;

public class RemoteControlInputService extends Service {
    private static final String SOCKET_NAME = "remote_control_input";

    private InputManager mInputManager;
    private LocalServerSocket mServer;
    private Thread mThread;
    private volatile boolean mRunning;

    @Override
    public void onCreate() {
        super.onCreate();

        // InputManager.getInstance() 是 @hide API，
        // 需要 Android.bp 里 platform_apis: true
        mInputManager = InputManager.getInstance();

        mRunning = true;
        mThread = new Thread(this::serveLoop, "remote-control-input");
        mThread.start();
    }

    private void serveLoop() {
        try {
            mServer = new LocalServerSocket(SOCKET_NAME);
        } catch (IOException e) {
            Log.e(TAG, "socket 创建失败", e);
            return;
        }

        while (mRunning) {
            try (LocalSocket client = mServer.accept()) {
                handleClient(client);
            } catch (IOException e) {
                if (mRunning) Log.e(TAG, "accept 失败", e);
            }
        }
    }

    /** 注入单点触摸 */
    private boolean injectTouch(int action, int x, int y, long downTime) {
        long now = SystemClock.uptimeMillis();

        MotionEvent event = MotionEvent.obtain(
                downTime != 0 ? downTime : now,   // downTime
                now,                              // eventTime
                action,
                x, y,                             // 坐标
                0                                 // metaState
        );

        event.setSource(InputDevice.SOURCE_TOUCHSCREEN);

        // ASYNC 不等待分发结果，延迟低
        boolean ok = mInputManager.injectInputEvent(
                event, InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);

        event.recycle();
        return ok;
    }

    /** 注入多点触摸（双指缩放等） */
    private boolean injectMultiTouch(int action, PointerProperties[] props,
                                     PointerCoords[] coords) {
        long now = SystemClock.uptimeMillis();

        MotionEvent event = MotionEvent.obtain(
                now, now, action,
                props.length,                     // pointerCount
                props, coords,
                0, 0,                             // metaState, buttonState
                1.0f, 1.0f,                       // xPrecision, yPrecision
                0, 0,                             // deviceId, edgeFlags
                InputDevice.SOURCE_TOUCHSCREEN,
                0,                                // displayId
                0                                 // flags
        );

        boolean ok = mInputManager.injectInputEvent(
                event, InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);

        event.recycle();
        return ok;
    }

    @Override
    public void onDestroy() {
        mRunning = false;
        try { if (mServer != null) mServer.close(); } catch (IOException ignored) {}
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
```

### `AndroidManifest.xml`

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.remotecontrol.input">

    <uses-permission android:name="android.permission.INJECT_EVENTS" />

    <application android:persistent="true">
        <service
            android:name=".RemoteControlInputService"
            android:exported="false"
            android:directBootAware="true" />
    </application>
</manifest>
```

`android:persistent="true"` 让它常驻，被系统杀掉后自动重启。

### `Android.bp`

```
android_app {
    name: "RemoteControlInputService",

    srcs: ["src/**/*.java"],
    manifest: "AndroidManifest.xml",
    resource_dirs: ["res"],

    // 关键：允许使用 @hide API（InputManager.getInstance 等）
    platform_apis: true,

    // 平台签名 → 拿到 INJECT_EVENTS
    certificate: "platform",

    // 装进 /system/priv-app/
    privileged: true,

    // 直接进 system 镜像
    system_ext_specific: false,

    optimize: { enabled: false },
    dex_preopt: { enabled: false },
}
```

### `remote-control` 侧的转发

`inject.cpp` 里加一个后端，把触控请求转发到 `remote_control_input` socket：

```cpp
// inject_socket.cpp（替代 inject_binder.cpp）
int fd = socket(AF_UNIX, SOCK_STREAM, 0);
sockaddr_un addr{};
addr.sun_family = AF_UNIX;
strncpy(addr.sun_path + 1, "remote_control_input", sizeof(addr.sun_path) - 1);
// 注意：Android 的 LocalServerSocket 用的是抽象命名空间（sun_path[0] = '\0'）
connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

// 发一个简化的触控请求结构体
// 接收 boolean 结果
```

**要点**：Android 的 `LocalServerSocket` 默认使用**抽象命名空间**（`sun_path[0] == '\0'`），
不是文件系统路径。C++ 侧构造 `sockaddr_un` 时要注意这一点。

---

## 部署

```bash
# 1. 编进镜像（或者用 Magisk 塞进 /system/priv-app/）
m RemoteControlInputService remote-control rcctl

# 2. 确认服务起来了
adb shell dumpsys activity services com.remotecontrol.input

# 3. 确认有 INJECT_EVENTS 权限
adb shell dumpsys package com.remotecontrol.input | grep -i inject

# 4. 冒烟测试
adb shell "/data/local/tmp/rcctl --socket /data/local/tmp/remote-control.sock tap 540 1200"
```

---

## 优缺点

| 优点 | 缺点 |
|---|---|
| 各用各自能用的 API，不打架 | 多一个进程 |
| 不创建额外输入设备（对比 uinput） | Java 服务需要平台签名 |
| 对外接口不变（`remote-control` 仍是唯一入口） | 多一次进程间通信 |
| 支持完整的多点触控和压力曲线 | 需要编进镜像（或用 Magisk 绕） |
| 是真正的「系统级」实现 | |

### 对比 uinput 方案

| | uinput（`01`/`02` 轨道） | Java 服务（本轨道） |
|---|---|---|
| 需要 AOSP 树 | 可选 | ✅ 必须 |
| 需要平台签名 | ❌ | ✅ |
| 额外输入设备 | ✅ 会创建 | ❌ 不创建 |
| 可被枚举 | **是** | 否 |
| 多点触控 | 需要自己实现 protocol B | 框架直接支持 |
| 实现难度 | 低 | 中 |

**如果「不产生可枚举设备」对你是硬需求，就必须走这条轨道。**

---

## 实施建议

**不要一开始就做这条。**

推荐顺序：

```
1. 先用 01-ndk-prototype 验证思路（当天）
       ↓
2. 再用 02-native-daemon + uinput 跑通完整链路
       ↓
3. 最后把触控后端换成这里的 Java 服务
```

因为 `inject.h` 的接口是统一的，第 3 步只需要换一个实现文件，**上层一行不用改**。

---

## 相关

- Android 12 的约束 → [`01-selection.md`](01-selection.md) 第 7 节的约束 1
- 主线方案 → `../02-native-daemon/README.md`
- 架构总览 → [`02-architecture.md`](02-architecture.md)
