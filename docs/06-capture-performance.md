# 抓帧与编码性能：实测与调优

> 场景前提：**目标设备固定 720p**（720×1280），不存在其它分辨率场景。
> 这个前提决定了什么值得优化、什么不值得 —— 见下面的结论。

---

## 一、结论先说

在 720p 上，**抓帧不是瓶颈，编码才是**。

| 环节 | 720p 实测 | 判断 |
|---|---|---|
| 抓一帧（SurfaceFlinger） | **8 ms** | 便宜。折合 ~120 fps 的上限 |
| JPEG 编码 + 投递 | **61.0 fps** | ✅ 跟得上 |
| PNG 编码 + 投递 | **45.8 fps** | ✅ 够用 |
| WebP 编码 + 投递 | **38.3 fps** | ✅ 够用 |

**修之前**同样是这三个格式：jpeg 61.0 / png **22.2** / webp **20.5** ——
WebP 和 PNG 只有现在的一半。根因是走了 Skia 那条"原生快路径"，
详见第七节。**这是本次调研最大的收获。**

用户感知最直接的对比（30fps 目标，客户端实际收到多少帧）：

| @30fps 目标 | 修复前 | 修复后 |
|---|---|---|
| jpeg | 30.0 | 30.0 |
| **webp** | **20.4** ❌ | **30.0** ✅ |
| **png** | **23.1** ❌ | **30.0** ✅ |

所以：

- **抓帧这条路已经没什么可榨的了。** 8ms 里大头是 SF 合成 + 一次像素拷贝，
  换成虚拟显示或 MediaCodec 零拷贝都省不掉这一份（理由见第四、五节）。
- **真正的瓶颈一直是编码，而它已经被解决了一半。** WebP 还有余量
  （见第七节末尾），但 38fps 已经超过绝大多数消费场景的需求。
- **JPEG 仍是首选格式**：帧率最高（61fps）、体积适中（43.9 KiB）。

> ⚠️ 这些是 **x86_64 模拟器 + swiftshader 软件渲染**的数字，绝对值不能直接
> 推真机。但**相对关系**（抓帧远比编码便宜）在真机上只会更明显 —— 因为
> GPU 合成比软件渲染快，而软件编码的开销不变。

---

## 二、怎么测的（以及几个会骗人的坑）

工具：`dev/02-native-daemon/tools/bench/stream_fps.py`

```bash
# 720p，jpeg/webp/png 各跑 8 秒对比
python3 dev/02-native-daemon/tools/bench/stream_fps.py \
    --host <设备IP> --port 8088 --fps 30 --secs 8 --compare
```

它同时报三组数，**缺一不可**：

```
投递帧率   客户端真收到多少帧        ← 用户感知
抓帧节奏   服务端 /params.capture.frames 的增长速度   ← 抓帧线程实际情况
抓帧耗时   /params.capture.lastCaptureMs              ← 底层源头成本
```

三个数对不上，就说明瓶颈在中间某一段而不是抓帧。

### 坑 1：`skipUnchanged` 会让投递帧率低到 1fps，但那不是性能问题

模拟器桌面是静态画面，默认 `skipUnchanged=1` 时只在画面变化时出帧。
第一次测的时候得到 `0.2 fps`，差点当成严重 bug —— 实际是停检在正常工作。

**测投递能力必须带 `skipUnchanged=0`**（工具默认已经带了）。

### 坑 2：按「帧间隔」算帧率是错的

一次 `read()` 可能返回好几帧，中位帧间隔会算出 0.0ms。
**必须用窗口内的帧计数**（`帧数 / 时长`），不能统计单帧间隔。

### 坑 3：宿主上同时在编译，会把模拟器测量拖慢

模拟器跑在同一台宿主机上。同一组测试：

| | jpeg@60 | webp@30 | png@30 |
|---|---|---|---|
| 宿主**正在**编译 AOSP | 47.0 fps | 17.4 fps | 20.0 fps |
| 宿主空闲 | **61.0 fps** | **20.4 fps** | **23.1 fps** |

差距大到能把结论带偏（47 vs 61）。**测性能之前先确认宿主没在编译。**

### 坑 4：别拿 NDK 版测性能

`out/ndk/` 那份是 `screencap(exec)` 后端（197ms/帧），
`out/` 那份才是 SurfaceFlinger（8ms/帧）。测之前先确认：

```bash
curl -s http://<设备IP>:8088/api/v1/config | jq -r .runtime.capture.backend
# 必须是 surfaceflinger，不能是 screencap(exec)
```

---

## 三、源头降采样（已实现，但 720p 场景下多半用不上）

`DisplayCaptureArgs` 的 `width`/`height` **直接决定 SF 的渲染尺寸**，不是事后缩放：

