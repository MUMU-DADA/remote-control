# 05 · 上位应用（remote-control 服务管理器）

> 一个**设备本地**的服务管理小应用：启停 `remote-control`、改监听端口、开关鉴权。
> 它**不做**看画面、点屏幕、管应用、传文件 —— 那些都在网页控制台里。

> ⚠️ **现状：对当前的 init 部署形态是失效的。** 应用写的是
> `/sdcard/remote-control.conf`，而现在跑着的服务由 init 启动、读的是
> `/data/misc/remote-control/remote-control.conf`，**没有任何东西会读前者**。
> 详见下面[「当前状态」](#当前状态失效中)。本文档描述的是代码的**设计意图**，
> 不是它今天能跑通的能力。

---

## 它做什么

源码里写得很清楚（[`java/com/remotecontrol/controller/MainActivity.java`](java/com/remotecontrol/controller/MainActivity.java) 开头）：

1. 启动 / 停止 `remote-control` 服务
2. 改它的对外监听端口
3. 开关接口访问鉴权

### 为什么原来那些功能被删了

不是没做完，是**刻意删的**：画面、触控、应用管理、文件管理已经在网页控制台里了
（有实时画面、流式触控、按键、应用列表，而且不用装在设备上）。应用再做一遍等于
两套 UI 各维护一份，行为还容易不一致。

应用真正的独有价值是**它是设备本地的**：服务没起来、网络不通、端口改错了导致
连不上 —— 这些情况下网页控制台自己也进不去，只有本机应用还能把它拉回来。

## 权限：`MANAGE_EXTERNAL_STORAGE` 是**必需**的

`AndroidManifest.xml` 声明了 `MANAGE_EXTERNAL_STORAGE`（「所有文件访问」）。
这与"应用不该要权限"的直觉相反，但它**是设计的一部分**：

应用没有 root，改不了进程；它能做的是写共享存储。配置写在
`/sdcard/remote-control.conf`，由常驻的 supervisor 监视并执行真正的启停。
**不申请这个权限的话，应用只能写自己的私有目录，守护进程读不到。**

> 所以本应用**不是**「一个权限都不申请」的协议客户端 —— 那是它被删掉的旧形态。
> 根 [`README.md`](../../README.md) 里如果还这么写，是错的。

## 当前状态：失效中

| | 应用假设 | 现在实际 |
|---|---|---|
| 谁在管服务 | `tools/remote-control-supervisord.sh`（常驻 root 脚本）监视配置文件 | **init 服务** `remote-control`，开机自启 + 崩了自动拉起 |
| 配置读哪里 | `/sdcard/remote-control.conf` | `/data/misc/remote-control/remote-control.conf`（init 用 `--config` 指定） |
| 状态写哪里 | `/sdcard/remote-control.status`（supervisor 写） | 无人写这个文件 |

结论：**应用今天的三个按钮都不会产生效果** —— 没人读它写的文件。

这不是文档没跟上，是**部署形态换代后应用被落下了**：
supervisord 是[阶段 1 的免 SELinux 原型](../02-native-daemon/README.md)，产品形态换成了
init（见 [`docs/09`](../../docs/09-deployment-and-update.md) §9.5），而应用还停在原型那套。

### 复活路径（尚未实施）

让它改走 daemon 已有的 **HTTP API**，而不是写文件。已实测的对应关系：

| 应用的功能 | HTTP 等价 | 实测结果 |
|---|---|---|
| 开关鉴权 | `POST /api/v1/config` body `{"auth":false}` | ✅ 热生效（`applied:["auth"]`） |
| 启停服务 | `POST /api/v1/service` | ✅ 软开关，见 [`docs/api/01-http.md`](../../docs/api/01-http.md) |
| 改监听端口 | ❌ **没有运行时入口** | `{"port":8088}` 被拒：`未知配置项`。端口是 bind 期定的，只能改配置 + 重启进程 |

也就是说**三个功能里两个能用 HTTP 现成做到，改端口做不到** ——
要保留这个功能，得让应用能触发"改配置 + 重启"，而那又绕回权限问题。
这一步需要先定方向，别急着写代码。

## 界面

单页，不是标签页：

```
┌─ remote-control 服务管理 ───────────┐
│ 只管服务的启停、端口与鉴权           │
│ （控制设备请用网页控制台）            │
├─────────────────────────────────────┤
│ 状态行（来自 /sdcard/...status）     │
│ [服务运行中]  ← Switch               │
│ [接口鉴权]    ← Switch               │
│ 端口 / 令牌  ← 输入框 + [应用]        │
└─────────────────────────────────────┘
```

## 构建

```bash
bash dev/05-controller-app/build-apk.sh          # 出 remote-control-controller.apk
bash dev/05-controller-app/build-apk.sh --install
```

不依赖 AOSP 的 `out/`（那个目录经常被别的构建占着），用独立 SDK build-tools。
JDK 直接用 AOSP 树自带的 `prebuilts/jdk/jdk11`。

> ⚠️ **改名之后这份代码从未构建过。** `build/` 里现存的产物全是改名前的：
> `autod-controller.apk`（09-28 08:10），`sources.txt` 指向已被删除的
> `java/com/autod/controller/MainActivity.java`。而 `build-apk.sh` 现在会产出
> `remote-control-controller.apk` —— 该文件在仓库里**不存在**。
> 要恢复"可交付"，先跑一次构建脚本并装机验证。

## 截图（历史）

[`docs/evidence/`](../../docs/evidence/) 下的 `controller-app-*.png` 是**旧 UI**
（四标签页那一版）的实机截图，拍的是已经被删掉的功能，**不要再当成当前界面**。
它们由 remote-control 自己拍摄（服务在拍控制它的应用，完整闭环），
作为历史记录保留。

## 附：当初的 SELinux 问题

四标签页那版是**直连 socket** 的，遇到：

```
avc: denied { write } for name="remote-control.sock"
  scontext=u:r:untrusted_app:s0:c105,c256,c512,c768
  tcontext=u:object_r:shell_data_file:s0  tclass=sock_file
```

**不能**用 `allow untrusted_app shell_data_file:sock_file write` 敷衍 ——
那是把口子开给所有第三方应用。正确做法是给上位应用一个专属域，策略已写好：
[`remote_control_controller.te`](../04-android-rom/device/remote_control_x64_arm64/sepolicy/remote_control_controller.te)。

> 但**当前这版应用不再连 socket**（它写配置文件），所以这份策略目前处于
> "写好了但用不上"的状态。当时的验证临时用过 `setenforce 0`，
> **那不是可交付方案**。复活时如果走 HTTP，需要的是网络权限与令牌，
> 这份 socket 策略要重新评估是否还适用。
