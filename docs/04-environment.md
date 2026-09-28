# 环境与构建

> 目标 Android 12 / ARM64。本文给出硬件配置建议、磁盘预算与构建流程，末尾是本机实际环境记录（**全程使用国内镜像源**）。

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

### 1.1 为什么躲不掉 `repo sync`

`autod` 用了这些平台私有库，**头文件只存在于 AOSP 源码里，NDK 里没有**：

| 头文件 | 位置 |
|---|---|
| `gui/SurfaceComposerClient.h` | `frameworks/native/libs/gui/` |
| `gui/SyncScreenCaptureListener.h` | 同上 |
| `android/gui/DisplayCaptureArgs.h` | 同上 |
| `input/Input.h` | `frameworks/native/libs/input/` |
| `android/hardware/input/IInputManager.h` | AIDL 生成 |

### 1.2 什么时候才真的需要全量编译

只有三种情况：

1. 把 `autod` 装进 `/system/bin/` 并开机自启
2. 走 Java 系统服务方案（系统服务必须编进镜像）
3. 给设备刷自定义 ROM

**但这三种都能用 Magisk 绕开**——Magisk 能往 `/system/bin/` 塞文件、注入 init 脚本，效果和刷机一样，且不需要编任何镜像。

---

## 2. 构建主机 vs 测试目标

**关键：编译 ARM64 不需要 ARM64 机器。** 这是标准的交叉编译：

| | 架构 | 说明 |
|---|---|---|
| **构建主机** | **x86_64** | 永远是 x86_64 Linux |
| **目标** | aarch64 | `lunch` 时选，产物是 ARM64 ELF |

所有给真机编 AOSP 的人都是这么干的。

### 2.1 测试环境的坑：ARM64 Cuttlefish

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

### 4.1 全量 vs 精简

| 项目 | 全量做法 | 精简做法 |
|---|---|---|
| Ubuntu 22.04 + 工具链 | 30 GB | 25 GB |
| AOSP 12 源码 | 110 GB | **85 GB** |
| `out/` 构建产物 | 100 GB | **35 GB** |
| ccache | 100 GB | **0** |
| Cuttlefish 镜像 + 实例 | 40 GB | **20 GB** |
| 余量 | — | 35 GB |
| **合计** | **430 GB** | **200 GB** |

### 4.2 四个省空间的开关

1. **`repo sync --depth=1 -c --no-tags`** → 省 ~25 GB
2. **不开 ccache** → 省 100 GB。代价：`make clean` 后要全量重编。只要不 clean，增量编译完全正常
3. **只 `m autod`，不编系统镜像** → 省 ~65 GB ← **单项最大**
4. **Cuttlefish 用预编译镜像** → 省 ~20 GB

编完之后 `autod` 二进制只有几 MB，可以删掉 `out/` 再进下一轮。

### 4.3 结论

**买 1 TB NVMe，按 500 GB 规划用量。** 500 GB 的盘现在市场上越来越少，也没比 1 TB 便宜多少。1 TB 让你不用天天盯磁盘，而实际占用按精简做法只有 200 GB 左右。

**选盘注意**：

- **必须 NVMe**。SATA SSD 或机械盘做 AOSP 构建是自虐，构建过程 I/O 极重
- **要有 DRAM 缓存**，别买无缓盘，写入放大会很快磨废
- 关注 **TBW** 指标

---

## 5. 机器怎么配

| 档 | CPU / 主板 | 内存 | 硬盘 | 电源 | 参考价 | 首次全量编译 |
|---|---|---|---|---|---|---|
| 入门 | Ryzen 7 9700X（8C/16T）/ B650 | 64 GB DDR5-5600 | 1 TB NVMe PCIe 4.0 | 750W | ~¥7500 | 3–5 小时，重构时难受 |
| **推荐 ← 甜点** | **Ryzen 9 9950X（16C/32T）/ X670E** | 64 GB DDR5-6000 | 1 TB NVMe PCIe 4.0（预算够上 2 TB） | 850W | ~¥11000 | 1–1.5 小时；日常 `m autod` 几分钟 |
| 高配 | Threadripper 7960X（24C/48T）/ TRX50 | 128 GB DDR5 RDIMM | 4 TB NVMe | 1000W | ~¥29000 | 除非要同时跑多个 Cuttlefish 实例，否则没必要 |

