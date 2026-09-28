# 02 · 架构设计

> `autod` 的组件划分、协议设计与关键决策。
>
> **接口细节（三条传输的参数、响应、错误码）以 [`api/`](api/README.md) 为准**，本文不重复罗列。

---

## 1. 总览

```
      本机进程                      网络客户端
         │                              │
         │ Unix socket                  │ HTTP/JSON + WebSocket
         │ SOCK_SEQPACKET               │ /api/v1/...
         │ + SCM_RIGHTS 传 memfd        │ （可选 Bearer 令牌）
         └───────────┬──────────────────┘
                     ▼
     ┌───────────────────────────────────────────┐
     │            autod (native daemon)          │
     │                                           │
     │  socket_server.cpp   Unix socket 服务端    │
     │  http_server.cpp     HTTP/1.1 + WS 升级    │
     │  rest_api.cpp        /api/v1/* 路由        │
     │  dispatch.cpp        ← 唯一的 Dispatcher   │
     │  capture_*.cpp       截图后端（编译期选）   │
     │  inject_*.cpp        注入后端（编译期选）   │
     │  config_file.cpp     /sdcard/autod.conf   │
     │                                           │
     │  SELinux domain: autod                    │
     └───────┬──────────────────────┬────────────┘
             │ Binder               │ syscall
             ▼                      ▼
    ┌──────────────────┐   ┌──────────────────────┐
    │  SurfaceFlinger  │   │  /dev/uinput         │
    │  captureDisplay  │   │  虚拟触摸屏 / 虚拟键盘 │
    └──────────────────┘   └──────────────────────┘
```

**三条传输、一套实现**：HTTP 和 Unix socket 都走同一个 `Dispatcher`，能力完全一致；WebSocket 是 HTTP 之上的流式外壳（画面流 / 触控流 / 日志流）。三者只是不同的"外壳"，不存在"某个功能只有一种传输支持"。

---

## 2. 组件职责

| 文件 | 职责 | 依赖平台 API |
|---|---|---|
| `protocol.h` | 对外二进制协议定义，客户端共用 | ❌ 无 |
| `socket_server.{h,cpp}` | Unix socket 监听、收发、`SCM_RIGHTS` 传 fd、`SO_PEERCRED` 取对端身份 | ❌ 无 |
| `http_server.{h,cpp}` | HTTP/1.1 服务端 + WebSocket 升级（自研，只实现协议子集） | ❌ 无 |
| `rest_api.{h,cpp}` | `/api/v1/*` 路由、JSON 编解码 | ❌ 无 |
| `dispatch.{h,cpp}` | 请求分发 + **操作串行化**（两条传输的唯一入口） | ❌ 无 |
| `capture.h` + `capture_surfaceflinger.cpp` / `capture_screencap.cpp` | 抓帧并拷进 memfd。前者直连 SF，后者 exec `/system/bin/screencap` | ✅ `libgui` / ❌（只 fork/exec） |
| `inject.{h,cpp}` + `inject_uinput.cpp` / `inject_vtp.cpp` / `inject_binder.cpp` | 手势与按键逻辑（平台无关）+ 可替换的注入后端 | ✅ 视后端而定 |
| `keyboard.cpp` / `clipops.cpp` / `appops.cpp` / `fileops.cpp` | 按键、剪贴板、应用管理、文件管理 | ⚠️ 走 `pm`/`am`/`cmd`/`dumpsys` 子进程 |
| `config_file.{h,cpp}` / `service_state.{h,cpp}` | `/sdcard/autod.conf`、运行时状态（API 的"控制自身"） | ❌ 无 |
| `webui.cpp` / `image_encoder.cpp` / `png_encoder.cpp` | 内置网页控制台与图像编码 | ❌ 无 |
| `main.cpp` | 初始化、参数解析、信号处理、拉起两条传输 | ⚠️ `libbinder` |

**平台私有 API 只集中在两处**：截图后端（`capture_surfaceflinger.cpp`）和 Binder 注入后端（`inject_binder.cpp`，Android 12 上不可用）。换 Android 版本时主要改这两个文件——`inject_uinput.cpp` 是纯 Linux syscall，`socket_server` / `http_server` / `dispatch` 完全不碰平台 API。

---

## 3. 三个关键设计决策

### 3.1 截图数据不走 Binder

**问题**：Binder 单次事务上限约 1 MB，而 1080p RGBA_8888 是 **8 MB**。

**方案**：

```
截图后端（SF 直连 / exec screencap）
     │ 像素
     ▼
capture_*.cpp  → memfd（逐行 memcpy，步长用 getStride()）
     │
     │ fd 通过 SCM_RIGHTS 传给客户端
     ▼
客户端  mmap() → 零拷贝读取
```

