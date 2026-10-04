# 02 · AOSP Native Daemon（主线）

> `remote-control`：Android 系统内的常驻 native 进程，对外提供截图与触控。
> **这是本项目的长期主线。**

---

## 现状

> 这一节写的是**已经落地并在真机验证过**的形态（init 服务 + 模拟器实跑），不是计划。
> 想知道还有什么没做，直接看文末的[「下一步」](#下一步)。

| 项 | 状态 |
|---|---|
| 协议层 + socket 层 | ✅ **已实现**，协议 v7 / 33 条命令，主机验证通过 |
| 请求分发（`dispatch.cpp`） | ✅ **已抽出**，被集成测试覆盖 |
| **触控注入（uinput 后端）** | ✅ **已实现，真实设备测试通过** |
| 触控注入（binder 后端） | ⚠️ 代码就绪，Android 12 上不可用（AIDL 无 cpp backend） |
| 帧通道（memfd + `SCM_RIGHTS`） | ✅ **已实现**，像素级校验通过 |
| **截图（`capture_surfaceflinger.cpp`）** | ✅ **已编进 ROM 并在真机运行**（`runtime.capture.backend=surfaceflinger`） |
| 截图（`capture_stub.cpp`） | ✅ 桩后端，供主机测试用 |
| 按键注入（`keyboard.cpp`） | ✅ 虚拟键盘（`/dev/uinput`），真机验证通过 |
| 屏幕方向（0/90/180/270） | ✅ 真机验证通过；设备转不动时退到改显示尺寸，并**如实回报** |
| 应用管理（`appops.cpp`） | ✅ 列表 / 启动 / 停止 / 装 APK（走 `pm`/`am`/`cmd` 子进程） |
| 文件管理（`fileops.cpp`） | ✅ 共享存储根下读写；越界与软链接逃逸都被拒 |
| 画面流（WebSocket + MJPEG） | ✅ jpeg/webp/png/h264，可中途改帧率/画质/分辨率 |
| Web 控制台（`webui.cpp`） | ✅ 内置单页，不用在设备上装任何东西 |
| 鉴权 / 服务开关 | ✅ 令牌鉴权；`enabled=0` 是**软开关**（不真停进程） |
| **init 固化** | ✅ 开机自启 + 崩了自动拉起 + 专属 SELinux 域 → [`docs/09`](../../docs/09-deployment-and-update.md) |

**全功能体检**：`python3 tools/functional-sweep.py [host:port]` —— 覆盖 33 条命令的
HTTP 面，判据一律取**设备侧证据**（重启看 `uptime` 归零、按键看焦点窗口真的变了、
截图看字节熵不是纯黑……）。接口说 ok 不算数。

---

## 目录结构

```
02-native-daemon/
├── daemon/
│   ├── Android.bp                    AOSP 构建（多个可执行目标）
│   ├── protocol.h                    对外协议（含布局断言 + ReplyPacket）
│   ├── main.cpp                      入口 / 选项 / 信号（分发已抽出）
│   ├── dispatch.{h,cpp}              请求分发 ← 集成测试直接复用
│   ├── socket_server.{h,cpp}         socket + SCM_RIGHTS + SO_PEERCRED
│   ├── remote-control.rc             init 服务定义 ← 编进 /system/etc/init/
│   │
│   ├── rest_api.{h,cpp}              HTTP/JSON 面
│   ├── http_server.{h,cpp}           内置 HTTP 服务端
│   ├── websocket.{h,cpp}             WS 握手 / 帧（画面流、触控、日志）
│   ├── webui.{h,cpp}                 内置控制台单页
│   ├── frame_hub.{h,cpp}             订阅者分发 + 编码（jpeg/webp/png/h264）
│   ├── encode_pool.{h,cpp}           编码并发限制
│   │
│   ├── capture.h                     截图对外接口
│   ├── capture_surfaceflinger.cpp    SurfaceFlinger 后端 ← 产品默认
│   ├── capture_screencap.cpp         screencap 子进程后端 ← NDK 版走这条
│   ├── capture_stub.cpp              桩后端 ← 合成图像，主机可测
│   │
│   ├── inject.h                      触控对外接口
│   ├── inject_backend.h              后端接口（内部）
│   ├── inject.cpp                    手势逻辑（平台无关）
│   ├── inject_uinput.cpp             /dev/uinput 后端 ← Android 12 上唯一可用
│   ├── inject_binder.cpp             IInputManager 后端 ← Android 12 上不可用
│   ├── keyboard.{h,cpp}              按键注入（虚拟键盘）
│   │
│   ├── appops.{h,cpp}                应用管理（pm/am/cmd 子进程）
│   ├── fileops.{h,cpp}               文件管理（共享存储边界）
│   ├── clipops.{h,cpp}               剪贴板（受平台限制，见 docs/api/01-http.md）
│   ├── config_file.{h,cpp}           配置文件读写
│   ├── service_state.{h,cpp}         鉴权 / 软开关
│   ├── subprocess.{h,cpp}            子进程封装
│   ├── selftest.{h,cpp}              --selftest
│   ├── log_buffer.{h,cpp}            环形日志（同时进 liblog）
│   ├── remote_control_log.h          日志兼容层（Android liblog / 主机 stderr）
│   └── vendor/                       内置 libwebp / libjpeg-turbo 头
│
├── tests/                            主机侧测试（g++ 直接编译，不需要 AOSP）
│   ├── Makefile                       `make run` = 跑全部 + 核对 README 里的检查数
│   ├── test_util.h                   公共工具（断言、计数上报、设备节点发现）
│   ├── test_inject_uinput.cpp        注入后端单元测试（38 项）
│   ├── test_integration.cpp          端到端集成测试（62 项）
│   ├── test_capture_screencap.cpp    screencap 后端测试（21 项）
│   ├── test_appops.cpp               应用管理后端（57 项）
│   ├── test_json.cpp                 JSON 解析/输出（49 项）
│   ├── test_keyboard.cpp             按键注入（79 项）
│   ├── test_websocket.cpp            WS 握手（47 项）
│   ├── test_transport.cpp            HTTP / Unix socket 生命周期（37 项）
│   ├── test_encoded_frame_cache.cpp  共享编码缓存（13 项）
│   ├── test_h264_encoder.cpp         H.264 编码器（43 项）
│   ├── test_fileops.cpp              文件路径与上传（35 项）
│   ├── test_sha256.cpp               SHA-256（9 项）
│   └── fake_screencap.c              冒充 /system/bin/screencap 的替身
│
├── client/
│   ├── Android.bp
│   ├── rcctl.cpp                     设备端 C++ 客户端
│   └── rc_client.py                  Python 参考客户端 + mock 服务端
└── tools/
    └── deploy_cuttlefish.sh          部署 + 冒烟测试
```

> **sepolicy 不在这个目录里。** 策略的落点是设备树
> `dev/04-android-rom/device/remote_control_x64_arm64/sepolicy/`（`remote_control.te`
> + `file_contexts` + `remote_control_controller.te`），由 `tools/integrate-sepolicy.sh`
> 接进树。为什么放那儿、以及为什么不能放 `system/sepolicy/private/`，
> 见 [`docs/09`](../../docs/09-deployment-and-update.md) §9.1。

### 后端可替换

截图和触控都做成了**后端可插拔**，编译期选择：

| 二进制 | 截图后端 | 触控后端 | 用途 |
|---|---|---|---|
| `m remote-control` | SurfaceFlinger | uinput | ✅ 产品默认 |
| `m remote_control_binder` | SurfaceFlinger | IInputManager | Android 13+ 或自改 AIDL |
| `m remote_control_stub` | **桩** | uinput | 无显示设备的 CI 冒烟测试 |

每个二进制只链一个后端——它们都定义同名工厂函数，链在一起会重复符号。

**这个设计让开发不必等 AOSP**：`capture_stub.cpp` 生成合成图像，
于是整条链路（socket → 协议 → 分发 → 注入/帧通道）都能在开发机上测试。

---

## ✅ 触控注入：uinput 后端已实现并验证

**Android 12 的 `IInputManager` 是 Java-only AIDL，native 进程调不了**
（完整论证见 [`docs/01-selection.md`](../../docs/01-selection.md) 第 7 节「硬约束与风险」。）
已实现的 `inject_uinput.cpp` 走 `/dev/uinput`，绕开这个限制。

### 验证方式：真实设备，不是 mock

```bash
cd tests
sudo make run   # 编译并运行全部测试
```

| 测试 | 覆盖范围 | 检查项 |
|---|---|---|
| `test_inject_uinput` | 注入后端本身 | 38 |
| `test_integration` | **端到端**：socket → dispatch → 帧通道 / 注入 → 内核 | 62 |
| `test_capture_screencap` | screencap 后端：fork/exec → 解析 → memfd | 21 |
| `test_appops` | 应用管理后端（`pm`/`am`/`cmd` 子进程解析） | 57 |
| `test_json` | JSON 解析 / 输出（用 AOSP 树内的 jsoncpp） | 49 |
| `test_keyboard` | 按键注入（键名映射 → 键码） | 79 |
| `test_websocket` | WebSocket 帧 / Close 控制帧 | 47 |
| `test_transport` | HTTP / Unix socket 生命周期 | 37 |
| `test_core_lifetime` | 动态库初始化与注入后端生命周期 | 7 |
| `test_encoded_frame_cache` | 共享编码缓存 | 13 |
| `test_h264_encoder` | H.264 编码器边界 | 43 |
| `test_fileops` | 文件路径与上传原子落盘 | 35 |
| `test_sha256` | SHA-256（NIST 官方向量 + 分块一致性） | 9 |

> `make run` 除了跑这 13 个套件，还会把各套件自报的检查数求和并与根
> [`README.md`](../../README.md) 里写的「单元/集成 N 项检查」比对 ——
> 数字对不上就直接失败。加测试忘了改 README 会被当场拦住。

集成测试用的是**真实的** `Dispatcher` / `SocketServer` / `Injector`，
只把截图后端换成桩（`capture_stub.cpp`）。覆盖链路：

```
客户端 ──SOCK_SEQPACKET──> SocketServer ──> Dispatcher ──┬──> Capture(桩)
                                                          │      └─ memfd ──SCM_RIGHTS──> mmap 校验像素
                                                          └──> Injector
                                                                  └─ /dev/uinput ──> 内核 ──> eventN 读回
```

**实测结果：13 个套件、497 项检查全部通过**

```
test_inject_uinput（38 项）
  [1] 单击       事件序列正确（BTN_TOUCH / TRACKING_ID / 坐标 / 压力 全对）
  [2] 滑动       200ms 产生 12 个 MOVE，坐标从 200 单调走到 800
  [3] 双指多点   使用 2 个槽位、2 个 TRACKING_ID，BTN_TOUCH 只在首尾各置位一次
  [4] 槽位耗尽   连按 12 个指针：前 10 个成功，第 11 个干净失败而非静默丢弃；
                 全部抬起后槽位能复用
  [5] 异常路径   孤立的 TouchUp 安全返回
  [6] 设备能力位 内核记录的 PROP/ABS/KEY 位图逐个解码验证

test_integration（62 项）
  [1] Info 请求      协议往返正确
  [2] Capture 帧通道 fd 通过 SCM_RIGHTS 到达，可 mmap，
                     8,294,400 字节，渐变像素值逐个校验通过
  [3] Tap 全链路     坐标 321,654 穿过整条链路未被破坏
  [4] Swipe 全链路   终点坐标准确到达
  [5] 协议健壮性     错误 magic 被拒绝且连接不断；异常后正常请求仍可用
  [6] 空闲超时       连上不发数据的连接会被断开，且服务不被卡死
  [7] fd 泄漏        200 次抓帧后 fd 数量不变（10 → 10）
                     —— 常驻服务最要紧的一类问题
  [8] FrameHub 生命周期 最后一个订阅退出期间并发重订阅，验证线程停止与重启

test_capture_screencap（18 项）
  正常路径 / 非方形尺寸 / 连续抓帧不泄 fd /
  未知像素格式 / 输出截断 / 子进程非零退出 / 可执行文件不存在 /
  尺寸超范围被拒（边界值 16384 放行）
```

### 主机化改造抓到的 bug

把平台相关代码改成主机可编译，不只是为了测试——它直接暴露了三个**从未被编译过**
的代码里的真实缺陷：

**1. `SocketServer` 缺移动构造。**
禁用了拷贝构造但没提供移动构造，工厂函数 `FromPath()` / `FromInitSocket()` 里的
`return s;` 编译不过。这段代码在 AOSP 构建时**也会失败**。

**2. `rcctl` 的 `-o` 选项是坏的。**
usage 里写着 `-o 文件`，但 `getopt_long` 的短选项 `-o` 返回的是 `'o'`，
而 switch 里只处理了长选项对应的 `kOptOut` —— 于是 `-o 路径` 落进 `default`
被静默吞掉，输出永远落到默认的 `/data/local/tmp/shot.png`。
**这类 bug 只有真正跑一遍才会发现**，编译期完全看不出来。

**3. 测试自身的求值顺序缺陷。**
`Check(LastValueOf(...,&x) && x == 540, "...", x)` —— C++ 实参求值顺序未指定，
打印的是旧值。会造成「断言通过但诊断信息骗人」，掩盖真实故障。

> 这三个都是同一个思路的收益：**让代码在开发机上跑起来**。
> 不需要等 AOSP、不需要设备，就能发现整类问题。

---

## 🖥️ 在开发机上跑真正的 remote-control

平台相关的部分已经全部隔离（`ProcessState` 用 `#ifdef __ANDROID__` 包住、
截图后端可替换、日志有兼容层），所以**真正的 `remote-control` 二进制可以在开发机上编译运行**：

```bash
cd dev/02-native-daemon
make                  # 编译 remote-control-host + rcctl-host
sudo make run         # 前台跑起来
# 另开终端用真正的客户端连它
python3 client/rc_client.py --socket /tmp/remote-control-host.sock info
python3 client/rc_client.py --socket /tmp/remote-control-host.sock capture -o shot.png
python3 client/rc_client.py --socket /tmp/remote-control-host.sock tap 540 960
./rcctl-host --socket /tmp/remote-control-host.sock info     # 设备端 C++ 客户端

sudo make smoke       # 或者一条命令跑完：起服务 + 两个客户端 + 收尾
```

**实测输出**（两个客户端都验证）：

```
── Python 客户端 ──
分辨率: 1080 x 1920
已写入 /tmp/remote-control-host-shot.png（PNG，1080x1920）
已点击 (540, 960)
已滑动 (200,1600) -> (800,400)

── C++ 客户端 (rcctl) ──
显示数量: 1
分辨率:   1080 x 1920
已写入 /tmp/rcctl-shot.ppm (PPM)
已点击 (100, 200)
已滑动 (100,1700) -> (900,200)
```

> 主机上 `rcctl` 的 capture 走 PPM 分支（没有 `AndroidBitmap_compress`），
> 设备上走 PNG。两条路径的帧解析逻辑相同。

PNG / PPM 逐像素校验，与桩后端的渐变完全吻合：

```
左上角 RGB = (0, 0, 64)        右上角 RGB = (255, 0, 64)
左下角 RGB = (0, 255, 64)      右下角 RGB = (255, 255, 64)
中点   RGB = (127, 127, 64)
```

**这意味着：除了 SurfaceFlinger 抓屏和 SELinux 之外，整个服务已经可以在开发机上
完整运行和交互了。** 剩下的两个环节都只能在目标设备上验证。

其中 `[5]` 验证了 **`INPUT_PROP_DIRECT`** —— 这是最容易被忽略的一步，
不设它 Android 的 InputReader 不会把设备识别成触摸屏。

### 已修掉的原设计缺陷

原 `inject.h` 恒定发 `pointerCount = 1`，**做不了双指缩放**。
新的 uinput 后端实现了完整的协议 B 槽位管理（10 个槽位），
支持真正的多点触控。

### 编译期选后端

`Android.bp` 里定义了两个 `cc_binary`：

| 目标 | 后端 | 可用性 |
|---|---|---|
| `m remote-control` | uinput | ✅ Android 12 可用 |
| `m remote_control_binder` | IInputManager | ⚠️ Android 12 上加载会失败 |

两者不能链进同一个二进制——都定义 `CreateInjectorBackend()`，会重复符号。

### 已知代价

uinput 会创建一个**可枚举的输入设备**，出现在 `/proc/bus/input/devices` 和
`/sys/class/input/`。如果需要规避这一点，得走
[`docs/08-input-injection.md`](../../docs/08-input-injection.md) 的 Java 系统服务方案。

---

## 已验证的部分

在远程机器上跑过真实端到端回环（Python mock 服务端 ↔ 客户端）：

| 项目 | 结果 |
|---|---|
| `Request` / `Reply` 结构体布局（C++ ↔ Python） | 44 / 40 字节，两侧一致 ✓ |
| `info` / `capture` / `tap` / `swipe` 协议往返 | 全部通过 ✓ |
| `memfd` + `SCM_RIGHTS` 传 fd | 成功 ✓ |
| PNG 输出 | 320×240，全部 chunk CRC 校验通过 ✓ |

**自己复现这个测试**（不需要设备）：

```bash
cd client

# 终端 1：起 mock 服务端
python3 rc_client.py --mock /tmp/remote-control.sock --mock-size 1080x1920

# 终端 2：跑客户端
python3 rc_client.py --socket /tmp/remote-control.sock info
python3 rc_client.py --socket /tmp/remote-control.sock capture -o /tmp/shot.png
python3 rc_client.py --socket /tmp/remote-control.sock tap 540 1200
python3 rc_client.py --socket /tmp/remote-control.sock swipe 540 1600 540 400
```

---

## 下一步

原先列在这里的五项（SurfaceFlinger 编译验证、真机跑起来、SELinux 调通、
真机端到端、`KeyEvent`）**都已完成**，见上面的「现状」表。真正剩下的：

| 项 | 状态 | 说明 |
|---|---|---|
| 帧通道零拷贝优化（直接导出 dmabuf） | ⏳ **未做** | 现在仍是 `memcpy` 进 memfd。全仓库没有 dmabuf 相关代码 |
| 请求处理并发 | 🤔 **刻意不做** | `Dispatcher::Handle()` 里串行是**有意的**：`Injector` 有状态（按下/抬起、槽位映射、手势 downTime），并发注入会互相破坏手势。编码侧已用 `encode_pool` 限流；HTTP 已有独立线程，不会和触控互相排队 |
| 文本输入（`input text` 那类） | ⏳ **未做** | 按键注入（`keyboard.cpp`）已有；整串文本输入没有 |
| 剪贴板写入 | ⛔ **平台做不到** | Android 10+ 只允许前台应用写。**不是本项目的缺陷**，见 [`docs/api/01-http.md`](../../docs/api/01-http.md) 第五节 |
| 热替换（启动后换二进制） | ⛔ **实测否决** | 三条 neverallow 互相咬住，两条绕开的路也都不通。见 [`docs/09`](../../docs/09-deployment-and-update.md) §9.2–9.5 |

---

## 分阶段落地

### 阶段 1：免 SELinux 原型

**不要一上来就写 sepolicy。** 先在已经宽松的环境里把链路跑通。

在 Magisk root 的真机、Cuttlefish（userdebug），或
**[`../04-android-rom/`](../04-android-rom/README.md) 里起的模拟器**上
（免真机路径；`ro.product.cpu.abilist` 含 `arm64-v8a`，arm64 的 `remote-control` 可直接跑）：

```bash
# 1. 编好 remote-control 和 rcctl 后
adb push $ANDROID_PRODUCT_OUT/system/bin/remote-control    /data/local/tmp/
adb push $ANDROID_PRODUCT_OUT/system/bin/rcctl /data/local/tmp/
adb shell chmod 755 /data/local/tmp/remote-control /data/local/tmp/rcctl

# 2. 以 root 身份前台跑（跳过 remote-control.rc 和整个 sepolicy/）
adb shell "/data/local/tmp/remote-control --socket /data/local/tmp/remote-control.sock --foreground &"

# 3. 冒烟测试
adb shell "/data/local/tmp/rcctl --socket /data/local/tmp/remote-control.sock info"
adb shell "/data/local/tmp/rcctl --socket /data/local/tmp/remote-control.sock capture -o /data/local/tmp/shot.png"
adb shell "/data/local/tmp/rcctl --socket /data/local/tmp/remote-control.sock tap 540 1200"
```

**目标只有一个**：确认 `captureDisplay()` 能拿到帧、触控能点中。

也可以用脚本自动化（在 `dev/02-native-daemon/` 目录下执行）：

```bash
./tools/deploy_cuttlefish.sh /path/to/remote-control /path/to/rcctl
```

> 注意：`deploy_cuttlefish.sh` 目前写的是新版 `cvd` 工具链的用法。
> **Android 12 用的是老的 `launch_cvd`**，需要相应调整。
>
> 在 `04-android-rom` 的模拟器上请见
> [`../04-android-rom/docs/05-adding-components.md`](../04-android-rom/docs/05-adding-components.md)：
> 该文给了三种加组件的方式（运行时推 / Magisk 模块 / 编进 ROM），
> 阶段 1 用其中的"运行时推到 `/data/local/tmp`"即可，不需要 remount。

### 阶段 2：固化 init 服务 + sepolicy

> **状态：已完成。** 实际落地的形态（含与计划的差异）记在
> [`docs/09`](../../docs/09-deployment-and-update.md) §9.1。这里保留原计划便于对照。

1. `daemon/remote-control.rc` → 编进 `/system/etc/init/`
2. sepolicy 落到设备树 `dev/04-android-rom/device/remote_control_x64_arm64/sepolicy/`
   （不是 `system/sepolicy/private/`，那样会撞 `sepolicy_freeze_test`）：
   先用 `permissive remote_control;` 定位，再逐条加 allow
3. 开机自启 —— ⚠️ **实际用的是 `shell` UID，不是 `system`**，两个原因都实测过：
   ① `system`(1000) 读写不了 `/sdcard`，那是**存储层（FUSE）**挡的，加 sepolicy 无效；
   ② 抓帧走的是 shell 应用显式申请的 `READ_FRAME_BUFFER`。
   详见 [`docs/09`](../../docs/09-deployment-and-update.md) §4.4。

**「工作量最大、最容易卡住」这个判断是对的** —— 实际还撞到两个计划里没写、
但会直接挡住构建的问题：产品清单要补 `PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST`，
以及策略不能放 `system/sepolicy/private/`。两条都记在
[`docs/09`](../../docs/09-deployment-and-update.md) §9.1。

绕开方式：用 Magisk 把二进制和 rc 塞进系统，效果和刷机一样，不需要编镜像。

### 阶段 3：性能与产品化

| 项 | 状态 |
|---|---|
| 帧通道改直接导出 `GraphicBuffer` 的 dmabuf fd（省一次 memcpy） | ⏳ **未做** —— 现在仍是 `memcpy` 进 memfd |
| 加请求处理线程池（当前是串行） | 🤔 **刻意不做** —— 串行是有意的，理由见上面「下一步」 |
| 实现 `KeyEvent` / 按键注入 | ✅ **已完成**（`keyboard.cpp`，79 项测试） |
| 真多点触控 | ✅ **已完成**（uinput 协议 B 槽位管理，10 个槽位） |
| 对外网络暴露（[`docs/01-selection.md`](../../docs/01-selection.md) 第 7 节约束 6） | ✅ **已完成**（`--http-bind`，配套 `tools/lan-up.sh` / `lan-forward.py`） |

---

## 编译

```bash
# 一次性（~85 GB）
repo init -u https://android.googlesource.com/platform/manifest -b android-12.0.0_r34
repo sync -c --depth=1 --no-tags -j8

# 每次改代码
source build/envsetup.sh
lunch aosp_arm64-userdebug     # 只编二进制，不需要 vendor blobs
m remote-control rcctl

# 产物
ls $ANDROID_PRODUCT_OUT/system/bin/remote-control
```

**放置位置**：把 `daemon/`、`client/` 拷进 AOSP 树的 `frameworks/native/cmds/remote-control/`，
保持 `daemon/` 和 `client/` 同级（`rcctl.cpp` 里 `#include "../daemon/protocol.h"`）。

---

## 关键实现注意

| 坑 | 后果 | 处理 |
|---|---|---|
| 忘记 `ProcessState::startThreadPool()` | `waitForResults()` 死等 | 已在 `main.cpp` 处理 |
| 逐行拷贝用 `getWidth()` 而非 `getStride()` | 图像斜切 | 已在 `capture_surfaceflinger.cpp` 处理 |
| 忘记 `buffer->unlock()` | SurfaceFlinger 缓冲耗尽 | 已在 `capture_surfaceflinger.cpp` 处理 |
| 结构体隐式填充 | 读到垃圾数据 | 已用 `static_assert` + `assert` 锁死 |
| Binder 装不下整帧 | 事务失败 | 已用 memfd + `SCM_RIGHTS` 规避 |

---

## 相关文档

- 方案选型 → `../../docs/01-selection.md`
- 架构与协议 → `../../docs/02-architecture.md`
- Android 12 触控约束 → [`docs/01-selection.md`](../../docs/01-selection.md) 第 7 节
- 硬件与构建 → [`docs/04-environment.md`](../../docs/04-environment.md)
- 抓帧性能实测 → [`docs/06-capture-performance.md`](../../docs/06-capture-performance.md)

---

## 如果暂时不想投入 AOSP

先跑 `tools/build-ndk.sh` 出 NDK 版（[`docs/04-environment.md`](../../docs/04-environment.md)）——当天就能验证思路，
协议层可以原样复用。
