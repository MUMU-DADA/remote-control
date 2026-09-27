# 实机验证证据

`emulator-android12-shot.png` —— **autod 在真实 Android 12 上抓的第一帧**。

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
N: Name="autod-touch"
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


---

# 上位应用（autod 控制台）

`controller-app-*.png` 是控制台应用的实机截图。**这些截图是 autod 自己拍的**
—— 服务在拍那个控制它的应用，完整闭环。

| 文件 | 内容 |
|---|---|
| `controller-app-1.png` | 首次启动，未连接 |
| `controller-app-3.png` | 已连接并查询到状态（含 autod 报出的前台应用 = 它自己） |
| `controller-app-4.png` | 应用标签页 |
| `controller-app-7.png` | 修复前的应用列表（显示成 APK 路径 —— 那个 bug 的现场） |
| `controller-app-8.png` | 修复后：显示应用标签「autod 控制台」 |

## 界面结构

```
┌─ socket 路径 ────────────── [连接] ─┐
│ 状态提示（绿=成功 / 红=失败）        │
│ [状态][应用][文件][控制]             │
├─────────────────────────────────────┤
│ 状态：显示参数 + 当前前台应用 + pid  │
│ 应用：列表（标签/图标由本应用解析）、│
│       点击启动、长按看清单/停止      │
│ 文件：浏览下载目录、下载 URL、        │
│       新建/删除/重命名               │
│ 控制：点击、滑动、截图并显示         │
└─────────────────────────────────────┘
```

## 分工：为什么标签不在 daemon 里解析

`pm list packages` 给不出应用标签；逐个 `dumpsys package` 对 100+ 应用太慢。
而上位应用一个 `PackageManager.getApplicationLabel()` 就有了，还带图标、
还是本地化的。

所以 **daemon 只返回包名和廉价元数据**（路径/版本/installer），
**展示层交给上位应用**。这是有意为之的分工，不是偷懒。

## 应用申请了什么权限

**一个都没有。** 所有能力都由 daemon 执行，应用只是协议的客户端。
这样即使应用被替换，能做的事也不会超过 daemon 暴露的协议范围。

## 构建

```bash
bash dev/05-controller-app/build-apk.sh          # 出 APK
bash dev/05-controller-app/build-apk.sh --install
```

不依赖 AOSP 的 out/（那个目录经常被别的构建占着），用独立 SDK build-tools。
JDK 直接用 AOSP 树自带的 `prebuilts/jdk/jdk11`。

## ⚠️ 部署时的 SELinux 问题

应用以自己的 UID（`untrusted_app` 域）连不上 `/data/local/tmp/autod.sock`
（标签 `shell_data_file`）：

```
avc: denied { write } for name="autod.sock"
  scontext=u:r:untrusted_app:s0:c105,c256,c512,c768
  tcontext=u:object_r:shell_data_file:s0  tclass=sock_file
```

**不能**用 `allow untrusted_app shell_data_file:sock_file write` 敷衍 ——
那是把口子开给所有第三方应用。正确做法是给上位应用一个专属域，
见 `dev/02-native-daemon/sepolicy/autod_controller.te`。

本次功能验证临时用了 `setenforce 0`，**这不是可交付的方案**。