```cpp
// frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp:5986
ui::Size reqSize(args.width, args.height);
...
if (args.width == 0 || args.height == 0) {
    reqSize = display->getLayerStackSpaceRect().getSize();   // ← 默认走这条
}
```

传了尺寸，SF 在**合成阶段**就只生成这么多像素 —— memfd 变小、mmap 变小、
软件降采样直接省掉。

实测（720p 源，jpeg @30fps）：

| 客户端 `maxWidth` | 抓帧耗时 | 每帧体积 |
|---|---|---|
| 720（= 原生宽度） | 8 ms | 43.9 KiB |
| 480 | **5 ms** | 24.3 KiB |

### ⚠️ 为什么 720p 场景下它多半不生效

`FrameHub` 按**所有订阅者里最大的 `maxWidth`** 抓帧，而 `defaultMaxWidth` 就是
720。所以默认配置下 `targetWidth == 原生宽度`，降采样分支根本不进。

**它只在客户端主动要更小的图时才起作用**（比如控制台在小窗口里预览）。
这不是缺陷 —— 720p 全尺寸本来就只有 0.92M 像素，SF 处理得起。

### ⚠️ 取尺寸必须用 `getDisplayState`，不能用 `getActiveDisplayMode`

```cpp
ui::DisplayState state;
SurfaceComposerClient::getDisplayState(token, &state);
const uint32_t srcW = state.layerStackSpaceRect.getWidth();
```

`getActiveDisplayMode().resolution` 是**面板物理模式**，
`layerStackSpaceRect` 才是 **SF 实际合成用的逻辑尺寸**（`wm size` 覆盖生效时
两者不一致）。用错的话高宽比会算错，画面会被拉变形。

排查过程记一笔：一开始就是用了 `getActiveDisplayMode`，在
`wm size 1440x2960` 的模拟器上才暴露出来。虽然 720p 场景走不到这条路径，
但既然是错的就修掉了。

---

## 四、✗ 虚拟显示镜像 —— 收益在延迟，不在吞吐

```cpp
static sp<IBinder> createDisplay(const String8& displayName, bool secure);
static void destroyDisplay(const sp<IBinder>& display);
```

思路：建个虚拟显示把物理显示的 layerStack 镜像过去，挂 BufferQueue，
从消费者侧 `dequeueBuffer` —— SF 合成出新帧就直接推进来，不用每次发
`captureDisplay` 请求。

**代价**：

- 虚拟显示**必须一直开着**才推帧 ✗ 没人在看时 SF 也要多合成一路，
  这跟「没需求就不抓」的原则直接冲突
- BufferQueue 的建立/格式协商/生命周期是一大坨代码
- **像素从 GraphicBuffer 拷到 CPU 那份开销省不掉** ✗ 和 `captureDisplay`
  走的是同一份合成结果

**结论：不做。** 定时抓帧已经把「请求-应答」那部分延迟抹掉了 ——
实测 `misses=0`，即消费者每次拿到的都是备好的最新帧。

---

## 五、✗ MediaCodec 零拷贝 —— 和接口目标冲突

scrcpy 的做法是 `MediaProjection` → 一个 `Surface` 直接喂 `MediaCodec`，
CPU 完全不碰像素。

但我们的接口要提供 **PNG / JPEG / WebP / 原始 RGBA**，这些都要 CPU 能读到的
像素。走 MediaCodec 只能出 H.264/H.265 —— 而那是已经单独做过的一条路。

**结论：不是替代方案，是并存的一条路。**

---

## 六、🔍 顺带发现：`sourceCrop` 可以只抓一块区域

`CaptureArgs`（`DisplayCaptureArgs` 的基类）里有：

```cpp
Rect sourceCrop;
float frameScaleX{1};
float frameScaleY{1};
```

现在用不上（要抓就得抓全屏）。但如果以后要加「只看某个应用窗口」或
「只监控状态栏」这类接口，`sourceCrop` + `LayerCaptureArgs`（按 layer 抓）
是现成的路。

---

## 七、根因找到了：Skia 那条"原生快路径"其实是慢路径 ✅ 已修

### 结论

`AndroidBitmap_compress`（Skia）在 WebP / PNG 上**比内置编码器慢一倍**。
直觉上"原生实现应该更快"，实测完全相反。

720p，60fps 目标，宿主空闲：

| 格式 | Skia `AndroidBitmap_compress` | 内置编码器 | 提升 | 体积变化 |
|---|---|---|---|---|
| jpeg | 60.3 fps / 43.9 KiB | 61.0 fps / 43.4 KiB | 持平 | 持平 |
| **webp** | 20.5 fps / 25.0 KiB | **38.3 fps** / 25.5 KiB | **+87%** | +2% |
| **png** | 22.2 fps / 72.9 KiB | **45.8 fps** / 85.3 KiB | **+106%** | +17% |

