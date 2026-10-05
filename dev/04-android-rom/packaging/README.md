# packaging · release 包的骨架（**源码**，不是产物）

这里的东西会被 `../scripts/release.sh` 原样拷进 zip 的根目录，构成"解压即用"的
那一层：统一入口、平台实现脚本、模板和首读文档。

> 产物在 `../release/`（已 gitignore）。别把解出来的包又提交回来。

---

## 目录

```
packaging/
├── START-HERE.md               ← 包内首读。含 @VER@ / @PLATFORM@ 等占位符，打包时替换
├── CONTROLLER-APP.md           ← 包内上位应用安装说明与当前能力状态
├── bin/
│   ├── linux/                  ← 进 Linux 包：emulator.sh 统一入口与平台实现
│   │   ├── emulator.sh         统一管理：create/start/stop/kill/restart/clone/delete/status/list/verify/reset/inspect
│   │   ├── lib.sh              公共函数（路径、config.ini、实例登记、adb/模拟器定位）
│   │   ├── start-headless.sh   无头启动（默认 -no-window，后台 + 等开机）
│   │   ├── stop.sh             优雅停（**先 sync 再 kill**，见下）
│   │   ├── status.sh           实例/进程/开机状态 + ROM 指纹
│   │   ├── console.sh          console / 服务管理公共函数
│   │   ├── storage.sh          镜像稀疏空间优化函数
│   │   └── verify.sh           4 组验收（ABI / 翻译层 / arm64 机器码 / arm64 应用）
│   ├── windows/                ← 进 Windows 包：emulator.ps1 统一入口与管理脚本
│   │   ├── emulator.ps1        统一管理：create/start/stop/kill/restart/clone/delete/status/list/verify/reset/inspect
│   │   ├── common.ps1
│   │   ├── start-headless.ps1
│   │   ├── stop.ps1
│   │   ├── status.ps1
│   │   ├── console.ps1         console / 服务管理公共函数
│   │   ├── storage.ps1         镜像稀疏空间优化函数
│   │   ├── reset.ps1            清空实例数据与快照
│   │   └── verify.ps1
│   └── darwin/                 ← 进 macOS 包：与 Linux 同语义的 emulator.sh 和实现脚本
│       ├── emulator.sh         统一管理：create/start/stop/kill/restart/clone/delete/status/list/verify/reset/inspect
│       ├── lib.sh
│       ├── start-headless.sh
│       ├── stop.sh
│       ├── status.sh
│       ├── reset.sh
│       ├── console.sh          console / 服务管理公共函数
│       ├── storage.sh          镜像稀疏空间优化函数
│       └── verify.sh
└── templates/
    └── README.md               ← 模板说明（config.ini / instance.env 的解释）
```

打包时另外**生成**（不在这里维护，避免两份真源）：

| 进包路径 | 来源 |
|---|---|
| `templates/config.ini` | `../emulator/config.ini`（硬件参数与 guest 首次服务配置的唯一真源） |
| `templates/instance.env` | `release.sh` 生成的实例登记模板 |
| `tools/net-bridge*.sh` | `../tools/`（**仅 linux 包**；Windows 侧 `-net-tap` 没实现） |
| `tools/arm64-probe.apk` | `../artifacts/arm64-probe.apk`（项目自建 arm64 验收探针） |
| `tools/remote-control-controller.apk` | release 前从 `dev/05-controller-app/` 当前源码重新构建 |
| `tools/CONTROLLER-APP.md` | `CONTROLLER-APP.md`（APK 安装方法、签名类型和能力状态） |
| `images/**` | `../artifacts/rom-<product>/`（ROM 交付目录，硬链接） |
| `runtime/**` | SDK 模拟器包 + platform-tools（按平台下载并校验 sha1） |

release 调用 `dev/05-controller-app/build-apk.sh` 重新构建 APK；本地 Android build-tools、JDK 11 与 API 31 `android.jar`
必须可用。构建签名写到 `.run/controller-app-debug.keystore`，不会覆盖上位应用目录里既有的密钥文件。

日常操作从包内统一入口开始：`./bin/emulator.sh help`（Windows: `.\bin\emulator.ps1 help`）。
它统一提供创建、启动、停止、强停、重启、克隆、删除、状态/列表、验收和重置；旧平台脚本仍作为实现层保留。
已有实例默认保留数据，`reset` 和 `delete` 会再次确认后才清理数据。导出/导入尚未验证恢复后数据完整性，当前不作为 release 能力提供。

---

## 两条**必须**守住的规矩（都是踩出来的）

1. **`initrd` 与 `config.ini` 绝不能链进工作目录。**
   模拟器会**透过符号链接**重写 `initrd`（把自己的 ramdisk + dtb 合进去），
   链过去就把 `images/` 里那份改了 —— `SHA256SUMS` 当场校验失败。
   `config.ini` 同理：要按包内真源覆盖，链过去会写穿。
   **每套**宿主脚本的工作目录构建逻辑里都各自写了一条 `NO_LINK` 名单。

2. **通过包内 stop 脚本请求正常关机。**
   `stop.sh` / `stop.ps1` 经 HTTP 请求 Android 关机，等待文件系统卸载和模拟器退出。
   `--force` / `-Force` 才允许硬停止；直接 `kill -9` 或 console kill 可能丢失未落盘数据。