**两条截图后端产出的是同一种 memfd**，上层（`dispatch.cpp` / `rest_api.cpp`）完全无感知：

| 后端 | 做法 | 单次耗时 |
|---|---|---|
| `capture_surfaceflinger.cpp` ← 默认 | 直连 `ISurfaceComposer::captureDisplay`，需 AOSP 树 | **8–12 ms**（720p） |
| `capture_screencap.cpp` | fork/exec `/system/bin/screencap`，NDK 即可编 | **~197 ms** |

`memfd` 是匿名内存文件，没有文件系统实体，可以安全地跨进程传递 fd。

**未采用的优化**：直接导出 `GraphicBuffer` 的 dmabuf fd（省掉一次 memcpy）。代价是客户端需要理解 gralloc 布局，可移植性差，属于后续优化项。

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

开发期的手动模式（`--socket <路径>`，默认 `/data/local/tmp/autod.sock`）保留，用于阶段 1 免 sepolicy 原型；此时文件权限由 `--socket-mode`（默认 0660）决定。init 模式下 `--socket-mode` 无效——init 建的文件我们不去改。

### 3.3 鉴权分层

```
Unix socket：
  第一层：socket 文件权限（init 模式 0660 system:system；手动模式由 --socket-mode 决定）
  第二层：SO_PEERCRED 取对端 uid/pid          → 记录进日志
  第三层：（待实现）uid 白名单校验

HTTP / WebSocket：
  第一层：绑定地址（内置默认 127.0.0.1，要对外必须显式写 0.0.0.0）
  第二层：可选 Bearer 令牌（配置 auth=1；三种携带方式见 api/04-config.md）
```

`socket_server.cpp` 里 `ServeConnection()` 已经取到 `SO_PEERCRED` 并打日志，鉴权钩子留在那里。

> ⚠️ **首启默认无鉴权。** 在无鉴权且 bind 被改成 `0.0.0.0` 的情况下，同一网络里任何人都能看屏幕、点屏幕、按键、读剪贴板、装应用。这个不对称是有意的：让"对外"成为一个需要主动做的决定。

**生产环境建议**：socket 侧建一个专用 AID（如 `AID_AUTOD`），需要调用的客户端进程加入该组；或校验 `cred.uid` 是否在允许列表内。HTTP 侧开启 `auth=1`。

---

## 4. 协议设计

### 4.1 传输层选择

| 选择 | 理由 |
|---|---|
| `AF_UNIX` | 本机通信，不走网络栈，延迟最低 |
| `SOCK_SEQPACKET` | 保留消息边界，不用自己处理粘包/拆包 |
| `SCM_RIGHTS` | 传 fd 的唯一方式（截图帧、APK 安装） |

**为什么另外还要 HTTP/WebSocket**：`SCM_RIGHTS` 和 `SOCK_SEQPACKET` 都过不了 `adb forward`（TCP 通道），所以 Unix socket 只有设备上的进程能用。跨机器访问走 HTTP/JSON 与 WebSocket——代价是丢掉了 fd 传递，大块数据（截图）改成 HTTP 响应体直接承载。两条路都进同一个 `Dispatcher`，能力一致。

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

当前协议 **v7**，共 **33 条命令**，完整清单见 [`api/03-socket.md`](api/03-socket.md)。运行中的服务也能自己报出来：

```bash
curl -s http://host:8088/api/v1/describe | jq '.protocolVersion, (.commands|length)'
```

每条命令带 `since` 字段标明它从哪个版本开始存在：

| 版本 | 新增 |
|---|---|
| v1 | 截图、基础触控、信息 |
| v2 | 应用管理、下载、文件操作 |
| v3 | 服务自身：配置、自检、统计、日志、生命周期 |
| v4 | 长按/拖拽/双击、按键注入、剪贴板 |
| v5 | 设备电源（关机/重启） |
| v6 | 服务软开关、运行中应用、历史日志、日志流 |
| v7 | 屏幕方向 `Rotate`（设备转不动时退到 `wm size`，见 `api/01-http.md`） |

**为什么命令集长这样**：必须保留消息边界与定长请求头，所以命令粒度按"操作"划分而不是按"界面"划分——长按、拖拽、双击各给一条命令，而不是让调用方自己拼 `Touch*` 序列（自己拼几乎一定会踩时序的坑，见 `03-reference.md` 第三部分）。

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
   ScreenshotClient::captureDisplay(args, listener)   ← Android 12 的签名
        │
