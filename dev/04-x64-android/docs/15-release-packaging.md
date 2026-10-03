# 15 · release 打包：一个平台一个 zip

> 一条命令产出**两个平台**的可交付包，每个包里都有三样东西：
> **完整无头运行环境** + **对应的虚拟机镜像** + **模板**。
> 命令：`./scripts/release.sh`　产物：`release/autosnap-<版本>-<平台>-x86_64.zip`

> ℹ️ **平台范围（后加的）**：`--platform` 现在支持 `linux` / `windows` / **`darwin`** / `both` / `all`。
> `both` 仍是 linux+windows（默认值没变）；加 macOS 那条线用 `--platform darwin`
> （还可选 `--darwin-arch aarch64|x64`）。
>
> ⚠️ **一次调用只认一份 ROM**：`darwin --darwin-arch aarch64` 需要 **arm64 原生 ROM**，
> 而 linux/windows/Intel-Mac 需要现有的 **x86_64 桥 ROM** —— 所以两者要**各打一次**。
> 平台与 ROM 不配套时 `release.sh` 会直接拒绝（不会产出"装得上、起不来"的包）。
> 细节与实测见 [`13-macos-port.md`](13-macos-port.md) §7.0.11、§7.0.13。

## 0. 本轮实测（真跑出来的数字）

| 项 | linux-x86_64 | windows-x86_64 |
|---|---|---|
| zip 体积 | **1.74 GiB**（511 个条目） | **1.85 GiB**（495 个条目） |
| 解压后 | 14.10 GiB | 14.50 GiB |
| 运行时 | `emulator-linux_x64-16428233.zip`（37.2.12 / build 16428233） | `emulator-windows_x64-16428233.zip`（**同一 build id**） |
| 无头后端 | `qemu/linux-x86_64/qemu-system-x86_64-headless` | `qemu/windows-x86_64/qemu-system-x86_64.exe` |
| 整包 `SHA256SUMS` | 447 个文件，`sha256sum -c` 全过 | 同左（435 行） |
| 解压即用 | ✅ **真启动**：`Boot completed in 25283 ms`，四组验收全绿，`stop.sh` 干净停机 | ⊘ 本机没 Windows（见 §9） |

```bash
# 一条命令打两个平台（运行时已缓存时不重下；首次下 333 MiB + 434 MiB）
cd dev/04-x64-android
./scripts/release.sh --smoke --smoke-port 5588 --smoke-adb /usr/bin/adb
```

冒烟的四组全部是**从解压出来的 zip 里**跑的（不是从仓库）：ABI `x86_64,arm64-v8a`、
翻译层接线（`libndk_translation.so` + binfmt `arm64_exe/arm64_dyn`）、自带的 aarch64
静态 ELF 打印 `ARM64_OK`、纯 arm64-v8a 探针 APK 装上并跑起来（16 条 `/system/lib64/arm64/*`
映射、`primaryCpuAbi=arm64-v8a`、`PROBE_RESULT ... kernel=x86_64`）。

---

## 1. 产物长什么样

```
release/
├── autosnap-<版本>-linux-x86_64.zip
└── autosnap-<版本>-windows-x86_64.zip

    解压后（以 linux 为例，windows 同构）：
    autosnap-<版本>-linux-x86_64/
    ├── START-HERE.md          ← 首读：一页纸的"解压就能跑"
    ├── RELEASE.json           ← 版本 / ROM 指纹 / 运行时 build id / 入口清单
    ├── SHA256SUMS             ← 整包逐文件校验（除自己外的每个文件都在里面）
    ├── bin/                   ← 入口：start-headless / stop / status / verify
    ├── runtime/               ← ① 无头运行环境（模拟器 + qemu x86_64 后端 + 自带 adb）
    │   └── RUNTIME.txt        ← 包名 / 渠道 / 版本 / build id / 来源 URL / sha1
    ├── images/                ← ② 虚拟机镜像（ROM 交付目录原样）
    │   ├── system-qemu.img … kernel-ranchu … system/build.prop
    │   ├── SHA256SUMS         ← ROM 自带的逐文件清单
    │   └── MANIFEST.txt       ← 指纹 / abilist / 翻译层统计
    ├── templates/             ← ③ 模板：config.ini（硬件唯一真源）/ instance.env / 说明
    └── tools/                 ← 验收探针（arm64-probe、arm64-probe.apk）
        └── net-bridge*.sh     ← 仅 linux 包：guest 桥接到物理 LAN
```

