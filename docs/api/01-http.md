# HTTP/JSON API 完整参考

> 基础地址：`http://<设备IP>:8088/api/v1`
> 所有响应都是 UTF-8 JSON，除了截图和文件下载（二进制）。

---

## 通用约定

### 响应信封

**成功**：命令自己的字段 + `ok: true`（大部分端点）或直接是数据。
**失败**：统一形状 ——

```json
{"ok": false, "status": 4099, "error": "不认识的键: nonsense。可用: home back …"}
```

`status` 是协议状态码（见 [05-errors.md](05-errors.md)），
`error` 是**服务端的原话**。

> 💡 `error` 字段值得直接显示给用户。它写的是"为什么"，
> 比如「设备上没有可用的 libcurl」或「不认识的键: xxx。可用: …」，
> 而不是一个需要查表的错误码。

### HTTP 状态码

| HTTP | 含义 | 协议状态 |
|---|---|---|
| 200 | 成功 | `kOk` (0) |
| 400 | 参数错 / 未知资源 | `kErrBadCmd` `kErrBadArg` `kErrPayload` |
| 401 | 需要令牌（[04-config.md](04-config.md)） | — |
| 403 | 权限不足 | `kErrPermission` |
| 404 | 找不到 | `kErrNotFound` |
| 500 | 服务端内部错 | 其他 |
| 501 | 这台设备/这个构建不支持 | `kErrUnsupported` |
| 503 | **服务被软开关关掉了** | — |
| 504 | 超时 | `kErrTimeout` |

### 鉴权

默认无鉴权。开启后 `/api/` 下的一切都要带令牌：

```bash
curl -H "Authorization: Bearer <令牌>" ...
curl -H "X-Autod-Token: <令牌>" ...
curl "http://host:8088/api/v1/config?token=<令牌>"      # 给 <img>/WebSocket 用
```

`GET /`（网页本身）**不校验** —— 它只是静态页面，而用户得先打开它
才有地方输入令牌。

### 坐标

屏幕像素，原点左上角。分辨率见 `GET /config` 的
`runtime.capture.primaryWidth/Height`。超出范围会被钳制或拒绝，**不会**自动缩放。

---

## 一、服务信息与自省

### GET /describe

**这台设备上到底能做什么** —— 唯一权威答案。写客户端前先问它，
不要假设某个命令一定可用。

```bash
curl http://host:8088/api/v1/describe
```

```json
{
  "service": "autod",
  "protocolVersion": 6,
  "pid": 21266,
  "capabilities": {
    "screenshot": true, "touch": true, "multiTouch": true,
    "appManagement": true, "download": true, "fileManagement": true,
    "keyInjection": true, "clipboard": true, "screenStream": true,
    "webUi": true, "power": true, "serviceSwitch": true,
    "runningApps": true, "logFile": true, "selfControl": true
  },
  "commands": [
    {"name":"Info","cmd":1,"since":1,"available":true,"params":"无","desc":"…"},
    …共 33 条
  ]
}
```

`available: false` 的命令会带 `reason`，说明为什么不可用（例如
`/dev/uinput` 不可写）。**不要**硬编码命令列表，读这个接口。

### GET /config

当前配置与运行时状态。

```json
{
  "ok": true,
  "protocolVersion": 6,
  "config": {
    "socketPath": "/data/local/tmp/autod.sock",
    "usingInitSocket": false,
    "initSocketName": "",
    "socketMode": "438",
    "displayId": 0,
    "touchWidth": 0,
    "touchHeight": 0,
    "verbose": false,
    "dropUid": -1,
    "dropGid": -1
  },
  "runtime": {
    "pid": 21266, "uid": 0, "gid": 0,
    "uptimeMs": 496569,
    "protocolVersion": 6,
    "verbose": false,
    "capture": {
      "backend": "surfaceflinger",
      "primaryWidth": 320,
      "primaryHeight": 480,
      "displayCount": 1,
      "displays": [ … ]
    },
    "inject": { "backend": "uinput" }
  }
}
```

| 字段 | 说明 |
|---|---|
| `capture.backend` | `surfaceflinger`（快，23ms/帧）或 `screencap(exec …)`（慢，120ms/帧） |
| `config.socketMode` | 十进制。`438` = `0666`，`432` = `0660` |
| `dropUid/dropGid` | -1 表示没有降权 |

