# 10 · HTTP/JSON API

> 📖 **接口的权威说明在 [`docs/api/`](api/README.md)。**
>
> 本文是**设计记录** —— 记录当时的取舍、踩过的坑和实测数据。
> 接口的参数、响应、错误码以 `docs/api/` 为准；两边不一致时，
> 是本文没跟上，不是接口变了。
>
> 也可以直接问服务：`GET /api/v1/describe` 是接口清单的机器可读版本。

---

> 目标：**服务对外提供 API，且 API 能控制服务自身的所有能力。**
>
> Unix socket 是**本机**的 —— 别的机器、别的语言、浏览器都够不着。
> 这一层把同样的能力通过 HTTP 暴露出去。

---

## 启动

```bash
autod --socket /data/local/tmp/autod.sock \
      --http-bind 127.0.0.1 --http-port 8088
```

**默认不启用。** 启用时默认只绑 `127.0.0.1`。

### 关于暴露到网络

这是一个能**截图、注入触控、安装应用、删除文件、关闭服务自身**的接口。
绑到 `0.0.0.0` 等于把设备交给同网段所有人。

所以：**绑非回环地址却不设 token 时，服务会拒绝启动**。

```
拒绝启动：绑定到 0.0.0.0 却不设 token —— 这会把设备控制权交给整个网络。
请用 --http-token 设置令牌，或改回 --http-bind 127.0.0.1
```

与其在文档里写"请不要这样"，不如让它在启动时就失败。

配了 token 之后，请求要带：

```
Authorization: Bearer <token>
```
或
```
X-Autod-Token: <token>
```

---

## 从局域网访问

adb forward 和模拟器都**只监听 127.0.0.1**，局域网里够不着。
`tools/lan-forward.py` 把它们暴露到 `0.0.0.0`：

```bash
bash tools/lan-up.sh            # 一键：起模拟器 + autod + 转发
bash tools/lan-up.sh --status   # 看状态
```

| 地址 | 用途 |
|---|---|
| `http://<本机IP>:8088/` | 网页控制台（实时画面 + 触控 + 按键） |
| `http://<本机IP>:8088/api/v1/...` | HTTP API |
| `adb connect <本机IP>:15555` | 完整 adb（可用 scrcpy） |

**端口映射**（不能直接用同一个端口号，见下）：

```
0.0.0.0:8088   → 127.0.0.1:18088   (adb forward)  → 模拟器:8088
0.0.0.0:15555  → 127.0.0.1:5555    (模拟器自身)   → adb
```

⚠️ `0.0.0.0:8088` 与 `127.0.0.1:8088` 在 Linux 上**会冲突**
（通配地址与具体地址重叠，`SO_REUSEADDR` 也救不了），
所以 adb forward 挪到了 18088。

### 为什么不用真桥接（`-net-tap` + 网桥）

模拟器确实支持 `-net-tap <interface>`，真桥接能让 guest 直接从
物理路由器拿到局域网 IP。但**在远程机器上这么做风险太高**：

把 `ens33` 加进网桥并迁移 IP 的过程中必然有一个"没有 IP"的窗口 ——
SSH 会当场断开，配错了只能到机器跟前救。

端口转发能达到同样的访问效果，且不动网络配置、随时可停。
这台机器本身**已经是桥接的**（`ens33` 拿的是物理 LAN 的
`192.168.0.108/24`，网关 MAC `60:be:b4:04:49:93` 是物理路由器），
所以也不需要改 hypervisor。

### ⚠️ 这些端口没有鉴权

同一局域网内任何设备都能控制模拟器。只在可信网络里用。

要暴露到不可信网络的话，先给 autod 加 `--http-token`
（`autod --http-bind 0.0.0.0 --http-token <令牌>` —— 绑非回环地址
却不设 token 时服务会拒绝启动）。

---

## 从宿主机访问

HTTP 走 TCP，所以**可以 `adb forward`** —— 这正是它相对 Unix socket 的意义：

```bash
adb forward tcp:8088 tcp:8088
curl http://127.0.0.1:8088/api/v1/describe
```

---

## 接口一览

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/` | 索引 |
| GET | `/api/v1/describe` | **能力清单**：24 条命令 + 可用性 + 不可用原因 |
| GET | `/api/v1/config` | 当前配置与运行时状态 |
| POST | `/api/v1/config` | 热改配置，`{"log-level":"debug"}` |
| POST | `/api/v1/selftest` | 环境自检（**有副作用**：会抓帧、建设备） |
| GET | `/api/v1/stats` | 请求数、错误数、各命令计数、运行时长 |
| GET | `/api/v1/log?since=N` | 取日志（增量拉取） |
| POST | `/api/v1/shutdown` | 优雅退出 **autod 自身**（不是设备） |
| POST | `/api/v1/power` | **设备**关机/重启：`{"action":"reboot"\|"shutdown"}` |
| POST | `/api/v1/restart` | 退出并由 init 重启 |

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/info` | 显示参数 |
| GET | `/api/v1/capture` | 截图，**默认返回 PNG** |
| GET | `/api/v1/stream` | **MJPEG 实时流**（见 11 号文档） |
| GET | `/api/v1/capture?format=raw` | 原始 RGBA 像素 |
| **WS** | **`/api/v1/touch`** | **流式触控（推荐）**：`down`/`move`/`up` 三原语 |
| POST | `/api/v1/tap` | `{"x":160,"y":240,"ms":50}`（一次性，或流断开时兜底） |
| POST | `/api/v1/swipe` | `{"x1":..,"y1":..,"x2":..,"y2":..,"ms":250}` |

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/apps?system=1&meta=1` | 应用列表 |
| GET | `/api/v1/apps/{包名}` | 应用详情与清单 |
| POST | `/api/v1/apps/{包名}/launch` | 启动（可选 `{"activity":".."}`） |
| POST | `/api/v1/apps/{包名}/kill` | 强制停止 |
| GET | `/api/v1/foreground` | 当前前台应用 |
| POST | `/api/v1/install?replace=1` | 安装；**请求体就是 APK 字节** |

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/v1/download` | `{"url":..,"filename":..,"subdir":..}` |
| GET | `/api/v1/files?path=` | 列出下载目录 |
| POST | `/api/v1/files` | `{"op":"mkdir","path":"a/b","recursive":true}` |