三档都是**核显即可**，编译完全不需要显卡（CPU + I/O 密集）。

**Cuttlefish 需要 GPU**：

- 无 GPU：`--gpu_mode=guest_swiftshader`，软件渲染，慢但能跑
- 有 GPU：`--gpu_mode=auto`，走 virtio-gpu / gfxstream

验证链路功能，软件渲染够用。**但测真实延迟必须用 GPU**——因为 `captureDisplay` 的瓶颈就是 GPU 合成那一步，软件渲染的数字没有参考价值。**建议：先核显跑通，要测延迟时再补 AMD / Intel 独显**（NVIDIA 在 Cuttlefish 上历史包袱较多）。

### 5.1 云构建机

| 优点 | 缺点 |
|---|---|
| 32C64G 约 ¥10–20/小时 | **必须支持嵌套虚拟化**才能跑 Cuttlefish，下单前找客服确认 |
| 首次全量编译两三小时就几十块 | 镜像上传下载费时间 |
| 不用管硬件 | 长期高频用不如自购 |

**省钱思路**：源码同步和首次编译是一次性成本。折中方案是**云上做一次全量构建 → 把 `out/` 拉回本地 → 之后本地增量编译**。本地增量只需要 16 GB 内存，一台普通机器就够。

### 5.2 采购清单

| 项目 | 说明 | 参考价 |
|---|---|---|
| 构建主机 | 9950X + 64 GB + 1 TB NVMe + Ubuntu 22.04 | ~¥11000 |
| **ARM64 测试机** | **Pixel 6 / 6a，解锁 bootloader + Magisk** | **~¥1000–1500（二手）** |
| 独显 | 仅在要测延迟时需要 | ¥0–2000 |

**推荐测试机：Pixel 6 / 6a**——出厂即 Android 12；bootloader 可解锁；AOSP 有完整设备树；Magisk root 后 `autod` 可直接从 `/data/local/tmp` 跑。

---

## 6. 软件环境

**系统必须 Ubuntu 22.04 LTS。** AOSP 12 官方只支持 Ubuntu 20.04 / 22.04。Debian 能编但会踩依赖坑，不值得（本机因此改用容器，见 8.4）。

**Android 12 的 Cuttlefish 用的是老工具链**——那时还没有 `cvd`，用的是 `launch_cvd`。编译目标是 `aosp_cf_x86_64_phone`。查文档注意对应版本。

---

## 7. 快速开始命令

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

## 8. 本机实际环境

### 8.1 硬件与并行度

| 项 | 配置 |
|---|---|
| CPU | AMD Ryzen 9 5950X，**16 vCPU** |
| 内存 | **31 GiB** |
| Swap | **35 GiB**（34 GiB 专用分区 + 1.1 GiB 原有） |
| 数据盘 | **458 GiB** ext4（`/dev/sda1`，挂在项目目录） |
| 宿主系统 | Debian GNU/Linux 13 (trixie)，内核 6.12 |
| 虚拟化 | VMware 虚拟机，`/dev/kvm` 可用 |

**并行度建议：`-j12`。** 经验值约 2 GB 内存 / 并行任务，31 GiB 内存跑 `-j16` 会触发 OOM。`repo sync` 用 `-j4`（清华镜像站限流，调大会大量 503）。

### 8.2 磁盘布局