### GET /stats

```json
{
  "uptimeMs": 496573,
  "requests": 1811,
  "errors": 0,
  "logDropped": 0,
  "byCommand": {"TouchMove": 1338, "GetConfig": 101, "RunningApps": 326, …}
}
```

`logDropped > 0` 说明日志环形缓冲被冲过，客户端按 `since` 拉可能漏行。

### POST /selftest

在**运行中的进程里**跑一遍环境自检。会真的抓一帧、建一次注入设备。

```json
{"ok": true, "passed": 8, "failed": 0, "checks": [ … ]}
```

首次部署时先跑这个。

### GET /info

显示参数。比 `/config` 轻，只要分辨率、格式、显示数量。

```json
{"ok":true,"primaryWidth":320,"primaryHeight":480,"primaryStride":320,
 "primaryFormat":1,"touchWidth":320,"touchHeight":480}
```

| 字段 | 说明 |
|---|---|
| `primaryWidth/Height` | 屏幕分辨率 |
| `primaryStride` | 每行像素数（可能大于 width） |
| `primaryFormat` | Android PixelFormat：1=RGBA_8888，2=RGBX，5=BGRA |
| `touchWidth/Height` | **触控坐标空间**。客户端发坐标必须按这个空间算 |

> ⚠️ `touchWidth/Height` 是**注入器的实际 ABS 范围**，不一定等于
> `primaryWidth/Height`。
>
> 服务端把请求里的 `x`/`y` **直接当 ABS 值**写下去（`inject_uinput.cpp`
> 里没有缩放），所以客户端必须按这个空间发坐标。而这个范围是
> **设备创建时定死的**（ioctl 改不了），因此它天然是一个稳定的
> 归一化空间 —— 转屏、改分辨率都不影响它，Android 会按比例映射到当前显示。
>
> 两者不一致就会点偏。实测踩过：显示 720 宽而 ABS 范围是 1279，
> 于是点画面正中央只落在 56% 处。

### POST /rotate

旋转设备方向。**有副作用，所以是 POST。**

```bash
curl -X POST http://<设备IP>:8088/api/v1/rotate \
     -H 'Content-Type: application/json' -d '{"to":"90"}'
```

`to` 可取 `0|90|180|270|portrait|landscape|free|status`。
不传等于 `status`。

```json
{
  "ok": true,
  "requested": 90,
  "applied": true,
  "method": "wm-size",
  "rotation": 1,
  "actualRotation": 0,
  "free": false,
  "width": 1280,
  "height": 720,
  "note": "这台设备不支持旋转（mRotation 不变），已改用 wm size 把显示尺寸设成 1280x720，应用会按横屏重新布局"
}
```

| 字段 | 说明 |
|---|---|
| `applied` | **真的转过去了吗**。false 时看 `note` |
| `method` | `user-rotation`（正规入口）/ `wm-size`（退路）/ `none` |
| `rotation` | 方向**设置项**。写什么读出来就是什么 |
| `actualRotation` | 系统**实际**的 `mRotation`（0-3）。跟 `rotation` 不一致 = 这台设备转不动 |
| `width/height` | 切完之后实际的显示尺寸 |

> ⚠️ **两条路不等价，所以要走哪条是运行时决定的。**
>
> 正规入口是 `cmd window user-rotation lock N`。但有些 ROM 根本没有
> 旋转支持 —— 实测自编的 x86_64 ROM 上命令返回成功、设置项也写进去了，
> 而 `mRotation` 死活不动。
>
> 那种设备上退到 `cmd window size WxH` 交换宽高：应用照样会按横屏
> 重新布局（已实测），但 `mRotation` 不变，**180° 也表达不出来**
> （`wm size` 只能交换宽高，说不出上下颠倒）—— 那时 `applied` 是 false。
>
> 所以判定用的是 `actualRotation` **加上**几何是否也对，而不是设置项：
> 只判方向的话，在不支持旋转的设备上转 0° 时 `mRotation` 本来就是 0，
> 会被误判成成功，而上一轮退路设的横屏尺寸**永远不会被还原**。