**为什么两个包各自带一份镜像**：交付方要的是"一个 zip 拿走就能跑"，
而不是"再配一个镜像包"。镜像在打包机上是**硬链接**进 staging 的（不复制实体），
所以两个 zip 的成本只是压缩时间，不是双份磁盘。

---

## 2. 怎么打

```bash
cd dev/04-x64-android

./scripts/release.sh                    # 两个平台各一个 zip
./scripts/release.sh --platform linux   # 只打 linux
./scripts/release.sh --list             # 只打印计划（不下载、不打包）
./scripts/release.sh --stage-only       # 只铺 staging（调结构用，不压缩）
./scripts/release.sh --zip-only         # 复用已有 staging，只压缩
./scripts/release.sh --zip-only --reuse-zip   # 连压缩也跳过：拿已有 zip 补跑冒烟
./scripts/release.sh --smoke            # 打完解压 linux 那份**真启动验收**
```

| 选项 | 作用 |
|---|---|
| `--version VER` | 发布版本号（默认 `<日期>-<git 短 sha>`，已跟踪文件有未提交改动时带 `-dirty`） |
| `--platform linux\|windows\|both` | 打哪些平台（默认 both） |
| `--reuse-zip` | 配合 `--zip-only`：已有 zip 就不重压，只做结构自检（补跑冒烟用） |
| `--channel NAME` | 模拟器渠道（默认 `Stable`） |
| `--images DIR` | 指定 ROM 交付目录（默认 `artifacts/rom-<product>`） |
| `--out DIR` | 输出目录（默认 `release/`） |
| `--zip-level N` | 压缩级别（默认 **1**：镜像那类数据 `-1` 与 `-6` 体积差 ~3%，时间差一倍） |
| `--*-emulator-zip PATH` / `--*-platform-tools-zip PATH` | 用本地包代替下载（**完全离线**打包，测试就靠它） |
| `--no-download` | 只用缓存，缺什么就报错 |
| `--slim` | 精简运行时（删 `include/` `lib/cmake` `lib/pkgconfig` `resources/macroPreviews`） |
| `--keep-stage` | 保留 staging（排查包内容时用） |
| `--no-verify-images` | 跳过 `sha256sum -c images/SHA256SUMS`（省一次 6 GB 读） |
| `--smoke` / `--smoke-port N` / `--smoke-adb PATH` | 打完真启动验收（见 §7） |

---

## 3. 运行时装的是哪一版：**两个平台同一个 build id**

运行时**不是**"拿本机现成的那份模拟器"，而是按 SDK 清单现取的：

1. 取 `https://mirrors.cloud.tencent.com/AndroidSDK/repository2-3.xml`（缓存到 `.run/release-cache/`）；
2. 按 `<remotePackage path="emulator">` + **渠道** + **host-os** 挑 archive：
   - 渠道不是用名字写的 —— 清单里是 `<channelRef ref="channel-0"/>` + `<channel id="channel-0">Stable</channel>`；
   - 每个包有**按 host-os 分的多个 archive**（linux / windows / macosx），
     直接取第一个会下到错的平台（这两个坑在 `windows/fetch-emulator.ps1` 里踩过，release.sh 同样处理）。
3. 下载 → **sha1 校验**（清单里给了就一定要对）→ 解包；
4. 核对**无头后端存在**，删掉其它平台/架构的 qemu 后端：
   - linux：`emulator/qemu/linux-x86_64/qemu-system-x86_64-headless`
   - windows：`emulator/qemu/windows-x86_64/qemu-system-x86_64.exe`
5. 版本、build id、来源 URL、sha1 写进 `runtime/RUNTIME.txt`（包内可查）与 `RELEASE.json`（机读）。

> **为什么钉渠道不钉"最新"**：两个平台的成品必须能说"是同一份工程"。
> Stable 渠道里 linux 与 windows 是**同一次发布**（同 build id），
> 这正是"两边跑的是同一份"最硬的证据。

`platform-tools`（adb）同样按 host-os 取，打进 `runtime/platform-tools/` ——
这样"解压即用"不要求宿主装 Android SDK。想用宿主那份：
`AUTOSNAP_ADB=/usr/bin/adb ./bin/start-headless.sh`。

---

## 4. 镜像怎么进去的

* `cp -al` **硬链接**进 staging：zip 只读，5.7 GB 不复制第二份（跨文件系统时自动退回复制）。
* 打包前 `sha256sum -c images/SHA256SUMS` 验一遍 ROM 交付目录**没被动过**。
* 整包 `SHA256SUMS` 里镜像那部分**直接复用 ROM 自带的清单**（改写路径），
  只对非镜像文件现算 —— 否则 6 GB 要被哈希两遍。
