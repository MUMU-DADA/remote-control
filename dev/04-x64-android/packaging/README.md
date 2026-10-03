# packaging · release 包的骨架（**源码**，不是产物）

这里的东西会被 `../scripts/release.sh` 原样拷进 zip 的根目录，构成"解压即用"的
那一层：启动脚本、模板、首读文档。

> 产物在 `../release/`（已 gitignore）。别把解出来的包又提交回来。

---

## 目录

```
packaging/
├── START-HERE.md               ← 包内首读。含 @VER@ / @PLATFORM@ 等占位符，打包时替换
├── bin/
│   ├── linux/                  ← 进 linux 包：bin/ 下 5 个 bash 脚本
│   │   ├── lib.sh              公共函数（路径、config.ini、实例登记、adb/模拟器定位）
│   │   ├── start-headless.sh   无头启动（默认 -no-window，后台 + 等开机）
│   │   ├── stop.sh             优雅停（**先 sync 再 kill**，见下）
│   │   ├── status.sh           实例/进程/开机状态 + ROM 指纹
│   │   └── verify.sh           4 组验收（ABI / 翻译层 / arm64 机器码 / arm64 应用）
│   └── windows/                ← 进 windows 包：同名同语义的 PowerShell 版
│       ├── common.ps1
│       ├── start-headless.ps1
│       ├── stop.ps1
│       ├── status.ps1
│       └── verify.ps1
└── templates/
    └── README.md               ← 模板说明（config.ini / instance.env 的解释）
```

打包时另外**生成**（不在这里维护，避免两份真源）：

| 进包路径 | 来源 |
|---|---|
| `templates/config.ini` | `../emulator/config.ini`（硬件参数的唯一真源） |
| `templates/instance.env` | `release.sh` 生成的实例登记模板 |
| `tools/net-bridge*.sh` | `../tools/`（**仅 linux 包**；Windows 侧 `-net-tap` 没实现） |
| `tools/arm64-probe.apk` | `../artifacts/arm64-probe.apk`（验收用，12 KB） |
| `images/**` | `../artifacts/rom-<product>/`（ROM 交付目录，硬链接） |
| `runtime/**` | SDK 模拟器包 + platform-tools（按平台下载并校验 sha1） |

---

## 两条**必须**守住的规矩（都是踩出来的）

1. **`initrd` 与 `config.ini` 绝不能链进工作目录。**
   模拟器会**透过符号链接**重写 `initrd`（把自己的 ramdisk + dtb 合进去），
   链过去就把 `images/` 里那份改了 —— `SHA256SUMS` 当场校验失败。
   `config.ini` 同理：要按包内真源覆盖，链过去会写穿。
   两个平台的工作目录构建逻辑里都各自写了一条 `NO_LINK` 名单。

2. **停机器必须先 `adb shell sync`，再 `adb emu kill`。**
   `emu kill` 是**硬断电**不是关机。实测：写完不 sync 直接停，最近写的数据
   **整个消失**（等 15 秒再停也一样没）。`stop.sh` / `stop.ps1` 里这一步带着
   注释，别"优化"掉。
