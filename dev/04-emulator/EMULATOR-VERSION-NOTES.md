# 模拟器版本与镜像的兼容性（实测记录）

> 2026-09-28 实测。这三条都是**跑了才知道**的，看文档看不出来。

---

## 结论：原版 ARM64 镜像在这台机器上起不来

不是配置问题，是**模拟器版本区间**问题：

| 模拟器 | 版本 | arm64 guest | 结果 |
|---|---|---|---|
| AOSP 自带（`prebuilts/android-emulator/`） | **30.8.3** | ✅ 有后端 | ❌ **太旧**，镜像要求 ≥31.2.7 |
| 下载的 `emulator-linux_x64-8807927` | **31.3.10** | ✅ 有后端 | ⚠️ 版本够，但仍报 `unexpected system image feature string` 后 1 秒退出 |
| 下载的 `emulator-linux_x64-16416033` | **37.2.11** | ❌ **已移除** | ❌ `FATAL \| QEMU2 emulator does not support arm64 CPU architecture` |

### 镜像到底要求什么

`.run/stock-image/arm64-v8a/source.properties` 里写着：

```
Pkg.Dependencies=emulator#31.2.7
AndroidVersion.ApiLevel=31
```

而 AOSP 12 自带的模拟器是 `Pkg.Revision=30.8.3`。**依赖不满足**。

### 症状长什么样

不会报"版本不兼容"，而是**启动 1 秒后干净退出**（退出码 0），日志停在：

```
emulator: INFO: userspace-boot-properties.cpp:249: Sending adb public key [...]
```

只有加 `-verbose` 才能看到真正的线索：

```
emulator: VERBOSE: FeatureControlImpl.cpp:172: WARNING:
  unexpected system image feature string, emulator might not function correctly,
  please try updating the emulator.
```

**没有这条 warning 的话，很容易误判成"配置不对"或"参数写错"。**

---

## 怎么判断"启动 1 秒就退出"是版本问题

```bash
# 1. 计时，确认不是被 timeout 杀的
S=$(date +%s)
timeout 15 $EMULATOR -sysdir $STOCK ... -verbose > /tmp/emu.log 2>&1
echo "存活 $(( $(date +%s) - S )) 秒"     # ~1 秒 = 自己退的

# 2. 找 feature string 警告
grep -i "feature string" /tmp/emu.log

# 3. 对照版本
grep Revision $EMULATOR_DIR/source.properties     # 模拟器版本
grep Dependencies $STOCK/source.properties        # 镜像要求的版本
```

---

## 可用的模拟器版本（dl.google.com 实测）

| 构建号 | 可达 | 说明 |
|---|---|---|
| 8807927 | ✅ 200 | 31.3.10，有 arm64 后端，但配新版原版镜像仍退出 |
| 9189900 | ✅ 200 | 未测 |
| 9322596 | ✅ 200 | 未测 |
| 8961541 | ❌ 404 | |
| 9597831 | ❌ 404 | |

下载地址形如：

```
https://dl.google.com/android/repository/emulator-linux_x64-<构建号>.zip
```

⚠️ 注意仓库 XML（`repository2-3.xml`）里**只有最新的几个构建号**，
老版本不在列表里但直链仍在。`emulator-linux_x64.zip`（不带构建号）是 404。

⚠️ 下载容易中途断开。用 `wget -c` 续传，并且**用 `Content-Length` 校验完整性** ——
`unzip -t` 通过比看文件大小可靠（293822881 字节 = 280.2 MiB，容易被误读成"没下完"）。

---

## 推荐路径

**绕开原版镜像，用自己编的模拟器镜像：**

```bash
./build-images.sh --fast --yes     # sdk_phone64_x86_64 + KVM
./run-emulator.sh --fast
```

理由：
- **同架构 + KVM**：开机几十秒，而 arm64 guest 只能 TCG（10~40 分钟）
- 镜像与模拟器**同源**（同一棵 AOSP 树），不存在版本错配
- 源码级验证完全等价 —— 截图走 libgui/SurfaceFlinger、触控走 uinput、
  协议与分发都与架构无关

只有 ABI 相关的疑虑（指针宽度、对齐、bionic 差异）才必须回到 arm64，
而那条只能靠**真机**，不要指望 x86_64 宿主上的 arm64 模拟器。

---

## 另一个坑：`pgrep -f` 自匹配

`/tmp/queue-emu-arm64.sh` 曾用这种方式等编译结束：

```bash
while pgrep -f 'soong_[u]i' >/dev/null; do sleep 20; done
```

`[u]` 的写法能避免匹配到自己，但**匹配到了另一个等待脚本的命令行** ——
那个脚本里含 `pgrep -f "soong_ui.*autod autodctl"`，于是它自匹配，永远等自己。
两个脚本互相等，永久死锁。

**等待编译结束要用状态判断，不要用 `pgrep -f`。** 现成的工具：

```bash
bash tools/check-no-build.sh    # 退出码 0 = 有编译在跑
```

它用 `ps -eo stat,comm` 并**排除 `Z`（僵尸）状态** —— 容器的 PID 1 是
`sleep infinity`，不回收子进程，被强杀的 `soong_build` 会以 `<defunct>`
永久留在进程表里，`pgrep` 照样匹配得到。
