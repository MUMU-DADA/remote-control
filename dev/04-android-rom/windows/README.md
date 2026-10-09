# windows · 在 x64 Windows 上跑同一份 ROM

> 宿主：Windows 10 1809+ / 11（x64）
> Guest：**Linux 侧编出来的同一份镜像**（`out/target/product/remote_control_x64_arm64/`）
> 加速：**WHPX（Windows Hypervisor Platform）** —— 同架构才有加速，这正是本项目的意义

---

## 为什么这条路在 Windows 上能成立

| | 老的 arm64 路线（跨架构全系统模拟，已实测评估后放弃） | 本项目（x86_64 + 翻译层） |
|---|---|---|
| guest 架构 | arm64 → 只能 TCG，Windows 的 Hyper-V/WHPX **帮不上忙** | **x86_64 → WHPX 直接加速** |
| 模拟器版本 | 34+ 版本已移除 arm64 软件后端，只能用旧版 | 任何现代版本（37.x）都支持 |
| arm64 应用 | 原生跑（但整机慢 10~40 分钟开机） | 用户态翻译层跑（开机几十秒） |

---

## 三步

```powershell
cd windows

.\fetch-emulator.ps1     # 1. 拿 emulator.exe（SDK）+ platform-tools 里的 adb
.\fetch-images.ps1       # 2. 从 Linux 构建机拉同一份镜像（按 SHA256SUMS 校验）
.\preflight.ps1          # 3. 前置检查：虚拟化/WHPX、磁盘、镜像齐全性、与 Linux 侧指纹比对
.\run-windows.ps1        # 4. 启动 + 验收（abilist / 翻译层 / arm64 应用）
```

> 日常开机关机用 **`emulator.ps1`**（`run-windows.ps1` 是"启动 + 验收"的
> 一次性流程，`emulator.ps1` 管实例的整个生命周期）：

```powershell
.\emulator.ps1 list                    # 有哪些实例、在不在跑、占多大
.\emulator.ps1 start  default          # 起（不存在会自动创建）
.\emulator.ps1 stop   default          # 优雅关机（等进程真的退出）
.\emulator.ps1 kill   default          # 卡住了才用：强杀
.\emulator.ps1 clone  default  dev2    # 复制一台（连已装应用一起）
.\emulator.ps1 delete dev2             # 停掉并删光
```

命令表和注意事项与 Linux 侧**完全对应**，见
[`../docs/12-emulator-control.md`](../docs/12-emulator-control.md)。

> **必需文件里最容易漏的是 `-qemu` 家族**（`system/vendor/product-qemu.img` + `ramdisk-qemu.img`）：
> 前者是带 GPT 分区表的包装版，后者是「系统 ramdisk + vendor ramdisk」的合并版
> （first-stage 的 `fstab.ranchu` 在里面）。只给裸 `*.img` 会以
> `partition(s) not found in /sys` / `failed to find device default fstab` 卡死并无限重启——
> 这是本机实测踩过的（见 [`../docs/02-build-traps.md`](../docs/02-build-traps.md) 坑 8）。

前置：OpenSSH 客户端（Win10 1809+ 自带）、≥80 GB 可用磁盘空间（快照或多实例建议 ≥100 GB）、≥8 GB 内存、
**BIOS 里开启虚拟化**（WHPX 需要；若用 Hyper-V/WSL2 已开启则天然满足）。
脚本会在启动前自动创建 `%USERPROFILE%\.android`（**模拟器自己不会建**，缺了它会无限重试刷日志，
见 [`docs/02-build-traps.md`](../docs/02-build-traps.md) 坑 10.1）。

---

## 硬件参数：和 Linux 侧同一份 config.ini

`emulator.ps1` / `run-windows.ps1` 都从 `..\emulator\config.ini` 读，
不在这两个脚本里写死默认值：

