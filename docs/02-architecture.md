# 02 · 架构设计

> `autod` 的组件划分、协议设计与关键决策。

---

## 1. 总览

```
                        外部调用方
                    （本机进程 / 网络客户端）
                              │
                              │  Unix domain socket
                              │  SOCK_SEQPACKET
                              │  + SCM_RIGHTS 传 memfd
                              ▼
              ┌───────────────────────────────┐
              │      autod (native daemon)    │
              │                               │
              │  socket_server.cpp            │  ← 协议解析 / fd 传递 / 鉴权
              │  capture.cpp                  │  ← 截图
              │  inject.cpp                   │  ← 触控注入
              │  protocol.h                   │  ← 对外协议定义
              │                               │
              │  SELinux domain: autod        │
              │  UID: system                  │
              └───────┬───────────────┬───────┘
                      │ Binder        │ Binder
                      ▼               ▼
           ┌──────────────────┐  ┌──────────────────────┐
           │  SurfaceFlinger  │  │ InputManagerService  │
           │  captureDisplay  │  │  injectInputEvent    │
           └──────────────────┘  └──────────────────────┘
```

---

## 2. 组件职责

| 文件 | 职责 | 依赖平台 API |
|---|---|---|
| `protocol.h` | 对外协议定义，客户端共用 | ❌ 无 |
| `socket_server.{h,cpp}` | 监听、收发、`SCM_RIGHTS` 传 fd、`SO_PEERCRED` 鉴权 | ❌ 无 |
| `capture.{h,cpp}` | 调 SurfaceFlinger 抓帧，拷进 memfd | ✅ `libgui` |
| `inject.{h,cpp}` | 构造 `MotionEvent` 并注入 | ✅ `libinput` + `IInputManager` |
| `main.cpp` | 初始化、参数解析、请求分发、信号处理 | ⚠️ `libbinder` |

**只有 `capture.cpp` 和 `inject.cpp` 依赖平台私有 API。** 换 Android 版本时主要改这两个文件。

---

## 3. 三个关键设计决策

### 3.1 截图数据不走 Binder

**问题**：Binder 单次事务上限约 1 MB，而 1080p RGBA_8888 是 **8 MB**。

**方案**：

```
SurfaceFlinger
     │ GraphicBuffer（gralloc 分配）
     ▼
capture.cpp  lock() → 逐行 memcpy → memfd
     │
     │ fd 通过 SCM_RIGHTS 传给客户端
     ▼
客户端  mmap() → 零拷贝读取
```

`memfd` 是匿名内存文件，没有文件系统实体，可以安全地跨进程传递 fd。

**未采用的优化**：直接导出 `GraphicBuffer` 的 dmabuf fd（省掉一次 memcpy）。代价是客户端需要理解 gralloc 布局，可移植性差。属于阶段 3 的优化项。

### 3.2 socket 由 init 创建

```
init 读取 autod.rc 里的 `socket autod seqpacket 0660 system system`
     │
     ├─ 创建 /dev/socket/autod
     ├─ 自动打上 SELinux 标签 autod_socket
     ├─ listen()
     └─ 通过环境变量 ANDROID_SOCKET_autod 把 fd 传给 autod
```

**好处**：

1. 标签和权限由 init 保证，不需要 daemon 自己 `chmod`/`chown`
2. 服务崩溃重启时 socket 不会丢
3. 避免 race condition（先 bind 还是先降权）

开发期的手动模式（`--socket <路径>`）保留，用于阶段 1 免 sepolicy 原型。

### 3.3 鉴权分层

```
第一层：socket 文件权限 0660 system:system  → 只有 system UID/组能连
第二层：SO_PEERCRED 取对端 uid/pid          → 当前仅记录日志
第三层：（待实现）uid 白名单校验
```

`socket_server.cpp` 里 `ServeConnection()` 已经取到 `SO_PEERCRED` 并打日志，鉴权钩子留在那里。

**生产环境建议**：建一个专用 AID（如 `AID_AUTOD`），需要调用的客户端进程加入该组；或在服务端校验 `cred.uid` 是否在允许列表内。

---

## 4. 协议设计

### 4.1 传输层选择

| 选择 | 理由 |
|---|---|
| `AF_UNIX` | 本机通信，不走网络栈，延迟最低 |
| `SOCK_SEQPACKET` | 保留消息边界，不用自己处理粘包/拆包 |
| `SCM_RIGHTS` | 传 fd 的唯一方式（截图帧） |

### 4.2 定长结构体

