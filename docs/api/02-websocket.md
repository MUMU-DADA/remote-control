# WebSocket 流

> 三条流，都是 `ws://<设备IP>:8088/api/v1/<名字>`
> 走 RFC 6455 的一个**子集** —— 只实现文本帧、二进制帧、ping/pong、close。
> **不做** permessage-deflate 扩展，**不做**分片续帧。

---

## 为什么用 WebSocket 而不是 HTTP 轮询

| | HTTP 轮询 / 一连串 POST | WebSocket |
|---|---|---|
| 拖拽跟手 | 每个移动点一次 TCP 往返 + HTTP 解析 | 每个点一个几字节的帧 |
| 实时画面 | 每帧一次请求，必然滞后一个周期 | 服务端推 |
| 日志 | 没日志时纯白问 | 有就推、没有就没有 |
| 反向控制 | 做不到 | **客户端能改服务端参数** |

实测：触控单事件往返 **1.1ms**（HTTP POST 13.8ms，**快 13 倍**）；
60 点拖拽 **48ms**（POST 一次发完整条路径是 865ms，而且中途没有任何反馈）。

---

## 通用约定

### 升级

标准的 WebSocket 升级。服务端对 `Sec-WebSocket-Key` 做
`base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))` 校验后回 101。

**key 不合法会明确拒绝**（400），而不是接受后挂住 ——
后者在浏览器里只表现为"连接失败"，没有任何可读信息。

### 鉴权

浏览器无法给 WebSocket 设请求头，所以令牌走**查询参数**：

```js
new WebSocket('ws://host:8088/api/v1/touch?token=' + encodeURIComponent(tok))
```

⚠️ 令牌会出现在 URL 里，可能被日志和浏览器历史留下。
只在确实没法带头的场合用它（WebSocket 就是这种场合）。

### 帧

| 方向 | 类型 | 掩码 |
|---|---|---|
| 客户端 → 服务端 | 文本（opcode 1） | **必须打掩码**（RFC 6455 5.1） |
| 服务端 → 客户端 | 文本（opcode 1）/ 二进制（opcode 2） | 从不打掩码 |

收到**不打掩码的客户端帧**会被明确拒绝（协议错误），而不是容忍 ——
容忍一个协议错误比拒绝它危险得多。

ping 会被自动回 pong。close 会让流正常结束。

### 通用心跳

三条流都接受：

```json
{"t":"ping","s":123}
→ {"t":"pong","s":123}
```

`s` 由客户端自定，服务端原样回。用它测往返延迟 ——
**触控手感好不好，用户感受到的是这个数，不是帧率。**

---

## 一、画面流 `/api/v1/stream`

```
ws://host:8088/api/v1/stream?fps=30&format=jpeg&quality=75&maxWidth=720&skipUnchanged=1
```

### 查询参数

| 参数 | 默认 | 范围 | 说明 |
|---|---|---|---|
| `fps` | 5 | 1-60 | 帧率 |
| `format` | `auto` | `auto`\|`jpeg`\|`webp`\|`png` | 编码格式 |
| `quality` | 按格式 | PNG 1-9 / JPEG·WebP 1-100 | 画质 |
| `maxWidth` | 720 | 0-8192（0 = 不缩放） | 降采样宽度 |
| `skipUnchanged` | 1 | 0\|1 | 画面没变时跳过编码 |

`auto` 在设备上是 JPEG（小、快）；主机上退回 PNG。
实测 320×480：PNG 86 KB / JPEG q75 17 KB / WebP 8.8 KB。

`skipUnchanged=1` 时静止画面**整帧不编码**（实测 10 秒只发 1 帧）。
暂停后重连、或想强制每帧都发，用 `skipUnchanged=0`。

### H.264（`format=h264`）

帧比 JPEG **小两个数量级**（P 帧几十~几百字节 vs 11 KB），但它有两处
前提：

1. **只能走 WebSocket** —— MJPEG 的 `multipart` 是"每段一张独立的图"，
   装不下带帧间依赖的流。带 `format=h264` 走 MJPEG 会返回 400。
2. **客户端必须支持 WebCodecs**（`VideoDecoder`，Chrome/WebView **94+**）。
   设备上的 WebView 是 91 —— 所以**在设备本机**打开控制台用不了，
   从桌面浏览器打开可以。

握手多一条消息：

```json
← {"t":"hello","format":"h264","fps":15,...}
← {"t":"size","w":320,"h":480}
← {"t":"codec","codec":"avc1.42C029"}     ← 第一帧之前
← 二进制 <Annex-B：SPS PPS IDR>
← 二进制 <Annex-B：非 IDR>
```

`codec` 串来自 SPS 的第 1~3 字节，**各设备不同，不能写死** ——
写死了 WebCodecs 的 `isConfigSupported` 会拒掉。

