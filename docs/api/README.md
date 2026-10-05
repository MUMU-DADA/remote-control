# remote-control API 文档

> Android 系统级截图 + 触控 + 设备管理服务。
> 本文档是**唯一权威**的接口说明 —— 代码里的 `/api/v1/describe` 是它的机器可读版本。

---

## 目录

| 文档 | 内容 |
|---|---|
| [01-http.md](01-http.md) | **HTTP/JSON API** —— 全部端点，逐个的参数与响应 |
| [02-websocket.md](02-websocket.md) | **WebSocket 流** —— 画面流、触控流、日志流 |
| [03-socket.md](03-socket.md) | **Unix socket 二进制协议** —— 33 条命令的线格式 |
| [04-config.md](04-config.md) | **配置与部署** —— 配置文件、命令行、supervisor、鉴权 |
| [05-errors.md](05-errors.md) | **状态码与错误处理** |

---

## 三种接入方式，共享操作命令

HTTP/JSON 和 Unix socket 的设备操作、查询命令共享 `Dispatcher`；HTTP 还提供实时流、网页和部分管理/上传路由。因此两种 API 有大量共用能力，但并不完全相同：

| 传输 | 地址 | 适合 | 不适合 |
|---|---|---|---|
| **HTTP/JSON** | `http://host:8088/api/v1/...` | 任何语言、任何工具（curl、浏览器、脚本） | 高频事件（每次都要 TCP 往返 + HTTP 解析） |
| **WebSocket（HTTP Upgrade）** | `ws://host:8088/api/v1/{stream,touch,logstream}?token=...` | 实时画面、触控、日志 | 一次性调用；它运行在 HTTP 监听器上 |
| **Unix socket** | `/data/local/tmp/remote-control.sock`（SEQPACKET） | 本机；用 `SCM_RIGHTS` 传递截图、APK 等 fd | 跨机器；HTTP 专属路由和 WebSocket 流 |

`/stream`、`/touch`、`/logstream` 是 WebSocket 路由；只有 `/stream` 另支持无 Upgrade 的 MJPEG，`/touch` 和 `/logstream` 仅支持 WebSocket。ADB 管理、网页和 `/files/upload` 也属于 HTTP 侧扩展；其余是否有对应 socket 命令请以 [03-socket.md](03-socket.md) 的命令表为准。

> ⚠️ 触控等有状态操作，以及其它非 `Info`/`Capture` 命令，共用 `Dispatcher::Handle` 中的 `opMutex_`，跨 HTTP 和 Unix socket 串行执行，以保护 `Injector` 的手势状态。
> `Info`/`Capture` 在分发前置分支绕过此锁；`Capture` 由抓帧后端自己的锁保护，所以抓帧不会因触控操作而排队。同一后端的抓帧仍会串行执行。

---

## 5 分钟上手

正式实例默认开启鉴权。先按 [04-config.md](04-config.md) 取得令牌，并在下面的 API 请求中带上它：

```bash
TOKEN='<remote-control.conf 中的 token>'
```

### 1. 确认服务在跑

```bash
curl -H "Authorization: Bearer $TOKEN" http://<设备IP>:8088/api/v1/describe
```

返回 33 条命令的清单、各自的可用性，以及整体 `capabilities`。
**这是"这台设备上到底能做什么"的唯一权威答案** —— 不要假设，问它。

### 2. 截图

```bash
curl -H "Authorization: Bearer $TOKEN" -o screen.png \
     http://<设备IP>:8088/api/v1/capture
```

默认返回 **PNG**（单次截图不在乎体积，要无损）。
**注意画面流的默认不是它** —— `auto` 在 `/stream` 里是 **JPEG**
（详见 [01-http.md](01-http.md) 的 `/capture`）。


### 3. 点一下

```bash
curl -X POST http://<设备IP>:8088/api/v1/tap \
     -H "Authorization: Bearer $TOKEN" \
     -H 'Content-Type: application/json' \
     -d '{"x":540,"y":1200}'
```

### 4. 打开网页控制台

浏览器打开 `http://<设备IP>:8088/` —— 实时画面、点击/长按/拖拽、
按键、剪贴板、应用管理、日志，都在那里。**不用装任何东西。**

### 5. 看能调什么

```bash
curl -H "Authorization: Bearer $TOKEN" http://<设备IP>:8088/api/v1/params
```

画面流的画质、帧率、停检开关，以及运行时能通过 WebSocket 改哪些。

---

## 坐标约定

**所有坐标都是 `GET /api/v1/info` 的 `touchWidth/Height` 空间，原点在左上角。**

```
(0,0) ────────────► x
  │
  │      屏幕
  ▼
  y
```