```
/dev/sda  500 GiB  VMware Virtual S
├── sda1  466 GiB  ext4  →  /root/AutoSnapshotAndroid
│   ├── README.md, SUMMARY.md, docs/, dev/, tools/    ← 项目文件
│   └── aosp/                                          ← AOSP 源码树
└── sda2   34 GiB  swap

/dev/sdb   20 GiB  VMware Virtual S
├── sdb1  18.5 GiB ext4  →  /          （根分区，剩 ~16 GiB）
└── sdb5   1.1 GiB swap
```

**为什么要分两个分区**：swapfile 放在项目目录里会被工作区同步机制当成普通文件下载，一个 34 GB 的文件会造成同步灾难。用独立 swap 分区彻底避开。

`/etc/fstab`：

```
UUID=ecbe983c-41a1-46eb-974b-dc61c52cdabe /root/AutoSnapshotAndroid ext4 defaults,noatime 0 2
UUID=e86f4d06-0dbc-4f6f-bf9b-19603ec93917 none swap sw,pri=10 0 0
```

`noatime` 减少构建期间的写入量。

### 8.3 镜像源

**全部走国内源**，因为 `android.googlesource.com` 在本机不可达。

| 环节 | 源 |
|---|---|
| Debian apt | `https://mirrors.tuna.tsinghua.edu.cn/debian` |
| Docker CE 安装 | `https://mirrors.tuna.tsinghua.edu.cn/docker-ce` |
| Docker Hub 镜像拉取 | `https://docker.m.daocloud.io` |
| Ubuntu apt（容器内） | `https://mirrors.tuna.tsinghua.edu.cn/ubuntu` |
| pip | `https://mirrors.tuna.tsinghua.edu.cn/pypi/web/simple` |
| repo 工具 | `https://mirrors.tuna.tsinghua.edu.cn/git/git-repo` |
| AOSP 源码 | `https://mirrors.tuna.tsinghua.edu.cn/git/AOSP` |

**两个关键坑**：

**坑 1：TUNA 的镜像是 git bare 仓库，不能用浏览器/HTTP 直链访问。** `https://mirrors.tuna.tsinghua.edu.cn/git/AOSP/platform/manifest` 用 wget 访问会返回 404，但用 `git ls-remote` 完全正常。**测连通性要用 git，不能用 wget。** 同理，`repo` 工具不能从 `.../git-repo/repo` 直接下载单文件，必须 `git clone`。

**坑 2：git 重定向配置。** 容器内已配置系统级 `insteadOf`，所有 google 源自动走清华：

```bash
git config --system url.https://mirrors.tuna.tsinghua.edu.cn/git/AOSP/.insteadof \
    https://android.googlesource.com
git config --system url.https://mirrors.tuna.tsinghua.edu.cn/git/git-repo.insteadof \
    https://gerrit.googlesource.com/git-repo
```

再加环境变量 `REPO_URL` 指向清华的 git-repo 镜像，避免 repo 自更新时去连 gerrit。

### 8.4 构建容器

宿主是 Debian 13（gcc 14 / Python 3.13），而 **AOSP 12 官方只支持 Ubuntu 20.04/22.04**。直接编会踩一堆兼容性问题，所以用容器隔离。

```bash
docker exec -it autod-builder bash     # 进入构建环境
```

| 项 | 值 |
|---|---|
| 镜像 | `autod-aosp12-builder`（基于 `ubuntu:22.04`） |
| 容器 | `autod-builder` |
| 挂载 | `/root/AutoSnapshotAndroid/aosp` ↔ `/aosp` |
| Java | OpenJDK 11（**AOSP 12 要求 JDK 11，不是 17**） |
| Python | 3.10 |

**AOSP 12 的依赖坑**：官方文档列的 `unicode-terminfo` **在 Ubuntu 22.04 已不存在**，装了会直接失败。32 位兼容包（`libc6-dev-i386`、`lib32ncurses5-dev` 等）在 22.04 里部分改名，脚本里做成了可选组，装不上就跳过——arm64 目标构建基本用不到。

### 8.5 目录结构

