# WebSocket 流

> 三条流，都是 `ws://<设备IP>:8088/api/v1/<名字>?token=<令牌>`
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
const token = '<从 remote-control.conf 读取的 token>';
const touchUrl = new URL('ws://host:8088/api/v1/touch');
touchUrl.searchParams.set('token', token);
const ws = new WebSocket(touchUrl);
```

⚠️ 令牌会出现在 URL 里，可能被服务端访问日志、代理和浏览器历史留下。
只在确实没法带头的场合用它（WebSocket 和 `<img>` 就是这种场合）。

### 帧

| 方向 | 类型 | 掩码 |
|---|---|---|
| 客户端 → 服务端 | 文本（opcode 1） | **必须打掩码**（RFC 6455 5.1） |
| 服务端 → 客户端 | 文本（opcode 1）/ 二进制（opcode 2） | 从不打掩码 |

收到**不打掩码的客户端帧**会被明确拒绝（协议错误），而不是容忍 ——
容忍一个协议错误比拒绝它危险得多。

ping 会被自动回 pong。收到客户端的 close 帧后，服务端会回送相同载荷的 close
帧，再结束流；客户端应等待这个响应后关闭 TCP 连接。
ping/close 响应的整帧写入期限为 1 秒，发送失败后结束连接。

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
ws://host:8088/api/v1/stream?fps=30&format=jpeg&quality=75&maxWidth=720&skipUnchanged=1&token=<令牌>
```

### 查询参数

| 参数 | 默认 | 范围 | 说明 |
|---|---|---|---|
| `fps` | 5 | 1-60 | 帧率 |
| `format` | `auto` | `auto`\|`jpeg`\|`webp`\|`png`\|`h264` | 编码格式，H.264 只支持 WebSocket |
| `quality` | 按格式 | PNG 1-9 / JPEG·WebP·H.264 1-100 | PNG 是压缩级别，H.264 换算为码率 |
| `maxWidth` | 720 | 0-8192（0 = 不缩放） | 降采样宽度 |
| `skipUnchanged` | 1 | 0\|1 | 画面没变时跳过编码 |

`auto` 在设备上是 JPEG（小、快）；主机上退回 PNG。
实测 320×480：PNG 86 KB / JPEG q75 17 KB / WebP 8.8 KB。

`skipUnchanged=1` 时静止画面**整帧不编码**（实测 10 秒只发 1 帧）。
暂停后重连、或想强制每帧都发，用 `skipUnchanged=0`。
JPEG/PNG/WebP 的相同内容、格式、quality 和输出尺寸会共享编码结果；
`skipUnchanged=0` 仍会传帧，但不强制重复编码相同像素。统计见 `/params.encoding`。
初始查询指定的格式若当前服务不支持，升级前返回 HTTP 400。

### H.264（`format=h264`）

静态画面的 H.264 P 帧可只有几十至几百字节，动态内容取决于码率和复杂度，
不能把这个体积比例推广到游戏。它有两处前提：

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
服务端会先存住、给每个 IDR 补上参数集再发 —— 单独发出去的话客户端
会拿到一个只有参数集、没有图像的 chunk。

每个二进制消息只包含一个完整访问单元。已接受的输入会独立轮询输出，
因此 `skipUnchanged=1` 停止新输入后仍可发出异步完成的最后一帧。
改变 fps、quality 或输出尺寸会重新配置编码器；重建后重新发送 `codec`
及关键帧，即使 codec 字符串与之前相同。H.264 输出宽高对齐到偶数，
实际尺寸以 `size` 消息为准。

启动、编码或输出错误时，服务端尝试发送 `{"t":"error","error":"…"}`，
随后结束连接。客户端应显示错误并选择重新连接或另一格式，不能等待该连接
无限自行重试。

