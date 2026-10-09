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

> ⚠️ **重新拉镜像之前必须先停掉实例。** 实例在跑的时候，模拟器**占着
> `images\` 里的镜像文件**，Windows 不允许别的进程以写方式打开它们 ——
> `scp` 会报 `open local "…": Broken pipe` 或 `: Failure`，而**网络其实完全正常**
> （2026-10-09 实测，白查了一阵）。
>
> ```powershell
> .\emulator.ps1 stop run-5580      # 先停
> .\fetch-images.ps1 -Force          # 再拉
> .\run-windows.ps1 -Headless        # 最后起（换新镜像后实例要重建/重置）
> ```

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

前置：OpenSSH 客户端（Win10 1809+ 自带）、**≥80 GB 可用磁盘空间（每多一个实例再留 ~65 GB）**、≥8 GB 内存、

> ⚠️ **磁盘这条 2026-10-09 实测修正过**，原来写的是"多实例建议 ≥100 GB"——**那个数在 Windows 上不够**。
>
> NTFS 上 `userdata-qemu.img` 是**实体文件**，一个实例实占 **~64 GiB**
> （`disk.dataPartition.size = 64g`）。用 1 GiB 全零文件做过对照实验：
> `[System.IO.File]::Create` + `SetLength` 出来的文件 **不是稀疏的**，
> 实占 1024 MB；`Copy-Item` 再复制一份又是 1024 MB。
>
> 而 Linux 侧同一个文件是**稀疏**的（`emulator.sh` 里那条 `tar --sparse` 注释写着
> "表观 48G、实占才 551M"）。**这是平台差异，不是脚本缺陷** —— 但直接后果是：
>
> | 用法 | Windows 实占 |
> |---|---|
> | 1 个实例（运行中） | ~65 GB |
> | 2 个实例（都运行中） | ~130 GB |
>
> 所以同时跑两台要按 ~130 GB 准备，`clone` 也要按"**再复制一整份 userdata**"来算，
> 不是"复制几个配置文件"那么便宜。
>
> ### 但**停止时会自动把零块退回去**（2026-10-09 修好）
>
> `storage.ps1` 里本来就有回收机制（`Optimize-ImageStorage` → `CompactZeroes`），
> 但它原来只在 `Build-SysDir`（**创建实例时**）被调用 —— 那一刻 sysdir 里全是
> 指向 `images\` 的链接，而真正占地方的 `userdata-qemu.img` 是**模拟器之后才建出来的**，
> 于是**从来没被回收过**。
>
> 而且"运行中顺手回收"**不行**，实测两个文件全部失败：
>
> ```
> 无法回收镜像零块 userdata-qemu.img：… 正由另一进程使用 …
> C: 可用 661.83 GB        ← 零回收
> ```
>
> **停机那一刻是唯一的窗口。** 现在 `Stop-Instance` 外面包了一层，停止成功后自动回收。
> 实测（同一台实例）：
>
> ```
> 停机后手动回收：C: 665 GB → 729.38 GB     回收 64.00 GiB，耗时 152.7 s
>                 userdata-qemu.img  逻辑 64 GB  实占 0 GB
> 打了补丁之后：  启动 729.38 → 725.84 GB，停止 → 又回到 729.38 GB（自动）
> ```
>
> 也就是说：**运行中占 64 GiB，停下来就还给你** —— 多实例交替跑不再累积占用。
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

## ✅ 2026-10-09：启动与验收全部跑通了

> **B3 完成。** 2026-10-08 那次"停在预检"是**误判**（判据拿错了，见上），
> 不是机器跑不了 —— 补上几处脚本缺陷后，在真机上跑通了全流程。

**第一次真跑就接连撞到 6 个脚本缺陷**（都已修，逐条记在下面），
其中一个直接说明：**这段代码以前不可能跑通**。

### 实测证据（Windows 10 19044 + WHPX + 自编 ROM）

```
==> 验收 1/4：镜像身份与 ABI        [OK] sdk=31 / device=remote_control_x64_arm64 / abilist=x86_64,arm64-v8a
==> 验收 2/4：翻译层接线            [OK] libndk_translation.so / binfmt: arm64_dyn arm64_exe arm_dyn arm_exe
==> 验收 3/4：aarch64 机器码        [OK] libc.so 的 ELF 头 = 03 00 b7 00（e_machine=0xB7=AArch64）
==> 验收 4/4：arm64 应用            [OK] pm install --abi arm64-v8a → Success
                                        [OK] 进程存活 3014
                                        [OK] 映射的 arm64 库条数 16
                                        [OK] 探针原生返回值
                                             PROBE_RESULT arm64-v8a native ok | built_for=arm64-v8a
                                                           | kernel=x86_64 | ptr=64 bit
