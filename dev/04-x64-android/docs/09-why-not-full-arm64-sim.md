# 为什么是「x86_64 ROM + 用户态翻译层」，而不是「跨架构模拟 arm64 Android」

> 这份文档是**实测结论**，不是理论推演。
> 它来自原先的 `dev/04-emulator/`（跨架构全系统 arm64 模拟路线，已实测后移除）——
> 那条路从零开始做了 28 轮实验，走到"差最后一环"，最终被放弃。
> 这里留下它的**结论、边界与可复用发现**，避免后来人重走。

---

## 1. 一句话结论

**x86_64 宿主上跑 arm64 安卓，正解是"同架构 + 用户态翻译"；不是"跨架构全系统模拟"。**

| | 跨架构全系统模拟（已放弃） | **同架构 + 用户态翻译（本项目）** |
|---|---|---|
| 做法 | 让模拟器跑一个**真正的 arm64 系统** | 编一份 **x86_64 系统**，arm64 只翻译**应用的原生代码** |
| 硬件加速 | 宿主 x86_64 **没有 arm64 KVM** ⇒ 只能 **TCG** | **KVM**（Linux）/ **WHPX**（Windows） |
| 开机 | guest 时间≈实时，**以十分钟计** | **23.8 秒** |
| 谁能编译 | 全都要翻译（内核 / ART / framework / HAL / 图形栈） | 只有你的 `.so` 被翻译，系统侧全是原生 |
| 结果 | boot 推进到 `zygote`+`SurfaceFlinger` 后卡在图形栈 ✗ | **`sys.boot_completed=1` + arm64 应用实测通过** ✅ |

---

## 2. 性能对比（实测锚点 + 推算）

| 场景 | 本项目（x86_64+KVM+libndk） | 跨架构 TCG | 差距 |
|---|---|---|---|
| 开机到 `boot_completed` | **23.8 s**（实测）| 估 5~8 min（实测 270 s 才到 `SurfaceFlinger`）| **≈ 15~20×** |
| 应用：整数 / 哈希 | **1.1× 慢**（实测）| TCG ≈ 5~10× 慢 | ≈ 5~9× |
| 应用：可并行浮点 / NEON | **≈1×（≈原生，实测）** | ≈ 5~15× 慢 | ≈ 5~15× |
| 应用：串行依赖 double 链 | **20~23× 慢**（实测，翻译层已知弱项）| 估 30~50× 慢 | ≈ 1.5~2×（唯一差距小的场景）|
| I/O / syscall / binder / 图形 | 原生 | 全部翻译 | ≈ 10~25× |
| **典型混合负载** | — | — | **≈ 5~15×** |

**关键不是"快几倍"，而是量级差 + 上限被钉死**：x86_64 宿主上永远没有 arm64 KVM，
TCG 就停在"原生 10~20%"这个量级；而翻译层方案的系统侧**永远原生**，翻译层本身还可替换/优化
（真需要 32 位 ARM 时，换 API 30 基座即可，见 [`06-arm32-fallback.md`](06-arm32-fallback.md)）。

另外两个容易忽略的差异：**多核**（KVM 真吃多核；MTTCG 扩展很差）、
**发热**（TCG 会把核心跑满，社区那句"跑出树莓派 3B 水平"就是这么来的）。

---

## 3. 那条路走到了哪里、为什么走不下去

**走到的最远处**（都是实测）：

```
ranchu 机器能起来 ✓ → arm64 guest 启动 ✓ → 零宿主段错误 / 零内核 panic / 零 guest 崩溃 ✓
→ zygote ✓ → SurfaceFlinger ✓ → healthd 正常读电池 ✓ → ... 卡在图形栈 ✗
```

**最后一环**：`vendor.hwcomposer-2-3`（A12 的 ranchu hwcomposer 其实是 cuttlefish 版）
的 NEEDED 里缺 `libgralloctypes.so` 与 `android.hardware.graphics.mapper@4.0.so`
（它们只在 `/system/lib64`，而 vendor 的 linker 命名空间看不到 `/system/lib64`，
本 build 又没有 VNDK 运行时）⇒ 加载失败 ⇒ `SIGABRT` ⇒ `init` 按 `onrestart`
反复重启 `surfaceflinger` ⇒ boot 永远收不了尾。

**为什么没补上**：靠 `PRODUCT_PACKAGES` 加模块在这个 build 配置下**不生效**
（`ninja: no work to do`）；而靠 `debugfs` 往镜像里**新增**文件，又撞上下面第 4 条第 3 款。

---

## 4. 可复用的发现（对别的项目也有用）

1. **`-cpu cortex-a53` 是 arm64 guest 的救命参数**：
   模拟器默认 `cortex-a57` 会触发 QEMU TCG 执行 guest 原生代码时把 64 位运算当 32 位
   （症状：指针高 32 位丢失、随机的 `SIGSEGV`）。A12 的 `keystore2` 反复崩、
   API 25 的 ART 在 `boot-framework.oat` 里崩，根因都是它。加 `-cpu cortex-a53` 即消失。

2. **`ranchu` 是可以在 x86_64 上起来的**，要改 QEMU 二进制三处：
   - **设备别名表**里把 `virtio-serial` 的**实现**从 PCI 版（`virtio-serial-pci`）
     改成 MMIO 版（`virtio-serial-device`）——表结构是 24 字节一组 `[实现名, 通用名, 标志]`；
   - 字符串补丁 `-soundhw` → `-name`（去掉无条件的 PCI 音频设备；命令行变成 `-name hda` 无害）；
   - `ioeventfd=off`（PCI 专有属性）→ `max_ports=511`（MMIO 合法属性）。

3. **`debugfs` 只能"替换/删除已有文件"，不能"新增"**：
   新增的文件 `debugfs ls` 看得到、`cat` 读得出、md5 也对，但 **guest 里的 init/linker
   枚举目录时看不到它们**（用标志 rc 文件实测过：新增的 rc 完全没被解析）。
   带 `shared_blocks`（e2fsdroid 块去重）的镜像尤其如此 —— `tune2fs` 清不掉它，
   `e2fsck -E unshare_blocks` 也不行。**要往 ROM 里加东西，只能靠编译系统。**

4. **内核补丁 `ramoops` → `noramop`**：`kernel-ranchu` 里附带的 DTB 与驱动匹配表都带这个
   字符串；改掉后 ramoops 驱动匹配不上、pstore 无后端，可消除 guest 内核
   在 `pstore_console_write → persistent_ram_write → __memcpy_toio` 上必现的 panic。

5. **`AVB` 的教训**：改过 `system` 分区就会与 `vbmeta` 里的摘要/哈希树对不上；
   `-writable-system` 并不会传 `androidboot.veritymode=disabled`。
   要么用 `avbtool` 重新生成，要么别改镜像。

---

## 5. 与官方镜像的关系

官方 `system-images;android-31;google_apis;x86_64` 本身就是"x86_64 + arm64 桥"：
`ro.product.cpu.abilist = x86_64,arm64-v8a`，34 秒开机就能跑 arm64 应用。
本项目的差别只在于**可控**：能加系统服务、改 framework、塞 `remote-control`、做交付裁剪。
详见 [`00-bridge-eval.md`](00-bridge-eval.md)。
