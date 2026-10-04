# 2026-10-04 截图与视频流优化验证

本目录保存本次优化的模拟器测量与浏览器验证证据。性能解释见
[`06-capture-performance.md`](../../06-capture-performance.md#116-模拟器端到端对照)。

## 环境和版本

- 设备：`emulator-5580`，Android 12 x86_64，ARM64 bridge，1280×720 横屏。
- 抓帧：SurfaceFlinger；H.264 通过 MediaCodec ByteBuffer 输入，模拟器 AVC 编码器
  为软件实现。此处没有测真机硬件编码收益或 Surface 输入路径。
- 优化起点：`6a74a177135356536fa6f888ea1bb0ea2ab2430e`。旧版测量使用设备原有
  的系统二进制，没有重新编译该提交；其 SHA-256 为
  `c98cbb9e1db9e9a7c0d99bd3899ae43ef408211dad13f1a895d0abe8b8f19906`。
- 新版源码：上述提交上的本次优化工作树，包括采集锁、共享图像编码、PNG 滤波、
  libyuv 转换、异步 H.264 输出、传输期限和浏览器呈现改动。
- 性能和浏览器验证使用的 x86_64 平台二进制 SHA-256：
  `335e2964ac14b80f7234b34ff0d1af0f96eae5944050b66d4eca06983df7e9c7`。
  最后补齐文件目录遍历的 `O_PATH` 兼容性后，平台二进制 SHA-256 为
  `c1df9f7fc6da124a242dd438dd09ab92b6ec57a312694bf5afee95b6b680ad5a`；
  最终版本通过 459 项单元/集成检查、94 项 API 文档检查及设备冒烟验证。
- 部署：临时 bind mount 升级运行二进制，临时加载媒体服务访问所需的 SELinux 策略。
  `remote_control` 域保持 enforcing，没有替换磁盘中的系统二进制或策略，未烧录 ROM。
- policy 源码通过平台、Treble、上下文和 neverallow 构建检查。完整权限问题、
  临时验证及恢复方法见 [`selinux.txt`](selinux.txt)。
- 客户端：Node 22+ 内置 WebSocket，通过主机 adb 转发连接设备；浏览器另行验证。
  与 policy 构建重叠的一次 PNG 测量已重测，本目录仅保留最终数据。

## 文件索引

| 文件 | 内容 |
|---|---|
| [`before-png4-static.json`](before-png4-static.json) | 旧版 4 路 PNG、静态桌面，5 秒 |
| [`after-png4-static.json`](after-png4-static.json) | 新版相同参数，5 秒，含缓存计数 |
| [`before-jpeg4-static.json`](before-jpeg4-static.json) | 旧版 4 路 JPEG、静态桌面，5 秒 |
| [`after-jpeg4-static.json`](after-jpeg4-static.json) | 新版相同参数，5 秒，含缓存计数 |
| [`before-png4-scroll.json`](before-png4-scroll.json) | 旧版 4 路 PNG、设置页重复滚动，6 秒 |
| [`after-png4-scroll.json`](after-png4-scroll.json) | 新版相同场景，6 秒，含变化与缓存计数 |
| [`after-h264-static.json`](after-h264-static.json) | 新版单路 H.264、静态桌面，5 秒；无有效旧性能基线 |
| [`selinux.txt`](selinux.txt) | 媒体权限失败、修复、enforcing 验证、构建检查和恢复方法 |
| [`builds.txt`](builds.txt) | 完整 ARM64 / API 26 NDK 构建及产物校验 |
| [`browser.txt`](browser.txt) | 实际浏览器 24 项检查与 H.264 配置热改 |
| [`browser-h264.png`](browser-h264.png) | 设备 H.264 经 WebCodecs 解码后的画面 |
| [`stream-regression.txt`](stream-regression.txt) | 编码器并发上限、断连释放、长按时截图 |
| [`mjpeg-regression.txt`](mjpeg-regression.txt) | multipart 图像完整性、慢接收端退出 |
| [`final-device-smoke.txt`](final-device-smoke.txt) | 最终二进制的存储访问与 H.264 首帧 |
| [`verification.txt`](verification.txt) | 最终测试、平台构建及 API 核验结果 |
| [`png-byte-comparison.txt`](png-byte-comparison.txt) | 60 组旧新 PNG 逐字节一致及哈希 |
| [`webui-desktop.png`](webui-desktop.png) | 桌面浏览器控制台验证截图 |
| [`webui-mobile.png`](webui-mobile.png) | 移动视口控制台验证截图 |

## 复测

先构建和运行待测版本，确认 `/api/v1/config` 报告 `surfaceflinger`。
本次测量不使用 NDK 的 screencap 后端；该后端的抓帧成本不能与此表直接比较。
连接同一模拟器，在桌面稳定后测静态内容：

```bash
adb -s emulator-5580 forward tcp:18088 tcp:8088
node dev/02-native-daemon/tools/bench/stream_latency.mjs \
  --base http://127.0.0.1:18088 --format png --clients 4 \
  --fps 60 --width 0 --seconds 5
```

JPEG 将 `--format png` 改成 `--format jpeg`。H.264 使用 `--format h264 --clients 1`，
须确认平台与 SELinux 允许创建 AVC 编码器。PNG 默认压缩级别 1；JPEG/H.264
默认 quality 75。工具默认 `skipUnchanged=0`，才能在静态屏幕测持续投递。

滚动场景使用同一设置页和相同滚动手势，另一个终端持续驱动滚动，
将测量窗口改为 `--seconds 6`。工具本身不产生滚动输入。滚动在 fps 窗口后停止，
随后工具保持流连接，依次请求 8 次单次截图。

```bash
adb -s emulator-5580 shell am start -a android.settings.SETTINGS
adb -s emulator-5580 shell 'for i in 1 2 3 4 5 6; do input swipe 640 620 640 180 350; input swipe 640 180 640 620 350; done'
```

## 统计口径和限制

- `streams[].fps` 是收包数除以连接建立起点到测量窗口结束的时长，不是屏幕实际
  呈现 fps。当前整数帧间隔 `1000 / 60 = 16ms`，投递结果可略超过 60fps。
- `firstFrameMs` 从开始建立全部连接计时，包含连接、抓帧、编码和投递；不含解码绘制。
- `pingMs` 为画面流上的 JSON ping/pong 往返，每 100ms 发一次，约 49–59 个样本。
  最慢的 pong 可能在窗口后返回，因此样本数不一定严格等于时长除以间隔。
- `screenshotMs` 含 HTTP 收到完整响应的耗时，每组仅 8 个样本，在流的 fps 窗口
  之后、流仍连接时取得。滚动场景这时已停止滚动，不能代表持续动画中的截图延迟。
- 百分位按排序样本的 `floor(n × p)` 下标取值；8 样本的 p95 等于最大值。
  这些短窗口不用于判断长时间尾延迟。
- `capture` 和 `encoding` 为窗口前后进程计数的差值；旧版没有编码缓存统计，
  部分旧记录也没有采集变化计数。高静态缓存命中率不能用于估计游戏画面的成本。
- 滚动不是逐帧确定性回放；页面内容、动画、系统与主机负载会影响结果。未采集
  同条件 CPU、浏览器呈现延迟、真实拥塞链路或真机硬件编码器数据。
- 传输的 1 秒整帧写入期限限制服务端等待，包含短写；不限制内核中已排队数据，
  也不是设备到屏幕的端到端延迟保证。
- H.264 的旧运行域曾因缺权限创建编码器失败，不能把旧失败或零帧当成性能基线。
