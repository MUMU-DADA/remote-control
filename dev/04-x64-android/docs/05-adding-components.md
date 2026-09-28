# 往这份 ROM 里加自己的东西（P5）

> 自编 ROM 相对于"直接用官方镜像"的核心价值就在这里：**能改系统**。
> 本文给出加一个系统组件（以 `remote-control` 为例）的完整落点与验证方式。

---

## 1. 三种加法，按侵入性从低到高

| 方式 | 做法 | 适用 | 首次生效 |
|---|---|---|---|
| **A. 运行时推** | `adb root && adb remount`（overlayfs，跑两次 remount）→ 推进 `/system/bin` → 手动起 | 调试期快速迭代 | 秒级 |
| **B. Magisk/KernelSU 模块** | 把二进制与 `init.rc` 打包成模块装进去 | 不想每次重编镜像 | 重启 |
| **C. 编进 ROM**（本文重点） | 源码进 AOSP 树 + 产品 mk 加 `PRODUCT_PACKAGES` + 重编 | 交付形态 | 重新构建 |

前两种在 `dev/02-native-daemon/` 里已有脚本可参考；这里只讲 C。

---

## 2. 方式 C：把它编进 ROM

### 2.1 源码放哪

AOSP 树的模块自己带 `Android.bp`，例如 `remote-control` 在：

```
aosp/frameworks/native/cmds/remote-control/daemon/Android.bp   → 模块名 remote-control / rcctl
```

> 我们的产品**不修改 AOSP 上游文件**，但"把自己的模块放进树里"是正常的 ROM 开发动作。
> 建议把这类改动**单独记录**，这样 `repo sync` 后能快速复原（本项目的 `docs/` 就是干这个的）。

### 2.2 加进产品

编辑 `dev/04-x64-android/device/remote_control_x64_arm64/product/remote_control_x64_arm64.mk`，在末尾加：

```make
# ---- 自制系统组件 ----
PRODUCT_PACKAGES += \
    remote-control \
    rcctl
```

然后：

```bash
cd dev/04-x64-android
./scripts/apply-overlay.sh        # 把改动同步进 AOSP 树（设备树是唯一真源）
./scripts/build-rom.sh            # 重编（增量，只编受影响的部分）
./tools/verify-rom.sh             # 自检：确认二进制进了 /system/bin
```

验证二进制真的进了镜像：

```bash
ls -la ../aosp/out/target/product/remote_control_x64_arm64/system/bin/remote-control
```

### 2.3 开机自启 + SELinux

`init.rc` 与 sepolicy 的落点跟模块走（`remote-control/daemon/Android.bp` 里已经写了
`init_rc: ["remote-control.rc"]`，Soong 会自动装到 `/system/etc/init/`）。
SELinux domain 按 `dev/02-native-daemon/` 的 sepolicy 清单接进产品即可：

```make
BOARD_SEPOLICY_DIRS += device/remote_control/remote_control_x64_arm64/sepolicy
```

装完 `init.rc` 后**必须验证**：

```bash
./scripts/run-linux.sh --no-wait
S=emulator-5580; ADB=../aosp/out/host/linux-x86/bin/adb
$ADB -s $S shell getprop init.svc.remote-control          # running 才算起来了
$ADB -s $S shell 'logcat -d | grep -i "avc: denied" | grep remote-control'   # 空才算 sepolicy 干净
```

---

## 3. ⚠️ 两个与"编进 ROM"直接相关的现状

### 3.1 构建当前开着 `ALLOW_MISSING_DEPENDENCIES`

原因：树里 `frameworks/native/cmds/remote-control/daemon/Android.bp` 把 `libwebp_vendored`
（声明为 `cc_library_static`）放进了 `shared_libs`，x86_64 变体解析失败，会让**整棵树**编不过。

- 这个开关只影响**有依赖问题的模块**（会被跳过），不影响我们的 ROM 内容；
- 但**要真的把 `remote-control` 编进 ROM**，就必须先修好它：
  把 `libwebp_vendored` 从 `shared_libs` 挪到 `static_libs`（或把模块改成 `cc_library_shared`）；
- 修好后用严格模式验证一次：`ALLOW_MISSING_DEPS=0 ./scripts/build-rom.sh`。

### 3.2 载荷可以瘦身（可选优化）

`docs/01-design.md` §3.1 实测说明：那 59 个 `/system/lib64/arm64/*.so`
**AOSP 自己会编**（与官方镜像逐字节相同），载荷里其实只需要：

```
libndk_translation.so + 20 个 proxy + program_runner + binfmt 规则 + ndk_translation.rc + ld.config.arm{64}.txt
```

瘦身做法：`fetch-payload.sh` 里跳过 `system/lib64/arm64` 与 `system/bin/arm64` 的提取，
载荷从 23 MB 降到约 3.6 MB，同时消掉 61 条 `overriding commands` 警告。
**等当前验收通过后再做**（要重编一次验证）。

---

## 4. 交付前的检查清单（加了组件之后）

```bash
./scripts/build-rom.sh --status      # EXIT=0
./tools/verify-rom.sh                # ROM 自检（含自制组件是否进镜像）
./tools/check-bridge-symbols.sh      # 翻译层依赖没被你的改动破坏
./scripts/run-linux.sh               # Linux 验收（4 组 + 组件自身检查）
./scripts/package-rom.sh             # 打包
```

Windows 侧再用同一份产物跑 `windows/run-windows.ps1`，两边比对 `ro.build.fingerprint`。
