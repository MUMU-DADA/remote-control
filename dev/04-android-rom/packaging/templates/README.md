# templates · 模板

包里的模板是"**改这里就能改整台机器行为**"的那一层。启动脚本每次都会读它们，
不需要重新打包，也不需要重编 ROM。

| 文件 | 是什么 | 改了会怎样 |
|---|---|---|
| `config.ini` | **AVD 硬件与服务配置模板**（屏幕 / 内存 / 核数 / GPU / 数据分区 / HTTP 服务） | 下次启动生效。`service.*` 只用于 guest 首次创建服务配置；之后由 API 或设备配置管理 |
| `instance.env` | **实例登记模板**（名字 ↔ 端口） | 供 `start-headless.sh --port N` 之外的实例管理参考；实际登记落在 `.run/instances/<名字>.env` |

## config.ini 里最值得知道的几个键

| 键 | 默认 | 说明 |
|---|---|---|
| `hw.lcd.width` / `hw.lcd.height` / `hw.lcd.density` | `1280` / `720` / `320` | 屏幕。无头运行时也决定截图分辨率 |
| `hw.ramSize` | `6144` | MB。宿主机内存的 1/4 左右比较稳 |
| `hw.cpu.ncore` | `4` | 核数。别超过宿主物理核 |
| `hw.gpu.mode` | `auto` | `auto` = 宿主有可用 GPU 渲染节点就用 `host`，否则 `swiftshader_indirect`；**起不来会自动退软件渲染** |
| `disk.dataPartition.size` | `16G` | 数据分区。**改小/改大都要 `reset`** 才会重建（旧 `userdata-qemu.img` 不删不会跟着变）。16G 为 4G 上传文件保留安装峰值空间 |
| `fastboot.forceColdBoot` | `yes` | `yes` = 每次冷启动、忽略快照。要用快照启动改成 `no` |

数据卷的逻辑容量与宿主实际占用不同：32 GiB 的历史实例基础镜像实测只分配约
550 MiB，qcow2 覆盖层另计。修改模板不会缩小已有数据卷，也不会删除其数据。
ZIP 解压不保存稀疏空洞；启动脚本会把镜像中的零块恢复为空洞，字节内容和校验和不变。

### 服务首次启动配置

| 键 | 默认 | 说明 |
|---|---|---|
| `service.enabled` | `1` | 服务软开启；`0` 时仍保留开关 API |
| `service.bind` | `0.0.0.0` | HTTP 监听地址 |
| `service.port` | `8088` | HTTP 监听端口 |
| `service.auth` | `1` | 是否开启鉴权；为空 token 会在首次启动随机生成 |
| `service.token` | 空 | 可预置令牌；留空由宿主安全随机生成并保存到 `.run/instances/<名字>.token`，无需 ADB 获取 |
| `service.adb_enabled` | `1` | 新实例初始 ADB 状态；运行中由 `/api/v1/adb` 控制并跨重启保留 |

测试场景可给启动脚本传 `--test-instance`（Windows `-TestInstance`）创建无鉴权的新实例。
`release.sh --test-release` 可生成默认关闭鉴权的测试包。
普通启动、状态、停机通过模拟器 console 与 HTTP，不需要 ADB；
默认宿主访问地址为 `http://127.0.0.1:18088`，可用 `AUTOSNAP_HTTP_PORT` 改转发端口。
`service.port` 是 guest 内监听端口，`service.bind=127.0.0.1` 则只供 guest 本机访问，
宿主的 NAT 转发无法到达该回环地址。服务监听支持 IPv4 地址。

## 「模板」还包括工作目录布局本身

启动时在包根下建的工作目录，布局是固定的（和上游项目一致，便于两边混用）：

```
.run/
├── instances/<名字>.env     名字 ↔ 端口登记（PORT=5580）
├── instances/<名字>.token   服务访问令牌（仅当前用户可读）
├── sysdir-<端口>/           模拟器的工作目录：镜像（符号链接/硬链接，只读）+ 状态 + 快照
├── datadir-<端口>/
└── emulator-<端口>.log      启动日志
```

> **一台机器的全部状态都在 `sysdir-<端口>/` 里** —— `build.avd/`、`*.qcow2`
> 覆盖层、`snapshots/`。所以"删工作目录"就等于"这台机器彻底消失"。
> 镜像本身是链接，删的只是链接，不会碰 `images/`。
