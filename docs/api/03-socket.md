# Unix socket 二进制协议

> 地址：手动模式由 `--socket` 指定；当前生产 rc 使用
> `/data/misc/remote-control/remote-control.sock`。`--init-socket` / `/dev/socket/remote-control`
> 仅保留作历史兼容模式。
> 类型：**`SOCK_SEQPACKET`**，不是 `SOCK_STREAM`

---

## 什么时候用它

| | Unix socket | HTTP |
|---|---|---|
| 跨机器 | ❌ 只能本机 | ✅ |
| 大块请求/响应数据 | APK 请求和截图响应可用 fd 传递（`SCM_RIGHTS`） | 上传请求体和截图响应走 HTTP 正文 |
| 权限模型 | 文件权限 + `SO_PEERCRED`（`--socket-mode` / `--socket-peer-uid`） | 令牌 |
| 用起来 | 得自己按结构体打包 | curl / 任何 HTTP 客户端 |

常见设备操作和查询共用 `Dispatcher`，但 HTTP 还提供 `/stream` 的 WebSocket/MJPEG 流、仅限 WebSocket 的 `/touch` 与 `/logstream`，以及 ADB 管理、网页和文件上传等路由；它们没有对应的 socket 命令。

---

## 为什么是 SEQPACKET

`SOCK_STREAM` 是字节流：一次 `read` 可能拿到半个请求，或两个半请求粘在一起。
那就得自己实现长度前缀和缓冲区管理。

`SOCK_SEQPACKET` **保留消息边界**：一次 `send` 对应一次 `recv`，
多读的部分自然就是 payload。整个协议因此只有"读一次结构体 + 读一次 payload"。

---

## 线格式

### 请求

一次 `sendmsg`：**结构体 + payload 拼在一起**。

```c
struct Request {          // 44 字节
    uint32_t magic;       // 偏移  0 —— 必须是 kMagic = 0x44545541（"AUTD"）
    uint32_t cmd;         // 偏移  4 —— 见下面的命令表
    uint32_t flags;       // 偏移  8 —— Flags 位或
    uint32_t pointerId;   // 偏移 12 —— 多点触控槽位
    int32_t  x;           // 偏移 16 —— 主坐标 / 滑动起点
    int32_t  y;           // 偏移 20
    int32_t  x2;          // 偏移 24 —— 滑动终点
    int32_t  y2;          // 偏移 28
    uint32_t durationMs;  // 偏移 32 —— 手势时长，0 = 用默认值
    float    pressure;    // 偏移 36 —— 0.0-1.0，0 = 用默认值
    float    size;        // 偏移 40 —— 接触面积，0 = 用默认值
};
// 紧跟其后：payload（NUL 分隔的 UTF-8 字符串），最多 4096 字节
```

> ⚠️ **布局提醒**：`Request` 里没有 64 位字段，所以是紧凑的 44 字节。
> 但 `Reply` 有 —— 而 `uint64_t` 如果紧跟在一串 `uint32_t` 后面，
> 编译器会**隐式插入 4 字节填充**把它对齐到 8 字节。
> 这就产生了 C++ 侧 40 字节、Python 侧 36 字节的经典错位。
> `Reply` 里的 `reserved` 字段就是**显式**的填充，见下。

**payload 约定**：字段之间用 `\0` 分隔。例如
`LaunchApp` 的 payload 是 `"com.android.settings\0.Settings"`。

### 应答

服务端回一个 `Reply` 结构体（40 字节），并可在同一个 `sendmsg` 中
通过 `SCM_RIGHTS` 附带一个 fd。`Info` 直接使用结构体字段；`Capture`
的 fd 中是图像字节；其他命令的 JSON 成功或错误正文通过 fd 返回，
`dataSize` 是该 JSON 正文的字节数。协议拒绝的无效请求可能只有结构体应答。

```c
struct Reply {            // 40 字节
    uint32_t magic;       // 偏移  0 —— 回填 kMagic
    uint32_t status;      // 偏移  4 —— 0 = 成功，否则见 05-errors
    uint32_t cmd;         // 偏移  8 —— 回显请求的 cmd
    uint32_t width;       // 偏移 12 ┐
    uint32_t height;      // 偏移 16 │ 仅 Capture 有效
    uint32_t stride;      // 偏移 20 │ stride 单位是**像素**，不是字节
    uint32_t format;      // 偏移 24 ┘ Android PixelFormat
    uint32_t reserved;    // 偏移 28 —— 显式填充，不要使用
    uint64_t dataSize;    // 偏移 32 —— memfd 的有效字节数
};
```

`reserved` 存在的原因就写在注释里：保证 `dataSize` 落在 8 字节边界，
让所有语言的实现都不必猜编译器的填充规则。