==> 验收全部通过 ✓
```

服务侧也逐项验过（都是**设备侧/真数据**，不是接口自报）：

| 项 | 证据 |
|---|---|
| 服务起来 | `GET /api/v1/info` → 200，`1280x720`；无令牌 → **401**（鉴权生效） |
| 真抓到画面 | `GET /api/v1/capture` → 230100 字节，文件头 `89 50 4e 47`（PNG）；`?format=jpeg` → 56548 字节，`ff d8 ff` |
| 触控真的生效 | 上滑前后各截一张：哈希 `A5AA45AB…` → `A36919FC…`，字节 230097 → 71331（打开了应用抽屉） |

### 修掉的 6 个缺陷（都在 Windows 侧脚本里）

| # | 症状 | 根因 |
|---|---|---|
| 1 | 预检报"WHPX 未启用" | 拿 `HypervisorPlatform` 的功能勾选状态当判据；实际 **Hyper-V 开着就够**，`emulator -accel-check` 说 usable |
| 2 | 预检把十个镜像逐条报"缺" | `$PSScriptRoot` 写在 `param` 默认值里，`[CmdletBinding()]` 下算成 `\images`。四个脚本全中 |
| 3 | 验收卡在 2/4 | `binfmt_misc` **只有 root adbd 读得到**，脚本没 `adb root`（Linux 侧一直是先提权的） |
| 4 | 验收卡在 4/4，日志停在"用自建 arm64 探针 APK" | `adb push` 把进度写 **stderr**，`$ErrorActionPreference="Stop"` 下变成**终止性 NativeCommandError**，脚本**就地静默退出** |
| 5 | 同一位置的第二个坑 | `$pid` 是 PowerShell **只读自动变量**，赋值抛 `VariableNotWritable` —— 这段**从来没能跑完过** |
| 6 | 探针装不上/启动不了 | 交付的 `arm64-probe.apk` 停在改名之前（包名 `org.autosnap.…`，脚本写死 `org.remotecontrol.…`）；而 `build-probe-apk.sh` 断言了一个**源码里不存在**的 class 路径，所以产物**再没重建过** |

还有两个是"假绿"，值得单独记：

* **0 字节当成"有"**：`Test-Path` 只看存在性，一个拉取失败留下的空 `arm64-probe.apk` 会让脚本去装空包；
* **logcat 缓冲骗过断言**：`logcat -d` 把整个缓冲区 dump 出来，上一次运行留下的
  `arm64-v8a native ok` 能满足断言 —— 这一次哪怕 JNI 报 `satisfiedLinkError` 也是绿的。
  （Linux 侧的 `run-linux.sh` 早就有"先清 logcat"这一步，注释写着"彩排时踩过"；Windows 侧没跟上。）
  顺带查出探针 C 侧的 JNI 符号多了一个下划线（`org_remote_control_…` vs 包名 `org.remotecontrol.…`），
  **探针其实从来没真正调到过原生代码** —— 修完才是真的。

---

## 原计划的说明（保留）

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
