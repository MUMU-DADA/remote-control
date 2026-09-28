# 验收报告（dev/04-x64-android）

> 这份文档是**结论性证据记录**：每条都给出可复现的命令与本机实测输出。
> 逐条展开的实测过程见 `PLAN.md`；踩过的坑见 `docs/02-build-traps.md`。

---

## 一、结论

| # | 目标 | 结论 | 证据 |
|---|---|---|---|
| G1 | 自编 **x86_64** Android 12 ROM | ✅ 完成 | `EXIT=0`，62083 个编译目标；产物 `system/vendor/product/ramdisk-qemu/system-qemu/...` |
| G2 | 在 **x86_64 Linux（KVM）** 上同架构运行 | ✅ 实测通过 | `sys.boot_completed=1`；开机 23.8 s（emulator 37.1.11）/ 30.2 s / 41.2 s（37.2.11）多轮复跑 |
| G3 | 能跑 **arm64 应用** | ✅ 实测通过 | 自建纯 arm64-v8a 探针 APK：装 `Success`、进程活、16 条 `/system/lib64/arm64/*` 映射、`primaryCpuAbi=arm64-v8a`、JNI 返回 `arm64-v8a native ok | kernel=x86_64` |
| G4 | 在 **x86_64 Windows（WHPX）** 上同架构运行 | ⊘ **不由 agent 验证**（用户决定，2026-09-28） | 见 §3：交付所需的一切已备齐（工具链、镜像集合、同 build id 等价性、首次运行对照表），留给用户侧执行 |
| G5 | arm64 不行时下放到 **arm32** | 📄 预案就绪 | `docs/06-arm32-fallback.md`（含 API 30/31 的 32 位栈实测对照） |

---

## 二、G1–G3 的证据链（本机实测）

### 2.1 构建

```
$ ./scripts/build-rom.sh --status
已完成 EXIT=0   [100% 1289/1289]
错误数 0
```

镜像指纹：

```
AutoSnap/autosnap_x64_arm64/autosnap_x64_arm64:12/SP1A.210812.016.C2/root09280236:userdebug/test-keys
```

### 2.2 ROM 自检（`tools/verify-rom.sh`）

```
[✓] system.img 733M / vendor.img 105M / product.img 290M / ramdisk.img 1.7M / kernel-ranchu 22M …
[✓] 翻译器 + proxy：21 个
[✓] bin/ndk_translation_program_runner_binfmt_misc_arm64（含 0755 可执行位）
[✓] etc/init/ndk_translation.rc / etc/ld.config.arm{,64}.txt / etc/binfmt_misc/*（4 条）
[✓] arm64 侧车库：59 个
[✓] 59 个文件与载荷逐字节相同（→ 这些库是 AOSP 自己编的，与 x86_64 框架同源）
[✓] ro.system.product.cpu.abilist64 = x86_64,arm64-v8a
[✓] ro.dalvik.vm.native.bridge = libndk_translation.so（vendor 生效值）
[✓] ro.enable.native.bridge.exec = 1 ；ro.dalvik.vm.isa.arm/arm64 = x86/x86_64
```

### 2.3 启动 + 四组验收（`scripts/run-linux.sh`）

```
[✓] ro.build.version.sdk = 31
[✓] ro.product.device = autosnap_x64_arm64
[✓] ro.product.cpu.abilist = x86_64,arm64-v8a
[✓] ro.dalvik.vm.native.bridge = libndk_translation.so ；exec=1
[✓] binfmt: arm64_exe / arm64_dyn 已注册
[✓] aarch64 ELF 直接执行 → ARM64_OK machine=x86_64
[✓] pm install --abi arm64-v8a → Success
[✓] 进程存活 / 16 条 arm64 库映射 / primaryCpuAbi=arm64-v8a
[✓] logcat: PROBE_RESULT arm64-v8a native ok | built_for=arm64-v8a | kernel=x86_64
验收全部通过 ✓
```

### 2.4 交付物

```
artifacts/rom-autosnap_x64_arm64/   5.7 GB
  system-qemu.img vendor-qemu.img product-qemu.img system_ext-qemu.img ramdisk-qemu.img   ← 模拟器必需（GPT 包装 + 合并 ramdisk）
  system.img vendor.img product.img ramdisk.img                                          ← 裸 ext4，留作挂载检视
  kernel-ranchu encryptionkey.img userdata.img advancedFeatures.ini config.ini source.properties
  system/build.prop vendor/build.prop
  initrd SHA256SUMS MANIFEST.txt

$ sha256sum -c SHA256SUMS   →  20/20 通过
```

---

## 三、G4（Windows）已验证到什么程度

| 验证项 | 结果 |
|---|---|
| Windows 模拟器包可达 + 完整性 | ✅ `emulator-windows_x64-15917651.zip`（421 MiB），sha1 `54fa750822ff…` **与仓库清单一致** |
| 包内是否带 x86_64 guest 后端 | ✅ 含 `emulator/emulator.exe` + `emulator/qemu/windows-x86_64/qemu-system-x86_64.exe` |
| **同 build id 等价性** | ✅ Windows 稳定包与 Linux 包同为 **build 15917651 / 37.1.11**；已下载该 Linux 包并**用它跑完整验收 → 全部通过，开机 23.8 s** |
| 镜像集合 | ✅ 与 Linux 侧完全同一份（`SHA256SUMS` 可逐文件校验） |
| 脚本正确性 | ✅ 修掉"按 host-os 选 archive"与"渠道名映射"两个真 bug；新增 `preflight.ps1` |
| 首次运行对照表 | ✅ `windows/EXPECTED-OUTPUT.md`（逐项期望值 + 已知正常现象 + 排查顺序） |
| **WHPX 实跑** | ⊘ **由用户侧负责**（用户已明确"windows 就不用你来尝试了"） |

> 也就是说：G4 的**工程部分**（脚本、镜像、文档、等价性验证）已全部完成并交付；
> 只剩下"在真实 Windows 上按一次回车"这一动作，属用户侧。

**用户侧闭环方式**：在一台开了虚拟化（BIOS VT-x/AMD-V + 「Windows 虚拟机监控程序平台」）的 x64 Windows 上执行

```powershell
cd dev\04-x64-android\windows
.\fetch-emulator.ps1 ; .\fetch-images.ps1 ; .\preflight.ps1 ; .\run-windows.ps1
```

然后对照 `EXPECTED-OUTPUT.md`：开机几十秒、四组验收全绿、`ro.build.fingerprint` 与 Linux 侧一致
（`AutoSnap/autosnap_x64_arm64/autosnap_x64_arm64:12/...:userdebug/test-keys`）。

---

## 四、怎么从零复现全部结论

```bash
cd dev/04-x64-android

./scripts/fetch-payload.sh      # 从官方镜像提取翻译层（23 MB，90 文件）
./scripts/apply-overlay.sh      # 注入 AOSP 树（不改上游一行；--revert 可撤）
./tools/build-probe-apk.sh      # 自建纯 arm64-v8a 探针 APK
./scripts/build-rom.sh          # 全量构建（约 1 小时；后台 + --status）
./scripts/accept.sh             # 自检 → 依赖检查 → 打包 → 启动验收（一条命令）
```

Windows 侧见 [`../windows/README.md`](../windows/README.md) 与
[`../windows/EXPECTED-OUTPUT.md`](../windows/EXPECTED-OUTPUT.md)。
