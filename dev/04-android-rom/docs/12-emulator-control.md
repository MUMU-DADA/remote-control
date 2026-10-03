# 12 · 模拟器实例控制

**Linux 和 Windows 各一套**，命令名、语义、配置来源完全对应：

| | Linux | Windows |
|---|---|---|
| 控制脚本 | `scripts/emulator.sh` | `windows/emulator.ps1` |
| 唯一配置源 | `emulator/config.ini` | **同一份** `emulator/config.ini` |
| 实例目录 | `.run/`（在 `dev/04-android-rom/` 下） | `windows/.run/` |
| 加速 | KVM | WHPX |
| 镜像来源 | `artifacts/rom-<product>/` | `windows/images/`（`fetch-images.ps1` 拉的同一份） |
| 网络 | 可桥接到物理 LAN（`-net-tap`） | 只有用户态 NAT（`-net-tap` 只在 Linux 实现） |

本文以 Linux 侧为例写；Windows 侧把 `./scripts/emulator.sh` 换成
`.\windows\emulator.ps1`、`--xxx` 换成 `-Xxx` 即可（选项表见下）。

## 速览

```bash
./scripts/emulator.sh create  dev2             # 建一台（备好工作目录，不启动）
./scripts/emulator.sh start   dev2             # 启动 + 等开机完成
./scripts/emulator.sh start   dev2 --no-wait   # 起了就返回
./scripts/emulator.sh stop    dev2             # 优雅关机（等进程真的退出）
./scripts/emulator.sh kill    dev2             # 强制关闭（SIGKILL）
./scripts/emulator.sh restart dev2
./scripts/emulator.sh reset   dev2             # 重置：清数据分区，回出厂状态
./scripts/emulator.sh clone   dev2  dev3       # 复制（连已装应用一起）
./scripts/emulator.sh delete  dev2             # 停掉并删光
./scripts/emulator.sh list                     # 所有实例
./scripts/emulator.sh status  dev2              # 一个实例的配置 vs 实际生效值
```

| 选项 | 作用 |
|---|---|
| `--port N` | 指定端口（默认从 5580 起偶数自动分配） |
| `--gpu M` | 覆盖 GPU 档位（`host`/`swiftshader_indirect`/…）。**给了就不自动回退** |
| `--memory MB` / `--cores N` | 覆盖 config.ini 里的值 |
| `--no-wait` | 启动后不等开机 |
| `--gui` | 带窗口启动（默认 `-no-window`，服务器上用） |
| `--bridge` / `--nat` | **仅 Linux**：强行桥接 / 强行 NAT（默认自动，见 §6） |

Windows 侧对应 `-Port` / `-Gpu` / `-Memory` / `-Cores` / `-NoWait` / `-Gui` / `-Yes`。
| `-y` / `--yes` | `reset` / `delete` 跳过二次确认 |

---

## 1. 实例是什么

**实例名 ↔ 端口一一对应**，名字到端口的映射登记在 `.run/instances/<名字>.env`。
默认实例名 `default`（端口 5580）。

工作目录沿用 `run-linux.sh` 的**按端口派生**规则，所以两套脚本可以混用 ——
`emulator.sh` 建的机器，`run-linux.sh --port 5584 --reuse` 照样能起来做验收：

```
.run/sysdir-<port>/     镜像（符号链接到 artifacts/，只读）+ 状态 + 快照
.run/datadir-<port>/
.run/emulator-<port>.log
```

> **实例的全部状态都在工作目录里** —— `build.avd/`、`*.qcow2`（userdata /
> cache / encryptionkey 的覆盖层）、`snapshots/`。所以"删工作目录"就等于
> "这台机器彻底消失"，`delete` 做的就是这件事。

---

## 2. 硬件参数：唯一真源是 `emulator/config.ini`

`create` / `start` 都从 [`emulator/config.ini`](../emulator/config.ini) 读，
命令行只是在**显式传参**时才覆盖：

| 项 | 默认 | 出处 |
|---|---|---|
| 屏幕 | 1280x720 横屏 @320dpi | `hw.lcd.*` / `skin.*` |
| CPU / 内存 | 4 核 / 6144 MB | `hw.cpu.ncore` / `hw.ramSize` |
| 数据分区 | 32G | `disk.dataPartition.size` |
| GPU | `auto`（自适应） | `hw.gpu.mode` |