- 坐标空间取 `info.touchWidth/Height`，**不是** `primaryWidth/Height` ——
  两者通常相等，但用 `--touch-range` 手工指定过就会分叉
- 服务端把 `x`/`y` 直接当 uinput 的 ABS 值写下去，**中间不做缩放**；
  超出 `0..touchWidth-1` 由内核钳到边界
- **`POST /rotate` 之后要重新读 `/info`** —— 转屏时注入器会被重建，
  坐标空间跟着显示走
- 网页控制台的画面会被 CSS 缩放，所以它按**渲染后的显示尺寸**换算 ——
  自己写客户端时如果也缩放了图片，别忘了这一步

逐条说明见 [01-http.md](01-http.md) 的「坐标」一节。

---

## 快速决策表

| 我想…… | 用哪个 |
|---|---|
| 截一张图 | `GET /capture` |
| 看实时画面 | `ws://.../stream?token=...`；零 JS 就用 `<img src=".../stream?fps=5&amp;token=...">`（MJPEG） |
| 点一下 / 滑一下 | `POST /tap` / `POST /swipe` |
| 拖拽（要跟手） | `ws://.../touch?token=...` —— **别用一连串 POST** |
| 输入文字 | `POST /key`（逐个键码），或配合输入法 |
| 装应用 | `POST /install`（请求体 = APK 字节） |
| 看日志 | `ws://.../logstream?token=...`（实时）或 `GET /logfile`（历史） |
| 看有哪些应用 | `GET /apps`（已安装）/ `GET /running`（正在运行） |
| 关掉服务对外能力 | `POST /service {"on":false}` |
| 关设备 / 重启设备 | `POST /power {"action":"shutdown"}` |

---

## 鉴权

**正式实例默认开启鉴权**，首次实例化随机生成 token；测试实例可显式关闭。
开启后，`/api/` 下的一切都要带令牌：

```bash
TOKEN='<从设备配置中读取的 token>'
curl -H "Authorization: Bearer $TOKEN" http://host:8088/api/v1/config
```

三种携带方式、令牌从哪来、怎么开关 —— 见 [04-config.md](04-config.md)。

⚠️ **无鉴权模式下，同一网络里任何人都能完全控制这台设备**：
看屏幕、点屏幕、按键、读剪贴板、装应用、删文件。

---

## 服务开关（和你想的可能不一样）

`POST /api/v1/service {"on":false}` **不会停进程**，只是不再对外提供服务。

- 进程继续运行（pid 不变）
- 普通业务 API 返回 `503`
- `/api/v1/service`、`/api/v1/adb`、`/api/v1/power`、`/api` 和 `/api/v1` 索引仍可访问，且仍须通过 HTTP 鉴权
- **网页本身也仍然放行** —— 否则用户够不着开关

真停进程需要杀 PID 或改 supervisor；那是另一个层面的操作。

---

## 支持的 Android 版本

**产品目标和当前设备运行验证是 Android 12（API 31）。** NDK 构建脚本默认也用 API 31；API 26 的编译已通过，但没有在 Android 8 设备上运行验证，不能据此承诺 Android 8 运行支持：

```bash
API=26 ABI=arm64-v8a bash tools/build-ndk.sh   # ✓ 编译验证；不代表 Android 8 运行验证
```

旧版曾因 API 30 才引入的 bionic 符号而无法用更低 API 编译；兼容改造已移除这些编译期依赖。构建与运行验证范围见 [`docs/03-reference.md`](../03-reference.md) 的「Android 版本与验证范围」。

> 这与**协议版本**是两件事，别混：协议 v7 说的是接口形态，
> API 级别说的是能跑在哪些设备上。

---

## 协议版本

当前协议 **v7**，33 条命令。每条命令带 `since` 字段标明它从哪个版本开始存在：

```bash
curl -s -H "Authorization: Bearer $TOKEN" \
  http://host:8088/api/v1/describe | jq '.protocolVersion, (.commands|length)'
```

| 版本 | 新增 |
|---|---|
| v1 | 截图、基础触控、信息 |
| v2 | 应用管理、下载、文件操作 |
| v3 | 服务自身：配置、自检、统计、日志、生命周期 |
| v4 | 长按/拖拽/双击、按键注入、剪贴板 |
| v5 | 设备电源（关机/重启） |
| v6 | 服务软开关、运行中应用、历史日志、日志流 |
| v7 | 屏幕方向 `Rotate`（`0/90/180/270/portrait/landscape/free/status`） |

---

- [06-debugging.md](06-debugging.md) —— 环境变量、强制回退路径、验证方法
