# 状态码与错误处理

---

## 两层状态

一次调用有两层状态，**都要看**：

| 层 | 位置 | 用途 |
|---|---|---|
| **HTTP 状态码** | 响应行 | 让通用 HTTP 工具（curl、代理、监控）能判断成败 |
| **协议状态码** | JSON 的 `status` 字段 | 精确原因，跨传输一致 |

```bash
T='<从设备配置中读取的 token>'
curl -s -o /dev/null -w '%{http_code}\n' -X POST \
    -H "Authorization: Bearer $T" \
    -H 'Content-Type: application/json' -d '{"key":"nonsense"}' \
    http://host:8088/api/v1/key
400
```

```json
{"ok":false,"status":4099,"error":"不认识的键: nonsense。可用: home back …"}
```

`4099` = `0x1003` = `kErrBadArg`。

---

## 协议状态码

| 值 | 十六进制 | 名称 | 含义 |
|---|---|---|---|
| 0 | `0x0000` | `kOk` | 成功 |
| 4097 | `0x1001` | `kErrBadMagic` | 请求头的 magic 不对（socket 协议） |
| 4098 | `0x1002` | `kErrBadCmd` | 未知命令 |
| 4099 | `0x1003` | `kErrBadArg` | 参数错 |
| 4100 | `0x1004` | `kErrNoDisplay` | 找不到显示 |
| 4101 | `0x1005` | `kErrCaptured` | 截图失败 |
| 4102 | `0x1006` | `kErrInjected` | 注入失败 |
| 4103 | `0x1007` | `kErrInternal` | 内部错误 |
| 4104 | `0x1008` | `kErrUnsupported` | 这台设备/这个构建不支持 |
| 4105 | `0x1009` | `kErrNotFound` | 包不存在 / 路径不存在 |
| 4106 | `0x100a` | `kErrPermission` | 权限不足 |
| 4107 | `0x100b` | `kErrTimeout` | 子进程或下载超时 |
| 4108 | `0x100c` | `kErrPayload` | payload 缺失或格式不对 |
| 4109 | `0x100d` | `kErrIo` | 文件/网络 IO 失败 |

---

## HTTP 映射

| HTTP | 协议状态 | 什么时候 |
|---|---|---|
| 200 | `kOk` | 成功 |
| 201 | —（HTTP 层） | 文件上传成功 |
| 400 | `kErrBadCmd` `kErrBadArg` `kErrPayload` | 参数错、未知资源、未知格式 |
| 401 | —（HTTP 层） | 需要令牌 / 令牌不对 |
| 403 | `kErrPermission` | 权限不足 |
| 404 | `kErrNotFound` | 包/路径/资源不存在 |
| 405 | —（HTTP 层） | 部分资源不接受该 HTTP 方法 |
| 409 | —（HTTP 层） | 上传目标已存在 |
| 413 | —（HTTP 层） | 请求体或文件超过限制 |
| 429 | —（HTTP 层） | 上传临时空间配额正被其他请求占用 |
| 500 | 其他 | 内部错误 |
| 501 | `kErrUnsupported` | 不支持 |
| 503 | —（HTTP 层） | **服务被软开关关闭**，或并发连接已达上限 |
| 504 | `kErrTimeout` | 超时 |
| 507 | —（HTTP 层） | 上传临时空间不足或无法检查 |

`HttpResponse::Error` 生成的 JSON 会把 HTTP 状态写入 `status`；未映射为协议码的状态（如 405、409、413、429、507）会原样保留。

### 两个 HTTP 层专属的状态

**401** 和 **503** 不来自协议状态，是 HTTP 层自己产生的：

```json
// 401 —— 需要令牌
{"ok":false,"status":401,"error":"需要访问令牌。请在页面顶部填入，
 或用 Authorization: Bearer <token> / X-Remote-Control-Token: <token>"}

// 503 —— 服务已关闭对外能力
{"ok":false,"status":503,"error":"服务已关闭对外能力。
 用 POST /api/v1/service {\"on\":true} 重新开启"}
```

并发连接达到上限时，HTTP 服务器在读取请求前直接返回 503，并带有 `Retry-After: 1`；
响应 JSON 只有 `ok` 和 `error`，没有 `status`。软关闭产生的 503 由路由处理器返回
JSON `status: 503`，恢复入口仍可用。客户端应检查响应 JSON 的 `status` 是否存在：
`status:503` 表示服务被软关闭；没有 `status` 的 503 表示连接名额暂满，应遵循
`Retry-After` 并做有上限的重试。

