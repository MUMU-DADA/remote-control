# 构建中实际撞到的坑（持续追加）

> 只记录**真正报过错**的东西，以及最后的解法。理论分析在 [`01-design.md`](01-design.md)。

---

## 1. `device/remote_control/.../remote_control_x64_arm64.mk produces files inside ... artifact path requirement`

**现象**（第一次构建 1 分 53 秒后失败）：

```
FAILED:
build/make/core/artifact_path_requirements.mk:26: warning:
  device/remote_control/remote_control_x64_arm64/product/remote_control_x64_arm64.mk produces files inside
  build/make/target/product/generic_system.mks artifact path requirement.
Offending entries:
  system/bin/arm64/linker64
  system/lib64/arm64/libc.so
  ...
build/make/core/artifact_path_requirements.mk:26: error: Build failed.
```

**原因**：翻译层载荷通过 `PRODUCT_COPY_FILES` 落到 `system/bin`、`system/lib64`、`system/etc`，
正好落在 `generic_system.mk` 声明的 artifact path 范围里。AOSP 的规则是
「别的产品不许往这个 mks 的地盘里塞文件」，除非显式放行。

**解法**（照上游 `build/make/target/product/aosp_x86_arm.mk`——同样是 x86+ARM 桥的 GSI）：

```make
PRODUCT_ENFORCE_ARTIFACT_PATH_REQUIREMENTS := relaxed
PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST += \
    system/bin/arm64/% \
    system/lib64/arm64/% \
    system/etc/ld.config.arm.txt \
    ...
```

**教训**：报错信息最后一行只有 `error: Build failed.`，**真正的文件清单在它上面那段 `warning:` 里**。
排查时不要只看最后一行。

---

## 2. `Tried to lock out/.lock, but timed out polling every 1s until 10s`

**现象**：构建 11 秒就"失败"，日志只有三行，进程列表里没有任何编译进程。

**原因**：上一次构建被打断后 `out/.lock` 没释放（或残留的 soong 进程还持锁）。

**解法**：

```bash
docker exec remote-control-builder bash -lc 'pkill -f soong_ui; rm -f /aosp/out/.lock'
```

然后重跑 `./scripts/build-rom.sh`。**注意别被日志的 `EXIT=1` 骗了——这不是编译错误。**

---

## 3. `dependency "libwebp_vendored" of "remote-control" missing variant`

**现象**（kati 之后、soong 生成 ninja 时失败）：

```
error: frameworks/native/cmds/remote-control/daemon/Android.bp:206:1:
  dependency "libwebp_vendored" of "remote_control_vtp" missing variant:
    os:android,image:,arch:x86_64,sdk:,link:shared
  available variants:
    os:android,image:,arch:x86_64,sdk:,link:static
```

**当时的原因**：`frameworks/native/cmds/remote-control/daemon/Android.bp` 曾把 `libwebp_vendored` 放进
`shared_libs`，但该模块声明的是 `cc_library_static`（只提供 `link:static` 变体）。这是 AOSP 树中的临时依赖配置问题；后续已调整为静态依赖，`remote-control` 也已集成进 ROM。

**当时的临时绕过方式**（本项目侧，避免和别人的在途改动打架）：

```bash
ALLOW_MISSING_DEPENDENCIES=true m -j12
```

当时 `build-rom.sh` 默认开启（`ALLOW_MISSING_DEPS=0` 可关）。Soong 会**跳过**有依赖问题的模块，
而不是让整棵树编不过（`build/soong/ui/build/soong.go:226`）。该绕过曾用于排除无关依赖问题，不能据此认为 `remote-control` 可以被跳过；当前产品依赖并集成此服务。

当前构建脚本仍默认允许 Soong 跳过存在依赖问题的模块。验证构建完整性时用 `ALLOW_MISSING_DEPS=0 ./scripts/build-rom.sh`，并检查构建日志中的 `missing dependencies`，避免目标服务被跳过。

---

## 4. 载荷校验：`MANIFEST.sha256` 自己把自己算进去

**现象**：`sha256sum -c MANIFEST.sha256` 报 1 个不匹配。

**原因**：用 `find ... > MANIFEST.sha256` 生成清单时，重定向先创建了空文件，
`find` 又把这个空文件本身算进了清单。

**解法**：生成时排除自己（`find . -type f ! -name MANIFEST.sha256`），先写临时文件再 `mv`。
两个脚本（`fetch-payload.sh` / `apply-overlay.sh`）现在都按这个写法。

---

## 5. 宿主机 `/tmp` 是 16 GB tmpfs，写满会连带打断 `docker exec`

**现象**（一次同时出现三种）：