> ⚠️ 这个端点在 v7 之前是坏的：它直接转发 `Cmd::Info`，
> 而那条命令把结果填在 `Reply` **结构体的字段**里（socket 协议的表达
> 方式），HTTP 层只看 JSON 正文 —— 于是返回 `{"ok":true,"status":0,
> "error":"ok"}`，看着成功，一个有用字段都没有。

写客户端时**先调这个拿分辨率**，再据此算坐标 —— 不要假设屏幕尺寸。

### GET /params

画面流的可调参数 + **当前抓帧节奏**。**别去翻文档猜默认值，问它。**

```json
{
  "ok": true,
  "endpoint": "/api/v1/stream",
  "transports": "WebSocket（带 Upgrade 头）/ MJPEG（不带）",
  "defaultFps": 5,
  "defaultMaxWidth": 720,
  "defaultSkipUnchanged": true,
  "defaultFormat": "jpeg",
  "nativeCodecs": true,

  "capture": {
    "activeFps": 30,
    "subscribers": 1,
    "frames": 4419,
    "lastCaptureMs": 10,
    "captureWidth": 720,
    "served": 5707,
    "misses": 0,
    "running": true,
    "subscriberList": [
      {"id":1, "fps":30, "maxWidth":480, "ageMs":4002,
       "peer":"192.168.0.108:49366", "format":"webp",
       "transport":"mjpeg", "isMaxFps":true}
    ]
  },

  "quality": {
    "jpeg": {"min":1, "max":100, "default":75},
    "webp": {"min":1, "max":100, "default":80},
    "png":  {"min":1, "max":9,   "default":1,
             "note":"zlib 压缩级别，不是图像质量"},
    "h264": {"min":1, "max":100, "default":75,
             "note":"换算成码率，不是图像质量"}
  },

  "codecs": {"png":true, "jpeg":true, "webp":true, "raw":true,
             "h264":true, "h264Max":2, "h264Used":0,
             "backend":"AndroidBitmap_compress", "forced":false},

  "params": [ … ],
  "wsCommands": [ … ]
}
```

#### `capture` —— 服务端**实际**在按什么节奏抓

抓帧线程按**所有订阅者的最高需求**跑，所以客户端只知道自己要了多少帧，
不知道设备实际被拉到了多快 —— 那会让「我明明只要 5fps，为什么设备这么烫」
变成一个查不出来的问题。这里如实报出来。

| 字段 | 说明 |
|---|---|
| `activeFps` | 当前抓帧节奏。**0 = 没有任何订阅者，一次都没在抓** |
| `subscribers` | 订阅者数量 |
| `frames` | 累计抓帧次数（不管有没有人收） |
| `lastCaptureMs` | 最近一次抓帧耗时 |
| `captureWidth` | 当前按多少宽抓。0 = 原始分辨率 |
| `served` | 取帧时「最新帧已备好」的次数 |
| `misses` | 没等到新帧的次数。**这个高 = 抓帧跟不上需求，客户端在等** |
| `running` | 抓帧线程活着吗 |
| `changeGen` | 当前是「第几代**不同**的画面」。内容变了才 +1 |
| `unchanged` | 与上一帧完全相同而**省下**的帧数 |
| `subscriberList` | 每个订阅者的明细，见下 |

`unchanged / frames` 高 = 画面基本静止，停检正在起作用。
静止画面上这个比例能到 99% —— 也就是说绝大多数抓帧都是白抓的
（这是「按时抓帧换低延迟」的固有代价，见 `docs/06-capture-performance.md`）。

`subscriberList` 的每一项：

| 字段 | 说明 |
|---|---|
| `id` | 订阅序号，递增 |
| `fps` | 这个客户端要的帧率 |
| `maxWidth` | 这个客户端要的降采样宽度（0 = 原始） |
| `ageMs` | 这个订阅挂了多久 |
| `peer` | 客户端地址 `ip:port`。socket 那条传输为空串 |
| `format` | 它拉的格式 |
| `transport` | `ws` 或 `mjpeg` |
| `isMaxFps` | **是不是它把抓帧节奏顶上来的** |