* 顺序上先写 `RELEASE.json` 再算 `SHA256SUMS`：**保证包内每个文件都在清单里**
  （反过来的话 `RELEASE.json` 自己就成了漏网之鱼）。

---

## 5. 模板（templates/）

| 文件 | 是什么 |
|---|---|
| `config.ini` | **AVD 硬件配置模板**：屏幕 / 内存 / 核数 / GPU 档位 / 数据分区。来源是项目的唯一真源 `emulator/config.ini`，打进来的是一份拷贝 |
| `instance.env` | 实例登记模板（名字 ↔ 端口），启动脚本按这个格式写 `.run/instances/<名字>.env` |
| `README.md` | 每个模板改了会怎样 + 工作目录布局说明 |

启动脚本**每次都读模板**，命令行只在显式传参时覆盖 ——
这条是硬要求：命令行优先于 `config.ini`，随手写死默认值会让模板失效
（上游为此踩过 `-memory 4096` 把 `hw.ramSize` 架空的坑）。

---

## 6. 包内入口脚本：同一套语义，两个平台

| | Linux | Windows |
|---|---|---|
| 启动（无头） | `bin/start-headless.sh` | `bin\start-headless.ps1` |
| 停止 | `bin/stop.sh` | `bin\stop.ps1` |
| 状态 | `bin/status.sh` | `bin\status.ps1` |
| 验收（4 组） | `bin/verify.sh` | `bin\verify.ps1` |
| 硬件参数 | `templates/config.ini`（**两边同一份**） | 同左 |
| 加速 | KVM（`-accel on`，没有就退 TCG 并明确告警） | WHPX（`-accel on`，未启用会告警怎么开） |
| 网络 | 默认 NAT；`--bridge` 可桥到物理 LAN（`tools/net-bridge*.sh`） | 只有 NAT |

三条**写死在脚本里**的规矩（都是踩出来的，别"优化"掉）：

1. **停机器先 `adb shell sync` 再 `adb emu kill`。** `emu kill` 是硬断电不是关机：
   实测"写完不 sync 直接停"会让最近写的数据**整个消失**（等 15 秒再停也一样）。
2. **`initrd` 与 `config.ini` 绝不在工作目录里做链接。** 模拟器会**透过符号链接**
   重写 `initrd`，链过去就改到 `images/` 里那份、`SHA256SUMS` 当场对不上；
   `config.ini` 要按模板覆盖，链过去会写穿。
3. **`images/` 只读**：启动时在 `.run/sysdir-<端口>/` 建工作目录把镜像链进去，
   模拟器写的状态（`userdata-qemu.img`、快照、`build.avd/`）全落在 `.run/` 下。

Windows 侧的链接按能力降级（符号链接 → 硬链接 → 复制；目录用 junction），
并且**每一档都读回确认** —— 实测在非 Windows 的 PowerShell 上
`New-Item -ItemType Junction` 既不报错也不建东西，静默少一个目录。

---

## 7. 怎么证明这个包是好的

两层，一层不依赖网络/镜像，一层真启动：

```bash
# ① 快速体检（几十秒，不需要网络，不需要真镜像）
bash tools/test-release.sh
#   [0] 所有脚本语法（bash -n + pwsh 语法分析）
#   [1] 假 ROM + 假运行时 → 真跑 release.sh → 断言两个 zip 的结构、
#       SHA256SUMS 逐文件校验通过、清单覆盖每个文件、RELEASE.json 字段、
#       START-HERE 占位符全替换、包内没有别的平台的后端
#   [2] 解压出来的包能自述（status、--help、Windows 工作目录构建）

# ② 真包 + 真启动（下载运行时、打两个 zip、解压 linux 那份起来做 4 组验收）
./scripts/release.sh --smoke --smoke-port 5588 --smoke-adb /usr/bin/adb
```

`--smoke` 会：解压 → `bin/start-headless.sh`（等 `sys.boot_completed=1`）→
`bin/verify.sh`（ABI / 翻译层 / aarch64 静态 ELF / 纯 arm64-v8a 探针 APK）→ `bin/stop.sh`。
冒烟目录留在 `.run/release-smoke/`（含启动日志与验收输出），`--clean-smoke` 可清掉。