### fd 传递

`Capture` 的响应 fd 是装有图像数据的 **memfd**。`InstallApp` 则在请求侧
接收一个 fd，内容是 APK；安装结果（成功或失败）以 JSON memfd 应答。
`Download` 在设备上保存文件，响应 fd 里是 JSON 元数据，**不是下载文件本身**。
其余 JSON 命令的响应也经 memfd 返回。

```c
union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u;
msg.msg_control = u.buf;
msg.msg_controllen = sizeof(u.buf);
// recvmsg 之后从 cmsg 里取 fd，mmap dataSize 字节
```

fd 内容不占 `SOCK_SEQPACKET` 消息正文空间，因此截图可作为单个完整文件传递，
无需拆成多个协议包；实际尺寸取决于图像分辨率和编码格式。

请求侧使用 `recvmsg` 接收伴随 fd。服务端每个请求最多保留一个 fd，额外 fd
会立即关闭；短于 `Request` 的包或 ancillary 数据被截断时，请求会被拒绝，
已经收到但未交给处理器的 fd 也会关闭。客户端同样应在用完应答 fd 后及时关闭。

---

## 最小客户端示例

### Python

```python
import socket, struct, array, mmap

MAGIC = 0x44545541
CMD_CAPTURE = 2

# ⚠️ 用 struct 显式描述，不要让 Python 猜对齐
REQ = struct.Struct("<IIIIiiiiIff")     # 44 字节
REP = struct.Struct("<IIIIIIIIQ")       # 40 字节（含显式 reserved）

s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
# 手动/开发服务示例；路径必须与 daemon 的 --socket 参数一致。
s.connect("/data/local/tmp/remote-control.sock")

# 发一个截图请求：magic, cmd, flags, pointerId, x, y, x2, y2, ms, pressure, size
req = REQ.pack(MAGIC, CMD_CAPTURE, 0, 0, 0, 0, 0, 0, 0, 0.0, 0.0)

# Capture 不需要请求 fd；响应会通过 SCM_RIGHTS 返回图像 memfd
fds = array.array("i")
sent = s.sendmsg([req])
assert sent == len(req)

msg, ancdata, flags, addr = s.recvmsg(REP.size, socket.CMSG_SPACE(4))
assert not flags & socket.MSG_CTRUNC
for level, typ, cdata in ancdata:
    if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
        fds.frombytes(cdata[:len(cdata) - (len(cdata) % fds.itemsize)])

magic, status, cmd, w, h, stride, fmt, _res, size = REP.unpack(msg)
assert magic == MAGIC and status == 0, f"status={status}"

assert len(fds) == 1
fd = fds[0]
buf = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ)
# buf 里是 RGBA 像素：w × h，每行 stride 像素
```

### C++

见 `dev/02-native-daemon/client/rcctl.cpp` —— 它是完整的参考实现，
包含 fd 接收、memfd 读取、以及用 `AndroidBitmap_compress` 编码 PNG。

---

## 命令表

33 条命令。`since` 是它从哪个协议版本开始存在。