> `isMaxFps` 是排查时唯一真正要看的那条。抓帧节奏由最高需求决定，
> 所以只要有一个客户端挂着 60fps，整个进程就一直在满速抓 ——
> 没有这个字段的话，你只能挨个关客户端去试。
>
> 控制台（`GET /`）的状态面板每 2 秒把这一段显示成一行：
>
> ```
> 抓帧  服务端 60fps · 2 个订阅 · 10ms · 宽 720  ⚠ 高于本页 10fps
> ```
>
> 服务端节奏高于本页需求时标黄 —— 那说明有别的客户端在拉。

#### `quality` —— 每种格式的取值范围

各格式量纲完全不同，**界面上的拖动条必须按当前格式取这个范围**，
不能写死。写死了就会出现「拖到 75，但 PNG 只认 1-9」。

#### `codecs` —— 这台设备到底能编什么

| 字段 | 说明 |
|---|---|
| `png/jpeg/webp/raw` | 各格式可用吗 |
| `h264` / `h264Max` / `h264Used` | H.264 可用性、并发上限、当前占用 |
| `backend` | `AndroidBitmap_compress`（快）或 `libjpeg/libpng`（回退） |
| `forced` | 是否被 `AUTOD_IMAGE_BACKEND` 强制指定 |

`nativeCodecs: false` 表示这台设备只有 PNG（没有 JPEG/WebP）。


---

## 二、服务开关

### GET /service

```json
{"ok": true, "serving": true, "note": "服务对外可用"}
```

### POST /service

```bash
curl -X POST http://host:8088/api/v1/service \
     -H 'Content-Type: application/json' -d '{"on":false}'
```

也接受 `{"action":"on"|"off"|"status"}`。

**这是软开关 —— 不会停进程。**

| | 关闭后 |
|---|---|
| 进程 | **继续运行，pid 不变** |
| `/api/` 下的一切 | `503` |
| `GET /`（网页本身） | **200**（否则用户够不着开关） |
| 本接口 | **200**（重新开启的入口） |

做真停进程的话就没人能开回来了 —— 网页打不开、接口不通，
只能跑到机器跟前救。状态持久化到配置文件，重启后保持。

---

## 三、截图与画面流

### GET /capture · POST /capture

| 参数 | 默认 | 说明 |
|---|---|---|
| `format` | `auto` | `auto`\|`png`\|`jpeg`\|`webp`\|`raw` |
| `quality` | 见下 | PNG 1-9（zlib 级别）/ JPEG·WebP 1-100 |

**`auto` 在单次截图时是 PNG**（无损，一次调用不在乎大小），
**在画面流里是 JPEG**（一路视频，带宽和编码耗时都重要）。
两个默认值不同是**有意**的。

```bash
curl -o s.png  'http://host:8088/api/v1/capture'                # PNG 86 KB
curl -o s.jpg  'http://host:8088/api/v1/capture?format=jpeg'    # JPEG 17 KB
curl -o s.raw  'http://host:8088/api/v1/capture?format=raw'     # 614400 B
curl -o s.webp 'http://host:8088/api/v1/capture?format=webp&quality=90'
```

未知格式返回 400，**不会**静默给你 PNG。

响应头：

| 头 | 含义 |
|---|---|
| `Content-Type` | `image/png` / `image/jpeg` / `image/webp` / `application/octet-stream` |
| `X-Autod-Width` / `X-Autod-Height` | 尺寸（不用解析图片就能拿到） |
| `X-Autod-PixelFormat` | Android PixelFormat（1=RGBA_8888，2=RGBX，5=BGRA） |
| `X-Autod-Stride` | 仅 `raw`：每行像素数（可能大于 width） |

`raw` 模式下：`614400 = 320 × 480 × 4`，字节序是 **RGBA**（BGRA 已在服务端转好）。

### GET /stream

**两条传输，同一个端点**：

| | MJPEG | WebSocket |
|---|---|---|
| 触发 | 不带 `Upgrade` 头 | 带 `Upgrade: websocket` |
| 客户端 | `<img src=".../stream?fps=5">`，零 JS | canvas + `createImageBitmap` |
| 控制 | 无 | **能反过来改参数，不用重连** |

完整说明见 [02-websocket.md](02-websocket.md)。

---

## 四、触控

### POST /tap

