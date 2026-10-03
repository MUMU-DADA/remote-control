# dev/ · 开发轨道

> 这个目录下是**产品代码与验证环境**，不是最终交付物。
> 另外两条根目录线：`docs/`（专题与接口权威文档）、`tools/`（环境与构建脚本）。

---

## 三个模块

| 目录 | 是什么 | 文档 |
|---|---|---|
| [`02-native-daemon/`](02-native-daemon/README.md) | **主线代码**：Android 原生守护进程（`remote-control`，截图 / 触控注入 / 设备管理，HTTP + WebSocket + Unix socket）与命令行客户端 `rcctl` | [README](02-native-daemon/README.md)（456 行） |
| [`04-x64-android/`](04-x64-android/README.md) | **验证与交付轨道**：自编 Android 12 ROM + 三套宿主脚本（Linux / Windows / macOS）+ release 打包 | [README](04-x64-android/README.md) ＋ **16 篇专题**见 [`04-x64-android/docs/`](04-x64-android/docs/00-bridge-eval.md) |
| [`05-controller-app/`](05-controller-app/README.md) | 上位应用：设备本地的服务管理器（需「所有文件访问」权限） | [README](05-controller-app/README.md)（127 行） |

**先看哪个**：想懂服务本身 → `02`；想跑起来 / 打 ROM / 出交付包 → `04`；想在设备上管理这个服务 → `05`。

---

## ⚠️ 编号为什么是 02 / 04 / 05（01、03、06 去哪了）

编号是**历史沿革**，不是「第几个模块」。不知道这段会以为是文档缺失，所以列在这里：

| 编号 | 状态 | 当年的定位 | 内容现在在哪 |
|---|---|---|---|
| `01-ndk-prototype` | ❌ 已移除 | 「让守护进程脱离 AOSP 树编译运行 —— 不需要 `repo sync` 110 GB」 | 工具还在：[`tools/build-ndk.sh`](../tools/build-ndk.sh)（NDK 版，screencap 后端） |
| `02-native-daemon` | ✅ **当前** | AOSP 原生守护进程（主线） | —— |
| `03-java-service` | ❌ 已移除 | 「补上 Android 12 的触控缺口：native 负责截图，Java 负责注入」 | 作为**备选注入路径**记在 [`docs/08-input-injection.md`](../docs/08-input-injection.md) |
| `04-emulator` | ❌ 已被取代 | 「不是产品轨道，是 `02-native-daemon` 的**验证基础设施**」（QEMU） | 被下面的 `06` 取代 —— 那是**产品轨道**，不只是开发内循环 |
| `04-x64-android` | ✅ **当前** | 自编 x86_64 ROM（原 `06-x64-android`，**改名占了 `04` 的位置**） | —— |
| `05-controller-app` | ✅ **当前** | 上位应用 | —— |
| `06-x64-android` | ➡️ 已改名 | 同 `04-x64-android`，旧名 | [`04-x64-android/PLAN.md`](04-x64-android/PLAN.md) 首行仍记着这件事 |

> 一句话：**`04` 这个号被用过两次**，现在的 `04-x64-android` 是当年的 `06-x64-android` 改名来的；
> `01` / `03` / `04-emulator` 已移除，各自的内容去向见上表。

---

## `04-x64-android` 的三条产品线（最容易混的地方）

这个模块同时容纳**三份不同的东西**，它们的「宿主–guest 架构」关系完全不同：

| 产品 | guest 架构 | 有没有翻译层 | 能跑在哪 | 文档 |
|---|---|---|---|---|
| `remote_control_x64_arm64` | x86_64 + **ARM 用户态翻译层** | 有（`libndk_translation`，Google 专有） | Linux/KVM、Windows/WHPX、**Intel Mac** | [`docs/00-bridge-eval.md`](04-x64-android/docs/00-bridge-eval.md) |
| `remote_control_arm64` | **原生 arm64** | 无 | Linux/aarch64、**Apple Silicon** | [`docs/13-macos-port.md`](04-x64-android/docs/13-macos-port.md) |
| （宿主脚本） | —— | —— | `packaging/bin/{linux,windows,darwin}/` 三套 | [`docs/12-emulator-control.md`](04-x64-android/docs/12-emulator-control.md) |

**关键约束**：两种 Mac 各自只有**一个**后端。
Apple Silicon 上**没有** x86_64 后端，所以现有的 x86_64 ROM 在 M 系列上起不来（不是慢，是没有）——
这条是本项目做 arm64 产品线的**唯一原因**。

---

## 相关文档入口

| 想找什么 | 去哪 |
|---|---|
| 服务对外接口（权威） | [`docs/api/`](../docs/api/README.md)（HTTP / WebSocket / socket / 配置 / 错误码 / 调试） |
| 选型与架构决策 | [`docs/01-selection.md`](../docs/01-selection.md)、[`docs/02-architecture.md`](../docs/02-architecture.md) |
| 设计取舍与踩过的坑 | [`docs/05-design-notes.md`](../docs/05-design-notes.md) |
| 环境（磁盘 / 镜像源 / 构建容器） | [`docs/04-environment.md`](../docs/04-environment.md) |
| 全部文档索引 | [`docs/README.md`](../docs/README.md)、[`04-x64-android/README.md`](04-x64-android/README.md) §6 |
| **文档体检工具** | `python3 tools/check-docs.py`（加 `--strict` 则有问题时非 0 退出）。改了文档路径 / 新增文档 / 改名之后跑一下：它会告诉你谁的链接断了、哪篇没进索引、哪条命令照着做会 `no such file` |

（本文件与各模块 README 的一致性由 `tools/check-docs.py` 检查。）