| 项 | 默认 |
|---|---|
| 屏幕 | 1280x720 横屏 @320dpi |
| CPU / 内存 | 4 核 / 6144 MB |
| 数据分区 | 64G |
| GPU | `auto`（自适应：有真显卡就 `host`，否则 `swiftshader_indirect`；`host` 起不来还会再退一次） |

> ⚠️ 以前这两个脚本的参数默认值是写死的（`-MemoryMB 4096`、`-Gpu
> swiftshader_indirect`），而命令行**优先于** config.ini —— 于是
> config.ini 里改内存根本没用。现在只在**显式传参**时才覆盖。

---

## 前置条件与注意事项

| 项 | 说明 |
|---|---|
| 虚拟化 | `-accel on` 走 WHPX。**判据用 `emulator -accel-check`**，别用功能勾选状态：Hyper-V 开着（`Microsoft-Hyper-V-All`=Enabled）就够了，`HypervisorPlatform` 显示 Disabled 也可能完全可用（2026-10-09 实测，见下） |
| 模拟器主目录 | 脚本启动前自动建 `%USERPROFILE%\.android`（可用 `$env:ANDROID_EMULATOR_HOME` 换位置）。**模拟器自己不建它**：缺了会报 `Unexpected error while creating: ...lock (error: 3)` 并且**不退出**、只无限重试刷日志 |
| 起不来时 | 脚本盯日志：命中 `error: 3`、缺硬件加速这类"只会重试"的错误会**立刻停进程 + 给中文诊断**，不再等到超时；服务不就绪时 `.\bin\status.ps1` 会打印日志最后一条 ERROR |
| 镜像 | 只需 `system.img / vendor.img / ramdisk.img / kernel-ranchu / encryptionkey.img / userdata.img / build.prop / advancedFeatures.ini / source.properties` + `system\build.prop` |
| `system\build.prop` | 模拟器靠它判断 guest 架构；AOSP 产物里天然有（`out/.../system/build.prop`），scp 时**必须带上 system 子目录** |
| `initrd` | 模拟器 `-initrd` 指向 `<sysdir>\initrd`；AOSP 产物里有，缺了就 `copy ramdisk.img initrd` |
| 首次启动 | WHPX 下几十秒；比 arm64 TCG 的 10~40 分钟快一个量级 |
| 反病毒 | 首次启动可能被实时防护拖慢，可临时排除镜像目录 |

---

## 前置：Windows → Linux 构建机的免密 SSH

`fetch-images.ps1` 是**从 Windows 拉**镜像的，所以 Windows 必须能免密登录构建机。
这一步以前没写在这里 —— 实测第一次跑就卡在这儿（`Permission denied (publickey)`）。

```powershell
# 1. Windows 侧生成密钥（已有就跳过）
ssh-keygen -t ed25519

# 2. 把公钥追加到构建机的 root 的 authorized_keys
type $env:USERPROFILE\.ssh\id_ed25519.pub | ssh root@192.168.0.108 "cat >> ~/.ssh/authorized_keys"

# 3. 验证
ssh root@192.168.0.108 echo OK
```

---

## ✅ 2026-10-08 在真 Windows 上跑过一遍（Windows 10 19044 + PowerShell 5.1）

第一次在真机上验证，**前三次都跑不通**，都是脚本自身的缺陷（已修）：

| # | 症状 | 根因 |
|---|---|---|
| 1 | `preflight.ps1` 直接报语法错误，指着一个完全正确的大括号 | `.ps1` 是 **UTF-8 无 BOM**，而 PowerShell 5.1 无 BOM 时按**系统 ANSI（GBK）**解码 —— 中文注释被解坏，撞坏了语法。**14 个 .ps1 全中**，整条 Windows 路线都跑不起来 |
| 2 | 打印完 `==> 远端：…` 之后再也不动 | `(ssh $Remote "…") -join ""` 这种**捕获 ssh 输出**的写法在 Windows 上会**永远挂住**（`ssh host echo hi` 正常、`cmd /c echo hi` 捕获也正常，只有 ssh 例外；`ssh -n` 也救不了）。改用 `Start-Process -RedirectStandardOutput` |
| 3 | scp 对每个文件报一次 `The system cannot find the path specified.` | `$todo` 里存的是 `"$RemoteDir/$f"`，**少了 `$Remote:` 前缀** —— scp 把它当成本地路径。这个脚本在任何机器上都拉不到东西 |