`op` 取值：`list` / `stat` / `exists` / `mkdir` / `delete` / `rename`

---

## 一个例子：浏览器里看设备屏幕

```bash
curl -o screen.png http://127.0.0.1:8088/api/v1/capture
```

或者在浏览器里直接打开 `http://127.0.0.1:8088/api/v1/capture`。

**PNG 是服务端编的**（`png_encoder`，经 dlopen 的 zlib）。返回原始像素的接口
只有写代码的人能用，"对外提供 api"的意义就少了一半。

响应头带尺寸，方便调用方不解析图片就知道分辨率：

```
Content-Type: image/png
X-Autod-Width: 320
X-Autod-Height: 480
X-Autod-PixelFormat: 1
```

实测：320x480 的帧，原始 614,400 字节 → PNG 22,967 字节（约 27 倍）。

---

## 设计要点

### 同一套实现，两条传输

HTTP 只是个**适配器**（`rest_api.cpp`），真正的操作还是走同一个 `Dispatcher`。

这样不会出现"某个功能只有一种传输支持"，也不会出现两边行为不一致 ——
那是最难查的一类问题。适配层只做格式转换（截图 → PNG、memfd → 响应体）。

操作锁也在**共同的那一层**：HTTP 与 socket 侧共用一把互斥量。因为
`Injector` 是有状态的（按下/抬起、槽位映射、手势的 downTime），
两条传输各自加锁等于没加。

### 不引入 HTTP 库

需要的只是 HTTP/1.1 的一个子集：请求行 + 头 + `Content-Length` 正文。
没有 keep-alive（一个请求一个连接），没有 chunked（遇到明确拒绝）。

`Transfer-Encoding: chunked` 会被 400 拒绝，而不是半懂不懂地解析 ——
对安全敏感的接口，错误的解析比明确的拒绝危险得多。

### 错误信息是服务端的原话

非二进制命令的应答体一律 JSON。即使 `status != OK`，服务端也会把
可读原因写在 JSON 的 `error` 字段里，HTTP 层原样透传。

所以调用方看到的是「设备上没有可用的 libcurl，服务端无法下载」，
而不是一个需要查表的错误码。

HTTP 状态码只是从协议状态**映射**出来的，用于让通用 HTTP 工具能判断成败：

| 协议状态 | HTTP |
|---|---|
| `kOk` | 200 |
| `kErrBadCmd` / `kErrBadArg` / `kErrPayload` | 400 |
| `kErrNotFound` | 404 |
| `kErrPermission` | 403 |
| `kErrUnsupported` | 501 |
| `kErrTimeout` | 504 |
| 其他 | 500 |

---

## 实测记录（Android 12 / x86_64 / userdebug）

```
GET  /api/v1/                     → 索引
GET  /api/v1/describe             → protocolVersion 3，24 条命令，capabilities 全 true
GET  /api/v1/config               → socket/mode/pid/uptime/后端名
POST /api/v1/config               → {"applied":["display","log-level"],"changed":true}
GET  /api/v1/capture              → PNG 22,967 字节，320x480，浏览器可直接看
GET  /api/v1/capture?format=raw   → 614,400 字节（= 320×480×4）
POST /api/v1/tap                  → {"ok":true,"x":160,"y":240}
POST /api/v1/swipe                → {"ok":true}
GET  /api/v1/apps?system=1&meta=1 → 137 个应用
GET  /api/v1/files?path=          → 列出下载目录
POST /api/v1/download             → HTTPS 420,815 字节 → /storage/emulated/0/Download/
GET  /api/v1/stats                → 11 请求 0 错误，含各命令分布
```

`docs/evidence/http-api-capture.png` 是通过 `curl` 拿到的设备截图 ——
里面是上位应用正在运行的画面。

---

## 相关文件

| 路径 | 内容 |
|---|---|
| `daemon/http_server.h/.cpp` | HTTP/1.1 子集实现 |
| `daemon/rest_api.h/.cpp` | REST → 协议命令的适配 |
| `daemon/json_parser.h/.cpp` | 极简 JSON 解析器（请求体用） |
| `daemon/png_encoder.h/.cpp` | PNG 编码（zlib 运行时加载） |
| `daemon/service_state.h/.cpp` | 服务自身状态与热改配置 |
| `docs/09-app-and-file-api.md` | 应用与文件管理的协议细节 |