---

## `error` 字段是服务端的原话

**值得直接显示给用户。** 它写的是"为什么"，而不是一个需要查表的码。

### 好的错误信息

```json
{"ok":false,"status":4099,"error":"需要 key（键名或键码）"}
{"ok":false,"status":4099,"error":"不认识的键: nonsense。可用: home back
 menu appswitch search power volumeup volumedown mute enter delete
 backspace space tab escape up down left right center playpause nextsong
 previoussong stop camera a-z 0-9 f1-f12，或直接给数字键码"}
{"ok":false,"status":4099,"error":"未知的电源操作: bogus（可用 reboot|shutdown|
 reboot-recovery|reboot-bootloader|reboot-sideload）"}
{"ok":false,"status":400,"error":"未知格式: bogus（可用 auto|png|jpeg|webp|raw）"}
{"ok":false,"status":4108,"error":"请求体为空：把 APK 字节作为请求体发送，
 或用 ?path= 指定设备上已有的文件"}
```

共同点：**说清楚哪个参数错了，以及可用的取值是什么。**

### 安装失败是 installer 的原话

```json
{"ok":false,"status":4103,
 "error":"安装失败: Exception occurred while executing 'install':
 java.lang.IllegalArgumentException: Error: Failed to parse APK file: …"}
```

多行、Java 堆栈风格、不好看 —— 但它是用户唯一能据此行动的信息
（签名冲突 / 版本降级 / 空间不足）。包装成"安装失败"就没用了。

### 能力不可用时带上原因

`GET /describe` 里 `available: false` 的命令会带 `reason`：

```json
{"name":"KeyEvent","cmd":8,"since":4,"available":false,
 "reason":"/dev/uinput 不可写，无法创建虚拟键盘"}
```

---

## 一些"看起来像错误但不是"的情况

### 空剪贴板 —— 200，`has:false`

```json
{"ok":true,"has":false}
```

空剪贴板是**正常状态**，不是错误。

### 停止不存在的应用 —— 200，`ok:true`

```json
{"ok":true,"package":"com.nonexistent.pkg","stillRunning":false}
```

幂等：它确实没在跑。**看 `stillRunning`，不是看 `ok`。**

### 画面没变 —— 不是错误，是没消息

`skipUnchanged=1` 时静止画面**整帧不编码**，客户端在 WebSocket 上
收不到任何东西。这是设计行为（MJPEG 那条会继续显示上一帧）。

想强制每帧都发：`?skipUnchanged=0`，或发 `{"t":"refresh"}`。

### 服务关闭时的 503

不是故障，是用户自己按的。网页收到 503 会**同步开关状态**而不是弹错误。

---

## 客户端的正确处理

### 1. 先看 HTTP 状态，再看 `ok`

```js
const r = await fetch(url, opts);
const j = await r.json();
if (!j.ok) {
  // j.error 是服务端的原话，直接显示
  showError(j.error || ('HTTP ' + r.status));
}
```

### 2. 401 和 503 要特殊处理

```js
if (r.status === 401) {
  // 令牌缺失/不对 —— 弹出输入框，别让用户对着点不动的页面猜
  showTokenPrompt();
  return;
}
if (r.status === 503) {
  if (j.status === 503) {
    // 服务被软关闭
    setServiceSwitch(false);
  } else {
    // HTTP 并发连接达到上限；按 Retry-After 有上限地重试
    const seconds = Number(r.headers.get('Retry-After') || 1);
    scheduleRetry(Math.max(1, seconds));
  }
  return;
}
```

### 3. `describe` 比文档更可信

不同设备的 `capture.backend`、`inject.backend`、可用命令都可能不同：

```js
const token = '<从 remote-control.conf 读取的 token>';
const d = await (await fetch('/api/v1/describe', {
  headers: {Authorization: 'Bearer ' + token}
})).json();
if (!d.capabilities.keyInjection) {
  disableKeyButtons('这台设备无法注入按键：'
                    + d.commands.find(c => c.name === 'KeyEvent').reason);
}
```

### 4. 重试要有上限

`kErrTimeout`（下载、子进程）和 `kErrIo` 值得重试；
`kErrBadArg` / `kErrUnsupported` 重试多少次都一样。

---

## 相关

- [01-http.md](01-http.md) —— 每个端点的具体错误
- [03-socket.md](03-socket.md) —— `status` 字段在二进制协议里的位置