> ⚠️ **不要在命令行上写死这些值。** 命令行**优先于** config.ini ——
> `run-linux.sh` 以前写死 `-memory 4096`，结果 config.ini 里的
> `hw.ramSize` 改什么都没用（`hardware-qemu.ini` 里永远是 4096）。
> 这个坑实测踩过，现在两边都改成从 config.ini 读了。

`status` 会把**配置里的值**和**上次启动实际生效的值**并排打出来，
改完 config.ini 忘了清状态时一眼就能看出来：

```
config.ini（唯一真源 dev/04-android-rom/emulator/config.ini）：
  hw.ramSize               6144
  disk.dataPartition.size  32G

上次启动**实际生效**的（hardware-qemu.ini）：
  hw.ramSize               6144
  disk.dataPartition.size  32g
```

---

## 3. GPU 自适应：探测 + **真的起来了**才算数

```
hw.gpu.mode = auto   →  宿主有可用的 /dev/dri/renderD* ?  host : swiftshader_indirect
                        再兜一层：host 起不来 → 自动退 swiftshader_indirect
```

两层是必要的，因为**有渲染节点 ≠ 驱动能用**。实测（本机 VMware 里的
VMware SVGA II 适配器）：`/dev/dri/renderD128` 存在也可读写，于是自适应
选了 `host`，结果模拟器直接报

```
ERROR | Could not start renderer! (Error: -2)
ERROR | Could not start renderer
```

然后被第二层接住，退到 `swiftshader_indirect` 正常开机。**只看探测结果的
"自适应"在这台机器上是起不来的** —— 所以判定标准是"真的起来了"。

判定与回退在 `scripts/common.sh` 的 `host_gpu_available()` / `resolve_gpu_mode()`；
`run-linux.sh` 和 `emulator.sh` 用的是同一套。想钉死某一档就显式传
`--gpu swiftshader_indirect`（显式指定时**不**回退，免得掩盖真实问题）。

---

## 4. 各命令的注意事项

### `stop` —— 先 `sync`，再 kill

> ⚠️ **`adb emu kill` 不是优雅关机，是硬断电。** 它给 QEMU 发信号让它立刻
> 终止，guest 根本没机会卸载文件系统或提交日志。
>
> 实测（`tools/verify-kill-is-hard-poweroff.sh`，同一台实例三组对照）：
>
> | 做法 | 重启后 |
> |---|---|
> | 写入后 `sync` 再关 | **在** |
> | 写入后不 sync 直接关 | **没了** |
> | 写入后等 **15 秒**再关 | **还是没了** |
>
> 后果不是"丢最后一点"，而是**最近写的东西整个没**，而且毫无征兆。
> 项目早期那条悬案「模拟器 `/data` 不持久（根因未查明）」就是它
> （另一半原因是 `run-linux.sh` 不带 `--reuse` 会 `rm -rf` 工作目录，
> 那是设计如此）。
>
> 所以 `stop` / `kill` 都会**先 `adb shell sync` 再 kill**。手动关机的话
> 记得自己先 `adb -s <序列号> shell sync`。

### `stop` 等的是**进程退出**，不是"命令返回"

`adb emu kill` 只是递个关机请求，guest 还要走完关机流程（卸载 `/data`、
收 qcow2）。这时候就重启会撞上 `multiinstance.lock`，第二台报
`another emulator instance is currently running`。所以 `stop` 最多等 60 秒
直到进程真的没了，超时会明确让你改用 `kill`。

### `kill` —— 先请它关，再 SIGKILL

强杀之前仍然会发一次 `adb emu kill` 并等 2 秒。不是为了"更快"，是因为
正常路径能把 qcow2 元数据落盘，直接 SIGKILL 会丢一点最后一次写入。
强杀是为了"现在就关掉"，差那 1~2 秒不值当。

### `reset` —— 不可逆

删掉数据分区和状态（`userdata-qemu.img*`、`cache.img*`、
`encryptionkey.img.qcow2`、`build.avd/`、`snapshots/`、`hardware-qemu.ini`），
**实例本身留着**（名字、端口、镜像、config.ini 不动）。

已装应用、应用数据、`/sdcard` 下的文件、快照，全没。默认要手打 `yes` 确认。

顺带的好处：`hardware-qemu.ini` 一起删掉了，所以**改过 config.ini 之后
reset 一次，新参数就全生效**（尤其是 `disk.dataPartition.size` ——
它不删 `userdata-qemu.img*` 是不会跟着变的）。

> 镜像本身在工作目录里是**符号链接**，`rm -f` 删的是链接不是目标 ——
> 所以 `reset`/`delete` 不会碰到 `artifacts/rom-*` 里那份交付产物。

