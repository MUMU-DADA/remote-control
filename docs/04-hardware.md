# 04 · 硬件与构建环境

> 目标：Android 12 / ARM64。本文给出硬件配置建议与磁盘预算。

---

## 1. 先说结论：不需要全量编译

这一点经常被误解，先拆开看：

| 步骤 | 必须吗 | 规模 |
|---|---|---|
| `repo sync` 拉源码 | ✅ **必须** | ~85 GB（`--depth=1`），一次性 |
| `m autod` 编模块 | ✅ **必须** | 首次 30–90 分钟，之后**几分钟** |
| 编完整系统镜像 | ❌ **不必须** | 额外 ~65 GB + 1–2 小时 |
| 刷机 | ❌ **不必须** | — |

**贵的是源码树，不是编译。**

### 为什么躲不掉 `repo sync`

`autod` 用了这些平台私有库，**头文件只存在于 AOSP 源码里，NDK 里没有**：

| 头文件 | 位置 |
|---|---|
| `gui/SurfaceComposerClient.h` | `frameworks/native/libs/gui/` |
| `gui/SyncScreenCaptureListener.h` | 同上 |
| `android/gui/DisplayCaptureArgs.h` | 同上 |
| `input/Input.h` | `frameworks/native/libs/input/` |
| `android/hardware/input/IInputManager.h` | AIDL 生成 |

### 什么时候才真的需要全量编译

只有三种情况：

1. 把 `autod` 装进 `/system/bin/` 并开机自启
2. 走 Java 系统服务方案（系统服务必须编进镜像）
3. 给设备刷自定义 ROM

**但这三种都能用 Magisk 绕开**——Magisk 能往 `/system/bin/` 塞文件、注入 init 脚本，效果和刷机一样，且不需要编任何镜像。

---

## 2. 构建主机 vs 测试目标

**关键：编译 ARM64 不需要 ARM64 机器。**

这是标准的交叉编译：

| | 架构 | 说明 |
|---|---|---|
| **构建主机** | **x86_64** | 永远是 x86_64 Linux |
| **目标** | aarch64 | `lunch` 时选，产物是 ARM64 ELF |

所有给真机编 AOSP 的人都是这么干的。

### 测试环境的坑：ARM64 Cuttlefish

Cuttlefish 用 crosvm + KVM。**跨架构没有 KVM 加速**，而 Cuttlefish 不支持 QEMU TCG 全模拟。

→ **ARM64 Cuttlefish 需要 ARM64 主机**（Ampere Altra、AWS Graviton 这类服务器）。不建议为此专门买机器。

**推荐替代路径**：

```
x86_64 Cuttlefish  →  快速迭代、调 Binder 调用、验证协议和链路
       ↓ 逻辑跑通
ARM64 真机        →  最终验证、测真实延迟
```

`autod` 源码是架构无关的（`protocol.h` 用定长类型，Python 客户端用 `"<"` 小端格式，aarch64 上同样成立）。两个目标都编一遍，首次之后增量都很快。

---

## 3. 四个硬指标

| 指标 | 最低 | 推荐 | 说明 |
|---|---|---|---|
| **内存** | 32 GB | **64 GB** | 经验值 ~2 GB / 并行任务。16 核配 64 GB 刚好 |
| **磁盘** | 500 GB | **1 TB NVMe** | 见下方预算 |
| **CPU 核数** | 8 核 | **16 核** | 编译几乎线性受益于核数 |
| **KVM** | 需要（若用 Cuttlefish） | 需要 | `egrep -c '(vmx\|svm)' /proc/cpuinfo` > 0 |

---

## 4. 磁盘预算

### 全量 vs 精简

| 项目 | 全量做法 | 精简做法 |
|---|---|---|
| Ubuntu 22.04 + 工具链 | 30 GB | 25 GB |
| AOSP 12 源码 | 110 GB | **85 GB** |
| `out/` 构建产物 | 100 GB | **35 GB** |
| ccache | 100 GB | **0** |
| Cuttlefish 镜像 + 实例 | 40 GB | **20 GB** |
| 余量 | — | 35 GB |
| **合计** | **430 GB** | **200 GB** |

### 四个省空间的开关

1. **`repo sync --depth=1 -c --no-tags`** → 省 ~25 GB
2. **不开 ccache** → 省 100 GB。代价：`make clean` 后要全量重编。只要不 clean，增量编译完全正常
3. **只 `m autod`，不编系统镜像** → 省 ~65 GB ← **单项最大**
4. **Cuttlefish 用预编译镜像** → 省 ~20 GB

编完之后 `autod` 二进制只有几 MB，可以删掉 `out/` 再进下一轮。

### 结论

**买 1 TB NVMe，按 500 GB 规划用量。**

500 GB 的盘现在市场上越来越少，也没比 1 TB 便宜多少。1 TB 让你不用天天盯磁盘，而实际占用按精简做法只有 200 GB 左右。

### 选盘注意

- **必须 NVMe**。SATA SSD 或机械盘做 AOSP 构建是自虐，构建过程 I/O 极重
- **要有 DRAM 缓存**，别买无缓盘，写入放大会很快磨废
- 关注 **TBW** 指标