两组数字各自复现过，不是噪声。

### 为什么

大概率在**参数**上，而不是实现质量：

- **WebP**：Skia 用 `method=3`（为了跟 Chrome 对齐），我们内置的用 `method=2`。
  按 `webp_encoder.h` 里原有的实测，method 2 相对 3 **正好快一倍** ——
  和这里量到的 +87% 对得上。
- **PNG**：Skia 用了较高的 zlib 级别，我们默认 1（最快、最大）。

也就是说 `webp_encoder.h` 里那套参数调优**一直没生效过** ——
只要设备是 API 30+，WebP 就轮不到内置 libwebp，`kDefaultWebpMethod = 2`
是一行死代码。

### 怎么修的

`image_encoder.cpp` 里加了 `PreferBuiltin(ImageFormat)`，**按格式**选编码器：

| 格式 | 走哪条 | 理由 |
|---|---|---|
| jpeg | Skia | 持平，没必要动 |
| webp | 内置 libwebp | 快 87%，只大 2% |
| png | 内置 zlib | 快 106%，大 17%（唯一要权衡的一项） |

两个开关都留着：

```bash
AUTOD_FORCE_FALLBACK=1   # 全走内置（验证老设备路径）
AUTOD_PREFER_NATIVE=1    # 全走 Skia（要最小体积时）
```

`/params` 的 `codecs.backend` 也跟着改成**逐格式**报，不再是一个笼统的名字：

```
"backend": "jpeg=AndroidBitmap_compress + webp=libwebp（内置） + png=zlib PNG"
```

### PNG 那 17% 体积要不要认？

72.9 → 85.3 KiB/帧。按 30fps 算：2.1 MB/s → 2.5 MB/s。

- 局域网内无所谓
- 但 PNG 本来就比 JPEG 大 65% 还更慢，**它不该是控制台的默认格式**
  （默认是 jpeg）。PNG 是给"要无损"的少数场景用的，
  那种场景下 2 倍帧率比 17% 体积重要。

如果哪天要反过来，`AUTOD_PREFER_NATIVE=1` 就够了，不用改代码。

### 顺带确认：`webp_encoder.h` 里原有的调优是对的

| method | 320×480 | 1080p | 相对 Skia 默认(m=3) |
|---|---|---|---|
| 0 | 3.3 ms | 24.8 ms | 快 3.1x，体积 +22% |
| **2（现在的默认）** | **5.2 ms** | **38.8 ms** | **快 2.0x，体积 +3.6%** |
| 3（Skia 用的） | 10.3 ms | 78.5 ms | — |
| 6 | 21.8 ms | 126.5 ms | 慢 2.1x，体积 -5.8% |

`thread_level=0`（单线程）也是实测结论：libwebp 的线程只并行熵编码那一小段，
只快 5%，不值得为它引入线程池。

### 还剩多少余量

| 格式 | 现在 | 上限（抓帧 8ms） | 还差 |
|---|---|---|---|
| jpeg | 61.0 fps | ~120 fps | 已经被编码卡住 |
| png | 45.8 fps | ~120 fps | 编码仍是瓶颈 |
| webp | 38.3 fps | ~120 fps | 编码仍是瓶颈 |

WebP 还有明显余量可挖（method=1/0 会更快，代价是体积），但 38fps
已经超过绝大多数消费场景的需求了。**先到这。**

---

## 八、停检（`skipUnchanged`）原来的实现比它要省的东西还贵 ✅ 已修

### 症状

抓帧节奏 60fps、画面**完全静止**、停检开着 —— CPU 占用 **63.9%** 一个核。
一个"用来省性能"的开关，本身成了最贵的一环。

### 根因

变化检测以前是**每个客户端各自**对降采样后的缓冲区做一遍 FNV 逐字节哈希：

```cpp
uint64_t HashBytes(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
```

720p RGBA 是 **3.52 MB**。逐字节 + 串行乘法依赖链，实测：

| 720p（3.52 MB） | 每帧耗时 |
|---|---|
| FNV 逐字节（原实现） | **3.655 ms** |
| memcmp 全等（静态画面） | **0.081 ms**（快 **45x**） |
| memcmp 不等（一变就退出） | ~0 ms（快 66 万倍） |

60fps 下单客户端就是 **22% 一个核**，N 个客户端算 N 遍。

而且它排在 BGRA→RGBA 转换和降采样**后面** —— 也就是说，
在一帧注定要被跳过的帧上，那些像素活儿照样全做完了才发现"哦，没变"。

### 修法：把变化检测挪到抓帧层，只做一次

