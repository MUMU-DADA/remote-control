# 02 · AOSP Native Daemon（主线）

> `autod`：Android 系统内的常驻 native 进程，对外提供截图与触控。
> **这是本项目的长期主线。**

---

## 现状

| 项 | 状态 |
|---|---|
| 协议层 + socket 层 | ✅ **已实现，主机验证通过** |
| 请求分发（`dispatch.cpp`） | ✅ **已抽出，被集成测试覆盖** |
| **触控注入（uinput 后端）** | ✅ **已实现，真实设备测试通过** |
| 触控注入（binder 后端） | ⚠️ 代码就绪，Android 12 上不可用（AIDL 无 cpp backend） |
| 帧通道（memfd + `SCM_RIGHTS`） | ✅ **已实现，像素级校验通过** |
| 截图（`capture_surfaceflinger.cpp`） | ⚠️ **未在真实 AOSP 环境编译验证** |
| 截图（`capture_stub.cpp`） | ✅ 桩后端，供主机测试用 |

---

## 目录结构

```
02-native-daemon/
├── daemon/
│   ├── Android.bp                    AOSP 构建（3 个可执行目标）
│   ├── protocol.h                    对外协议（含布局断言 + ReplyPacket）
│   ├── main.cpp                      入口 / 选项 / 信号（分发已抽出）
│   ├── dispatch.{h,cpp}              请求分发 ← 集成测试直接复用
│   ├── socket_server.{h,cpp}         socket + SCM_RIGHTS + SO_PEERCRED
│   ├── autod_log.h                   日志兼容层（Android liblog / 主机 stderr）
│   │
│   ├── capture.h                     截图对外接口
│   ├── capture_surfaceflinger.cpp    SurfaceFlinger 后端 ← 需要 AOSP 环境
│   ├── capture_stub.cpp              桩后端 ← 合成图像，主机可测
│   │
│   ├── inject.h                      触控对外接口
│   ├── inject_backend.h              后端接口（内部）
│   ├── inject.cpp                    手势逻辑（平台无关）
│   ├── inject_uinput.cpp             /dev/uinput 后端 ← Android 12 上唯一可用
│   └── inject_binder.cpp             IInputManager 后端 ← Android 12 上不可用
│
├── tests/                            主机侧测试（g++ 直接编译，不需要 AOSP）
│   ├── Makefile
│   ├── test_util.h                   公共工具
│   ├── test_inject_uinput.cpp        注入后端单元测试（31 项）
│   └── test_integration.cpp          端到端集成测试（32 项）
│
├── client/
│   ├── Android.bp
│   ├── autodctl.cpp                  设备端 C++ 客户端
│   └── autod_client.py               Python 参考客户端 + mock 服务端
├── init/
│   └── autod.rc                      init 服务定义
├── sepolicy/
│   ├── autod.te                      SELinux domain 模板
│   └── file_contexts
└── tools/
    └── deploy_cuttlefish.sh          部署 + 冒烟测试
```

### 后端可替换

截图和触控都做成了**后端可插拔**，编译期选择：

| 二进制 | 截图后端 | 触控后端 | 用途 |
|---|---|---|---|
| `m autod` | SurfaceFlinger | uinput | ✅ 产品默认 |
| `m autod_binder` | SurfaceFlinger | IInputManager | Android 13+ 或自改 AIDL |
| `m autod_stub` | **桩** | uinput | 无显示设备的 CI 冒烟测试 |

每个二进制只链一个后端——它们都定义同名工厂函数，链在一起会重复符号。

**这个设计让开发不必等 AOSP**：`capture_stub.cpp` 生成合成图像，
于是整条链路（socket → 协议 → 分发 → 注入/帧通道）都能在开发机上测试。

---

## ✅ 触控注入：uinput 后端已实现并验证

**Android 12 的 `IInputManager` 是 Java-only AIDL，native 进程调不了**
（完整论证见 [`../../docs/06-constraints.md`](../../docs/06-constraints.md) 约束 1）。
已实现的 `inject_uinput.cpp` 走 `/dev/uinput`，绕开这个限制。

### 验证方式：真实设备，不是 mock

```bash
cd tests
sudo make run   # 编译并运行全部测试
```

