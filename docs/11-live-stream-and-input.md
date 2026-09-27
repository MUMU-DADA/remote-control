# 11 · 实时画面流、按键、手势与网页控制台

> 这一版（协议 v4）加了五件事：实时画面流、按键注入、
> 常见触控手势、一个浏览器里能用的控制台、剪贴板访问。

---

## 1 · 实时画面流

```
GET /api/v1/stream?fps=5
```

**MJPEG over `multipart/x-mixed-replace`**。选它的理由：

浏览器把这种响应当成"一张会不断更新的图"，`<img src="...">` 就能显示，
**不需要 JS 解帧**。用 WebSocket 传帧的话，前端要写解包、渲染、
丢帧处理，而收益只是省一点带宽 —— 对一个控制台来说不划算。

每帧是一张完整 PNG（服务端编，见 `png_encoder`）。
响应头带 `X-Autod-Frame` 帧号，方便调试。

帧率限制在 1..30：**上限不是为了省事** —— 每帧都要抓屏 + 编 PNG，
30fps 在低端设备上会把 CPU 吃满，而画面只会更卡。

实测：`fps=5` 时 8 秒收到 39 帧（约 4.9fps）。

---

## 2 · 按键注入

```
POST /api/v1/key    {"key":"home"}         或   {"key":"172"}
                    {"key":"power","long":true}     长按
```

键名接受三种写法：`home` / `KEY_HOME` / `172`（原始 Linux 键码）。
单字符 `a`-`z` `0`-`9` 直接当字符键，`f1`-`f12` 也支持。

**实现走 `/dev/uinput` 建虚拟键盘** —— Android 12 没有可用的 native 按键
注入接口（`IInputManager` 是 Java-only），和触控是同一个结论。

设备在内核里长这样：

```
N: Name="autod-keyboard"
S: Sysfs=/devices/virtual/input/input34
```

### 两个踩过的坑

**单字符先按字符解释，再考虑当数字键码。**
`"1"` 既可能是"数字键 1"（Linux `KEY_1` = 2），也可能是"键码 1"（`KEY_ESC`）。
先走数字解析的话 `key "1"` 会变成 ESC —— 实测就是这么错的。

**F 键不能用 `KEY_F1 + (n-1)`。**
F1..F10 是 59..68，然后 **F11/F12 跳到 87/88**（中间是别的功能键）。
按公式算 `f12` 会得到 70 —— 一个不存在的位置。要显式列表。

---

## 3 · 常见触控手势

```
POST /api/v1/longpress   {"x":..,"y":..,"ms":800}
POST /api/v1/drag        {"x1":..,"y1":..,"x2":..,"y2":..,"ms":600}
POST /api/v1/doubletap   {"x":..,"y":..}
```

**这三个不是 Tap/Swipe 的语法糖，它们的时序有硬要求：**

| 手势 | 时序要求 | 不这么做会怎样 |
|---|---|---|
| 长按 | 按下后**一个 MOVE 都不能发** | 系统判成拖拽，长按菜单不弹 |
| 拖拽 | 起点先停顿 120ms，再慢速移动 | 被判成滑动（fling） |
| 双击 | 两次点击间隔 120ms | 太慢超过系统阈值就不算双击 |

让调用方自己拼 `Touch*` 序列几乎一定会踩这些，所以内置。

坐标字段同时接受 `x/y` 和 `x1/y1` —— 三种手势里 drag 天然要两个点，
用 `x1/y1/x2/y2` 更自然，而单点手势用 `x/y` 更自然。两种都收，
省得调用方记两套。

---

## 4 · 网页控制台

浏览器打开 **`http://<服务地址>:<端口>/`** 就是控制台，不需要额外部署。

页面做的事：
- `<img src="/api/v1/stream">` 实时画面
- 在画面上 **单击 = 点击**，**按住不动 = 长按**，**按住拖动 = 拖拽**
- Home / 返回 / 最近 / 菜单 / 电源 / 音量 / 方向键 等按钮
- 也可以直接填键名或键码
- 剪贴板查看与写入

**零依赖、单文件、不引任何 CDN。** 设备上的服务未必能访问外网，
而一个需要联网加载前端框架的控制面板在离线环境里就是一块白屏。

### 坐标换算