**服务端保证**：第一个二进制帧一定是完整的访问单元（`SPS PPS IDR`）。
MediaCodec 本来把 SPS/PPS 作为单独一个 CODEC_CONFIG buffer 吐出来，
服务端会先存住、拼到下一个真正的帧前面再发 —— 单独发出去的话客户端
会拿到一个只有参数集、没有图像的 chunk。

⚠️ **并发有硬上限**：真机的硬件编码器通常只支持 1~2 路。超了不是
变慢，是创建失败。当前默认 2，`GET /api/v1/params` 的
`codecs.h264Max` / `h264Used` 能查到占用。

### 消息时序

```
客户端连接
  ← 文本 {"t":"hello","format":"jpeg","fps":30,"quality":75,
           "maxWidth":720,"skipUnchanged":true,"chrome":false}
  ← 文本 {"t":"size","w":320,"h":480}
  ← 二进制 <JPEG 字节>            ← 第一帧
  ← 二进制 <JPEG 字节>
  …
```

`hello` 和 `size` 分开发是有意的：客户端要据此建 canvas，
而第一帧到达之前它没法知道该建多大。

### 客户端控制命令

**不用重连就能改**：

```js
ws.send(JSON.stringify({t:'fps',            v:10}));
ws.send(JSON.stringify({t:'quality',        v:50}));
ws.send(JSON.stringify({t:'format',         v:'webp'}));
ws.send(JSON.stringify({t:'skipUnchanged',  v:0}));
ws.send(JSON.stringify({t:'refresh'}));          // 立刻重发一帧（不管变没变）
ws.send(JSON.stringify({t:'ping', s:1}));
```

回复：

```json
{"t":"ack","fps":10}
{"t":"ack","quality":50}
{"t":"ack","format":"webp"}
{"t":"ack","skipUnchanged":false}
{"t":"pong","s":1}
```

`refresh` 用于页面重新可见时立刻刷新一次（标签页切回来时画面可能已经旧了）。

### 客户端渲染示例

```js
const ws = new WebSocket('ws://host:8088/api/v1/stream?fps=30&format=jpeg');
ws.binaryType = 'blob';
ws.onmessage = async (ev) => {
  if (typeof ev.data === 'string') {          // 控制消息
    const m = JSON.parse(ev.data);
    if (m.t === 'size') { cvs.width = m.w; cvs.height = m.h; }
    return;
  }
  const bmp = await createImageBitmap(ev.data);
  ctx.drawImage(bmp, 0, 0);
  bmp.close();     // ⚠️ 不 close 会攒着不放，几分钟就吃满内存
};
```

用 `createImageBitmap` 而不是 `<img>`：解码在 worker 线程上、绘制是同步的，
而且能 `close()` **主动释放** —— `<img>` 的旧帧什么时候被回收是浏览器说了算。

### 主循环为什么用 poll

服务端的主循环用 `poll` 同时等两件事：客户端消息、下一帧的时间点。
早先是"select 查消息 + sleep 1ms"，有两个毛病：空转（每秒 1000 次唤醒），
以及控制消息要等下一轮才被看到 —— 而中间可能正卡在一次抓帧里
（screencap 后端要 120ms），ping 往返能到 100ms 以上。

### MJPEG 那条

同一端点**不带 `Upgrade` 头**就是 MJPEG：

```html
<img src="http://host:8088/api/v1/stream?fps=5">
```

`multipart/x-mixed-replace`，浏览器原生支持，零 JS。
适合嵌到别的页面或调试；控制台自己用的是 WebSocket 那条。

---

## 二、触控流 `/api/v1/touch`

```
ws://host:8088/api/v1/touch
```

**拖拽跟手必须用这条。** 一个手势一个 HTTP 请求是行不通的 ——
每次 POST 都要 TCP 往返 + HTTP 解析，拖拽时每个移动点都这么来一遍，
手感必然是"一顿一顿的"。

### 流式三原语

```js
ws.send(JSON.stringify({t:'down', x:100, y:200, id:0}));
ws.send(JSON.stringify({t:'move', x:105, y:205, id:0}));   // 高频
ws.send(JSON.stringify({t:'up',   x:110, y:210, id:0}));
```

| 字段 | 说明 |
|---|---|
| `t` | `down` / `move` / `up` / `cancel` |
| `x` `y` | 屏幕像素 |
| `id` | 触控槽位，0-9。多点触控时区分手指，默认 0 |
| `pressure` | 可选，0.0-1.0 |
| `ms` | 可选，手势时长 |

服务端**收到就立刻注入**，不再等"整个手势"。

### 回复策略

| 事件 | 是否回复 | 为什么 |
|---|---|---|
| `down` / `up` / `cancel` | ✅ `{"ok":true,"t":"up"}` | 状态变化，值得确认 |
| `move` | ❌ **不回复** | 最高频。回一个等于把上行流量翻倍，而客户端不需要 |
| `move` 出错时 | ✅ `{"ok":false,…}` | 出错必须知道 |
| 其他（见下） | ✅ | |

