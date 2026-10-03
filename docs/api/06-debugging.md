# 调试与验证

---

## `REMOTE_CONTROL_FORCE_FALLBACK=1` —— 强制走回退编码器

Android 11+ 永远探测得到 `AndroidBitmap_compress`，所以
**libjpeg / 内置 libwebp / zlib 那条回退路径在开发机上根本跑不到**。

而"没跑过的代码"和"没有的代码"在出故障时是一样的。

```bash
adb shell "REMOTE_CONTROL_FORCE_FALLBACK=1 setsid nohup \
           /data/local/tmp/remote-control-supervisord.sh > /dev/null 2>&1 &"
```

环境变量会从 supervisor 继承给 remote-control（脚本用的是 `nohup "$BIN" …`，
没有清环境）。

> ⚠️ **这条假设的是「原型部署」**（supervisord 从 `/data/local/tmp` 拉起）。
> 产品形态是 init 服务，`remote-control.rc` 里没有注入环境变量的地方 ——
> 要在 init 形态下试回退编码器，得临时把 `.rc` 的命令行前面加上
> `sh -c 'REMOTE_CONTROL_FORCE_FALLBACK=1 exec /system/bin/remote-control …'`，
> 或退到原型部署。生产形态与原型部署的区别见
> [`../09-deployment-and-update.md`](../09-deployment-and-update.md) §9.5。

### 它做什么

只影响**编码器选择** —— 跳过 `AndroidBitmap_compress` 的探测，
改用 `dlopen libjpeg` + 内置 libwebp + zlib。

**不改协议、不改截图后端、不改注入后端。**

### 怎么确认它生效了

```bash
curl -s http://host:8088/api/v1/params | jq .codecs
```

```json
{"png": true, "jpeg": true, "webp": true, "raw": true,
 "backend": "libjpeg: 6b  27-Mar-1998 + libwebp（内置） + zlib PNG",
 "forced": true}
```

`forced: true` 是**关键字段** —— 一个开着这个变量的实例，
它的 `codecs` 和能力都**不代表这台设备**。别把强制的结果当设备真相。

服务端日志也会打：

```
W remote-control: REMOTE_CONTROL_FORCE_FALLBACK=1 —— 跳过 AndroidBitmap_compress，
        强制走回退编码器（仅用于验证老设备路径）
I remote-control: 图像编码器: libjpeg: 6b … + libwebp（内置） + zlib PNG（回退路径，强制）
```

### 实测：两条路差多少

同一台设备（Android 12 / 320×480 / 同一画面）：

| 格式 | Skia（默认） | 回退路径 | 差异 |
|---|---|---|---|
| PNG | 86,434 B | 116,713 B | **+35%**（zlib 比 Skia 弱） |
| JPEG | 11,166 B | 10,621 B | **−5%**（libjpeg 略小，没有 ICC） |
| **WebP** | 5,220 B | **4,732 B** | **−9%** |

三种格式产出的文件都用 `file(1)` 独立确认过格式正确。

> ⚠️ 帧率对比要在**同一个截图后端**下做。在 NDK 构建（screencap 后端，
> 120ms+/帧）上两路都是 2 fps 左右 —— 那是抓帧封顶，不是编码器的差距。

---

## 其它环境变量

| 变量 | 作用 |
|---|---|
| `REMOTE_CONTROL_CONFIG` | 配置文件路径（默认 `/data/misc/remote-control/remote-control.conf`） |
| `REMOTE_CONTROL_IDLE_TIMEOUT_SEC` | socket 空闲超时。**测试用** —— 不压到 1 秒的话集成测试要等 30 秒 |
| `REMOTE_CONTROL_FORCE_FALLBACK` | 见上 |

---

## 常驻测试建议

回退路径容易悄悄坏掉（改个编码器、动个 build flag 就可能）。
建议在 CI 里跑三种组合：

```
1. 默认（有 Skia）           → 验证主路径
2. REMOTE_CONTROL_FORCE_FALLBACK=1    → 验证老设备路径
3. 两者都跑一遍 /params，确认 codecs 与实际编码结果一致
```

第 3 条是关键 —— `codecs` 说 `webp:true` 就得真的能编出 WebP。
「报告的能力」和「实际的能力」不一致是最难查的一类问题。
