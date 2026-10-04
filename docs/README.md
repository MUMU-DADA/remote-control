# 文档索引

一份**读哪本、按什么顺序读**的地图。想快速上手看
[README](../README.md) 的「三分钟跑起来」，想查接口直接跳
[api/](api/README.md)。

> **权威性**：同一件事只应有一处权威描述。本页标出每份文档的定位，
> 避免"两个地方都写、说法还不一样"。发现冲突时，以标注为**权威**的那份为准。

---

## 一、想理解这个项目（按顺序读）

| 文档 | 定位 |
|---|---|
| [01-selection.md](01-selection.md) | **选型与结论** —— 为什么是系统级 native 服务而不是内核/APK/无障碍；官方与开源实现对照 |
| [02-architecture.md](02-architecture.md) | **架构** —— 三条传输、抓帧/编码/注入的分层 |
| [03-reference.md](03-reference.md) | **技术参考** —— Android 版本兼容性矩阵、各版本差异、坑清单 |
| [04-environment.md](04-environment.md) | 环境与构建（宿主、目录、依赖） |
| [05-design-notes.md](05-design-notes.md) | **设计记录** —— 为什么这么写；踩过的坑与结论 |
| [06-capture-performance.md](06-capture-performance.md) | 抓帧与编码性能实测与调优 |
| [07-dependencies.md](07-dependencies.md) | 依赖清单：用了什么、为什么不用手写 |
| [08-input-injection.md](08-input-injection.md) | 备选注入路径（Java 系统服务 / 特权 APK） |

## 二、要用这个服务（接口）

| 文档 | 定位 |
|---|---|
| [api/README.md](api/README.md) | **API 总览 + 快速上手**（权威入口） |
| [api/01-http.md](api/01-http.md) | **HTTP/JSON API 完整参考**（权威） |
| [api/02-websocket.md](api/02-websocket.md) | WebSocket 流（画面/触控/日志） |
| [api/03-socket.md](api/03-socket.md) | Unix socket 二进制协议（33 条命令，权威清单） |
| [api/04-config.md](api/04-config.md) | 配置与部署 |
| [api/05-errors.md](api/05-errors.md) | 状态码与错误处理 |
| [api/06-debugging.md](api/06-debugging.md) | 调试与验证 |

## 三、要改这套东西

| 文档 | 定位 |
|---|---|
| [09-deployment-and-update.md](09-deployment-and-update.md) | init 固化（自启/保活）+ 部署方式；含**实施记录**（做了什么、为什么没做某些事） |
| [evidence/README.md](evidence/README.md) | Android 12 KVM 模拟器运行验证证据（截图与检查输出） |
| [../dev/02-native-daemon/README.md](../dev/02-native-daemon/README.md) | 服务本体：构建、部署、分阶段落地 |
| [../dev/05-controller-app/README.md](../dev/05-controller-app/README.md) | 上位应用 |

## 四、ROM 与模拟器（`dev/04-android-rom/`）

| 文档 | 定位 |
|---|---|
| [00-bridge-eval.md](../dev/04-android-rom/docs/00-bridge-eval.md) | **决策记录**：为什么是「x86_64 ROM + 用户态翻译层」 |
| [01-design.md](../dev/04-android-rom/docs/01-design.md) | 这份 ROM 为什么这么写 |
| [02-build-traps.md](../dev/04-android-rom/docs/02-build-traps.md) | 构建实际撞到的坑（持续追加，**改动前值得先扫一遍**） |
| [03-delivery.md](../dev/04-android-rom/docs/03-delivery.md) | 交付与验收：一份 ROM，两个 x64 平台 |
| [04-acceptance-runbook.md](../dev/04-android-rom/docs/04-acceptance-runbook.md) | 验收排错手册 |
| [05-adding-components.md](../dev/04-android-rom/docs/05-adding-components.md) | 往 ROM 里加自己的组件 |
| [08-emulator-version-notes.md](../dev/04-android-rom/docs/08-emulator-version-notes.md) | 模拟器版本与镜像兼容性（实测） |
| [10-network-bridge.md](../dev/04-android-rom/docs/10-network-bridge.md) | 网络：桥接模式 |
| [11-snapshots-and-multi.md](../dev/04-android-rom/docs/11-snapshots-and-multi.md) | 快照与多实例 |
| [12-emulator-control.md](../dev/04-android-rom/docs/12-emulator-control.md) | 实例控制脚本（创建/启动/停止/克隆/导出…） |
| [13-macos-port.md](../dev/04-android-rom/docs/13-macos-port.md) | **macOS 支持**：arm64 原生 ROM 产品线 + 第三套宿主脚本（**代码已完成、离线验证通过；真机验证未做**，见该文 §8） |
| [14-macos-host-notes.md](../dev/04-android-rom/docs/14-macos-host-notes.md) | 把服务做成 macOS 被控端的 API 等价性与权限调研（ScreenCaptureKit / TCC / launchd） |
| [15-release-packaging.md](../dev/04-android-rom/docs/15-release-packaging.md) | **release 打包**：`scripts/release.sh` 一个平台一个 zip（完整无头运行环境 + 虚拟机镜像 + 模板） |

### 历史记录（写于当时，**不代表现状**）

这些是一次性的过程记录或事后被推翻的方案，保留是为了"为什么这么选"的来龙去脉，
**不要当成当前用法**：

| 文档 | 说明 |
|---|---|
| [09-why-not-full-arm64-sim.md](../dev/04-android-rom/docs/09-why-not-full-arm64-sim.md) | 与 `00-bridge-eval` 同一决策的另一角度（内容有重叠） |
| [07-verification-report.md](../dev/04-android-rom/docs/07-verification-report.md) | 一次性验收报告 |
| [06-arm32-fallback.md](../dev/04-android-rom/docs/06-arm32-fallback.md) | 预案（未启用） |

---

## 验证这些文档

文档不能只靠人读。仓库里有可执行的核对：

```bash
python3 tools/check-api-docs.py [host:port]     # 文档与活服务一致性（98 项断言）
python3 tools/functional-sweep.py [host:port]   # 全功能体检（判据取设备侧证据）
cd dev/02-native-daemon/tests && make run       # 13 个测试套件 + 核对 README 里的检查数
```

`check-api-docs.py` 还会**读 README 里声称的断言条数**并与实际比对 ——
改了断言不同步改 README 就会失败。文档里的数字因此不会悄悄过期。

同样的闸门也加在了测试侧：`make run` 会把 13 个套件自报的检查数求和，
与 README 里写的「单元/集成 N 项检查」比对；当前为 497 项，对不上就失败。
这条闸门曾抓到新增 `test_sha256` 后 README 仍停在 298 项的过期计数。

> `functional-sweep.py` 对**平台上确实做不到**的项（如后台进程写剪贴板）
> 单独记为「已知限制」，不计入失败，这样它才能当退出码 0/1 的回归闸门用。
> 但标注不是永久豁免：一旦该项意外通过，脚本会提示复核并去掉标注。