⚠️ **并发有硬上限**：真机的硬件编码器通常只支持 1~2 路。超了不是
变慢，是创建失败。当前默认 2，`GET /api/v1/params` 的
`codecs.h264Max` / `h264Used` 能查到占用。

### 尺寸消息 `{"t":"size"}`

服务端在**尺寸变化时**发这条 —— 不只是第一帧之前：

```json
{"t":"size","w":480,"h":853}
```

什么时候会变：

- 客户端改了 `maxWidth`
- 设备转屏（`POST /rotate`）
- 别的途径改了显示分辨率（`wm size`）

只发一次是不够的：那样客户端画布会一直停在旧尺寸上。
实测踩过 —— 改到 480 之后画布还是 720。

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
ws.send(JSON.stringify({t:'maxWidth',       v:480}));  // 中途改分辨率，不重连
ws.send(JSON.stringify({t:'refresh'}));          // 立刻重发一帧（不管变没变）
ws.send(JSON.stringify({t:'ping', s:1}));
```

回复：

```json
{"t":"ack","fps":10}
{"t":"ack","quality":50}
{"t":"ack","format":"webp"}
{"t":"ack","skipUnchanged":false}
{"t":"ack","maxWidth":480}
{"t":"pong","s":1}
```

`refresh` 用于页面重新可见时立刻刷新一次（标签页切回来时画面可能已经旧了）。

`maxWidth` 是**降采样宽度**（`0` = 原始分辨率，不降采样）。
它是 `FrameHub` 的抓帧宽度取所有订阅者里**最大的**那个 ——
你改小了自己这一路，别的客户端不受影响；改大了会抬高整机的抓帧开销。

> ⚠️ 改 `maxWidth` 之后画面尺寸就变了，服务端会重发一条 `size`
> （见下面「尺寸消息」）。客户端必须据此调整画布，否则图会被拉伸。
>
> 它也**不影响触控坐标**：坐标始终按 `/info` 的 `touchWidth/Height`
> 算，不是按图尺寸（见 `01-http.md` 的 `/info`）。

### 客户端渲染示例

下面的独立图片帧示例同一时间只解码一帧，并保留最新待解码帧；
处理 H.264 时应另外维护参考帧和关键帧恢复，不能照此任意丢弃 delta chunk。

```js
const token = '<从 remote-control.conf 读取的 token>';
const streamUrl = new URL('ws://host:8088/api/v1/stream?fps=30&format=jpeg');
streamUrl.searchParams.set('token', token);
const ws = new WebSocket(streamUrl);
ws.binaryType = 'blob';
let pending = null, decoding = false;
async function decodeLatest() {
  if (decoding || !pending) return;
  decoding = true;
  const data = pending;
  pending = null;
  try {
    const bmp = await createImageBitmap(data);
    try { ctx.drawImage(bmp, 0, 0); }
    finally { bmp.close(); }
  } catch (error) {
    console.error('image decode failed', error);
  } finally {
    decoding = false;
    decodeLatest();
  }
}
ws.onmessage = (ev) => {
  if (typeof ev.data === 'string') {          // 控制消息
    const m = JSON.parse(ev.data);
    if (m.t === 'size') { cvs.width = m.w; cvs.height = m.h; }
    return;
  }
  pending = ev.data;
  decodeLatest();
};
```

用 `createImageBitmap` 而不是 `<img>`：解码在 worker 线程上、绘制是同步的，
而且能 `close()` **主动释放** —— `<img>` 的旧帧什么时候被回收是浏览器说了算。
内置控制台还合并 `requestAnimationFrame` 绘制，并用连接/格式代数丢弃旧异步结果；
H.264 解码队列拥塞时会重置解码器并请求新的关键帧。

### 主循环为什么用 poll

服务端的主循环用 `poll` 同时等两件事：客户端消息、下一帧的时间点。
早先是"select 查消息 + sleep 1ms"，有两个毛病：空转（每秒 1000 次唤醒），
以及控制消息要等下一轮才被看到 —— 而中间可能正卡在一次抓帧里
（screencap 后端要 120ms），ping 往返能到 100ms 以上。

共享抓帧等候与在途图像编码等候每次最多 20ms，获取编码并发名额最多 5ms，
随后返回消息处理循环；实际编码计算本身仍可能占用更长时间。
画面流的控制消息、二进制帧和 MJPEG part 写入均有覆盖所有短写的 1 秒期限。
超时或写入失败会关闭连接，避免半帧后继续发送导致协议失步；这个期限不是
完整帧从设备到屏幕的延迟保证。

### MJPEG 那条

同一端点**不带 `Upgrade` 头**就是 MJPEG：

```html
<img src="http://host:8088/api/v1/stream?fps=5&amp;token=<令牌>">
```

`multipart/x-mixed-replace`，浏览器原生支持，零 JS。
适合嵌到别的页面或调试；控制台自己用的是 WebSocket 那条。

---

## 二、触控流 `/api/v1/touch`

```
ws://host:8088/api/v1/touch?token=<令牌>
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
| `x` `y` | 坐标空间见 [01-http.md](01-http.md) 的「坐标」—— 是 `/info` 的 `touchWidth/Height`，**不是**屏幕像素 |
| `id` | 非负 int32 指针 ID，不是硬件槽位号；默认 0。同一触点的 `down` / `move` / `up` / `cancel` 使用相同 ID。并发触点数由注入后端决定：uinput 最多 10 个，`virtual_touchpad` 最多 2 个；后者按 `id % 2` 映射槽位，因此同时活动的 ID 要有不同奇偶性 |
| `pressure` | 可选，0.0-1.0 |
| `ms` | 可选，手势时长 |