页面上的像素要按**渲染后的显示尺寸**换算，不能用 `naturalWidth` ——
画面会被 CSS 缩放过。换算时还要夹到屏幕范围内，否则边缘点击会越界。

### 实测

设备上用 WebView 打开控制台，画面正常、实时流正常、
状态行显示真实数据（分辨率、后端、pid、协议版本）。

`docs/evidence/webui.png` 是截图 —— 画面里能看到浏览器自身，
因为那就是实时流的内容（无限镜像）。

---

## 5 · 剪贴板

```
GET  /api/v1/clipboard?op=get      → {"ok":true,"has":true,"text":"..."}
GET  /api/v1/clipboard?op=info
POST /api/v1/clipboard  {"op":"set","text":"..."}
```

### 实现方式：exec 一个 Java 辅助工具，**以 shell 身份运行**

`daemon/tools/cliptool/ClipTool.java` → 编成 dex/jar → 推到设备。

为什么绕这一圈，三个理由：

**1. NDK 构建里没有 libbinder。** daemon 发不了 Binder 事务，
而 Java 侧一个调用就够了。

**2. 访问控制是按「UID + 包名」判定的。** 实测：

| 身份 | 结果 |
|---|---|
| root + 包名 `android` | ❌ 被拒（"not in focus nor is it a system service"） |
| shell(2000) + 包名 `com.android.shell` | ✅ 通过 |

因为 `com.android.shell` 持有 `READ_CLIPBOARD_IN_BACKGROUND`
（signature 级，实测 `granted=true`）。**daemon 是 root，所以要先
`setuid(2000)` 再 exec** —— 这是 `RunCommandAs` 存在的原因。

**3. 不能靠 `service call`。** `service call clipboard 4` 能调通，
但返回的是编组过的 `ClipData` Parcel（`ClipDescription` + 嵌套 Item +
`CharSequence`），从十六进制里解析它既啰嗦又随版本变。

### ⚠️ 写入必须回读确认

`ClipboardService.checkAndSetPrimaryClip` 在权限不足时是：

```java
if (!clipboardAccessAllowed(...)) {
    return;      // ← 静默返回，不抛异常
}
```

**所以"没报错"不等于"写进去了"。** 辅助工具写完会回读一次，
读不到就返回退出码 6，daemon 据此报错而不是谎报成功。

### 关于 `ActivityThread`

`app_process` 起的进程拿 Context 要靠 `ActivityThread.systemMain()`，
但它是 `@hide`，**所有 SDK 变体的 android.jar 里都没有**
（实测 public/system/system-server/test 五个变体全都没有）。
所以辅助工具编译期只用公开 API，运行期用反射取。

而且 `systemContext()` 的包名是 `"android"` —— 这正是访问被拒的原因。
最终改成**直接调 `IClipboard` 并显式传包名**，和 UID 对上。

---

## 实测汇总（Android 12 / x86_64 / userdebug）

```
GET  /api/v1/describe            → protocolVersion 4，28 条命令
                                   capabilities 含 keyInjection/clipboard/
                                   screenStream/webUi，全部 true
GET  /api/v1/stream?fps=5        → 8 秒 39 帧，PNG 320x480
POST /api/v1/key {"key":"home"}  → {"ok":true,"keyCode":102}
                                  内核确认 Name="autod-keyboard"
POST /api/v1/longpress           → ok
POST /api/v1/drag                → ok（x1/y1 与 x/y 两种写法都收）
POST /api/v1/doubletap           → ok
GET  /api/v1/clipboard?op=get    → {"has":true,"text":"..."}
POST /api/v1/clipboard set       → 写入后读回是真实内容
GET  /                           → 网页控制台，10989 字节
```

网页引用的 9 个接口逐个验证，全部 200。

---

## 相关文件

| 路径 | 内容 |
|---|---|
| `daemon/keyboard.h/.cpp` | uinput 虚拟键盘与键名解析 |
| `daemon/clipops.h/.cpp` | 剪贴板访问（降权 exec 辅助工具） |
| `daemon/webui.h/.cpp` | 内置网页控制台（单文件 HTML） |
| `daemon/tools/cliptool/ClipTool.java` | 剪贴板 Java 辅助工具 |
| `daemon/png_encoder.h/.cpp` | PNG 编码（流与截图共用） |
| `docs/10-http-api.md` | HTTP API 总览与安全模型 |
