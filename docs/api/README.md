# autod API 文档

> Android 系统级截图 + 触控 + 设备管理服务。
> 本文档是**唯一权威**的接口说明 —— 代码里的 `/api/v1/describe` 是它的机器可读版本。

---

## 目录

| 文档 | 内容 |
|---|---|
| [01-http.md](01-http.md) | **HTTP/JSON API** —— 全部 29 个端点，逐个的参数与响应 |
| [02-websocket.md](02-websocket.md) | **WebSocket 流** —— 画面流、触控流、日志流 |
| [03-socket.md](03-socket.md) | **Unix socket 二进制协议** —— 32 条命令的线格式 |
| [04-config.md](04-config.md) | **配置与部署** —— 配置文件、命令行、supervisor、鉴权 |
| [05-errors.md](05-errors.md) | **状态码与错误处理** |

---

## 三条传输，一套实现

同一个 `Dispatcher` 处理所有请求，三种传输只是不同的"外壳"：

| 传输 | 地址 | 适合 | 不适合 |
|---|---|---|---|
| **HTTP/JSON** | `http://host:8088/api/v1/...` | 任何语言、任何工具（curl、浏览器、脚本） | 高频事件（每次都要 TCP 往返 + HTTP 解析） |
| **WebSocket** | `ws://host:8088/api/v1/{stream,touch,logstream}` | 实时双向：画面、触控、日志 | 一次性调用 |
| **Unix socket** | `/data/local/tmp/autod.sock`（SEQPACKET） | 本机、零拷贝传大块数据（截图/APK） | 跨机器 |

**能力完全一致**：HTTP 能做的 Unix socket 都能做，反之亦然。
两条路都走同一个 `Dispatcher`，不存在"某个功能只有一种传输支持"。

> ⚠️ 操作锁在 `Dispatcher::Handle` 里 —— 也就是说**两条传输的操作是串行的**。
> 这是刻意的：`Injector` 是有状态的（按下/抬起、槽位映射、手势 downTime），
> 并发注入会互相破坏手势。代价是一次抓帧（~23ms）会让同时在跑的触控事件
> 排队最多 23ms。

---

## 5 分钟上手

### 1. 确认服务在跑

```bash
curl http://<设备IP>:8088/api/v1/describe
```

返回 32 条命令的清单、各自的可用性，以及整体 `capabilities`。
**这是"这台设备上到底能做什么"的唯一权威答案** —— 不要假设，问它。

### 2. 截图

```bash
curl -o screen.png http://<设备IP>:8088/api/v1/capture
```

默认返回 **JPEG**（小、快）；要无损加 `?format=png`，要原始像素加 `?format=raw`。

### 3. 点一下

```bash
curl -X POST http://<设备IP>:8088/api/v1/tap \
     -H 'Content-Type: application/json' \
     -d '{"x":540,"y":1200}'
```

### 4. 打开网页控制台

浏览器打开 `http://<设备IP>:8088/` —— 实时画面、点击/长按/拖拽、
按键、剪贴板、应用管理、日志，都在那里。**不用装任何东西。**

### 5. 看能调什么

```bash
curl http://<设备IP>:8088/api/v1/params
```

画面流的画质、帧率、停检开关，以及运行时能通过 WebSocket 改哪些。

---

## 坐标约定

**所有坐标都是屏幕像素，原点在左上角。**

```
(0,0) ────────────► x
  │
  │      屏幕
  ▼
  y
```

- 分辨率从 `GET /api/v1/config` 的 `runtime.capture.primaryWidth/Height` 读
- 超出范围的坐标会被设备侧钳制或拒绝，**不会**自动缩放
- 网页控制台的画面会被 CSS 缩放，所以它按**渲染后的显示尺寸**换算 ——
  自己写客户端时如果也缩放了图片，别忘了这一步

---

## 快速决策表

| 我想…… | 用哪个 |
|---|---|
| 截一张图 | `GET /capture` |
| 看实时画面 | `ws://.../stream`（或 `<img src=".../capture">` 轮询） |
| 点一下 / 滑一下 | `POST /tap` / `POST /swipe` |
| 拖拽（要跟手） | `ws://.../touch` —— **别用一连串 POST** |
| 输入文字 | `POST /key`（逐个键码），或配合输入法 |
| 装应用 | `POST /install`（请求体 = APK 字节） |
| 看日志 | `ws://.../logstream`（实时）或 `GET /logfile`（历史） |
| 看有哪些应用 | `GET /apps`（已安装）/ `GET /running`（正在运行） |
| 关掉服务对外能力 | `POST /service {"on":false}` |
| 关设备 / 重启设备 | `POST /power {"action":"shutdown"}` |

---

## 鉴权

**默认无鉴权**（首启就是这个状态）。开启后，`/api/` 下的一切都要带令牌：

```bash
curl -H "Authorization: Bearer <令牌>" http://host:8088/api/v1/config
```

三种携带方式、令牌从哪来、怎么开关 —— 见 [04-config.md](04-config.md)。

⚠️ **无鉴权模式下，同一网络里任何人都能完全控制这台设备**：
看屏幕、点屏幕、按键、读剪贴板、装应用、删文件。

---

## 服务开关（和你想的可能不一样）

`POST /api/v1/service {"on":false}` **不会停进程**，只是不再对外提供服务。

- 进程继续运行（pid 不变）
- 所有 `/api/` 返回 `503`
- **网页本身和开关接口仍然放行** —— 否则你就把自己锁在门外了

真停进程需要杀 PID 或改 supervisor；那是另一个层面的操作。

---

## 支持的 Android 版本

**Android 11（API 30）及以上。** 这是实测出来的硬下限：

```bash
API=30 bash tools/build-ndk.sh   # ✓
API=29 bash tools/build-ndk.sh   # ✗ memfd_create / AndroidBitmap_compress
```

两个 API 都是 `__INTRODUCED_IN(30)`，API 28/26 报同样两条。

低于 Android 11 的设备**没有实测过**；理论上能压到 Android 8/9
（两处改动，见 [`docs/03-reference.md`](../03-reference.md) 的
「最低支持的 Android 版本」），但现在没做。

> 这与**协议版本**是两件事，别混：协议 v6 说的是接口形态，
> API 30 说的是能跑在哪些设备上。

---

## 协议版本

当前协议 **v6**，32 条命令。每条命令带 `since` 字段标明它从哪个版本开始存在：

```bash
curl -s http://host:8088/api/v1/describe | jq '.protocolVersion, (.commands|length)'
```

| 版本 | 新增 |
|---|---|
| v1 | 截图、基础触控、信息 |
| v2 | 应用管理、下载、文件操作 |
| v3 | 服务自身：配置、自检、统计、日志、生命周期 |
| v4 | 长按/拖拽/双击、按键注入、剪贴板 |
| v5 | 设备电源（关机/重启） |
| v6 | 服务软开关、运行中应用、历史日志、日志流 |

---

- [06-debugging.md](06-debugging.md) —— 环境变量、强制回退路径、验证方法
