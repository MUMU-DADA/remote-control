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
./scripts/emulator.sh start   dev2             # 启动 + 等 HTTP 服务就绪
./scripts/emulator.sh start   dev2 --no-wait   # 起了就返回
./scripts/emulator.sh start   dev2 --test-instance  # 新测试实例：不开鉴权（仅首次启动生效）
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
| `--no-wait` | console 就绪后返回，不等 HTTP 管理服务 |
| `--gui` | 带窗口启动（默认 `-no-window`，服务器上用） |
| `--bridge` / `--nat` | **仅 Linux**：强行桥接 / 强行 NAT（默认自动，见 §6） |
| `--test-instance` | **新实例**不开鉴权：宿主不生成 `.token`，guest 首启就按 `auth=0` 持久化。只在**首次启动**生效 —— 复用实例沿用 guest 已存的设置，flag 不会改写一台已有机器。等价于发布包 `start-headless.sh --test-instance`（Windows `-TestInstance`） |

Windows 侧对应 `-Port` / `-Gpu` / `-Memory` / `-Cores` / `-NoWait` / `-Gui` / `-Yes`。
| `-y` / `--yes` | `reset` / `delete` 跳过二次确认 |

---

## 1. 实例是什么

**实例名 ↔ 端口一一对应**，名字到端口的映射登记在 `.run/instances/<名字>.env`。
默认实例名 `default`（console 端口 5580，ADB 端口 5581）。

普通启动、状态查询和关机使用 emulator console 认证与 HTTP 管理接口，
**不要求 ADB**。console 的 `redir` 将宿主 `127.0.0.1:18088` 转发到 guest
服务端口（默认 8088）；第二个实例默认使用 18089，按 console 端口递增。
console 认证文件默认是 `~/.emulator_console_auth_token`。

首次启动从 `emulator/config.ini` 的 `service.*` 读取服务开关、绑定地址、
端口、鉴权和 ADB 开关。空令牌由宿主生成并保存到
`.run/instances/<名字>.token`，权限 0600；脚本只输出文件位置。
复用实例沿用已保存的 guest 服务设置与令牌，修改模板后需 `reset` 才会
应用新值。鉴权实例若丢失令牌文件，恢复该文件后才能继续启动和关机。

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
| 数据分区 | 64G | `disk.dataPartition.size` |
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
  disk.dataPartition.size  64G

上次启动**实际生效**的（hardware-qemu.ini）：
  hw.ramSize               6144
  disk.dataPartition.size  64g
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

### `stop` —— 请求 Android 正常关机

`stop` 通过已认证的 `POST /api/v1/power` 请求 `shutdown`，由 Android
执行同步和卸载文件系统，再等待模拟器进程实际退出，最多 60 秒。
进程退出前重启会撞上 `multiinstance.lock`，报
`another emulator instance is currently running`。

关机时 HTTP 连接可能先断开。脚本会检查进程是否已经退出；确认退出后
仍报告成功。HTTP 服务或令牌不可用、或超时仍在运行时，普通 `stop` 会
报告失败，由调用者选择 `kill`；发行包的对应入口使用 `--force`。

历史实测（`tools/verify-kill-is-hard-poweroff.sh`）中，写入后未经 `sync`
就执行 `adb emu kill` 会丢失文件，等待 15 秒后硬停止仍会丢失。
因此正常关机依赖 Android 的关机流程。

### `kill` —— console 硬停止，再 SIGKILL

`kill` 先发送 emulator console 的 `kill`，等待 2 秒，仍运行时再向
模拟器进程发送 SIGKILL。这条路径用于卡住的实例，未落盘数据可能丢失。
`stop` / `kill` / `status` 都不调用 ADB。

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
克隆 guest 已在 `/data` 保存服务设置与令牌，Linux 脚本会同时复制其宿主
服务元数据和 `.token` 文件；新实例使用独立 console 和宿主 HTTP 端口。

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
| 目的 | **启动 ROM**，显式 `--verify` 验收 ABI / 翻译层 / arm64 应用 | **管实例的命**（建、起、停、复制……） |
| 默认语义 | 每次全新冷启动（`rm -rf` 工作目录） | 保留状态；要清就明确 `reset` |
| 附带 | 普通启动等 HTTP 就绪；`--verify` 才跑 4 项 ADB 验收 | 只起停，不做验收 |
| 硬件参数 | 从 config.ini 读 | 同左 |

两边共用同一套工作目录与硬件参数，可以混着用。

---

## 6. Windows 侧怎么验的（在没有 Windows 的机器上）

`emulator.ps1` 是在 Linux 上用 PowerShell 7 实跑验证的：

