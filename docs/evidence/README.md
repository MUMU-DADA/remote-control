# Android 12 模拟器运行验证

本页记录 `remote-control` 在 Android 12 userdebug x86_64 运行环境中的截图、触控冒烟验证。
测试运行于 KVM 加速的 Android 模拟器，不是实体 Android 设备；首帧见
[`emulator-android12-shot.png`](emulator-android12-shot.png)。

## 证据索引

| 分组 | 文件 |
|---|---|
| 应用界面 | [`app-white.png`](app-white.png)、[`app-clipboard.png`](app-clipboard.png)、[`app-clipboard2.png`](app-clipboard2.png)、[`app-v2.png`](app-v2.png)、[`app-v2-auth.png`](app-v2-auth.png)、[`app-v2-bottom.png`](app-v2-bottom.png)、[`v7-apps.png`](v7-apps.png)、[`v7-collapse.png`](v7-collapse.png) |
| 控制器应用 | [`controller-app-1.png`](controller-app-1.png)、[`controller-app-2.png`](controller-app-2.png)、[`controller-app-3.png`](controller-app-3.png)、[`controller-app-4.png`](controller-app-4.png)、[`controller-app-5.png`](controller-app-5.png)、[`controller-app-6.png`](controller-app-6.png)、[`controller-app-7.png`](controller-app-7.png)、[`controller-app-8.png`](controller-app-8.png) |
| Web UI 与 WebView | [`webui.png`](webui.png)、[`webui-v5.png`](webui-v5.png)、[`webui-v5-final.png`](webui-v5-final.png)、[`webui-v6.png`](webui-v6.png)、[`webui-v6-log.png`](webui-v6-log.png)、[`webui-v6-logbox.png`](webui-v6-logbox.png)、[`webui-ws-fixed.png`](webui-ws-fixed.png)、[`webui-ws-final.png`](webui-ws-final.png)、[`webui-streaming-touch.png`](webui-streaming-touch.png)、[`webview-probe.png`](webview-probe.png) |
| API 与画面流 | [`http-api-capture.png`](http-api-capture.png)、[`stream-debug.png`](stream-debug.png)、[`stream-jpeg-q50.jpg`](stream-jpeg-q50.jpg)、[`stream-jpeg-q75.jpg`](stream-jpeg-q75.jpg) |
| 2026-10-04 流优化数据 | [完整记录与文件索引](stream-opt-2026-10-04/README.md)，包括前后版本测量、浏览器验证、构建与设备检查 |

## 环境

| 项 | 值 |
|---|---|
| 系统 | Android 12（API 31），userdebug，x86_64 |
| 设备 | Cuttlefish 之外的 AOSP 模拟器，KVM 加速 |
| 镜像 | Google 官方 system-image `system-images;android-31;default;x86_64`（r05） |
| 模拟器 | 31.3.10（构建号 8807927）—— 镜像要求 ≥31.2.7 |
| 身份 | root（`adb root`），补充组含 `3011(uhid)` |
| 分辨率 | 320x480 @60Hz |

## 结果

```
[1] 运行环境
  ✓ 以 root 运行 (uid=0)
  ✓ /dev/uinput 可写（权限 0660, uid=3011 gid=3011）
  ✓ 平台构建：可访问 Binder / SurfaceFlinger
[2] 截图
  ✓ 后端: surfaceflinger
  ✓ 显示 : 320x480 @60Hz
  ✓ 抓帧成功 320x480，614400 字节，耗时 33 ms
[3] 触控
  ✓ 后端: uinput
  ✓ 测试点击成功（屏幕中心 160,240，耗时 21 ms）
```

端到端：

```
info     显示数量: 1, 分辨率: 320 x 480
capture  截图 320x480 stride=320 format=0x1 size=614400 → PNG 86082 字节
tap      已点击 (160, 240)
swipe    已滑动 (160,400) -> (160,100)
```

内核侧确认虚拟设备真的注册了：

```
N: Name="remote-control-touch"
S: Sysfs=/devices/virtual/input/input16
H: Handlers=event14
B: PROP=2          ← INPUT_PROP_DIRECT
```

截图内容校验：前 20 行 6400/6400 像素非黑，首像素 RGB(154,114,134)。

## 顺带实测出的延迟

| 操作 | 实测 | 此前估算 |
|---|---|---|
| SurfaceFlinger 抓帧 | **33 ms** | 20–35 ms ✓ |
| uinput 点击注入 | **21 ms** | — |

## 这次验证抓到的 bug

`--selftest` 在 `startThreadPool()` **之前** return，导致 SurfaceFlinger 的
异步抓帧回调没人处理，`waitForResults()` 永久阻塞。

真机症状是"打印完显示信息就卡死"——只有跑起来才会发现。