### `clone` —— 连已装应用一起复制

源实例**会被停掉**（跑着的时候 qcow2 还在写，抄出来是脏的），
然后把整个工作目录 `cp -a` 过去（`-a` 保留符号链接，镜像不复制实体 ——
两台共用同一份只读镜像是正确的）。

复制完必须清掉这几样，让模拟器**按新路径重建**：

| 删掉 | 为什么 |
|---|---|
| `hardware-qemu.ini` | 里面 `disk.*.path` 全是**绝对路径**，指着原实例的工作目录。照抄过去两台机器会读写同一份 `userdata` —— 那不是复制，是共用一块硬盘 |
| `emu-launch-params.txt`、`*.lock`、`bootcompleted.ini` | 上一次运行的残留 |
| `snapshots/` | 快照里的 `hardware.ini` 绑定了存档时的硬件配置与路径，换实例一定对不上（模拟器会拒绝加载），不如不复制 |

`config.ini` 会用当前真源重新覆盖一份，保证新实例带上最新的硬件参数。

Windows 克隆时任一状态文件复制失败都会使操作失败，并清理目标目录与新登记；
不会把部分复制的实例报告为成功。Linux 侧的复制命令也受脚本错误处理约束，
失败会中止克隆。

**独立性有专门的验证脚本**：

```bash
./tools/verify-clone-independent.sh
```

它在克隆出来的机器上写一个标记文件，再回原实例里找 —— 找不到才算过。

（网络会自动走 NAT —— 见下面 §6。）

### `delete` —— 删工作目录

停掉（卡住就强杀）+ `rm -rf` 工作目录 / datadir / 日志 + 注销登记。
交付目录 `artifacts/rom-*` 不受影响。

---

## 5. 和 `run-linux.sh` 的分工

| | `run-linux.sh` | `emulator.sh` |
|---|---|---|
| 目的 | **启动并验收 ROM**（ABI / 翻译层 / arm64 应用） | **管实例的命**（建、起、停、复制……） |
| 默认语义 | 每次全新冷启动（`rm -rf` 工作目录） | 保留状态；要清就明确 `reset` |
| 附带 | 跑 4 项验收 | 只起停，不做验收 |
| 硬件参数 | 从 config.ini 读 | 同左 |

两边共用同一套工作目录与硬件参数，可以混着用。

---

## 7. Windows 侧怎么验的（在没有 Windows 的机器上）

`emulator.ps1` 是在 Linux 上用 PowerShell 7 实跑验证的：

```bash
bash tools/test-windows-emulator.sh      # 55 项，全在 Linux 上跑
```

做法是搭一个沙箱（把 `windows/` 整个复制到 `.tmp/` 下），放几个假的
"镜像"文件和假的 `adb.exe`，然后真跑 `create / list / status / clone /
reset / delete`，断言文件系统层面的结果 —— **绝不碰真的 `images/` 和实例**。

能在 Linux 上验的是**文件与生命周期逻辑**（链镜像、复制、重置、删除，
这些出错会直接毁数据）；验不了的是进程管理（`Get-CimInstance` /
`Start-Process` 是 Windows 专有）。

这个测试一上来就抓到两个真问题：