⑤ SurfaceFlinger 做一次 GPU 合成到捕获缓冲         ← 主要耗时（8–20 ms）
        │
⑥ listener->waitForResults()  → ScreenCaptureResults
   检查 result.result != OK（Android 12 用的是 status_t）
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

> 上面是 **SF 直连后端**的数据流（Android 12 的调用形态）。`capture_screencap.cpp` 后端把 ③–⑨ 换成一次 `fork/exec /system/bin/screencap` + 读它的 stdout，其余（memfd 落盘、`SCM_RIGHTS` 传输、客户端 `mmap`）完全相同——代价是慢一个数量级（~197ms vs 8–12ms），收益是 NDK 就能编、任何 root 设备都能跑。

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
  ├─ ProcessState::setThreadPoolMaxThreadCount(0) + startThreadPool()
  │                                       ← 截图必需，且必须在任何抓帧之前
  ├─ 解析命令行 + 读 /sdcard/autod.conf（优先级：CLI > 配置文件 > 内置默认）
  ├─ Capture::Init()                      → 连接 SurfaceFlinger，解析显示
  ├─ Injector::Init()                     → 打开 /dev/uinput，注册虚拟设备
  ├─ SocketServer::Start()                → 接管 init 传来的 fd（或自己 bind）
  ├─ 降权（--uid/--gid，可选）             ← 必须在上面这些 fd 都拿到之后
  ├─ signal(SIGTERM / SIGINT / SIGPIPE)
  ├─ HttpServer::Start()                  → 独立 pthread 跑 HTTP + WebSocket
  └─ SocketServer::Run(handler)           ← 主线程阻塞在 accept
        │
        └─ 每个连接：ServeConnection() → Dispatcher::Handle()
```

**并发模型**：两条传输各跑各的（HTTP 在独立线程，避免互相排队），但**操作是串行的**——锁在 `Dispatcher::Handle()` 里。这是刻意的：`Injector` 是有状态的（按下/抬起、槽位映射、手势 downTime），并发注入会互相破坏手势。代价是一次抓帧（~8–12ms）会让同时在跑的触控事件排队最多十几毫秒。

**退出与重启**：`Shutdown` 让两条传输的 accept 立刻返回并正常退出（退出码 0）；`Restart` 以**退出码 1** 退出，由 init 的 `oneshot` + 外部监督脚本据此重新拉起。

---

## 7. 部署形态

| 层 | 组成 | 说明 |
|---|---|---|
| 服务本体 | `/system/bin/autod`（init 拉起）或 `/data/local/tmp/autod`（root 手动） | 同一个二进制，靠 `--init-socket` / `--socket` 区分 |
| 配置 | `/sdcard/autod.conf`（key=value 纯文本） | 上位应用与守护进程都能读写；`enabled` 每 2s 热读 |
| supervisor | `tools/autod-supervisord.sh`（root 常驻） | 按配置**保活**，状态写 `/sdcard/autod.status` |
| 上位应用 | `dev/05-controller-app/` | **只做服务管理**：启停、改端口、开关鉴权 |
| 运行身份 | init 模式 `user system` / `group system uhid graphics` | ⚠️ **待核实**：`captureDisplay` 的权限检查比的是 **UID** 而非 GID，"在 graphics 组里"并不满足；若抓帧报 `PERMISSION_DENIED`，按 `autod.rc` 的备选方案改用 `user shell`。`autod --selftest` 的 [2] 会直接报出来 |

上位应用是普通 Android 应用（**没有 root**），起不了 root 守护进程，但能写共享存储——所以它只写配置文件，真正的进程管理交给 supervisor；应用侧因此只依赖"文件能写"这一件事。网页控制台里已有的能力（画面、触控、应用管理）应用不再重复实现，避免两套 UI 行为不一致。

> **`enabled=0` 是软开关，不停进程**：进程停掉就没人能把它开回来了（网页打不开、接口不通，只能跑到机器跟前）。软开关只让 `/api/` 返回 `503`，同时始终放行"重新开启"这一条。详见 [`api/04-config.md`](api/04-config.md)。

---

## 8. 相关文档

- 方案选型理由与硬约束 → [`01-selection.md`](01-selection.md)
- 版本 API 差异 / 触控能力矩阵 → [`03-reference.md`](03-reference.md)
- 抓帧与编码性能 → [`06-capture-performance.md`](06-capture-performance.md)
- 接口的权威说明 → [`api/README.md`](api/README.md)
- 设计与踩坑记录 → `05-design-notes.md`
- 代码实现 → `../dev/02-native-daemon/`
- 部署与集成脚本 → `../tools/`
