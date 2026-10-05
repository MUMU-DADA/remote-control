# AutoSnapshot 无头虚拟机 · @VER@（@PLATFORM_LABEL@）

**解压就能跑**的 Android 12 虚拟机：模拟器本体（无头）、系统镜像、配置模板都在包里，
不需要宿主机装 Android SDK、不需要联网。

| 项 | 值 |
|---|---|
| 发布版本 | `@VER@` |
| 平台 | @PLATFORM_LABEL@（guest：@GUEST_DESC@） |
| 打包时间 | @BUILT_AT@ |
| ROM 指纹 | `@ROM_FINGERPRINT@` |
| 运行时 | `@RUNTIME_PKG@`（@RUNTIME_VER@ build @BUILD_ID@） |
| 镜像 | `images/`（@IMAGES_SIZE@，含 `SHA256SUMS` + `MANIFEST.txt`） |
@PLATFORM_HINT@
---

## 1. 快速开始

@QUICKSTART@

跑起来之后：

* 设备序列号就是 `emulator-<端口>`（默认 `emulator-5580`），用包内 adb 直接连：
  `@ADB_EXAMPLE@`
* 停机器：`@STOP_CMD@`（**先 sync 再关**，别直接杀进程 —— 见 §5）
* 验收：`@VERIFY_CMD@`（宿主 / 产品类型 / 服务产物架构 / 设备 / 服务，逐项打勾）

统一管理入口是 `bin/emulator.sh`（Windows 为 `bin\emulator.ps1`），支持 `start`、`stop`、
`kill`、`restart`、`status`、`list`、`verify`、`reset`、`create`、`clone` 和 `delete`。
实例存在时执行 `start` 会复用原数据；只有新实例会从空数据启动。`reset` 和 `delete` 会先确认，再清除所选实例的数据。

@MANAGER_EXAMPLES@

当前入口不提供归档导出/导入：现有恢复实现尚未验证数据完整性，不应作为备份使用。

---

## 2. 包里有什么

```
@ROOT_DIR@/
├── START-HERE.md          ← 本文件
├── RELEASE.json           ← 版本、ROM 指纹、运行时 build id、入口清单
├── SHA256SUMS             ← 整包逐文件校验
├── bin/                   ← 统一入口 bin/emulator.*，以及平台实现脚本
├── images/                ← 虚拟机镜像（ROM 交付目录，只读）
├── runtime/               ← 无头运行环境（模拟器 + qemu x86_64 后端 + adb）
│   └── RUNTIME.txt        ← 包名/版本/build id/来源 URL/sha1
├── templates/             ← 模板：config.ini（硬件）/ instance.env（实例登记）
└── tools/                 ← 上位应用 APK、验收探针 @TOOLS_EXTRA@
```

上位应用源码构建的 APK 位于 `tools/remote-control-controller.apk`，安装命令和当前能力说明见
[`tools/CONTROLLER-APP.md`](tools/CONTROLLER-APP.md)。

`images/` 是**只读**的：启动时会在 `.run/sysdir-<端口>/` 里建一份工作目录，
把镜像链接进去，模拟器写的状态（`userdata-qemu.img`、快照、`build.avd/`）全落在
`.run/` 下，不会污染镜像本身。

---

## 3. 硬件参数（屏幕 / 内存 / 核数 / GPU）

改 [`templates/config.ini`](templates/config.ini) —— 它是硬件与 guest 首次服务配置的真源，
启动脚本每次都会读它。命令行只在**显式传参**时覆盖（命令行优先于它）。

| 键 | 默认 | 说明 |
|---|---|---|
| `hw.lcd.width` / `height` / `density` | 1280 / 720 / 320 | 无头运行时也决定截图分辨率 |
| `hw.ramSize` | 6144 | MB |
| `hw.cpu.ncore` | 4 | 核数 |
| `hw.gpu.mode` | auto | 有可用 GPU 就用 `host`，否则软件渲染；**起不来会自动退软件渲染** |
| `disk.dataPartition.size` | 64G | 数据卷容量上限；实际占用随数据增长，改容量须重建数据卷 |

详见 [`templates/README.md`](templates/README.md)。

文件属性里的“大小”是虚拟磁盘的逻辑容量，“占用空间”才是宿主实际分配。
启动脚本会回收解压后 raw 镜像里的零块；镜像字节、校验和与 guest 分区容量保持不变。
Linux/macOS 的只读镜像通过链接共享，Windows 同卷使用硬链接，避免每台实例复制整份系统。

---

## 4. 多开几台

同一份镜像可以起多台（各自独立的数据与快照）：

```
@MULTI_EXAMPLE@
```

端口从 5580 起偶数分配（console 口为 `port`，可选 ADB 口为 `port+1`）。
**同时只让一台上物理 LAN** —— guest 的 MAC 是 QEMU 默认值，所有实例相同。

---

## 5. ⚠️ 三条别踩

1. **用包内 stop 脚本正常关机。** 脚本经 HTTP 请求 Android 关机并确认模拟器退出，
   无需 ADB。`--force` / `-Force`、console kill 或 `kill -9` 属于硬停止，可能丢失未落盘数据。
2. **`initrd` 与 `config.ini` 不在工作目录里做链接。** 模拟器会**透过符号链接**
   重写 `initrd`，链过去就会改到 `images/` 里那份，`SHA256SUMS` 当场对不上。
   各平台的工作目录构建逻辑都写死了这条例外。
3. **改过 `config.ini` 之后旧快照全部作废。** 快照要求硬件配置与存档时逐项一致，
   不一致模拟器会拒绝加载（`The emulator hardware cannot load snapshot`）。

---

## 6. ⚠️ 交付/再分发约束

| 项 | 说明 |
|---|---|
@DIST_ROWS@
