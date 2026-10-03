# templates · 模板

包里的模板是"**改这里就能改整台机器行为**"的那一层。启动脚本每次都会读它们，
不需要重新打包，也不需要重编 ROM。

| 文件 | 是什么 | 改了会怎样 |
|---|---|---|
| `config.ini` | **AVD 硬件配置模板**（屏幕 / 内存 / 核数 / GPU 档位 / 数据分区） | 下次启动生效。命令行**优先于**它 —— 所以 `start-headless.sh` 只在显式传参时才覆盖，不写死默认值 |
| `instance.env` | **实例登记模板**（名字 ↔ 端口） | 供 `start-headless.sh --port N` 之外的实例管理参考；实际登记落在 `.run/instances/<名字>.env` |

## config.ini 里最值得知道的几个键

| 键 | 默认 | 说明 |
|---|---|---|
| `hw.lcd.width` / `hw.lcd.height` / `hw.lcd.density` | `1280` / `720` / `320` | 屏幕。无头运行时也决定截图分辨率 |
| `hw.ramSize` | `6144` | MB。宿主机内存的 1/4 左右比较稳 |
| `hw.cpu.ncore` | `4` | 核数。别超过宿主物理核 |
| `hw.gpu.mode` | `auto` | `auto` = 宿主有可用 GPU 渲染节点就用 `host`，否则 `swiftshader_indirect`；**起不来会自动退软件渲染** |
| `disk.dataPartition.size` | `32G` | 数据分区。**改小/改大都要 `reset`** 才会重建（旧 `userdata-qemu.img` 不删不会跟着变） |
| `fastboot.forceColdBoot` | `yes` | `yes` = 每次冷启动、忽略快照。要用快照启动改成 `no` |

## 「模板」还包括工作目录布局本身

启动时在包根下建的工作目录，布局是固定的（和上游项目一致，便于两边混用）：

```
.run/
├── instances/<名字>.env     名字 ↔ 端口登记（PORT=5580）
├── sysdir-<端口>/           模拟器的工作目录：镜像（符号链接/硬链接，只读）+ 状态 + 快照
├── datadir-<端口>/
└── emulator-<端口>.log      启动日志
```

> **一台机器的全部状态都在 `sysdir-<端口>/` 里** —— `build.avd/`、`*.qcow2`
> 覆盖层、`snapshots/`。所以"删工作目录"就等于"这台机器彻底消失"。
> 镜像本身是链接，删的只是链接，不会碰 `images/`。