```bash
bash tools/run-all-tests.sh --with-device   # 一把跑完（含本文件这些）
bash tools/test-windows-emulator.sh         # 只跑这一个，全在 Linux 上跑
#   前置：.tmp/pwsh/ 要有 PowerShell 7 的 linux-x64 版（.tmp/ 是 gitignore 的，
#   新克隆不会有）。脚本头部写了确切的下载命令与必需的环境变量。
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

## 7. 导出 / 导入：**两条都已验证**（格式 2）

> ✅ **2026-10-09：恢复修好了。** 归档格式升到 **2**：数据镜像不再原样搬
> （backing + 覆盖层），改成 `qemu-img convert -O qcow2 -c` **压平成一张自包含的
> qcow2**，`import` 时 `convert -O raw` 转回 raw 当 backing —— **单文件、没有覆盖层**。
> 实测（`tools/verify-export-import.sh`，含开机读标记）：**恢复出来的机器有原来的数据**。
> 归档也从 1.9G 降到 161M（只存已分配的簇 + `-c` 压缩）。
>
> **格式 1 的老归档仍然被拒绝**（要 `--unsafe`）—— 那条路线恢复出来 `/data` 是空的，
> 原因仍未查明（但已排除"稀疏空洞布局"那条，见 §7 末尾）。

```bash
./scripts/emulator.sh export  default            # 打成一个归档
./scripts/emulator.sh inspect default-xxx.tar    # 不解包，看里面是什么
./scripts/emulator.sh import  default-xxx.tar -n newname
```

| 命令 | 状态 |
|---|---|
| `export` | ✅ 归档**逐字节忠实**（qcow2 的 md5 与源完全一致，解出来也一致） |
| `inspect` | ✅ 正常 |
| `import` | ✅ **恢复出来的实例数据完好**（2026-10-09 起，格式 2）。老归档（格式 1，没有 `format` 字段的）**仍然拒绝**执行，要 `--unsafe` —— 那条路线恢复出来的 `/data` 是空的，理由见下 |

### 归档格式

```
INSTANCE-MANIFEST.json   名字 / 时间 / ROM 指纹 / 需要哪些镜像
sysdir/…                 实例状态（**不含**指向 ROM 的符号链接）
datadir/…
host-service.env         Linux guest 服务设置（不含宿主端口）
host-service.token       Linux guest 服务访问令牌
```

Linux 导出会携带 guest 的宿主服务凭据，归档权限为 0600。导入恢复同一
组凭据；这仅验证了宿主脚本处理，`import` 的 guest 数据恢复限制仍如下。

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

### ✅ 2026-10-09：探针**跑通了**，而且找到了一条**可用的恢复路径**

这个探针写出来之后一直没有结论记录在案（只留下"写好了"这句）。这次把它跑完，
两路都通过：

```
== 启动 tB（由压平镜像恢复的新实例），看数据在不在
   tB boot=1
   tB /data/local/tmp: [qi-731324]      ← 数据在
   tB /sdcard:         [qi-731324]      ← 数据在
== 对照：tA 自己重启一次
   tA 重启后标记: [qi-731324]           ← 对照组也对