> 冒烟默认用宿主的 `/usr/bin/adb`（`--smoke-adb`）：打包机上可能已经有别的
> adb server 在服务正在跑的实例，用自带 adb（版本不同）会把它踢掉重启。
> 包**默认**仍然用自带的 adb —— 那是交付给别人的形态。

---

## 8. 坑记录（都在这套脚本里修掉了）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | `curl: (6) Could not resolve host: emulator-linux_x64-16428233.zip` | SDK 清单里的 `complete/url` 是**相对路径**，直接拿去下载当成了主机名 | 不以 `http` 开头就拼 `$SDK_MIRROR/` |
| 2 | `sha256sum -c` 在包里必然失败 | 清单的中间文件写在**包根**（`SHA256SUMS.images` / `.rest`），被自己的 `find` 收进清单，打完包就消失了 | 中间文件挪到 staging 目录 |
| 3 | 清单里没有 `RELEASE.json`，行数也对不上 | 先算清单后写 json；计数又漏算了 `RELEASE.json` | 先写 `RELEASE.json` 再算 `SHA256SUMS`；计数按"除自己之外"算 |
| 4 | 清单漏了 `images/MANIFEST.txt` | ROM 自带的 `SHA256SUMS` 是在写 `MANIFEST.txt` **之前**算的（`package-rom.sh` 的顺序如此），所以镜像里有文件不在那份清单里 | 用 `comm` 找出"镜像里清单没覆盖的文件"补算（`comm` 必须与 `sort` 同用 `LC_ALL=C`，否则报"没有正确排序"并退出非 0） |
| 5 | 结构自检把 `system-qemu.img` / 无头后端 / `templates` **全报成缺失** | `unzip -Z1 \| grep -qxF` 里 grep 一命中就退出，unzip 吃 **SIGPIPE(141)**，`pipefail` 下一律非 0 —— 文件明明在包里 | 清单先落成文件再比对（`smoke_linux` 里 `unzip ... \| head -1` 是同一个坑，一起修了） |
| 6 | 第 4 组验收没打印失败项就退出 | `adb shell pidof <没跑起来的包>` 返回**非 0**，`set -e` 把脚本静默带走；根因是**包名写死了**（APK 早改名为 `org.autosnap.arm64probe`） | helper 一律 `\|\| true`；包名改成**从设备上发现**（`pm list packages -3`），并改成在整份 logcat 里找 `PROBE_RESULT`（不依赖 tag） |
| 7 | 包里的 `images/` 被写脏 | `initrd` / `config.ini` 被链进工作目录后写穿 | 工作目录构建里写死 `NO_LINK` 名单（两个平台各一份） |
| 8 | 在 PowerShell 里 `New-Item -ItemType Junction` "成功"但没建东西 | 非 Windows 上该 cmdlet 静默无效 | 每一档链接都读回确认，收尾再核对 `images/` 每项都在 |
| 9 | 冒烟**全绿**，脚本却以**失败**退出（调用者拿到退出码 1，且 staging 不清理） | `smoke_linux` 的最后一句是 `[ "$CLEAN_SMOKE" = 1 ] && rm -rf ...`：条件为假时这条 AND 列表返回 1 → **函数返回 1** → `set -e` 在"清理 staging / 完成"之前把脚本带走 | 改成真正的 `if` 并 `return 0`。**通用陷阱**：函数/`if` 块的最后一句别留 `[ 条件 ] && 命令` |

---

## 9. 上限与后续

* **Windows 侧没有真机验收**：本机没有 Windows，WHPX 那条路径只能做到
  "脚本语法 + 文件与生命周期逻辑在 pwsh 里实跑 + 与 Linux 同 build id 等价性"。
  首次在 Windows 上跑请把 `bin\start-headless.ps1` 与 `bin\verify.ps1` 的输出贴回来。
* **`--slim` 是保守裁剪**：只删开发用文件（头文件/cmake/pkgconfig/宏预览）。
  更激进的裁剪（Qt、Vulkan）不能默认做 —— `emulator` 启动器依赖 Qt，
  删了会直接起不来；真要瘦身得改成直接调 `qemu-system-x86_64-headless`，那是另一条路。
* **快照模板没进包**：`快照` 能 7 秒开机（见 [`11-snapshots-and-multi.md`](11-snapshots-and-multi.md)），
  但一个快照约 1.1 GB 且与硬件配置强绑定，作为"可选加速包"比塞进主包合适。
* **翻译层许可**：`images/` 里的 `libndk_translation*` 是 Google 专有二进制，
  **对外交付整机前必须过法务**（三个文档都写了这条，不是重复而是必须显眼）。
