# 验收排错手册

> 对应 `scripts/run-linux.sh` 的 4 组验收。每条都是"症状 → 先查什么 → 怎么修"。
> 命令默认在 `dev/04-x64-android/` 下执行，`S=emulator-5580`。

```bash
S=emulator-5580
ADB=../aosp/out/host/linux-x86/bin/adb
```

---

## 0. 先确认三件事

```bash
./scripts/status.sh                      # 载荷/注入/构建/产物/设备
ls -la ../aosp/out/target/product/remote_control_x64_arm64/{system.img,vendor.img,ramdisk.img,kernel-ranchu}
grep "^EXIT=" ../aosp/out/remote-control-build.log   # EXIT=0 才算编完
```

---

## 1. 起不来 / `adb devices` 里没有设备

| 症状 | 查什么 | 处置 |
|---|---|---|
| 模拟器进程直接退出 | `.run/emulator-5580.log` 尾部 | `run-linux.sh` 已自动在 SDK 版与 AOSP 版之间回退；两个都挂就看日志里的 `ERROR` |
| 报 `No AVD specified` | 环境变量 | 必须是"构建模式"：脚本已设 `ANDROID_PRODUCT_OUT=<产物目录>`；手工调试时别漏 |
| 报缺 `kernel-ranchu` / `initrd` | 产物 | `initrd` 由构建生成；缺了就 `cp ramdisk.img initrd` |
| 内核没起来、日志出现 x86 专属字样 | `system/build.prop` | 模拟器靠它识别 guest 架构；AOSP 产物天然有，别只拷 `*.img` 不拷 `system/` 目录 |

---

## 2. 卡在开机动画 / 反复重启

看内核与 init 日志：

```bash
$ADB -s $S shell 'dmesg | tail -50'
grep -iE "Rebooting into recovery|Setting <policy> on /data/misc failed|avc: denied" .run/emulator-5580.log | tail
```

| 症状 | 原因 | 处置 |
|---|---|---|
| `Rebooting into recovery` + `/data/misc` 加密策略失败 | init 给 `/data/misc` 设 fscrypt 策略时目录非空（ENOTEMPTY），而兜底诊断要 exec `/vendor/bin/toybox_vendor` 被 SELinux 拒 | `./scripts/apply-overlay.sh --patch-initrc`（`encryption=Require` → `Attempt`）→ `./scripts/build-rom.sh` 重编 |
| 首次启动很慢 | 冷启动 + `-no-snapshot` | 正常，x86_64+KVM 下几十秒；确认 `-accel on` 且 `emulator-check accel` 返回 0 |
| 数据分区坏了 | userdata 覆盖层 | `rm -rf .run/datadir` 后重启（等价 `-wipe-data`） |

---

## 3. 验收 1 组失败：ABI 列表不对

```bash
$ADB -s $S shell getprop ro.system.product.cpu.abilist64
$ADB -s $S shell getprop ro.vendor.product.cpu.abilist64
$ADB -s $S shell getprop ro.odm.product.cpu.abilist64
$ADB -s $S shell getprop ro.product.cpu.abilist          # init 按 product→odm→vendor→system 派生
```

应全部是 `x86_64,arm64-v8a`。若某一路为空：

- 构建期问题 → 查 `BoardConfig.mk` 的 `TARGET_NATIVE_BRIDGE_ARCH/ABI`，以及
  `get_build_var TARGET_CPU_ABI_LIST`（容器内）。
- **只改了部分分区** → 常见于手工改镜像：`odm` 优先级高于 `vendor`/`system`，三处必须一致。

---

## 4. 验收 2 组失败：翻译层没接线

```bash
$ADB -s $S shell getprop ro.dalvik.vm.native.bridge          # 期望 libndk_translation.so（在 vendor 分区）
$ADB -s $S shell getprop ro.enable.native.bridge.exec        # 期望 1
$ADB -s $S shell 'ls /proc/sys/fs/binfmt_misc/'              # 期望 arm64_exe arm64_dyn arm_exe arm_dyn
$ADB -s $S shell 'ls -l /system/bin/ndk_translation_program_runner_binfmt_misc_arm64'   # 必须 0755
```

| 症状 | 原因 | 处置 |
|---|---|---|
| `native.bridge` 是 `0` | 属性没进 vendor 分区 | 查 `device/.../product/remote_control_x64_arm64.mk` 的 `PRODUCT_VENDOR_PROPERTIES`（放 system 会被 `runtime_libart.mk` 的强赋值挡住） |
| `binfmt_misc` 是空的 | `ndk_translation.rc` 没进镜像，或内核没 `CONFIG_BINFMT_MISC` | 查产物 `system/etc/init/ndk_translation.rc`；内核用构建自带的 `kernel-ranchu`（实测带该配置） |
| 执行器不是 0755 | 拷贝时丢了可执行位 | `PRODUCT_COPY_FILES` 用 `cp`（非 `cp -p`），源文件必须是 0755；`apply-overlay.sh` 会断言这一点 |

---

## 5. 验收 4 组失败：arm64 应用装不上 / 起不来

### 5.1 `ABI arm64-v8a not supported on this device`
= 系统没声明 arm64 → 回到第 3 组排查。

### 5.2 装上了但一启动就崩 / `dlopen failed`

```bash
$ADB -s $S logcat -d | grep -iE "ndk_translation|native bridge|linker|CANNOT LINK|SIGILL|SIGSEGV" | tail -30
$ADB -s $S shell "ls /system/lib64/arm64 | wc -l"        # 期望 59
```

| 症状 | 原因 | 处置 |
|---|---|---|
| `CANNOT LINK EXECUTABLE ... symbol not found` | 翻译层与我们的框架版本错配（宿主侧 proxy 找不到符号） | `./tools/check-bridge-symbols.sh` 看缺什么；必要时从官方镜像补对应宿主库进载荷 |
| `lib/arm64-v8a/*.so` 找不到 | APK 里没有 arm64 库 | 用 `unzip -l app.apk | grep lib/` 确认；探针 APK 自带 `lib/arm64-v8a/libarm64probe.so` |
| 进程起来但立刻退出 | 应用自身要求（权限/服务） | 先用探针 APK 定位是"翻译层问题"还是"应用问题" |

> 判断顺序很重要：**先用自建探针 APK**（只做 `System.loadLibrary` + 一次 JNI 调用）。
> 探针过了说明翻译层没问题，剩下的是应用兼容性；探针不过才是 ROM 的问题。

---

## 6. 性能异常

```bash
# 同一台设备上跑两个基准（build-probe 里没有，需要时可临时编）
# 预期：整数/哈希 ~1.1×，可并行浮点 ≈1×，串行依赖 double 链 20× 上下
```

若目标应用恰好是"串行浮点密集"，这是翻译层的固有特性，不是配置问题——
要么换算法/并行化，要么接受，要么走真机 arm64（G5 之外的路线）。

---

## 7. 收尾

```bash
./scripts/run-linux.sh --stop        # 停模拟器
./scripts/package-rom.sh             # 打包交付目录
```