```
/root/AutoSnapshotAndroid/          458 GiB 挂载点
├── README.md                       项目索引
├── SUMMARY.md                      方案总结与决策记录
├── docs/                           专题文档
├── dev/                            开发轨道
├── tools/                          环境、构建与集成脚本
└── aosp/                           AOSP 源码树（~85 GB，android-12.0.0_r34）
    ├── .repo/
    ├── build/
    ├── frameworks/
    └── ...
```

> ⚠️ **`aosp/` 必须独立成子目录。** 早期版本把源码树直接放在项目根目录，结果 AOSP 的 `art/`、`bionic/`、`build/` 等目录和项目文件混在同一层。

### 8.6 常用命令

```bash
# 进入构建环境
docker exec -it autod-builder bash

# 在容器内
cd /aosp
source build/envsetup.sh
lunch aosp_arm64-userdebug       # 只编二进制，不需要 vendor blobs
m autod autodctl                 # 编我们自己的模块（分钟级）

# 产物
ls $ANDROID_PRODUCT_OUT/system/bin/
```

**首次同步**（已完成，供重装时参考）：

```bash
setsid nohup bash tools/repo-sync.sh > /var/log/aosp-sync.log 2>&1 &
tail -f /var/log/aosp-sync.log

# 查看进度
du -sh /root/AutoSnapshotAndroid/aosp
df -h /root/AutoSnapshotAndroid
docker exec autod-builder pgrep -f "repo/main.py" >/dev/null && echo 运行中 || echo 已停止
```

### 8.7 模拟器（跑 arm64 安卓验证 `autod`，不需要真机）

> ⚠️ **待核实**：`dev/04-emulator/` 正在改造中（新增了 `windows-arm64/` 一路）。下面的命令与当前状态可能已经不符，以该目录的 README 为准。

| 项 | 值 |
|---|---|
| lunch 目标 | **`sdk_phone64_arm64-userdebug`** |
| 产物目录 | `out/target/product/emulator64_arm64/` |
| 模拟器二进制 | `aosp/prebuilts/android-emulator/linux-x86_64/emulator`（30.8.3，内含 `qemu-system-aarch64`） |
| arm64 内核 | `kernel/prebuilts/5.10/arm64/kernel-5.10-gz` → 产物里的 `kernel-ranchu` |
| 宿主额外依赖 | `apt-get install -y libpulse0 libgl1`（模拟器二进制的动态依赖，缺了起不来） |

**两条必须记住的限制：**

1. **`aosp_arm64-userdebug` 是 GSI，起不了模拟器**（Android 12 的 `build/make/target/product/aosp_arm64.mk` 注释写明），产物在 `generic_arm64/`，没有 `kernel-ranchu` / `ramdisk.img` / `vendor.img`。模拟器必须用 `sdk_phone64_arm64`。
2. **跨架构没有硬件加速**：guest arm64 / 宿主 x86_64 ⇒ 只能 TCG，`/dev/kvm` 用不上，首次开机 **10~40 分钟**。这是物理限制。

### 8.8 注意事项

**⚠️ 不要对工作区做全量同步。** `aosp/` 里有 10 万+ 文件、~85 GB，工作区镜像机制会遍历整个目录树，全量同步会试图下载 AOSP 源码。**需要取文件时用单文件下载（`rw_download`），不要用全量同步。**

**磁盘占用预期**（数据盘 458 GiB；**实测当前已用 245 GB、可用 209 GB**，源码已同步完）：

| 阶段 | 占用 |
|---|---|
| 源码（shallow） | ~85 GB |
| `out/` 只编 `autod` | ~35 GB |
| `out/` 全量编系统镜像 | ~100 GB |

空间充裕，不需要开 ccache 省，也不需要频繁清理。

---

## 9. 相关文档

- 硬件/源码树为什么躲不掉 → `01-selection.md` 约束 4
- 源码同步后的下一步 → `../dev/02-native-daemon/README.md`
- 纯 NDK 免源码树方案 → `../dev/01-ndk-prototype/README.md`
- 延迟数据 → `03-reference.md`