```
clang: fatal error: error in backend: IO failure on output stream: No space left on device
docker exec remote-control-builder ...  →  OCI runtime exec failed:
    write /tmp/runc-process2691731756: no space left on device
df -h /                        →  还有 7 G 可用（所以一开始没往这儿想）
```

**根因**：`/tmp` 是**独立的 16 GB tmpfs**（`mount | grep /tmp` 可见），与根分区不是一回事。
它被写满时：`clang` 写临时文件失败、`docker exec` 也失败（runc 要往宿主 `/tmp` 写进程文件），
而 `df -h /` 仍然显示有空间，极易误判。

**本次的占用方**：宿主机上长期跑着的模拟器持有 **5.2 GB 已删除但仍未释放**的文件
（`lsof +L1` 能看到 `qemu-system` 名下的大块），加上一些实验残留（core dump 1.5 GB 等）。

**处置**：
1. 项目脚本统一把 `TMPDIR` 指到数据盘（`scripts/common.sh` 里 export，所有脚本继承）；
2. `rm -rf /tmp/cores`（core dump）等无争议残留；
3. 排查命令：`df -h /tmp`、`du -sh /tmp/* | sort -rh | head`、`lsof +L1 | awk '$7>104857600'`。

---

## 6. 自建探针 APK 装不上：`resources.arsc` 必须不压缩且 4 字节对齐

**现象**（`pm install --abi arm64-v8a` 直接失败）：

```
Failure [-124: Failed parse during installPackageLI: Targeting R+ (version 30 and above)
requires the resources.arsc of installed APKs to be stored uncompressed and aligned
on a 4-byte boundary]
```

**根因**：`tools/build-probe-apk.sh` 组装 APK 时用 `zip -r` 把 `resources.arsc` 一起压缩了。
targetSdk ≥ 30 的硬要求是它必须 **stored**。

**解法**：`resources.arsc` 单独用 `zip -X -0` 打进去，其余（`AndroidManifest.xml`、
`classes.dex`、`lib/`）再压缩；最后 `zipalign -f -p 4` 对齐。
脚本里加了自检：`unzip -v | awk '$NF=="resources.arsc"'` 必须显示 `Stored`。

**怎么提前发现的**：拿官方 `google_apis;x86_64` 镜像做了一次**验收彩排**
（`run-linux.sh --verify --port 5560`），14 项里只有"设备名"按预期失败——问题全出在工具侧，
而不是 ROM 侧。彩排还顺带修掉两处验收脚本缺陷：binfmt 检查正则顺序依赖、logcat 未清导致匹配旧日志。

---

## 7. 启动自编镜像时缺 `ANDROID_BUILD_TOP` → 模拟器去找不存在的 `kernel-qemu`

**现象**（验收脚本启动阶段失败，两个模拟器版本都报同一句）：

```
ERROR | Your system directory is missing the 'kernel-qemu' image file.
        Please specify one with the '-kernel <filepath>' option
```

而 sysdir 里明明有 `kernel-ranchu`（22 MB）、`ramdisk.img`、`initrd`、`system.img`：
**用本项目 `scripts/run-linux.sh`（或手动补上下面两个环境变量）跑同一个目录是能起来的**。

**根因**：模拟器进"构建模式"需要**两个**环境变量，我只给了第一个：

| 变量 | 作用 | 少了会怎样 |
|---|---|---|
| `ANDROID_PRODUCT_OUT` | 告诉模拟器"不用 AVD，直接从构建产物启动" | 报 `No AVD specified` |
| **`ANDROID_BUILD_TOP`** | 构建树根，模拟器据此解析内核/镜像路径 | **去找 legacy 的 `kernel-qemu` 并失败** |

**解法**：`scripts/run-linux.sh` 的 launch 函数里两个都 export（已修）。

**怎么发现的**：又是彩排——用官方镜像 + `PRODUCT_OUT=<官方 sysdir>` 跑一遍
`run-linux.sh` 的启动路径，把"启动逻辑本身"的 bug 挡在了真正验收之前。

---

## 8. ⭐ 自编 ROM 首启必挂的两个坑：`ramdisk-qemu.img` 与 `QEMU_DISABLE_AVB`

构建成功 ≠ 能开机。自编镜像第一次启动连挂两关，症状都不直观（**没有 `-show-kernel` 时看起来就是"卡住不动"**）。
**排查方法：给模拟器加 `-show-kernel`**，直接看 guest 内核与 first-stage init 的输出。

### 坑 8.1 交付目录里少了 `ramdisk-qemu.img` → `failed to read default fstab`

```
init: init first stage started!
init: Unable to open /lib/modules, skipping module loading.
init: [libfs_mgr]ReadDefaultFstab(): failed to find device default fstab
init: Failed to create FirstStageMount failed to read default fstab for first stage mount
init: InitFatalReboot: signal 6
init: Reboot ending, jumping to kernel          ← 无限重启
```

