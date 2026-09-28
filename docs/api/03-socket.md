# Unix socket 二进制协议

> 地址：`/data/local/tmp/remote-control.sock`（默认）或 init 创建的 `/dev/socket/remote-control`
> 类型：**`SOCK_SEQPACKET`**，不是 `SOCK_STREAM`

---

## 什么时候用它

| | Unix socket | HTTP |
|---|---|---|
| 跨机器 | ❌ 只能本机 | ✅ |
| 传大块数据（截图/APK） | ✅ **零拷贝**（memfd + `SCM_RIGHTS`） | ⚠️ 要过一次内存 |
| 权限模型 | 文件权限（`--socket-mode`） | 令牌 |
| 用起来 | 得自己按结构体打包 | curl / 任何 HTTP 客户端 |

**能力完全一致** —— 两条路走同一个 `Dispatcher`。

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

服务端回一个 `Reply` 结构体（40 字节），若命令产生了数据，
**同一个 `sendmsg` 里带一个 fd**（通过 `SCM_RIGHTS` 辅助数据）。

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

产生数据的命令（`Capture` / `InstallApp` 的反向 / 下载）通过
`SCM_RIGHTS` 回一个 **memfd**，里面是完整数据。

```c
union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u;
msg.msg_control = u.buf;
msg.msg_controllen = sizeof(u.buf);
// recvmsg 之后从 cmsg 里取 fd，mmap dataSize 字节
```

**没有大小上限** —— 所以截图不必分块，一次给完整帧。

---

## 最小客户端示例

### Python

```python
import socket, struct, array, mmap, os

MAGIC = 0x44545541
CMD_CAPTURE = 2

# ⚠️ 用 struct 显式描述，不要让 Python 猜对齐
REQ = struct.Struct("<IIIIiiiiIff")     # 44 字节
REP = struct.Struct("<IIIIIIIIQ")       # 40 字节（含显式 reserved）

s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.connect("/data/local/tmp/remote-control.sock")

# 发一个截图请求：magic, cmd, flags, pointerId, x, y, x2, y2, ms, pressure, size
req = REQ.pack(MAGIC, CMD_CAPTURE, 0, 0, 0, 0, 0, 0, 0, 0.0, 0.0)

# 收 fd 需要 SCM_RIGHTS
fds = array.array("i")
msg, ancdata, flags, addr = s.sendmsg(
    [req], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, fds)])

data = b"\0" * socket.CMSG_LEN(4)
msg, ancdata, flags, addr = s.recvmsg(REP.size, socket.CMSG_LEN(4))
for level, typ, cdata in ancdata:
    if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
        fds.frombytes(cdata[:len(cdata) - (len(cdata) % fds.itemsize)])

magic, status, cmd, w, h, stride, fmt, _res, size = REP.unpack(msg)
assert magic == MAGIC and status == 0, f"status={status}"

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
| 12 | `LaunchApp` | 2 | `<包名>[,activity]` | 启动 |
| 13 | `KillApp` | 2 | `<包名>` | 强制停止 |
| 14 | `ForegroundApp` | 2 | 无 | 当前前台应用 |
| 15 | `InstallApp` | 2 | 无（**fd = APK**） | 安装 |
| 16 | `Download` | 2 | `<url>[,filename[,subdir]]` | 下载 |
| 17 | `FileOp` | 2 | `<op>[,path[,arg]]` | 文件操作（边界 = 共享存储根，op 含 `roots` 可问出边界；见 [01-http.md](01-http.md) 的 `/files`） |
| 20 | `Describe` | 3 | 无 | 能力清单 |
| 21 | `GetConfig` | 3 | 无 | 配置 + 运行时状态 |
| 22 | `SetConfig` | 3 | `<key>\0<value>` | 热改配置 |
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
- **操作是串行的** —— 锁在 `Dispatcher::Handle` 里。
  `Injector` 是有状态的（按下/抬起、槽位映射、手势 downTime），
  并发注入会互相破坏手势
- 空闲连接会被服务端断开（防止"连上不发数据"占住线程）

---

## 相关

- [01-http.md](01-http.md) —— 同样的能力，HTTP 版本
- [05-errors.md](05-errors.md) —— `status` 的完整取值