```json
{"x": 540, "y": 1200, "ms": 50}
```

`ms` 可选，默认 50。返回 `{"ok":true,"x":540,"y":1200}`。

### POST /longpress

```json
{"x": 540, "y": 1200, "ms": 800}
```

`ms` 默认 800（Android 的 longPressTimeout 约 500ms，留了余量）。

> ⚠️ 长按期间**一个 MOVE 都不能发**，否则系统判成拖拽，长按菜单不弹。
> 这就是为什么它是一个单独的接口，而不是"tap 加长 ms"。

### POST /drag

```json
{"x1": 100, "y1": 1600, "x2": 800, "y2": 400, "ms": 600}
```

与 `swipe` 的区别：**起点先停顿 120ms，再慢速移动** ——
这样才会被识别成"按住拖动"而不是"甩一下"。

坐标字段两种写法都收：`x/y/x2/y2` 或 `x1/y1/x2/y2`。

### POST /doubletap

```json
{"x": 540, "y": 1200, "ms": 120}
```

`ms` 是两次点击的间隔，默认 120（系统双击阈值约 300ms）。

### POST /swipe

```json
{"x1": 540, "y1": 1600, "x2": 540, "y2": 400, "ms": 300}
```

### POST /touch（WebSocket）

流式触控。`down` / `move` / `up` 三个原语 ——
**拖拽跟手必须用它，别用一连串 POST**。

```js
const ws = new WebSocket('ws://host:8088/api/v1/touch');
ws.send(JSON.stringify({t:'down', x:100, y:200, id:0}));
ws.send(JSON.stringify({t:'move', x:105, y:205, id:0}));   // 高频
ws.send(JSON.stringify({t:'up',   x:110, y:210, id:0}));
```

实测：单事件往返 **1.1ms**（HTTP POST 是 13.8ms）。
完整说明见 [02-websocket.md](02-websocket.md)。

### POST /key

```json
{"key": "home"}
{"key": "172"}                    // Linux 键码
{"key": "power", "long": true}    // 长按
```

键名接受三种写法：`home`（助记名）/ `KEY_HOME`（内核文档写法）/ `172`（数字键码）。
单字符 `a`-`z` `0`-`9` 直接当字符键，`f1`-`f12` 也支持。

可用键名列表在错误信息里返回：

```
{"ok":false,"status":4099,"error":"不认识的键: xxx。可用: home back menu
 appswitch search power volumeup volumedown mute enter delete backspace
 space tab escape up down left right center playpause nextsong
 previoussong stop camera a-z 0-9 f1-f12，或直接给数字键码"}
```

**实现走 `/dev/uinput` 虚拟键盘** —— Android 12 没有可用的 native 按键注入接口。

---

## 五、剪贴板

### GET /clipboard?op=get · POST /clipboard

| op | 说明 | 响应 |
|---|---|---|
| `get` | 读内容 | `{"ok":true,"has":true,"text":"…"}` |
| `info` | 只问有没有 | `{"ok":true,"has":false}` |
| `set` | 写入（POST） | `{"ok":true,"length":34}` |

```bash
curl 'http://host:8088/api/v1/clipboard?op=get'
curl -X POST http://host:8088/api/v1/clipboard \
     -H 'Content-Type: application/json' \
     -d '{"op":"set","text":"hello"}'
```

空剪贴板返回 `has:false`，**不是错误**（HTTP 200）。

> ⚠️ 写入会**回读确认**。`ClipboardService` 在权限不足时是静默 `return`
> 不抛异常的 —— 不确认就会报"成功"而实际没写进去。

---

## 六、应用管理

### GET /apps

| 参数 | 默认 | 说明 |
|---|---|---|
| `system` | 0 | 1 = 含系统应用 |
| `meta` | 0 | 1 = 带 apkPath / versionCode / installer |

```json
{"ok":true, "count":137, "includeSystem":true, "withMetadata":true,
 "apps":[{"package":"com.android.settings","apkPath":"/system_ext/…",
          "installer":"null","versionCode":31,"system":true}]}
```

### GET /apps/{包名}

应用详情：版本、UID、APK 路径、数据目录、签名摘要、权限、
以及 activities / services / receivers / providers 清单。

```bash
curl http://host:8088/api/v1/apps/com.android.settings
```