修完之后：前置检查全绿，镜像按 SHA256SUMS 校验通过（20 个文件）。

> ⚠️ **当时那句"WHPX 未启用是唯一硬项"是错的**，2026-10-09 订正 —— 见下面的
> 「两条被误判的拦路虎」。

`tools/check-scripts.py` 是给第 1 条加的回归闸门：**所有 .ps1 必须带 UTF-8 BOM**。

---

## ⚠️ 仍未验证的部分

> **2026-10-08 决定：B3 就停在这里**（"先不动 WHPX"）。
> 下面这段保留原样 —— 它是"要接着做的话该怎么做"的说明，不是待办欠账。
> 已经验到的部分见上面两节：镜像拉取 + SHA256SUMS 校验、预检全绿、
> SDK/模拟器下载 + sha1 校验、两侧 system.img 指纹一致。

`run-windows.ps1` 的**启动与验收**这一段还没跑过。⚠️ 但**拦路的原因不是 WHPX**
—— 见下面两条订正，那条拦路虎是我自己的误判，现在已经拆掉。

### 两条被误判的拦路虎（2026-10-09 订正）

**① "WHPX 未启用" —— 判据拿错了。** 当时看的是
`Get-WindowsOptionalFeature -FeatureName HypervisorPlatform` 的状态（Disabled），
就下了"加速不可用"的结论。实测那台机器：

| 项 | 值 |
|---|---|
| `Microsoft-Hyper-V-All` | **Enabled** |
| `HypervisorPlatform`（Windows 虚拟机监控程序平台） | Disabled |
| `HypervisorPresent` | True |
| `emulator -accel-check` | **`WHPX(10.0.19044) is installed and usable.`** |

也就是说：**Hyper-V 开着就够用**，那个"虚拟机监控程序平台"的勾不必再单独勾。
正确的判据是**模拟器自己的 `-accel-check`**，`preflight.ps1` 已改成先跑它。

**② `preflight.ps1` 把镜像全报成"缺"** —— 因为 `$PSScriptRoot` 写在 `param` 的
默认值里。带 `[CmdletBinding()]` 时参数绑定发生在 `$PSScriptRoot` 赋值**之前**，
默认值算成了 `\images`（丢了盘符和目录），而且**不报错**。同一台机器 A/B：

```
param([string]$X = "$PSScriptRoot\images")                 → [\images]  ✗
[CmdletBinding()] param([string]$X = "$PSScriptRoot\images") → [\images]  ✗（就是这种）
```

`fetch-emulator.ps1` / `fetch-images.ps1` / `run-windows.ps1` **三个都中同一招**，
默认值都改成了"留空 + 绑定后再补"。四个脚本都已重新语法校验并在真机上跑通
（预检退出码 0）。

`tools/check-scripts.py` 加了这条检查，防止以后再写回去。

原计划说明如下：

- `run-windows.ps1` 的**参数与流程**在 Linux 上用**同源代码的 SDK emulator 37.2.11** 验证过
  （同一份 `google_apis;x86_64` 镜像 29.0 秒开机、属性一致，见评估文档 §2.2）；
- 但 **WHPX 路径本身没有跑过**——首次在 Windows 上执行时请把 `run-windows.ps1` 的输出贴回来。
  出问题时按这个顺序看：**先在交互式桌面会话里跑**（SSH 落在 session 0，模拟器会报
  `SetProcessDpiAwarenessContext … COM error 0x5: 拒绝访问`，这不是 Hyper-V 的问题）、
  再看镜像目录、最后看 adb 版本。**加速本身已经确认可用**，不用再查 WHPX 了。