| 测试 | 覆盖范围 | 检查项 |
|---|---|---|
| `test_inject_uinput` | 注入后端本身 | 31 |
| `test_integration` | **端到端**：socket → dispatch → 帧通道 / 注入 → 内核 | 32 |

集成测试用的是**真实的** `Dispatcher` / `SocketServer` / `Injector`，
只把截图后端换成桩（`capture_stub.cpp`）。覆盖链路：

```
客户端 ──SOCK_SEQPACKET──> SocketServer ──> Dispatcher ──┬──> Capture(桩)
                                                          │      └─ memfd ──SCM_RIGHTS──> mmap 校验像素
                                                          └──> Injector
                                                                  └─ /dev/uinput ──> 内核 ──> eventN 读回
```

**实测结果：63 项检查全部通过**

```
test_inject_uinput（31 项）
  [1] 单击       事件序列正确（BTN_TOUCH / TRACKING_ID / 坐标 / 压力 全对）
  [2] 滑动       200ms 产生 12 个 MOVE，坐标从 200 单调走到 800
  [3] 双指多点   使用 2 个槽位、2 个 TRACKING_ID，BTN_TOUCH 只在首尾各置位一次
  [4] 异常路径   孤立的 TouchUp 安全返回
  [5] 设备能力位 内核记录的 PROP/ABS/KEY 位图逐个解码验证

test_integration（32 项）
  [1] Info 请求      协议往返正确
  [2] Capture 帧通道 fd 通过 SCM_RIGHTS 到达，可 mmap，
                     8,294,400 字节，渐变像素值逐个校验通过
  [3] Tap 全链路     坐标 321,654 穿过整条链路未被破坏
  [4] Swipe 全链路   终点坐标准确到达
  [5] 协议健壮性     错误 magic 被拒绝且连接不断；异常后正常请求仍可用
```

### 集成测试直接抓到的 bug

`SocketServer` 禁用了拷贝构造但**没有提供移动构造**，工厂函数
`FromPath()` / `FromInitSocket()` 里的 `return s;` 编译不过。

这段代码此前从未被编译过——**在 AOSP 构建时也会失败**。已补上移动构造与移动赋值。

> 这也是把 `socket_server.cpp` 改成主机可编译的价值：不需要等 AOSP 同步完
> 就能发现这类问题。

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
| `m autod` | uinput | ✅ Android 12 可用 |
| `m autod_binder` | IInputManager | ⚠️ Android 12 上加载会失败 |

两者不能链进同一个二进制——都定义 `CreateInjectorBackend()`，会重复符号。

### 已知代价

uinput 会创建一个**可枚举的输入设备**，出现在 `/proc/bus/input/devices` 和
`/sys/class/input/`。如果需要规避这一点，得走
[`../03-java-service/`](../03-java-service/README.md) 的 Java 系统服务方案。

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
python3 autod_client.py --mock /tmp/autod.sock --mock-size 1080x1920

# 终端 2：跑客户端
python3 autod_client.py --socket /tmp/autod.sock info
python3 autod_client.py --socket /tmp/autod.sock capture -o /tmp/shot.png
python3 autod_client.py --socket /tmp/autod.sock tap 540 1200
python3 autod_client.py --socket /tmp/autod.sock swipe 540 1600 540 400
```

---

## 下一步

触控后端已完成。剩余工作：

| 项 | 状态 | 依赖 |
|---|---|---|
| 截图（`capture_surfaceflinger.cpp`）编译验证 | ⏳ 等 AOSP 同步 | `frameworks/native/libs/gui` |
| 在真机上跑 `autod`（`--inject-backend` 已就绪） | ⏳ 待开始 | 一台 root 的 ARM64 设备 |
| SELinux 规则调通 | ⏳ 待开始 | 上面两项 |
| 真机端到端（真实截图 + 触控） | ⏳ 待开始 | 上面三项 |
| `KeyEvent` / 文本输入 | ⏳ 未实现 | —— |
| 帧通道零拷贝优化（直接导出 dmabuf） | ⏳ 未实现 | —— |

---

## 分阶段落地

### 阶段 1：免 SELinux 原型

**不要一上来就写 sepolicy。** 先在已经宽松的环境里把链路跑通。

在 Magisk root 的真机或 Cuttlefish（userdebug）上：

```bash
# 1. 编好 autod 和 autodctl 后
adb push $ANDROID_PRODUCT_OUT/system/bin/autod    /data/local/tmp/
adb push $ANDROID_PRODUCT_OUT/system/bin/autodctl /data/local/tmp/
adb shell chmod 755 /data/local/tmp/autod /data/local/tmp/autodctl