包名不存在返回 404。

### POST /apps/{包名}/launch

```json
{}                                    // 启动默认 Activity
{"activity": ".Settings$ApnEditorActivity"}
```

```json
{"ok":true,"package":"com.android.settings",
 "component":"com.android.settings/.Settings"}
```

### POST /apps/{包名}/kill

强制停止。

```json
{"ok":true,"package":"com.android.settings","stillRunning":false}
```

> ⚠️ 对**不存在**的包也返回 `ok:true`（幂等：它确实没在跑）。
> `stillRunning` 才是真实结果。

### GET /foreground

```json
{"ok":true,"package":"com.android.settings"}
```

### GET /running

**正在运行的进程**，带优先级状态和系统应用标记。

```json
{"ok":true, "count":24,
 "apps":[{"package":"com.autod.controller",
          "process":"com.autod.controller",
          "pid":4006, "uid":"u0a105", "state":"fg", "system":false}]}
```

| 字段 | 说明 |
|---|---|
| `process` | 完整进程名。`com.android.webview:webview_service` 这种是子进程 |
| `package` | 冒号前那段（结束进程时用的是包名） |
| `state` | adj：`fg` 前台 / `vis` 可见 / `prcp` 常驻 / `psvc` 持久服务 / `cch` 缓存 |
| `system` | 是否系统应用 —— **三条判据合起来判的**，见下 |

**系统判定的三条判据**（任何单独一条都有盲区）：

1. 包名在 `pm list packages -s` 里 —— 权威，但覆盖不到没有对应包的进程
   （`system_server` 的进程名就叫 `system`）
2. uid 不是 `u0aNN` 形式（纯数字如 `1000`）—— 可靠，但 launcher3 这类
   预装应用用的是 app uid（`u0a90`），会漏
3. 进程名含 `.process.` → 取前面那段当包名再查一次
   （`android.process.acore` → `android`）

数据源是 `dumpsys activity lru`，不是 `ps` —— ps 给的是 UID 不是包名，
而且要额外查 PackageManager 才能对上。

### POST /install

```bash
# 上传 APK（请求体 = 文件字节）
curl -X POST --data-binary @app.apk \
     'http://host:8088/api/v1/install?replace=1'

# 安装设备上已有的文件
curl -X POST 'http://host:8088/api/v1/install?path=/sdcard/app.apk'
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `replace` | 1 | 0 = 不覆盖已装的同名应用 |
| `path` | — | 指定设备上的文件，此时请求体应为空 |
| `keep` | 0 | 1 = **不删除**文件（调试用） |

**文件在安装完成或失败后都会删除。**

原因：不删的话每次安装都在 `/sdcard` 上留一个几十 MB 的 APK，
用一阵子就是一堆，而且没人会想起来清。成功、失败、连打开失败都删。

中间还有一层：`InstallApp` 自己会在 `/data/local/tmp/autod-install-*.apk`
落一个临时文件，同样无论成败都清掉。

响应：`{"ok":true,"bytes":16805,"replace":true}`
失败时 `error` 是 installer 的原话（签名冲突、版本降级、空间不足）。

---

## 七、文件与下载

### POST /download

```json
{"url":"https://example.com/f.zip","filename":"f.zip","subdir":"mydir"}
```

`filename` 和 `subdir` 可省（从 URL 推断）。

```json
{"ok":true,"path":"f.zip","absPath":"/storage/emulated/0/Download/f.zip",
 "bytes":420815,"url":"https://example.com/f.zip"}
```

依赖设备上的 `libcurl`（运行时 dlopen）。没有就明确报错，不会假装成功。
**支持 HTTPS。**

### GET /files?path=

列出下载目录（或指定子目录）。

```json
{"ok":true,"dir":".","count":3,
 "entries":[{"name":"a.apk","dir":false,"size":16805,"mtime":…}]}