退出码=0
```

**它试的路线**（和 `export`/`import` 的 tar 路线不同）：

1. `qemu-img convert -O qcow2 -c <sysdir>/userdata-qemu.img.qcow2 flat.qcow2`
   —— 读穿 backing 链，压成**一张自包含**的 qcow2（压缩后 149MB，
   而原来两个文件合计 2.5G）；
2. `qemu-img convert -O raw flat.qcow2 <新 sysdir>/userdata-qemu.img`
   —— 转回 raw 当新实例的 backing，**不需要覆盖层**；
3. 起新实例 → guest 里的数据**都在**。

### ⚠️ 同时纠正一条前提：**实占块数不是保真度指标**

探针头部原来把"tar 解开后实占块数差 8 块"当作"空洞布局还原不完全"的证据。
这次两条都验了，**都不成立**：

| 验的东西 | 结果 |
|---|---|
| tar 往返（拿真实的 64 GiB backing） | 源 1126904 块 → 解出 **1126904 块，差 0** |
| 压平路线里的块数变化 | 1,126,904 → **351,528** 块，**而数据完好** |

块数会变是因为 `qemu-img convert` **跳过全零簇**（以及 `-c` 压缩）——
**它是压缩/稀疏策略的产物，和"数据在不在"没有关系**。当初用块数差异
推断保真度损失，方向就错了。

> 所以 §7 顶上说"最可疑的是空洞布局"这条**可以划掉了**。

### ✅ 2026-10-09 定案：**是"恢复后那份覆盖层"的问题**，而且**老归档可救**

第 36 轮把范围压到两处差别（backing 怎么来、覆盖层在不在）。这一轮用**同一份数据**
分别走两条路线，起起来后读同一个标记：

| 恢复方式 | 结果 |
|---|---|
| 格式 1 原样恢复（tar 搬两件套，**backing + 覆盖层都在**） | 开机 ✓，`/data/local/tmp/<标记>` → **No such file** ✗ |
| 把**同一对文件**压平成**单张 raw**（`qemu-img convert -O raw <覆盖层>`）后恢复 | 开机 ✓，标记 = **`fmt1-1791530552`** ✓ |

**字节完全相同**（第 36 轮逐字节、逐块、连 md5 都比过），差别只在
**恢复出来的实例里带不带那份覆盖层**。

**它意味着什么**：`qemu-img convert -O raw <覆盖层>` 会**读穿 backing 链**、
写出**一张自包含的 raw** —— 所以**格式 1 老归档里的数据是能救回来的**，
只要恢复时先把那对文件压平。新代码对格式 2 做的正是这件事，
格式 1 目前只是被闸拦住；**迁移路径是现成的**。

（机制本身仍未完全说清 —— 覆盖层怎么就让 guest 看不到 backing 上的数据，
还没定位到具体那一步。但"压平就好"是**实测**的，不是推断的。）

> 复现用的归档由 `tools/diagnose-format1-archive.sh` 造出（旧代码的布局：
> 原样搬两个文件、manifest 写 `"format": 1`）—— 新代码已经不生产这种归档了。

### 2026-10-09 再进一步：**文件层面已证明忠实，问题在"起来之后"**

| 验的 | 结果 |
|---|---|
| `export dev3` → `import -n tZ --unsafe`，逐文件对比 | `userdata-qemu.img` 表观/实占块数**完全一致**（1,126,904 块）；`userdata-qemu.img.qcow2` 表观**一致**、实占 **2,679,184 块一致**、**md5 相同**（`7e8b3c8e…`） |
| 归档内容 | 22 个条目，覆盖层**在里面**（1.37G），backing 也在 |
| `tools/verify-export-import.sh`（已有的那个，26 项） | **全过** —— 其中一条就是 `★ 已知缺口：恢复出来的机器**没有**原来的数据` |

**两条合起来把范围压到很小**：恢复出来的**文件是对的**（逐字节），
但 **guest 读不到那些数据**。也就是说差别不在"搬了什么"，
而在"**起来之后这些文件被怎么用**"。

和压平路线的差别只剩两处：

| | 压平路线（数据在 ✓） | tar 路线（数据丢 ✗） |
|---|---|---|
| 新实例的 backing | 由扁平镜像**转出的 raw** | **归档里那份**（已证明逐字节相同） |
| 覆盖层 | **删掉**（单文件起） | **一并恢复**（两份都在） |

下一步的入口就是这两处 —— 具体做法：造一份数据，**同一份数据分别走两条路线**，
起起来后对比**运行时**（模拟器实际打开了哪些文件、mtime 变没变、guest 里看到什么），
而不是继续在文件层面找。真正要解释的是
> **为什么 tar 路线恢复出来 `/data` 是空的、而压平路线是好的** —— 两条路线
> 的差别（一个原样搬两个文件、一个读穿链再压平）就是下一步的入口。

> ⚠️ 在查清之前，**不要把这个归档当备份用**。`export` 现在会主动打印
> 这条警告，`import` 不加 `--unsafe` 会直接拒绝。

---

## 8. 网络：默认不会让两台同时上物理 LAN

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

---

## 9. 安全边界一览：**哪些操作会丢数据、默认在哪一侧**

这一节是把散在各个脚本里的"保护开关"盘成一张**可以对着查**的表。
2026-10-09 逐条实测过（不是读代码猜的）。

| 操作 | 保护是什么 | 默认在哪一侧 | 实测/依据 |
|---|---|---|---|
| `delete NAME` | 二次确认 | **安全**：不加 `--yes` / `-Yes` 就停下来问 | 非交互环境下 **20 秒内**报"等了 20 秒没人输入…请显式加 -Yes"，退出非 0，**实例不会被删** |
| `reset NAME` | 二次确认 | **安全**：同上 | 同上（两处走同一个 `Confirm-OrDie`） |
| `import FILE` | 版本闸 | **安全**：不加 `--unsafe` **直接拒绝** | `[x] 已拒绝执行（这是保护，不是故障）`；理由：恢复出来的 `/data` 是空的，见 §7 |
| `stop NAME` | 优雅关机 | **优雅**：先走 HTTP 请求 Android 正常关机；只有显式 `--force` 才允许**降级**为硬停 | `stop.sh` 里 `if ! service_request_poweroff; then [ FORCE ] || die; warn "…未落盘数据可能丢失"; console_kill` |
| `kill NAME` | 无（动词本身就是强制） | **会丢数据**，所以两边都先告警 | Linux：`warn "强制停止可能丢失未保存的数据"`；Windows：`Write-Warn "强制停止可能丢失未保存的数据。"` |
| `restart NAME` | 先优雅、失败才降级 | **优雅优先**，降级时告警 | 仓库侧 `cmd_stop \|\| cmd_kill`；交付侧 `stop.sh` 带 `--force`，效果相同（都先试 HTTP 关机） |
| `start --wipe-data` | **没有二次确认** | 显式 flag 才生效 | ⚠️ 见下面的说明 |

### `--force` 的作用域是**被检查**的

* Linux（交付侧）：一条总检查 —— `--force 只用于 stop/kill/restart/reset/delete`。
* Windows：**按动词查表** —— 每个动词有自己的允许参数集，
  `-Force` 用在 `status` 上会得到 `-force 不适用于 status 命令`。
  这一条比 Linux 更严。

### ⚠️ 一处**刻意的不对称**，写下来免得以后被当成漏做

`delete` / `reset` 要二次确认，而 `start --wipe-data` **不要**。理由：

* 前两个动词**本身**就是破坏性的 —— 敲下去的那一刻意图可能还没成形；
* `--wipe-data` 是一个**显式修饰符**，敲全了才生效，两个平台行为一致。

也就是说这里赌的是"显式写出来的东西就是想要的"。如果你觉得该反过来
（给 `--wipe-data` 也加确认），那是个**产品决定**，不是 bug 修复 ——
改它会同时影响脚本化调用，别顺手改。

---

## 10. 两条血统：仓库侧的管理器和**交付包里的**不是同一份实现

这一节是 2026-10-09 查清楚的，之前一直含糊。

交付包的 `bin/` 是这么来的（`scripts/release.sh`）：

```bash
cp -a "$PACKAGING_DIR/bin/$(target_platform "$plat")/." "$root/bin/"
```

所以：

| | 仓库侧（开发用） | 交付版（用户拿到手的） |
|---|---|---|
| Linux | `scripts/emulator.sh`（918 行） | `packaging/bin/linux/emulator.sh`（451 行） |
| Windows | `windows/emulator.ps1` | `packaging/bin/windows/emulator.ps1` |

**两份是手工维护的两套实现，内容完全不同。** 这一轮之前没有一处文档说清这件事，
于是"两条血统不一样"这个意外在一轮会话里撞见了四次（`-Force` 的作用域校验、
`export`/`import`、`inspect`、`Read-LineWithTimeout` 各写一份）。

### 动词集合的差异（`tools/check-lineage-parity.py` 守着）

| 差异 | 在哪 | 为什么 |
|---|---|---|
| ~~`export` / `import`~~ | ~~只在仓库侧~~ **两边都有了** | 2026-10-09：归档逻辑抽成 `packaging/bin/linux/archive.sh`，两套管理器 source 同一份 —— 原来"只在仓库侧"的理由（`import` 未通过验证）到这一天为止 |
| `verify` | 只在**交付版**（两侧都是） | 交付包自带验收；开发侧走 `run-linux.sh --verify` |
| `inspect` | 只在**交付版 Windows** | 读归档用；归档由 Linux 侧的 `export` 产生 |

> ⚠️ 那个检查**不要求两边一样**（本来就不该一样：交付版是精简过的）。
> 它要求的是：**不一样的地方必须是写下来的、有理由的，而不是悄悄长出来的。**

参数作用域那一层比不了（交付版 Windows 有按动词查表，仓库侧没有），
那部分靠人读 —— 不假装机器能查。

### 交付面文档与这套说法**是一致的**（2026-10-09 核过）

这一条特意写下来，免得以后有人重新怀疑一遍"是不是把没验证的功能当卖点"：

| 文档 | 说什么 |
|---|---|
| `packaging/START-HERE.md` | "当前入口**不提供**归档导出/导入：现有恢复实现尚未验证数据完整性，不应作为备份使用。" |
| `packaging/README.md` | "导出/导入尚未验证恢复后数据完整性，**当前不作为 release 能力提供**。" |
| `windows/README.md`／`macos/README.md` | 一处都没提（交付包也没有这两个动词） |
| 本文档 §7 | `import` 标 ❌ + 失败现象 + 安全闸 + "不要把这个归档当备份用" |

**三层说法一致**：dev 文档把限制写清楚、交付文档明说不提供、交付包确实没有这两个动词。