服务端**收到就立刻注入**，不再等"整个手势"。

### 回复策略

| 事件 | 是否回复 | 为什么 |
|---|---|---|
| `down` / `up` / `cancel` | ✅ 回显对应事件名，如 `{"ok":true,"t":"down"}` | 状态变化，值得确认 |
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
ws://host:8088/api/v1/logstream?since=0&token=<令牌>
```

### 查询参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `since` | 0 | 从这个序号之后开始推。0 = 全量 |

### 消息

```json
{"t":"lines", "dropped":0, "lines":[
  {"seq":1200,"ms":496573,"time":"09-28 08:28:29","level":1,"tag":"","text":"截图后端就绪…"},
  {"seq":1201,"ms":496580,"time":"09-28 08:28:29","level":3,"tag":"","text":"注入失败: …"}
]}
```

| 字段 | 说明 |
|---|---|
| `seq` | 单调序号。客户端记下它，断线重连时用 `?since=<seq>` 续上 |
| `ms` | 单调时钟（毫秒） |
| `time` | 本地时区墙上时间，格式 `MM-DD HH:MM:SS` |
| `level` | `0`=Debug `1`=Info `2`=Warn `3`=Error |
| `tag` | 一般空 |
| `text` | 日志正文，**不含时间前缀**；用 `ms` 稳定排序，用 `time` 查看墙上时间 |

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

触控流回调中的注入会调用 `Dispatcher::Handle`，并与 HTTP、Unix socket 上的触控等有状态操作共用 `opMutex_`；同一时刻这类操作只执行一个，以免并发注入破坏手势状态。

`Info` 和 `Capture` 在 Dispatcher 前置分支绕过 `opMutex_`。画面流抓帧由 Capture 后端自己的锁保护，因此不会因触控操作排队；调用同一后端的抓帧仍会串行。

`opMutex_` 放在 Dispatcher 而非 HTTP 处理器，是因为**流式回调在处理器返回之后才运行**；HTTP 处理器中的锁无法覆盖回调里的触控注入。

---

## 相关

- [01-http.md](01-http.md) —— HTTP/JSON API
- [03-socket.md](03-socket.md) —— Unix socket 二进制协议