`FrameHub` 抓完一帧顺手跟上一帧比一次（`memcmp`），
给每帧打一个「第几代**不同**的画面」`changeGen`（内容变了才 +1）。
客户端只比一个整数：

```cpp
if (p.skipUnchanged && st->haveChangeGen &&
    f->changeGen == st->lastChangeGen) {
    *unchanged = true;
    return {};          // ← 在任何像素操作之前就返回
}
```

于是静态画面下，客户端**连 BGRA 转换和降采样都不做**，
更不用说 3.66 ms 的哈希。

### 实测（同一台设备、同一套脚本、背靠背）

| 配置 | 优化前（FNV） | 优化后（memcmp） | 变化 |
|---|---|---|---|
| 60fps **停检开** | **63.9%** CPU / 693 帧 | **40.9%** CPU / 728 帧 | **−36%** |
| 60fps **停检关** | **95.9%** CPU / 734 帧 | **76.2%** CPU / 728 帧 | **−21%** |

注意帧数是**涨的**（693→728）—— 不是靠少干活换来的 CPU 下降。

`/params` 也把省下的比例报出来了：

```
frames=2065  changeGen=3  unchanged=2062     ← 静止画面上 99.9% 的帧是白抓的
```

`unchanged / frames` 高 = 画面基本静止，停检正在起作用。

### ⚠️ 一处刻意的语义变化

现在比的是**抓帧尺寸**下的像素，不是各客户端降采样之后的。
所以客户端要的图比抓帧尺寸小时，理论上会有极少数"其实降采样后一样"的帧
被多送一次。方向是安全的（宁可多送、不可漏送），而省下的哈希是它的 45 倍。

### 还剩什么

优化后 60fps 静止画面仍占 **40.9%**，那基本上就是**抓帧本身**（8~11 ms/帧）。
也就是说：为了确认"画面没变"，我们仍然每秒钟抓 60 次。

这是「按时抓帧换取低延迟」的固有代价 —— 消费者来的时候最新帧已经备好，
不用等一次抓帧。要再往下压，只能动这个设计（比如静止一段时间后把
**探测用的**抓帧率降下来，检测到变化立刻恢复），代价是画面重新开始变化时
多等最多一个探测周期。**这是个取舍，没有做。**

---

## 九、优先级

| # | 事项 | 收益 | 代价 | 状态 |
|---|---|---|---|---|
| 1 | **WebP/PNG 改走内置编码器** | webp **+87%**、png **+106%** | PNG 体积 +17% | ✅ **已实现** |
| 1.5 | **停检改到抓帧层（memcmp + 代数）** | 停检开 **−36%** CPU、停检关 **−21%** | 无 | ✅ **已实现** |
| 2 | WebP `method` 再往下调（1 或 0） | 再快 ~1.5x | 体积 +10~22% | 🔍 备用，暂不需要 |
| 3 | 源头降采样（`args.width`） | 客户端要小图时抓帧 8→5ms | 小 | ✅ 已实现 |
| 4 | EncodePool 并发上限调优 | 未知 | 小 | 🔍 待做 |
| 5 | 虚拟显示镜像 | 只降延迟，不提吞吐 | 大，且与"没需求不抓"冲突 | ✗ 不做 |
| 6 | MediaCodec 零拷贝 | 数量级 | 只能出 H.264 | ✗ 已作为独立路径 |
| 7 | `sourceCrop` 区域抓帧 | 按区域比例 | 小 | 🔍 备用 |

---

## 十、还没验证的

- **真机的抓帧与编码成本**。模拟器是 swiftshader 软件渲染 + 4 vCPU，
  绝对值不能推真机。
- **源头降采样后的画质**：SF 用它的缩放滤波器（大概率双线性），
  和原来的盒式平均降采样不完全一样，锐度需要对比。
- **PNG 在 720p 的实际用途**：72.7 KiB/帧 @23fps ≈ 1.7 MB/s，
  比 JPEG 大 65% 还更慢。如果 PNG 只是给「要无损」的少数场景用，
  那 23fps 也许够；但如果控制台默认用它就会很难受。

---

## 相关

- [`03-reference.md`](03-reference.md) —— 各版本 API 差异、触控能力矩阵（含「编码器按格式选」的结论）
- [`05-design-notes.md`](05-design-notes.md) —— 设计取舍与踩过的坑
- [`api/02-websocket.md`](api/02-websocket.md) —— 流的参数（fps / quality / format / maxWidth）
- `dev/02-native-daemon/tools/bench/` —— 编码器基准程序（`webp_bench.cpp` 等）
- `dev/02-native-daemon/tools/bench/stream_fps.py` —— 本文所有端到端数字的测量工具