# 2. 以 root 身份前台跑（跳过 autod.rc 和整个 sepolicy/）
adb shell "/data/local/tmp/autod --socket /data/local/tmp/autod.sock --foreground &"

# 3. 冒烟测试
adb shell "/data/local/tmp/autodctl --socket /data/local/tmp/autod.sock info"
adb shell "/data/local/tmp/autodctl --socket /data/local/tmp/autod.sock capture -o /data/local/tmp/shot.png"
adb shell "/data/local/tmp/autodctl --socket /data/local/tmp/autod.sock tap 540 1200"
```

**目标只有一个**：确认 `captureDisplay()` 能拿到帧、触控能点中。

也可以用脚本自动化（在 `dev/02-native-daemon/` 目录下执行）：

```bash
./tools/deploy_cuttlefish.sh /path/to/autod /path/to/autodctl
```

> 注意：`deploy_cuttlefish.sh` 目前写的是新版 `cvd` 工具链的用法。
> **Android 12 用的是老的 `launch_cvd`**，需要相应调整。

### 阶段 2：固化 init 服务 + sepolicy

原型通了之后再补：

1. `init/autod.rc` → 编进 `/system/etc/init/`
2. `sepolicy/autod.te` → 用 `permissive autod;` 定位，再逐条加 allow
3. 改成 `system` UID 开机自启

**这是工作量最大、最容易卡住的部分。**

绕开方式：用 Magisk 把二进制和 rc 塞进系统，效果和刷机一样，不需要编镜像。

### 阶段 3：性能与产品化

- 帧通道改直接导出 `GraphicBuffer` 的 dmabuf fd（省一次 memcpy）
- 加请求处理线程池（当前是串行）
- 实现 `KeyEvent`、真多点触控
- 对外网络暴露（需要独立的网关层，见 `../../docs/06-constraints.md` 约束 6）

---

## 编译

```bash
# 一次性（~85 GB）
repo init -u https://android.googlesource.com/platform/manifest -b android-12.0.0_r34
repo sync -c --depth=1 --no-tags -j8

# 每次改代码
source build/envsetup.sh
lunch aosp_arm64-userdebug     # 只编二进制，不需要 vendor blobs
m autod autodctl

# 产物
ls $ANDROID_PRODUCT_OUT/system/bin/autod
```

**放置位置**：把 `daemon/`、`client/` 拷进 AOSP 树的 `frameworks/native/cmds/autod/`，
保持 `daemon/` 和 `client/` 同级（`autodctl.cpp` 里 `#include "../daemon/protocol.h"`）。

---

## 关键实现注意

| 坑 | 后果 | 处理 |
|---|---|---|
| 忘记 `ProcessState::startThreadPool()` | `waitForResults()` 死等 | 已在 `main.cpp` 处理 |
| 逐行拷贝用 `getWidth()` 而非 `getStride()` | 图像斜切 | 已在 `capture.cpp` 处理 |
| 忘记 `buffer->unlock()` | SurfaceFlinger 缓冲耗尽 | 已在 `capture.cpp` 处理 |
| 结构体隐式填充 | 读到垃圾数据 | 已用 `static_assert` + `assert` 锁死 |
| Binder 装不下整帧 | 事务失败 | 已用 memfd + `SCM_RIGHTS` 规避 |

---

## 相关文档

- 方案选型 → `../../docs/01-selection.md`
- 架构与协议 → `../../docs/02-architecture.md`
- Android 12 触控约束 → `../../docs/06-constraints.md`
- 硬件与构建 → `../../docs/04-hardware.md`
- 延迟数据 → `../../docs/05-latency-and-touch.md`

---

## 如果暂时不想投入 AOSP

先做 [`../01-ndk-prototype/`](../01-ndk-prototype/README.md)——当天就能验证思路，
协议层可以原样复用。