**根因**：first-stage 的 `fstab.ranchu` 在 **vendor ramdisk** 里
（`PRODUCT_COPY_FILES ...:$(TARGET_COPY_OUT_VENDOR_RAMDISK)/first_stage_ramdisk/fstab.ranchu`），
而 AOSP 把「系统 ramdisk + vendor ramdisk」合并成 **`ramdisk-qemu.img`**
（`build/make/core/Makefile:5787`：`(cat $(INSTALLED_RAMDISK_TARGET) $(INTERNAL_VENDOR_RAMDISK_TARGET) > ramdisk-qemu.img)`）。
`ramdisk.img` 只有系统那一半，于是 init 找不到 fstab。

**解法**：交付目录必须带上 **`ramdisk-qemu.img`**（`package-rom.sh` 原先只拷了 `ramdisk.img`）。
> API 30/31 的**成品镜像**没有这个问题，因为它们的 `ramdisk.img` 本身就是合并版
> （所以社区文档里那句"补 `cp ramdisk.img initrd`"只对成品镜像成立）。

### 坑 8.2 带 AVB 构建 → `vbmeta digest error isn't allowed`

补上 `ramdisk-qemu.img` 后，分区能找到了（`dm-0`），但换成 AVB 校验失败：

```
init: [libfs_avb]Device path not found: /dev/block/by-name/system
init: [libfs_avb]Invalid hash size:
init: [libfs_avb]Failed to verify vbmeta digest
init: [libfs_avb]vbmeta digest error isn't allowed
init: Failed to setup verity for '/system': No such file or directory
init: Failed to mount /system → Failed to mount required partitions early → InitFatalReboot
```

**根因**：模拟器镜像默认不需要 AVB，但我们的产品没关。AOSP 为此准备了一个**只读不写的变量**
`QEMU_DISABLE_AVB`（`build/make/target/board/BoardConfigEmuCommon.mk:63` 读它 → `BOARD_AVB_ENABLE := false`；
全树搜不到赋值处，说明就是设计成命令行传入的）。

**解法**：构建时带上它——`build-rom.sh` 已内置：

```bash
m -j12 QEMU_DISABLE_AVB=true
```

副作用是镜像不带 AVB 校验（模拟器场景本来也不需要），并且用 `fstab.ranchu.noavb` + dummy vbmeta，出图更快。

> **教训**：自编 ROM 的"能不能编过"和"能不能开机"是两件事。前者靠构建日志，
> 后者必须**真的启动一次并开 `-show-kernel`**。本项目在 `run-linux.sh` 里没默认开
> `-show-kernel`，是这次踩坑后才知道要加（排错时手动加）。

---

## 9. 别在被执行的脚本上改文件（bash 是边读边执行的）

**现象**：

```
release.sh: 行 1162: $'\220\214': 未找到命令
```

一个"未找到命令"、命令名是两个乱码字节、行号还落在一个**空行**上 ——
看起来像脚本里混进了坏字符。

**真因**：`release.sh` 在**跑的时候**被替换了（我为了改文档顺手编辑了它）。
bash **不是**一次读完整个脚本，它边执行边按字节偏移继续读文件；
文件换成不同长度之后，它按旧偏移读到了新文件的中间，
**把半个汉字当成了命令名**（0x90 0x8C 正是某个多字节字符的续接字节）。

**怎么定案的**（猜错了很多方向，最后靠时间线）：
`release.sh` 的 mtime 是 18:38:57，而那次运行的日志最后写入是 **18:41:33** ——
在文件被换掉之后。中间怀疑过 UTF-8 被写坏、`$'...'` 拼接写错、heredoc 没闭合、
数组赋值出问题，全不是。

**注意它只坏在收尾**：四个 zip 都正常产出了（压缩那几步在文件被换之前就跑完了），
丢的是最后的"产物"清单、清理 staging、"完成"那几行。
**所以"产物看着是好的"不能证明这次运行是完整的。**

**规矩**：

- 长任务（release 打包、全量构建）跑起来之后，**不要编辑那个脚本**；
  要么等它结束，要么改副本再跑。
- 看到「`$'\xxx\yyy': 未找到命令`」这种乱码命令名、且行号落在空行/注释上，
  先怀疑"文件被边跑边改了"，而不是去找坏字符。
- 判断一次运行是否**完整**，看收尾那几行有没有打出来（如"完成。"），
  不要只看产物在不在。

**适用范围**：任何"边执行边读自身"的脚本语言（sh/bash 尤其明显）。
Python/Perl 这类先整体解析的不受影响。