| cmd | 名称 | since | payload | 说明 |
|---|---|---|---|---|
| 1 | `Info` | 1 | 无 | 显示参数（分辨率、格式） |
| 2 | `Capture` | 1 | 无 | 截图，**回 fd** |
| 3 | `Tap` | 1 | 无（用 x/y/ms） | 单击 |
| 4 | `Swipe` | 1 | 无（用 x/y/x2/y2/ms） | 滑动 |
| 5 | `TouchDown` | 1 | 无（用 pointerId/x/y） | 手动多点触控：按下 |
| 6 | `TouchMove` | 1 | 同上 | 移动 |
| 7 | `TouchUp` | 1 | 同上 | 抬起 |
| 8 | `KeyEvent` | 4 | `<键名或键码>` | 按键注入。键名表与三个映射坑见 [01-http.md](01-http.md) 的 `POST /key` |
| 10 | `ListApps` | 2 | 无（用 flags） | 应用列表 |
| 11 | `AppInfo` | 2 | `<包名>` | 应用详情 |
| 12 | `LaunchApp` | 2 | `<包名>[\0<activity>]` | 启动 |
| 13 | `KillApp` | 2 | `<包名>` | 强制停止 |
| 14 | `ForegroundApp` | 2 | 无 | 当前前台应用 |
| 15 | `InstallApp` | 2 | 无（**fd = APK**） | 安装 |
| 16 | `Download` | 2 | `<url>[\0<filename>[\0<subdir>]]` | 下载 |
| 17 | `FileOp` | 2 | `<op>[\0<path>[\0<arg>]]` | 文件操作（边界 = 共享存储根，op 含 `roots` 可问出边界；见 [01-http.md](01-http.md) 的 `/files`） |
| 20 | `Describe` | 3 | 无 | 能力清单 |
| 21 | `GetConfig` | 3 | 无 | 配置 + 运行时状态 |
| 22 | `SetConfig` | 3 | 重复的 `<key>\0<value>` 对 | 热改配置 |
| 23 | `SelfTest` | 3 | 无 | 环境自检 |
| 24 | `Stats` | 3 | 无 | 请求统计 |
| 25 | `Log` | 3 | `[sinceSeq]` | 取日志 |
| 26 | `Shutdown` | 3 | 无 | 优雅退出服务 |
| 27 | `Restart` | 3 | 无 | 退出并由 supervisor 重启 |
| 30 | `LongPress` | 4 | 无（用 x/y/ms） | 长按 |
| 31 | `Drag` | 4 | 无（用 x/y/x2/y2/ms） | 拖拽 |
| 32 | `DoubleTap` | 4 | 无（用 x/y/ms） | 双击 |
| 33 | `Clipboard` | 4 | `get` \| `set\0<文本>` \| `info` | 剪贴板 |
| 34 | `Power` | 5 | `reboot` \| `shutdown` \| `reboot-*` | 设备电源 |
| 35 | `ServiceSwitch` | 6 | `on` \| `off` \| `status` | 服务对外开关 |
| 36 | `RunningApps` | 6 | 无 | 运行中的进程 |
| 37 | `LogFile` | 6 | 无 | 落盘的历史日志 |
| 38 | `Rotate` | 7 | `0` \| `90` \| `180` \| `270` \| `free` \| `status` | 屏幕方向（设备转不动时退到 wm size，见应答的 method） |

> 编号 9、18、19、28、29 是预留的（历史遗留的间隔），不要复用。

---

## Flags

`Request.flags` 是位或。不同命令用不同的位。

| 位 | 名称 | 用于 | 含义 |
|---|---|---|---|
| 0 | `kFlagRawRGBA` | Capture | 原始 RGBA_8888（默认） |
| 1 | `kFlagPng` | Capture | 服务端编码成 PNG |
| 2 | `kFlagGrayscale` | Capture | 只要灰度，数据量 1/4 |
| 3 | `kFlagIncludeSystem` | ListApps | 含系统应用（默认只列第三方） |
| 4 | `kFlagWithMetadata` | ListApps | 附带版本/安装时间（更慢） |
| 5 | `kFlagReplace` | InstallApp | `-r` 覆盖安装 |
| 6 | `kFlagRecursive` | FileOp | 递归删除目录 |
| 7 | `kFlagKeyLongPress` | KeyEvent | 长按（保持按下更久） |
| 8 | `kFlagAsync` | Tap/Swipe/手势 | 不等待分发完成 |
| 9 | `kFlagForce` | ServiceSwitch | **服务关了也放行这条** |

> ⚠️ 位 8 和位 9 曾经**都写成 `1u << 8`** —— 后果是任何带 `async`
> 的手势都会被当成"强制放行"，而服务关了之后手势本来就该被拒。
> 位是全局命名空间，加新标志前先看一眼这张表。

`kFlagForce` 存在的原因：关掉服务之后，`ServiceSwitch` 本身必须仍然
可达，否则就没有任何入口能把它开回来。

---

## 拒绝的行为

| 情况 | 应答 |
|---|---|
| `magic` 不匹配 | `kErrBadMagic`，且不执行任何操作 |
| 未知 `cmd` | `kErrBadCmd` |
| payload 超 4KB | `kErrPayload` |
| 参数缺失/越界 | `kErrBadArg` |
| **服务已关闭对外能力** | `kErrPermission`（`ServiceSwitch` 除外） |

服务关闭时 socket **也会被拒** —— 不然"关掉服务"只是关掉了 HTTP，
本机进程照样能通过 socket 完整控制设备，软开关就成了摆设。

---

## 超时与并发

- 连接是并发处理的（每连接一个线程）
- 触控等有状态操作及其它非 `Info`/`Capture` 命令共用 `Dispatcher::Handle` 的 `opMutex_`，并在 HTTP 与 Unix socket 请求间串行执行，保护 `Injector` 的按下/抬起、槽位映射和手势 `downTime`
- `Info`/`Capture` 在 Dispatcher 前置分支绕过 `opMutex_`；抓帧使用后端自己的锁，不会因触控排队。同一后端的抓帧仍串行
- 空闲连接会被服务端断开（防止"连上不发数据"占住线程）

---

## 相关

- [01-http.md](01-http.md) —— 对应设备命令的 HTTP/JSON 端点与请求、响应格式
- [05-errors.md](05-errors.md) —— `status` 的完整取值