```

### POST /files

```json
{"op":"mkdir","path":"a/b","recursive":true}
{"op":"delete","path":"a/b","recursive":true}
{"op":"rename","path":"a","to":"b"}
```

| op | 需要的字段 |
|---|---|
| `list` | `path` |
| `stat` / `exists` | `path` |
| `mkdir` | `path`，可选 `recursive` |
| `delete` | `path`，可选 `recursive` |
| `rename` | `path` + `to` |

---

## 八、电源

### POST /power

```json
{"action": "shutdown"}
{"action": "reboot"}
{"action": "reboot-recovery"}
```

| action | 说明 |
|---|---|
| `reboot` | 重启 |
| `shutdown` / `poweroff` | 关机 |
| `reboot-recovery` | 重启到 recovery |
| `reboot-bootloader` | 重启到 bootloader |
| `reboot-sideload` | 重启到 sideload |

```json
{"ok":true,"action":"reboot","method":"svc power",
 "note":"设备将在约 0.5 秒后重启"}
```

走 `svc power reboot|shutdown`（= `PowerManager.reboot/shutdown`），
它会先做正常的关机流程（通知应用、卸载文件系统）。
`reboot` 二进制只作为退路（有些精简 ROM 没有 svc）。

**应答先发出去再执行** —— fork 一个子进程延迟 500ms 再动。
不这么做的话设备已经开始关机，客户端只会看到连接被重置，
无从判断命令是否被受理。

需要 root 或 shell 权限。

---

## 九、日志

### GET /log?since=N

内存环形缓冲（2048 行），按序号增量拉。

```json
{"ok":true, "latestSeq":1234, "dropped":0,
 "lines":[{"seq":1200,"timeMs":496573,"time":"09-28 08:28:29",
           "level":"I","tag":"","text":"截图后端就绪…"}]}
```

用法：第一次 `since=0` 拿全部，之后用上次返回的 `latestSeq` 作为新的 `since`。
`dropped > 0` 说明中间被冲掉了。

### GET /logfile

落盘的历史日志，`/sdcard/autod.log`，**最多 10KB**。

```json
{"ok":true, "path":"/sdcard/autod.log", "bytes":5413, "maxBytes":10240,
 "text":"09-28 08:31:46 W 图像编码器: AndroidBitmap_compress…\n…"}
```

内存缓冲关掉进程就没了，而排障时最需要的恰恰是"上次为什么退出的"。
所以同时往共享存储写一份。

裁剪带**滞回**：超过 10KB 裁到 7.5KB。裁到"刚好 10KB"的话下一行又超、
又裁一次 —— 每写一行就要读写 20KB。带滞回后每 ~2.5KB 才裁一次，
文件仍然满足"只留最近 10KB"（瞬时可能到 10KB + 一行）。

### GET /logstream（WebSocket）

实时日志流，见 [02-websocket.md](02-websocket.md)。

### 日志时间格式

接口和落盘文件用**同一个** `MM-DD HH:MM:SS`（本地时区）。
两个字段都给：`timeMs` 是单调时钟（排序稳定，不受系统时间被改的影响），
`time` 是墙上时间（"几点发生的"只有它能给）。

---

## 十、服务自身

| 端点 | 说明 |
|---|---|
| `POST /shutdown` | 优雅退出 **autod 自身**（不是设备） |
| `POST /restart` | 退出并由 supervisor 重启（退出码 1） |

⚠️ 别和 `POST /power` 搞混：`/shutdown` 关的是**服务**，`/power` 关的是**设备**。

### POST /setconfig 的等价物

配置通过 `POST /config` 修改（见 [04-config.md](04-config.md) 的配置项表）：

```bash
curl -X POST http://host:8088/api/v1/config \
     -H 'Content-Type: application/json' \
     -d '{"log-level":"debug"}'
```

```json
{"ok":true,"applied":["log-level"],"requiresRestart":[],"rejected":[],"changed":true}
```

**能热改的立刻生效**（`verbose`/`log-level`、`display`、`socket-mode`、
`touch-range`）；改不动的**如实报告**在 `requiresRestart` 里 ——
socket 一旦 bind 就定了，setuid 不可逆。假装成功的后果是调用方以为改了
而行为没变，那种不一致最难排查。

---

## 相关

- [02-websocket.md](02-websocket.md) —— 三条 WebSocket 流
- [03-socket.md](03-socket.md) —— Unix socket 二进制协议
- [04-config.md](04-config.md) —— 配置、部署、鉴权
- [05-errors.md](05-errors.md) —— 状态码
