# 05 · 延迟与触控能力

> 截图与触控的延迟拆解、吞吐上限，以及触控能力覆盖范围。

> ⚠️ **本文数字是按管线结构估算的，不是实测值。**
> `capture.cpp` / `inject.cpp` 尚未在真实设备上运行过。实测方法见文末。

---

## 1. 截图延迟

按管线拆解（1080p RGBA_8888）：

| 环节 | 耗时 | 说明 |
|---|---|---|
| 客户端 → socket → autod | 0.1 ms | Unix socket，无网络栈 |
| Binder 到 SurfaceFlinger | 0.3–1 ms | 一次 Binder 往返 |
| **GPU 合成一帧到捕获缓冲** | **8–20 ms** | ← **主要瓶颈** |
| 等待 fence | 含在上面 | |
| `memcpy` 8 MB 到 memfd | 1–3 ms | 移动端内存带宽约 5–10 GB/s |
| 客户端 `mmap` | ~0 | 零拷贝 |

### 合计

| 刷新率 | 端到端延迟 |
|---|---|
| 60 Hz | **20–35 ms** |
| 120 Hz | **10–20 ms** |

### 为什么合成是瓶颈

平时 HWC（硬件合成器）把各图层直接叠加后推给屏幕，不经过 CPU。

一旦要**捕获**，SurfaceFlinger 必须额外做一次 GPU 合成，把结果渲染到一块 CPU 可读的缓冲区。这一步**绕不掉**——这正是为什么所有人都走 `captureDisplay()` 而不是读 framebuffer。

### 吞吐

| 实现方式 | 帧率 |
|---|---|
| 当前实现（同步 `waitForResults()` 串行） | **15–30 fps** |
| 改流水线（不等每帧结果） | **30–60 fps** |

流水线的代价是持续占用 GPU，会拖慢前台应用。

### 对比 adb 方案

| 方式 | 单次耗时 |
|---|---|
| `adb exec-out screencap` | 100–300 ms |
| **daemon 方案** | **20–35 ms** |

**快 5–10 倍。**

---

## 2. 触控延迟

| 环节 | 耗时 |
|---|---|
| 客户端 → socket → autod | 0.1 ms |
| 构造 `MotionEvent` | ~0.05 ms |
| Binder `injectInputEvent` | 0.3–1 ms |
| InputDispatcher → 应用 | 下一帧，8–17 ms |
| **应用响应** | **取决于应用主线程** |

| 指标 | 数值 |
|---|---|
| 事件进入输入管线 | **1–2 ms** |
| 到应用真正响应 | 再加 1 帧 |
| **注入速率** | **1000–3000 事件/秒** |

注入速率上限来自 Binder：每次 `injectInputEvent` 是一次 Binder 往返（约 0.3–1 ms）。够 60–120 Hz 的滑动采样。

### 对比 adb 方案

| 方式 | 单次耗时 |
|---|---|
| `adb shell input tap` | 30–80 ms（fork + exec） |
| **daemon 方案** | **1–2 ms** |

**快 50 倍以上。**

### `/dev/uinput` 路径的差异

如果触控改走 uinput（Android 12 上可能需要）：

| | `IInputManager` | `/dev/uinput` |
|---|---|---|
| 单次开销 | 0.3–1 ms（Binder） | 2–10 µs（一次 write） |
| 吞吐 | 1000–3000 事件/秒 | 数万事件/秒 |
| 额外输入设备 | ❌ 不产生 | ✅ 会新增 `/dev/input/eventX` |
| 可被枚举 | 否 | **是**（见 `06-constraints.md`） |

uinput 更快，但会创建一个可枚举的输入设备。

---

## 3. 触控能力矩阵

### 已实现

| 能力 | 说明 |
|---|---|
| 单击 | 可配持续时间 |
| 长按 | 单击 + 长 duration |
| 双击 | 两次单击 |
| 滑动 | 直线插值，可配时长和步数 |
| 压感 / 接触面积 | 每个手势可设常量值 |

### 需要补代码

| 能力 | 现状 |
|---|---|
| **真多点触控（双指缩放等）** | ⚠️ 协议留了 `pointerId`，但 `inject.cpp` 目前**恒定发 `pointerCount = 1`** |
| 压力 / 面积曲线 | 现在是每手势常量，需改成逐点变化 |
| 按键注入 | `Cmd::KeyEvent` 已声明，**未实现** |
| 文本输入 | 未实现 |
| 鼠标 / 滚轮 | 未实现 |

### 多点触控要做什么

真正的多指事件需要：

1. `pointerCount = N`
2. 平行的 `PointerProperties[]` 和 `PointerCoords[]` 数组
3. 正确的动作编码：
   ```
   ACTION_POINTER_DOWN | (index << ACTION_POINTER_INDEX_SHIFT)
   ACTION_POINTER_UP   | (index << ACTION_POINTER_INDEX_SHIFT)
   ```
4. 维护指针 ID 的生命周期

### 框架层面的上限

- `MotionEvent` 最多 **16 个指针**
- 工具类型可指 finger / stylus / mouse
- 支持 hover（鼠标悬停）

### 拿不到的

**真实硬件指纹**。具体包括：

- 电容屏原始采样数据
- 与加速度计 / 陀螺仪的物理相关性（真实触摸时设备会有微小震动）
- 与触控采样率的相位关系
- 真实的接触面积变化曲线

**这些在 framework 层注入解决不了。** 如果应用做了行为分析，注入事件和真实触摸在这些维度上是有差异的。

---

## 4. 优化空间

| 优化 | 收益 | 代价 |
|---|---|---|
| **只捕获需要的区域**（`sourceCrop`） | 合成和 memcpy 按面积等比下降，**收益最大** | 需改协议支持区域参数 |
| 跳过 memcpy，直接导出 dmabuf fd | 省 1–3 ms | 客户端要懂 gralloc 布局 |
| 流水线捕获 | 吞吐翻倍 | 持续占 GPU |
| 服务端不编 PNG，发原始 RGBA | 省 15–40 ms | 带宽增大 |
| 降采样（如输出 540p） | memcpy 省 4 倍 | 精度下降 |

**PNG 编码很贵**：1080p 在设备上编码 PNG 要 15–40 ms。如果调用方能接受原始像素，**不要编 PNG**。

---

## 5. 怎么实测

上面的数字需要实测校验。在 `autod` 里加三处 `CLOCK_MONOTONIC` 打点：

```cpp
// capture.cpp 的 Grab() 里
const int64_t t0 = NowNs();                    // 收到请求
// ... ScreenshotClient::captureDisplay() ...
const int64_t t1 = NowNs();                    // 捕获返回
// ... memcpy 完成 ...
const int64_t t2 = NowNs();                    // 数据拷完

ALOGI("capture: 合成=%lldus 拷贝=%lldus",
      (t1 - t0) / 1000, (t2 - t1) / 1000);
```

`inject.cpp` 的 `SendSingle()` 里同理，测 `injectInputEvent` 的往返。

跑一次就能拿到目标设备的真实数字。**不同 SoC 差异很大**，尤其是 GPU 合成那一步。

---

## 6. 相关文档

- 为什么合成是瓶颈 → `01-selection.md`
- Android 12 的触控约束 → `06-constraints.md`
- 代码实现 → `../dev/02-native-daemon/`
