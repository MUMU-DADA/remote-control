# arm32 下放预案（目标里的"如果 arm64 支持不行"）

> ⚠️ **历史文档 —— 预案，未启用。** 记录当时的备选方案与判断依据，不代表当前实现。

> 目标原文：*"可以跑 arm64 的安卓 app（如果 arm64 支持不行，可以下放到 arm32）"*。
> 现状：**arm64 这条路是成立的**（官方镜像实测：装 + 跑 + 映射 22~29 条 `/system/lib64/arm64/*.so`），
> 所以本文是**预案**，不是当前路线。只有在"目标应用恰好是 32 位 ARM（armeabi-v7a）only"时才需要。

---

## 1. 先把两个 API 级别的 32 位支持实测清楚

| 项 | **API 30**（Android 11，`google_apis;x86_64`） | **API 31**（Android 12，本项目在用） |
|---|---|---|
| `ro.product.cpu.abilist32` | `x86,armeabi-v7a,armeabi` ✅ | **空** ❌ |
| 32 位宿主翻译器 | `/system/lib/libndk_translation.so`（1654288 B）✅ | **无** ❌ |
| 64 位宿主翻译器 | `/system/lib64/libndk_translation.so`（2293844 B）✅ | ✅ |
| 32 位 ARM program runner | `/system/bin/ndk_translation_program_runner_binfmt_misc` ✅ | **无** ❌ |
| 64 位 ARM program runner | `..._binfmt_misc_arm64` ✅ | ✅ |
| 32 位 ARM 系统库 | `/system/lib/arm` 59 个 ✅ | 无 |
| 32 位 ARM linker/app_process | `/system/bin/arm/{linker,app_process}` ✅ | 无 |
| binfmt 规则 | `arm_exe`/`arm_dyn`/`arm64_exe`/`arm64_dyn` 四条**都有效** ✅ | 四条都在，但 **`arm_exe` 指向的 runner 在镜像里不存在** → 是**死接线** ⚠️ |

**结论**：API 31 的镜像从框架到翻译层都是**纯 64 位**；它载荷里那两条 32 位 ARM 规则是残留
（`arm_exe` → `/system/bin/ndk_translation_program_runner_binfmt_misc`，该文件在 API 31 镜像里不存在）。
32 位 ARM 的完整栈只在 API 30（Android 11）那一代存在。

---

## 2. 两条可选路

### 路 A（推荐，若真要 arm32）：以 **Android 11 为基座**

| 步骤 | 说明 |
|---|---|
| 1 | `repo init -b android-11.0.0_r48` 拉一份 Android 11 树（~85 GB，数小时） |
| 2 | 设备树照抄本项目（`device/remote_control*/`），BoardConfig 用 **4 ABI** 版：`TARGET_2ND_ARCH := x86` + `TARGET_NATIVE_BRIDGE_2ND_ARCH := arm` + `TARGET_NATIVE_BRIDGE_2ND_ABI := armeabi-v7a armeabi`（AOSP 里 `build/make/target/board/emulator_x86_64_arm64/BoardConfig.mk` 就是这个形状） |
| 3 | 载荷改成从 **API 30** 镜像提取（`fetch-payload.sh --api 30`） |
| 4 | 验收：`abilist32` 含 `armeabi-v7a` → 装一个**纯 armeabi-v7a** 的 APK → 起 → 映射 `/system/lib/arm/*` |

- ✅ 与官方 Android 11 镜像同构，风险最低；
- ❌ 要新树 + 新载荷，成本主要在 `repo sync`。

### 路 B（同树快速试验，**未验证、风险高**）

在现有 Android 12 树上把板级改成 4 ABI，让 AOSP 自己编出 32 位 x86 与 arm32 侧车
（arm64 侧车就是这么来的，见 `01-design.md` §3.1——实测与官方镜像逐字节相同）。
但**32 位宿主翻译器（`/system/lib/libndk_translation.so` + 32 位 proxy + 32 位 program runner）
在 API 31 载荷里根本不存在**，只能从 API 30 镜像搬 → **Android 11 的翻译器跑在 Android 12 框架上**。

- ✅ 不换树，改两行板级 + 换一个载荷就能试；
- ❌ 翻译层与框架版本不匹配（社区共识是"必须同版本"），成功率未知；
- ⚠️ 建议只作为"有没有必要走 A"的探路，**不要直接拿去做交付**。

---

## 3. 无论哪条路，验收标准都是这 5 条

```bash
S=emulator-5580; ADB=../aosp/out/host/linux-x86/bin/adb
$ADB -s $S shell getprop ro.product.cpu.abilist32      # 期望含 armeabi-v7a
$ADB -s $S shell 'ls /system/lib/arm | wc -l'          # 期望 59
$ADB -s $S shell 'ls /proc/sys/fs/binfmt_misc/'        # 期望含 arm_exe arm_dyn
$ADB -s $S install --abi armeabi-v7a <纯 32 位 ARM 的 APK>
$ADB -s $S shell 'pidof <包名>' | xargs -I{} $ADB -s $S shell "grep -c '/system/lib/arm/' /proc/{}/maps"
```

> 官方 Android 11 镜像上这 5 条**已经跑通过**（见 `../04-android-rom/docs/00-bridge-eval.md` §2.5：
> `primaryCpuAbi=armeabi-v7a`、17 条 `/system/lib/arm/*` 映射、Activity 前台无崩溃）。

---

## 4. 决策建议

| 情况 | 选什么 |
|---|---|
| 目标应用是 arm64（或含 arm64 的 fat APK） | **什么都不用做**——当前 ROM 已经支持 |
| 目标应用**只有** armeabi-v7a | 先确认能不能换新版 APK；不能换再走路 A（Android 11 基座） |
| 只是想"顺手都支持" | 不建议：多一整棵树、多一条发布链路，且翻译层只对 64 位做过充分验证 |