1. **`New-Item -ItemType Junction` 在非 Windows 上既不报错也不建东西**
   —— 命令"成功"、`Test-Path` 却是 false。原来那版直接 `return "junction"`
   就走了，结果工作目录里**静默少了一个目录**，要等模拟器起来报
   "文件找不到"才发现。现在每一档链接都**读回确认**，并且
   `Build-SysDir` 收尾会核对 `images\` 里每一项都在。
2. **`run-windows.ps1` 有一处语法错误，那个脚本从来没能运行过**：
   `Write-Host "... $(((& $adb ...) -join "").Trim())"` —— 双引号字符串里的
   `$( )` 里再套 `""`，PowerShell 词法分析直接崩。语法分析不需要 Windows，
   所以测试的第 [0] 节会解析 `windows/*.ps1` 下的每一个文件。

> 教训和本项目其它地方一样：**"命令没报错"不等于"事情做成了"，
> 要读回结果**。还有 —— 写完的脚本得有个不用目标平台就能跑的检查，
> 否则它会一直躺在那里没人知道是坏的。

---

## 6. 导出 / 导入：**导出可用，恢复未通过验证**

```bash
./scripts/emulator.sh export  default            # 打成一个归档
./scripts/emulator.sh inspect default-xxx.tar    # 不解包，看里面是什么
./scripts/emulator.sh import  default-xxx.tar -n newname --unsafe
```

| 命令 | 状态 |
|---|---|
| `export` | ✅ 归档**逐字节忠实**（qcow2 的 md5 与源完全一致，解出来也一致） |
| `inspect` | ✅ 正常 |
| `import` | ❌ **恢复出来的实例 `/data` 是空的**。已加安全闸：不加 `--unsafe` 直接拒绝执行 |

### 归档格式

```
INSTANCE-MANIFEST.json   名字 / 时间 / ROM 指纹 / 需要哪些镜像
sysdir/…                 实例状态（**不含**指向 ROM 的符号链接）
datadir/…
```

两个设计要点：

* **镜像不进归档**（5.7G 只读镜像没意义），只记下"要哪些"和 ROM 指纹，
  恢复时按本地那份重新链；指纹不一致会警告。
* **必须 `tar --sparse`**：`userdata-qemu.img` 表观 48G、实占 551M。
  不加的话归档会从 1.5G 涨到 50G。GNU tar 的 `--sparse` 走 SEEK_HOLE，
  空洞根本不读。

### 为什么 `import` 被拦住了

**实测（复现三次）**：

| 实验 | 结果 |
|---|---|
| 原实例重启（对照组） | 标记文件**在** ✓ |
| 归档解出来手工对比 | 与源**逐字节相同** ✓ |
| 从归档恢复到新实例 → 开机 | 标记文件**没了** ✗ |

也就是说：**归档是好的，恢复出来却不对**。已经排除的：

* ❌ 归档不忠实 —— qcow2 的 md5 与源完全相同
* ❌ 解包丢数据 —— 解开后的 qcow2 md5 与归档内一致
* ❌ `hardware-qemu.ini` 被删（绝对路径指错机器）——
  单独删掉它再启动原实例，数据**还在**
* ❌ `--sparse` 本身有 bug —— 合成稀疏文件（3G 容器 8M 数据）往返逐字节一致

**最可疑、还没证实的**：那个 32G 的稀疏 raw backing 文件。tar 解开后它的
**实占块数**和源差 8 块（1126712 → 1126704），而文件系统的空洞布局对
qcow2 覆盖层是有意义的 —— 覆盖层里没存的簇要去 backing 上读，
空洞位置错了就可能读到错的内容。合成测试数据太少没暴露出来。

**下一步该试的**：不要用 tar 装那两个文件，改用模拟器自带的 `qemu-img`
把「raw backing + qcow2 覆盖层」**压平成一张自包含 qcow2**
（`qemu-img convert -O qcow2 -c`），恢复时再 `convert -O raw` 转回去。
这样只存已分配的簇，根本没有稀疏文件这回事，
而且 Linux/Windows 两侧模拟器包里都自带 `qemu-img`。
探针脚本已经写好了：`tools/diagnose-qemuimg-export.sh`。

> ⚠️ 在查清之前，**不要把这个归档当备份用**。`export` 现在会主动打印
> 这条警告，`import` 不加 `--unsafe` 会直接拒绝。

---

## 7. 网络：默认不会让两台同时上物理 LAN

guest 的 `eth0` MAC 是 QEMU 的默认值 `52:54:00:12:34:56`，**所有实例一模一样**
（模拟器没暴露改它的参数，见 [`11-snapshots-and-multi.md`](11-snapshots-and-multi.md) §4）。
两台同时桥接 = 局域网上出现两个同 MAC 的设备。

所以 `emulator.sh` **默认按"桥有没有被别的实例占用"决定**：

| 情况 | 结果 |
|---|---|
| 桥不存在 / 没开桥接 | NAT（和以前一样） |
| 桥空着 | **桥接**（第一台照常拿物理 LAN） |
| 桥的 `brif` 里已经有 `tap*` | **NAT**（第二台自动让开） |
| `--bridge` / `--nat` | 强行指定，不看上面 |

> 这个坑是实测踩出来的：先跑了 `run-linux.sh`（它按设计总是桥接），
> 又用 `emulator.sh` 起了第二台，结果 `br0` 的 `brif` 里同时挂着
> `tap5580` 和 `tap5582` —— 两台 guest 同 MAC 在同一个二层域里。
>
> ⚠️ `run-linux.sh` 是**专家工具**，它仍然总是桥接（验收流程依赖网络行为），
> 所以**别在已经有一台桥接着的时候跑它**。要临时不桥接：
> `NET_BRIDGE_IF= ./scripts/run-linux.sh --port N`。