```c
struct Request {          // 44 字节
    uint32_t magic;       // 'AUTD' = 0x44545541
    uint32_t cmd;
    uint32_t flags;
    uint32_t pointerId;
    int32_t  x, y;        // 主坐标
    int32_t  x2, y2;      // 次坐标（滑动终点）
    uint32_t durationMs;
    float    pressure;
    float    size;
};

struct Reply {            // 40 字节
    uint32_t magic;
    uint32_t status;
    uint32_t cmd;
    uint32_t width, height, stride, format;
    uint32_t reserved;    // ← 显式填充，见下
    uint64_t dataSize;
};
```

### 4.3 ⚠️ 那个填充字段是必须的

`Reply` 里 `uint64_t dataSize` 如果紧跟在一串 `uint32_t` 后面，**编译器会插入 4 字节隐式填充**把它对齐到 8 字节边界：

```
offset 28: <padding 4 bytes>
offset 32: dataSize (8)
总计: 40 字节
```

而 Python 侧如果按紧凑格式（`<IIIIIIIQ` = 36 字节）解析就会**错位 4 字节**。

这类 bug 的现象是"服务端说发完了，客户端读到垃圾"，很难定位。

**解决**：显式写出 `reserved` 字段，让布局在两边都确定，并加断言锁死：

```cpp
// protocol.h
static_assert(sizeof(Request) == 44, "布局变了，需同步 Python 客户端");
static_assert(sizeof(Reply) == 40, "布局变了，需同步 Python 客户端");
```

```python
# autod_client.py
assert REQUEST_SIZE == 44
assert REPLY_SIZE == 40
```

### 4.4 命令集

| 命令 | 说明 | 附带 fd |
|---|---|---|
| `Info` | 查询显示参数 | ❌ |
| `Capture` | 截图 | ✅ memfd |
| `Tap` | 单击 | ❌ |
| `Swipe` | 滑动 | ❌ |
| `TouchDown/Move/Up` | 手动多点触控序列 | ❌ |
| `KeyEvent` | 按键注入（**已声明未实现**） | ❌ |

---

## 5. 截图数据流（详细）

```
① 客户端 sendmsg(Request{cmd=Capture})
        │
② autod 收到请求
        │
③ SurfaceComposerClient::getPhysicalDisplayIds()  → 选显示
        │
④ sp<SyncScreenCaptureListener> listener = new ...
   ScreenshotClient::captureDisplay(displayId, args, listener)
        │
⑤ SurfaceFlinger 做一次 GPU 合成到捕获缓冲         ← 主要耗时（8–20 ms）
        │
⑥ listener->waitForResults()  → ScreenCaptureResults
   检查 result.fenceResult.ok()
        │
⑦ buffer->lock(USAGE_SW_READ_OFTEN, &base)
        │
⑧ memfd_create() + ftruncate(totalSize)
   mmap(fd)
   逐行 memcpy，步长用 getStride() 而非 getWidth()   ← 用错会斜切
   munmap()
        │
⑨ buffer->unlock()                                  ← 忘记会导致 SF 缓冲耗尽
        │
⑩ sendmsg(Reply, SCM_RIGHTS: memfd)
        │
⑪ 客户端 recvmsg 取 fd → mmap → 读像素
```

**两个容易踩的坑**：

- **步长**：`GraphicBuffer::getStride()` ≠ `getWidth()`，逐行拷贝必须用 stride
- **必须 unlock**：忘记 unlock 会让 SurfaceFlinger 的缓冲区逐渐耗尽

**另一个必做项**：`main.cpp` 里必须调

```cpp
android::ProcessState::self()->setThreadPoolMaxThreadCount(0);
android::ProcessState::self()->startThreadPool();
```

否则 Binder 回调永远收不到，`waitForResults()` 会**死等**。参考 `screencap.cpp` 里关于 b/36066697 的注释。

---

## 6. 生命周期

```
开机
  │
  ├─ init 解析 /system/etc/init/autod.rc
  ├─ init 创建 /dev/socket/autod（打标签、设置权限）
  │
  ▼
on property:sys.boot_completed=1
  │
  ├─ init fork/exec /system/bin/autod --init-socket autod
  ├─ 应用 seclabel u:r:autod:s0（SELinux 域转换）
  │
  ▼
main()
  ├─ ProcessState::startThreadPool()      ← 截图必需
  ├─ Capture::Init()                      → 解析显示
  ├─ Injector::Init()                     → 连 InputManagerService
  ├─ SocketServer::Start()                → 接管 init 传来的 fd
  ├─ signal(SIGTERM/SIGINT)
  └─ SocketServer::Run(handler)           ← 阻塞在 accept
        │
        └─ 每个连接：ServeConnection() 串行处理请求
```

**当前限制**：串行处理。高并发需要工作线程池。

---

## 7. 相关文档

- 方案选型理由 → `01-selection.md`
- Android 12 的触控约束 → `06-constraints.md`
- 延迟数据 → `05-latency-and-touch.md`
- 代码实现 → `../dev/02-native-daemon/`