---

## 5. 三档配置

### 入门档 ~¥7500

| 部件 | 型号 |
|---|---|
| CPU | Ryzen 7 9700X（8C/16T） |
| 主板 | B650 |
| 内存 | 64 GB DDR5-5600 |
| 硬盘 | 1 TB NVMe PCIe 4.0 |
| 电源 | 750W |
| 显卡 | 核显即可 |

首次全量编译约 3–5 小时。够用，但重构时会难受。

### 推荐档 ~¥11000 ← 甜点

| 部件 | 型号 |
|---|---|
| CPU | **Ryzen 9 9950X**（16C/32T） |
| 主板 | X670E |
| 内存 | 64 GB DDR5-6000 |
| 硬盘 | 1 TB NVMe PCIe 4.0（预算够上 2 TB） |
| 电源 | 850W |
| 显卡 | 核显即可 |

首次全量编译约 1–1.5 小时，日常 `m autod` 几分钟。

### 高配档 ~¥29000

| 部件 | 型号 |
|---|---|
| CPU | Threadripper 7960X（24C/48T） |
| 主板 | TRX50 |
| 内存 | 128 GB DDR5 RDIMM |
| 硬盘 | 4 TB NVMe |
| 电源 | 1000W |

除非要同时跑多个 Cuttlefish 实例，否则没必要。

### 显卡要不要买

- **编译完全不需要**（CPU + I/O 密集）
- **Cuttlefish 需要**：
  - 无 GPU：`--gpu_mode=guest_swiftshader`，软件渲染，慢但能跑
  - 有 GPU：`--gpu_mode=auto`，走 virtio-gpu / gfxstream

验证链路功能，软件渲染够用。**但测真实延迟必须用 GPU**——因为 `captureDisplay` 的瓶颈就是 GPU 合成那一步，软件渲染的数字没有参考价值。

**建议：先核显跑通，要测延迟时再补 AMD / Intel 独显**（NVIDIA 在 Cuttlefish 上历史包袱较多）。

---

## 6. 软件环境

**系统必须 Ubuntu 22.04 LTS。** AOSP 12 官方只支持 Ubuntu 20.04 / 22.04。Debian 能编但会踩依赖坑，不值得。

**Android 12 的 Cuttlefish 用的是老工具链**——那时还没有 `cvd`，用的是 `launch_cvd`。编译目标是 `aosp_cf_x86_64_phone`。查文档注意对应版本。

---

## 7. 云构建机

| 优点 | 缺点 |
|---|---|
| 32C64G 约 ¥10–20/小时 | **必须支持嵌套虚拟化**才能跑 Cuttlefish，下单前找客服确认 |
| 首次全量编译两三小时就几十块 | 镜像上传下载费时间 |
| 不用管硬件 | 长期高频用不如自购 |

**省钱思路**：源码同步和首次编译是一次性成本。折中方案是**云上做一次全量构建 → 把 `out/` 拉回本地 → 之后本地增量编译**。

本地增量只需要 16 GB 内存，一台普通机器就够。

---

## 8. 采购清单

| 项目 | 说明 | 参考价 |
|---|---|---|
| 构建主机 | 9950X + 64 GB + 1 TB NVMe + Ubuntu 22.04 | ~¥11000 |
| **ARM64 测试机** | **Pixel 6 / 6a，解锁 bootloader + Magisk** | **~¥1000–1500（二手）** |
| 独显 | 仅在要测延迟时需要 | ¥0–2000 |

**推荐测试机：Pixel 6 / 6a**

- 出厂即 Android 12
- bootloader 可解锁
- AOSP 有完整设备树
- Magisk root 后 `autod` 可直接从 `/data/local/tmp` 跑

---

## 9. 快速开始命令

```bash
# 一次性（~85 GB）
repo init -u https://android.googlesource.com/platform/manifest -b android-12.0.0_r34
repo sync -c --depth=1 --no-tags -j8

# 每次改代码（首次 30–90 分钟，之后几分钟）
source build/envsetup.sh
lunch aosp_arm64-userdebug
m autod

# 部署（需要 Magisk root）
adb push $ANDROID_PRODUCT_OUT/system/bin/autod /data/local/tmp/
adb shell chmod 755 /data/local/tmp/autod
```

`lunch` 目标选择：

| 目标 | 用途 | 需要 vendor blobs |
|---|---|---|
| `aosp_arm64-userdebug` | 只编 `autod` | ❌ |
| `aosp_oriole-userdebug` | Pixel 6 完整镜像 | ✅ |
| `aosp_raven-userdebug` | Pixel 6 Pro 完整镜像 | ✅ |
| `aosp_cf_x86_64_phone` | Cuttlefish 测试 | ❌ |

---

## 10. 相关文档

- 构建产物怎么部署 → `../dev/02-native-daemon/README.md`
- 纯 NDK 免源码树方案 → `../dev/01-ndk-prototype/README.md`
- 延迟数据 → `05-latency-and-touch.md`