### 一次性手势（省一次 HTTP 往返）

```js
ws.send(JSON.stringify({t:'tap',       x:100, y:200}));
ws.send(JSON.stringify({t:'longpress', x:100, y:200, ms:800}));
ws.send(JSON.stringify({t:'doubletap', x:100, y:200}));
ws.send(JSON.stringify({t:'drag', x1:100, y1:300, x2:200, y2:200, ms:600}));
```

### 长按不需要专门的命令

流式模型里"按下 → 手指不动 → 延迟抬起"**本来就是长按**，
系统会自己识别（Android 的 longPressTimeout 约 500ms）。
这正是流式的自然之处。

反过来说：**长按期间一个 `move` 都不能发** —— 一旦收到移动超过
touchSlop 的 MOVE，系统会把这次按压从"长按"改判成"拖拽"。

### 客户端实现建议

```js
// 移动合并到每帧一个。pointermove 在高刷屏上能到 200Hz，
// 而设备侧的注入和屏幕刷新都跟不上；全发只是白占带宽。
let pending = false;
canvas.addEventListener('pointermove', (ev) => {
  const p = toScreen(ev);
  if (pending) return;
  pending = true;
  requestAnimationFrame(() => {
    pending = false;
    ws.send(JSON.stringify({t:'move', x:p.x, y:p.y, id:slot}));
  });
});

// 指针离开画面也要收尾，否则那根"手指"会在设备上一直按着
canvas.addEventListener('pointercancel', endPointer);
```

---

## 三、日志流 `/api/v1/logstream`

```
ws://host:8088/api/v1/logstream?since=0
```

### 查询参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `since` | 0 | 从这个序号之后开始推。0 = 全量 |

### 消息

```json
{"t":"lines", "dropped":0, "lines":[
  {"seq":1200,"ms":496573,"level":1,"tag":"","text":"截图后端就绪…"},
  {"seq":1201,"ms":496580,"level":3,"tag":"","text":"注入失败: …"}
]}
```

| 字段 | 说明 |
|---|---|
| `seq` | 单调序号。客户端记下它，断线重连时用 `?since=<seq>` 续上 |
| `ms` | 单调时钟（毫秒） |
| `level` | `0`=Info `1`=Warn `2`=Error `3`=Debug |
| `tag` | 一般空 |
| `text` | 日志正文，**不含时间前缀**（时间在落盘文件里有，接口里用 `ms`） |

### 客户端命令

```js
ws.send(JSON.stringify({t:'ping', s:1}));   // → {"t":"pong","s":1}
ws.send(JSON.stringify({t:'clear'}));       // 服务端把游标归零，重新全量推
```

### 批次

一次最多推 200 行。日志刷起来可能一秒几十行，无节制地写会把这条连接
变成瓶颈。客户端要自己限制 DOM 行数：

```js
// 日志流是持续的，DOM 不限行数的话开一天就能把标签页拖死
while (box.childElementCount > 500) box.removeChild(box.firstChild);
```

### 自动滚动

只在**贴近底部**时自动滚：

```js
const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 24;
// …追加…
if (atBottom) box.scrollTop = box.scrollHeight;
```

用户往上翻看历史时把他拽回底部，是最招人烦的行为之一。

---

## 服务端实现说明

### 帧读写

`websocket.h/cpp` 是自包含的 RFC 6455 子集，没有外部依赖。
SHA-1 和 base64 也是自己实现的 —— 只为握手算一次摘要，
为它引入依赖不划算。（SHA-1 在这里**不用于安全**：RFC 6455 的握手
只是防止"普通 HTTP 请求被误当成 WebSocket"。）

握手对着 **RFC 6455 §1.3 的标准向量**验证：

```
dGhlIHNhbXBsZSBub25jZQ==  →  s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

必须离线测：算错的表现是**浏览器直接拒绝连接**，页面上什么都不显示。

### 关闭时的资源清理

连接线程是 detached 的，主线程走到 `main()` 结尾会析构 `Dispatcher`
（含它的操作锁），而流式回调还在那些线程里跑 ——
表现是 `FORTIFY: pthread_mutex_lock called on a destroyed mutex`。

`HttpServer::Stop()` 因此会主动 shutdown 活跃连接并等它们退出（最多 2 秒）。

### 操作锁

流式回调里的每次注入/抓帧都会走 `Dispatcher::Handle`，
而锁在**那一层**。所以同一时刻只有一个操作在执行 ——
一次抓帧（~23ms）会让同时在跑的触控事件排队最多 23ms。

锁放这里而不是调用方，是因为**流式回调是在处理器返回之后才跑的**：
早先锁在 HTTP 处理器里，对流式响应完全无效（那时锁早就释放了）。

---

## 相关

- [01-http.md](01-http.md) —— HTTP/JSON API
- [03-socket.md](03-socket.md) —— Unix socket 二进制协议
