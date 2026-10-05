# 09 · 部署与更新：init 固化与 API 热更新

> **状态：已实施。** §1–§4 已落地并实测（验收全过，见 §9.1）；服务端的
> `/api/v1/update`、`/update/apply` 和 `/update/rollback` 已提供版本上传、原子切换
> 与回滚。ROM 必须把 init 服务接到 `remote-control-launch`，并提供允许 launcher
> 执行版本槽载荷的 SELinux 策略，API 热更新才会真正替换正在运行的进程。
> 尚未接入 launcher 的旧 ROM 仍可暂存并校验文件，但应继续使用
> `build-rom.sh` + `package-rom.sh` + `emulator.sh restart` 完成发布。
>
> **读法建议**：§1–§4 是"为什么这么改"的推导，§9 是**实际做了什么、以及哪些没做**。
> 只想知道现状的话直接看 §9.5。
>
> 原文保留了下述三类标注，便于区分结论的来源：
> `[源码]` = 在本仓库的 `aosp/` 树里逐行核对；`[实测]` = 在 `emulator-5580` 实例上真跑；
> `[未验证]` = 只有推断或只有上游文档，实施时必须先测。
>
> 当前部署方式（手工）见 [`../dev/02-native-daemon/README.md`](../dev/02-native-daemon/README.md) 阶段 1；
> 加组件的三种方式见 [`../dev/04-android-rom/docs/05-adding-components.md`](../dev/04-android-rom/docs/05-adding-components.md)。

---

## 0. 一句话

**正规形态** = init 服务（开机自启 + 崩了自动拉起）+ 专属 SELinux 域 + 编进镜像。

**但** init 不重读 rc，模拟器的 `/system` 又没有任何持久写入通道 —— 所以
"**启动后还能换**"必须靠间接层：`/system` 里放一个永不改动的**壳**，
真正的二进制放 `/data` 里的**版本槽**，`ctl.restart` 秒级切换。HTTP API 负责
把载荷安全写入版本槽并切换 `current` 指针；init/launcher 负责启动它。

---

## 1. 要解决的问题（根因，不是症状）

### 1.1 症状

从控制网页的「电源 → 重启」（或任何等价调用）重启设备后，**控制台再也打不开**，
必须回到宿主手工跑一次部署脚本才能恢复。

### 1.2 因果链（实测确认）

| 环节 | 证据 |
|---|---|
| 重启是**服务自己**发起的（当时二进制还叫 `autod`） | `/sdcard/remote-control.log`：`09-29 00:01:42 E 收到电源请求: reboot（子进程 pid=1115 将在 0.5s 后执行）`，日志到此中断 `[实测]` |
| 走的是设备重启 | `daemon/dispatch.cpp`：fork 子进程 → `svc power reboot` → 退路 `/system/bin/reboot`；网页按钮 `daemon/webui.cpp:344` → `POST /power {"action":"reboot"}` `[源码]` |
| guest 确实重启了 | `/proc/uptime` = 2484s，与 `00:01:42` 对得上 `[实测]` |
| 重启后进程没了 | `pidof remote-control` 空、`getprop \| grep init.svc` 空 `[实测]` |
| **ROM 里根本没有它** | `/system/bin/remote-control` 不存在、`/system/etc/init/` 无对应 rc、`artifacts/rom-*/` 里没有任何 `remote-control*` `[实测]` |
| 唯一启动途径是手工 | `dev/04-android-rom/scripts/run-linux.sh`、`…/scripts/emulator.sh` 里**一个字都没提** remote-control（`grep` 命中 0）`[源码]` |

**结论**：重启 = 进程死 + 无人重启 + 8088 无监听。这不是"网页坏了"，是
**服务跑在自己会重启的那台设备里，却没有开机自启**。

---

## 2. 正规形态：三条腿

| 腿 | 落点 | 现状 |
|---|---|---|
| ① 进镜像 | `/system/bin/remote-control`、`/system/etc/init/remote-control.rc` | rc 已写好，`Android.bp:210` 的 `init_rc: []` 会让 Soong 自动安装 ✅；**但产品清单里没有它**（见 §4.3）❌ |
| ② init 管自启 + 保活 | `remote-control.rc` 的 `on property:sys.boot_completed=1` + 非 oneshot | 自启已写 ✅；`oneshot` 反向抵消了保活 ❌（见 §3） |
| ③ 专属 SELinux 域 | `sepolicy/remote_control.te` + `file_contexts` → `system/sepolicy/private/` | 策略文件已写好 ✅；**没接进树**，且工具与文件名对不上 ❌（见 §4.1/4.2） |

---

## 3. init 的自启与保活语义（android-12.0.0_r34 源码依据）

| 依据 | 语义 |
|---|---|
| `system/core/init/service.cpp:295`（`Service::Reap`） | `oneshot && !RESTART && !RESET` → 进程退出后置回 `SVC_DISABLED` |
| 同上 `:300` | "Disabled and reset processes do not get restarted automatically" → **直接 return，不再拉起** |
| 同上 `:344` | 非 oneshot → 置 `SVC_RESTARTING` |
| `system/core/init/init.cpp:365` + `service.h:201` | 重启间隔 = `restart_period`，**默认 5 秒**；`service_parser.cpp:367` 要求 ≥ 5 |
| `service.cpp:318` | "崩 4 次熔断"**只对 `critical` / updatable 进程生效**；我们没写 `critical` → 会一直拉 |
| `service.cpp:414` | 显式 `start` 会清掉 `SVC_DISABLED`（所以 `disabled` 只影响"class_start 时要不要自动起"，不影响后续自动重启） |

> ⚠️ **`.rc` 里现在那行注释是错的。** `remote-control.rc:43` 写"崩了自动重启，但不循环重启"，
> 可它下一行就是 `oneshot` —— 而 `oneshot` 的语义恰恰是"退出后不再重启"。
> 想保活就**必须删掉 `oneshot`**。

**建议的形态**（保留 `disabled`，保证在 `sys.boot_completed` 之后才起）：

```
service remote-control /system/bin/remote-control-launch
    class core
    user shell                      # 见 §4.4
    group shell uhid graphics
    seclabel u:r:remote_control:s0  # 下划线，见 §4.1
    # 当前实现由 daemon 自己 bind socket，并按 SO_PEERCRED 校验 UID
    disabled
    restart_period 5                # 崩了 5 秒后拉起（等于默认值，写出来是为了显式）

on property:sys.boot_completed=1
    start remote-control
```

**init 侧能/不能覆盖的范围**（写清楚，避免误期待）：

- ✅ 进程崩溃、被 `kill`、`POST /restart` 主动退出（退出码 1）→ 自动拉回
- ❌ 宿主上的模拟器进程被关、宿主重启 → 那是宿主侧的事（`emulator.sh start` 钩子或宿主 systemd）
- ❌ `.rc` 本身改动 → **init 不重读 rc**（`property_service.cpp` 只有 `ctl.start/stop/restart` 与 `sys.powerctl`，没有 reload）`[源码]`，必须重启

---

## 4. 阻塞项：不修就一定起不来或有功能缺口

### 4.1 域名字不一致（改名重构引入，致命）

```
sepolicy/remote_control.te:28    type remote_control, domain;        ← 下划线
sepolicy/file_contexts:11,18     remote_control_exec / _socket       ← 下划线，与 .te 一致 ✅
daemon/remote-control.rc:35      seclabel u:r:remote-control:s0      ← 连字符 ❌ 域不存在
sepolicy/remote_control.te:133   （注释里也抄成了连字符，一并改）
```

上游 sepolicy 里**没有任何带连字符的类型名**（`grep -rE "^type [A-Za-z0-9_-]*-[A-Za-z0-9_-]*,"` 命中 0）`[源码]`，
SELinux 标识符是 `[A-Za-z0-9_]`。**修法：`.rc` 改成 `u:r:remote_control:s0`。**

### 4.2 `integrate-sepolicy.sh` 找不到文件（同一个改名遗留）

`tools/integrate-sepolicy.sh:80` 取 `$SRC/remote-control.te`（连字符），
实际文件是 `sepolicy/remote_control.te`（下划线）→ 脚本会以"缺文件"直接退出。`[源码]`

### 4.3 产品清单里没有模块 → 根本不会进 `system.img`

`dev/04-android-rom/device/remote_control_x64_arm64/product/remote_control_x64_arm64.mk` 里
`grep PRODUCT_PACKAGES` **命中 0** `[源码]`。文档 `05-adding-components.md:39` 已给出写法：

```make
PRODUCT_PACKAGES += \
    remote-control \
    rcctl
```

### 4.4 运行身份与存储路径：UID 1000 读写不了 `/sdcard`

**实测**（SELinux 当时是 permissive，所以已排除 sepolicy 因素）：

```
su 1000(system)  读 /sdcard/...  → Permission denied      写 → Permission denied
su 2000(shell)   读 /sdcard/...  → OK                     写 → WRITE_OK
```

这是**存储层（FUSE）**挡的，加 sepolicy 规则无效。当时守护进程的配置与日志默认就落在
`/sdcard/remote-control.conf`（`daemon/config_file.cpp` 的 `DefaultPath()`）与 `/sdcard/remote-control.log`（`daemon/main.cpp`）
→ 用 `user system` 跑的话，**上位应用写进去的配置改不动服务**（软开关、改端口、开鉴权全失效）。

> 📌 **后续进展**：配置与日志已迁出 `/sdcard`，现在都在 init 创建的
> `/data/misc/remote-control/`（`0770 shell shell`）。**结论不变** —— 那个目录同样只给 `shell`，
> `user system` 一样读写不了，所以仍然必须 `user shell`。
> 见 [`remote-control.rc`](../dev/02-native-daemon/daemon/remote-control.rc)。

| | `user system` | `user shell` |
|---|---|---|
| 截图 | `SurfaceFlinger.cpp:5897` 的 `validateScreenshotPermissions` 只认 `AID_GRAPHICS` 或 `checkPermission(READ_FRAME_BUFFER)`，**没有** AID_SYSTEM 白名单 → 靠平台签名权限过关 `[未验证]` | Shell 应用显式申请了 `READ_FRAME_BUFFER`（`frameworks/base/packages/Shell/AndroidManifest.xml:178`），screencap 走的就是这条 ✅ |
| `/dev/uinput` | `group uhid` 即可（`0660 uhid:uhid`） | 同 |
| `/sdcard` 配置/日志 | ❌ 实测被拒 | ✅ 实测可读写 |
| 权限面 | 大 | 小 |

> **建议 `user shell`**，一次绕开两个未验证项。
> 若坚持 `user system`，则配置/日志必须迁到 `/data/misc/remote-control/`（`system:system 0700`），
> 并让上位应用改配置走 **`POST /config`**（由服务自己持久化），不再直接写文件。

> ⚠️ `--selftest` **测不出 UID 问题**：`main.cpp` 里 selftest 在降权之前就 return 了。
> 要验只能**整个进程以该 UID 启动**（init 的行为就是 exec 时已经是那个 UID）。

### 4.5 Unix socket 的权限边界

当前 `remote-control.rc` 不声明 init 管理的 socket；daemon 自己 bind
`/data/misc/remote-control/remote-control.sock` 并设置 `0660`。因此即使由 init 启动，
`--socket-mode` 仍然生效。它只在历史 `--init-socket` 模式下无效，因为那种模式接管
init 已创建的 socket。`docs/02-architecture.md` 对此只说明了历史兼容模式。

控制器应用当前走 HTTP API；若其他 `untrusted_app` 客户端直接连接 Unix socket，默认会被
文件权限和 `SO_PEERCRED` 校验挡住。跨 UID 客户端需要显式设置
`--socket-peer-uid <uid>` 并配置对应 SELinux 规则；单独放宽文件模式不能绕过 UID 校验。

---

## 5. 热替换通道（API 设计与 AOSP 约束）

> 本节记录版本槽、launcher 和 SELinux 的设计。早期“直接让 coredomain 从
> `/data` 执行载荷”的实现已被 §9.2–§9.4 的 neverallow 实测否决；当前 API
> 仍采用版本槽的安全写入和原子指针，但必须先把 launcher/domain 接入 ROM。
> HTTP 契约见 [`api/01-http.md`](api/01-http.md) 的“服务更新”一节。

### 5.1 先看物理限制（决定哪些做法根本不可行）

| 手段 | 生效时机 | 跨重启 | 依据 |
|---|---|---|---|
| `adb remount`（overlayfs） | 立即 | ❌ | `fs_mgr/fs_mgr_overlayfs.cpp:146` overlay upper 在 `/mnt/scratch`；无 scratch 分区时是临时 fs `[源码]` |
| `-writable-system`（模拟器） | 立即 | ❌ | 模拟器 `-help-writable-system`：临时副本 *"will be destroyed at emulator exit"* `[未验证]`（只信上游文案） |
| 重编 + 重打包 ROM | 重启实例 | ✅ | 正规发布通道（慢） |
| 离线注入 `system.img` | 重启实例 | ✅ | 但 `PRODUCT_USE_DYNAMIC_PARTITIONS` + verity：guest 里 `/vendor`=`dm-3`、`/product`=`dm-2`、`ro.boot.veritymode=enforcing` `[实测]` → 要重做超级分区与 AVB，**不是捷径** |
| Magisk 模块 | 重启 | ✅ | 该 guest **未安装**：`/data/adb/` 是空目录、无 `magisk` 二进制 `[实测]`；要装得先 patch ramdisk |
| **`/data` 放载荷 + init 只跑壳** | 需配套策略 | 版本槽可跨重启 | 当前实现：launcher 校验槽文件后复制到 sealed memfd，再由 `execveat` 启动 |

**另外**：init 不重读 rc（§3）→ `.rc` 天然"一次定稿"。这正是"壳"必须存在的原因。

### 5.2 方案：壳（launcher）+ 版本槽

```
/system/bin/remote-control-launch          ← 壳：几十行 C，几乎永不改（正规部分）
/system/etc/init/remote-control.rc         ← 一次定稿，指向壳
        │  读指针 → 校验 sha256 → sealed memfd → execveat
        ▼
/data/misc/remote-control/
    ├── current -> releases/<sha256>/remote-control  ← 原子切换的指针
    ├── releases/<sha256>/remote-control              ← 已校验的版本槽文件
    └── remote-control.conf                ← 配置：服务自己持久化（见 4.4）
```

**为什么这样就能"启动后更新替换"**：

- 换载荷 = 上传新二进制 + 调用 `apply` 切指针并请求服务重启 → **秒级生效，不重启设备、不重编镜像**。
- launcher 保持 init 的服务监督关系；载荷在独立子进程中通过 sealed memfd 启动，启动失败可自动回退。
- 跨重启保留（`/data` 持久）；回滚 = 指针切回去 + `ctl.restart`。

### 5.3 设计要点（每条都是会踩的坑）

1. **版本槽不可原地覆盖**：上传先写临时文件，完成 SHA-256 校验后再原子 `rename` 到版本目录。
2. **sepolicy 要接入启动壳和 memfd 转换**：固定入口使用独立的
   `remote_control_loader` 域；载荷槽只授予读取/校验权限，复制到带
   `postinstall_file` 标签的 sealed memfd 后，通过 `domain_auto_trans` 进入
   `remote_control` 域执行。运行 `tools/integrate-sepolicy.sh --check` 可同时检查
   语法和 neverallow 约束。
3. **`/data` 是 `nosuid,nodev`**（`[实测]` mount 输出），不影响读取和 memfd staging；但目录权限要够：
   服务用 `user shell` 时，`releases/` 各层需对 shell 可读可进（`0755`，或属主给 shell）。
4. **版本必须可观测**：`--version` + `/api/v1/info` 带 `buildId` + sha256。
   不做必踩"推上去了，但跑的其实还是旧进程"——项目里 `integrate-aosp.sh` / `build-remote-control.sh`
   的源码新鲜度检查就是为同一个坑加的。
5. **坏版本要能自救**：新载荷起不来时，init 会按 `restart_period` 一直拉它（§3 的熔断不适用），服务会彻底失联。
   launcher 会等待载荷写入内容等于目标 SHA-256 的 ready 文件；超时或提前退出时按
   `current -> previous -> last-good -> 镜像原版` 的顺序回退，再退出让 init 拉起回退版本。
   因此连续升级失败时，至少仍会回到上一份已运行的载荷；所有槽版本都不可用时才使用镜像原版。
6. **更新源绝不能放 `/sdcard`**：上位应用能写共享存储，把"可执行载荷"和它放一起 = 把 root 执行权交给任何能写 `/sdcard` 的东西。必须放 `/data/misc/remote-control/`（`0700`）。
7. **配置跟着搬**：见 §4.4；上位应用改用 `POST /config`。

### 5.4 另外三条通道（按场景保留）

| 场景 | 做法 | 代价 |
|---|---|---|
| 当次会话试一把 rc 改动 | `adb root; adb remount; push → mv → setprop ctl.restart remote-control` | 重启失效，仅供调试 |
| 真机/长期形态 | Magisk 模块：`/data/adb/modules/remote-control/system/{bin,etc/init}/…` | 要先装 Magisk（patch ramdisk）；换来"不改 sepolicy 就能覆盖 `/system`" |
| 发布/交付 | 重编 + `package-rom.sh` + `emulator.sh restart` | 慢，但是唯一真相源；实例 sysdir 是**符号链接**（`.run/sysdir-5580/system.img → artifacts/rom-*/system.img`）→ **不用重建实例** `[实测]` |

---

## 6. API 热更新的实施与验收

> 下面保留 ROM 接入步骤，并增加设备内 API 验收。旧的 `rc-update.sh` 命令仍可用于
> 宿主侧调试；设备内发布优先使用 HTTP API。

### 6.1 步骤（命令级）

```bash
# 1 修 §4 的阻塞项（4.1 域名、4.2 脚本路径、4.3 产品清单、4.4 UID/路径、4.5 socket 权限）
# 2 .rc 定稿：指向壳；--config 指到 /data/misc/remote-control/remote-control.conf；以后不再动 rc
# 3 加壳：daemon/launcher.cpp（进同一个 Android.bp）+ sepolicy 新类型 + file_contexts
# 4 确认 API 热更新端点：
#     POST /api/v1/update          上传并校验 ELF，保持 current 不变
#     POST /api/v1/update/apply    原子切 current，并请求服务重启
#     POST /api/v1/update/rollback 按 previous → last-good → 镜像原版回退，
#                                  并请求服务重启
#     GET  /api/v1/update           查看 running/buildId/current/staged/launcherManaged
bash tools/integrate-sepolicy.sh
cd dev/04-android-rom
./scripts/apply-overlay.sh
ALLOW_MISSING_DEPS=0 ./scripts/build-rom.sh   # libwebp 已改 static_libs（Android.bp:77），
                                             # 但 build-rom.sh:80-83 仍默认开着 ALLOW_MISSING_DEPS，建议收回
./tools/verify-rom.sh && ./scripts/package-rom.sh
./scripts/emulator.sh restart default        # 实例 sysdir 是符号链接，重启即用新镜像
```

### 6.2 验收清单（缺一不可）

| # | 检查 | 期望 |
|---|---|---|
| 1 | `getprop init.svc.remote-control` | `running` |
| 2 | `logcat -d \| grep 'avc: denied' \| grep remote_control` | 空 |
| 3 | `kill -9 $(pidof remote-control)` | ≤5 秒自己回来（保活） |
| 4 | `POST /power {"action":"reboot"}` | 重启后**啥都不用做**，控制台直接可达（本次要修的病） |
| 5 | `POST /api/v1/update` + `/update/apply` | 上传返回 `201`；接着轮询 `/api/v1/update`，5 秒内 `running/buildId` 变为目标 SHA-256 |
| 6 | `POST /api/v1/update/rollback` | 返回 `202`；轮询 `/api/v1/update` 确认回到 `previous` 或 fallback 的 `last-good` |

---

## 7. 原计划的风险与未验证项（结论已更新）

> 本节是旧热替换方案的记录，不是当前部署的风险清单。热替换相关结论以 §9.2–§9.5 为准。

**原方案曾考虑的风险与退路（保留作历史记录）**

- 坏载荷导致崩溃循环 → 当前 launcher 探活会按 `current -> previous -> last-good -> 镜像原版` 自动回退。
- **壳与 `.rc` 本身仍只能靠重编 ROM 更新**（init 不重读 rc）。所以设计上要把"会变的东西"全部挤进载荷，
  让壳薄到几乎不需要动 —— 这是整套方案能长期成立的前提。
- 手工部署的二进制与镜像里的可能**不同源**（改名重构期间就出现过：guest 里跑的是名字不同的旧构建）；
  原计划因此提出用 `verify` 子命令比对 sha256。

**旧方案中仍未验证的事项**

1. 若将来改为 `user system`，能否抓帧 —— `validateScreenshotPermissions` 对 AID_SYSTEM 没有 UID 白名单，只能靠平台签名权限；`[未验证]`
   （注意 `CAPTURE_DISPLAY_BY_ID` 那条路径在 `SurfaceFlinger.cpp:5336` 是显式白名单了 AID_SYSTEM 的，但走 `captureDisplay` 不是这条路。）
2. `-writable-system` 的临时副本是否真在模拟器退出时销毁（§5.1，只信上游 help 文案；当前更新方式不依赖它）。
3. 本次结论全部基于**改名重构后**的树（`autod → remote-control`）。若后续还有批量改名，
   §4.1/4.2 这类"连字符 vs 下划线"的错配会再次出现 —— 建议把这两条加进改名脚本的自检。

---

## 8. 明确不做（原计划）

- **不把更新源放 `/sdcard`**（见 §5.3-6）。
- **不用"进程保活"替换软开关**：`enabled=0` 必须继续是 HTTP 503 的软开关、不真停进程，
  否则没人能把它开回来（现状是对的，保持）。
- **不为了热替换去改 AOSP 上游文件**（设备树/sepolicy 走 `apply-overlay.sh` 与
  `integrate-sepolicy.sh` 的既有落点）。

---

## 9. 实施记录（2026-09-29）

> 本次按本计划实施后的实际结果。早期“无配套策略、直接 exec `/data` 载荷”的
> 核心机制被 AOSP 策略挡死；下面把它逐条记下来，并说明 API 热更新需要补齐的
> launcher/domain 接入条件，避免把上传成功误认为版本已经运行。

### 9.1 §3 / §4 已落地

| 项 | 结果 |
|---|---|
| §3 保活 | `oneshot` 已删（它和"崩了自动拉起"语义相反）、加 `restart_period 5`、保留 `disabled` + `on property:sys.boot_completed=1` |
| §4.1 域名 | `.rc` 的 `seclabel` 改下划线；`remote_control.te` 注释里的文件名一并改 |
| §4.2 脚本 | `integrate-sepolicy.sh` 不再按连字符找文件；顺带发现它**从来没接进过树** |
| §4.3 镜像 | 产品清单补 `PRODUCT_PACKAGES`，**还要补 `PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST`** —— 只写前者会撞 artifact path requirement，构建直接失败（计划里没提这条） |
| §4.4 UID | 改 `user shell`（`/sdcard` 是 FUSE 挡的，加 sepolicy 也没用） |
| §4.5 socket | 默认 `0660`，跨 UID 时显式配置 `--socket-peer-uid` |

**两处计划里没写、但会挡住构建的**：

1. **策略不能放 `system/sepolicy/private/`**。放进去 sepolicy_freeze_test 必挂
   （它 diff 当前树与 `prebuilts/api/31.0/`，多一个文件就 `Only in ...`），
   ninja 直接停。**正确落点是设备树**：`device/remote_control_x64_arm64/sepolicy/`
   + `BoardConfig.mk` 里的 `SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS`。
   （同一份 BoardConfig 里 goldfish 的 x86 策略走的是 `BOARD_SEPOLICY_DIRS`，
   但**两者不等价** —— 见下。）

   ⚠️ **用哪个变量决定了策略编进哪个分区，而分区决定看得见哪些类型**：

   | 变量 | 编进 | 看得见 | 看不见 |
   |---|---|---|---|
   | `BOARD_SEPOLICY_DIRS` | **vendor** | vendor 侧 HAL 类型（抓帧要的） | 平台私有类型（`odsign_prop` 等） |
   | `SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS` | **system_ext** | 平台私有类型 | vendor 侧 HAL 类型 |

   **一个分区拿不到两边** —— 实测两头都撞过：先用 vendor，`odsign_prop`
   报 `unknown type`；换 system_ext，`hal_graphics_allocator_default`
   又报 `unknown type`。当前选 **system_ext**，因为服务要跑
   `pm`/`am`/`cmd`（它们要读平台私有属性），而抓帧所需的 HAL 权限
   由 `hal_client_domain()` 宏提供 —— 宏用的是 **attribute**，可见性规则
   与实现类型不同，在 system_ext 里也能用（实测抓帧正常）。

   两者都能避开 `sepolicy_freeze_test`（那只管 `system/sepolicy/{public,private}`）。
2. **`remote_control_controller.te` 里有三个东西在 Android 12 上不存在**：
   `app_use_file_type`、`appdomain_different_pkg`（都是更新版本才有的），
   还有一处 SELinux 语法错误
   （`allow init x:{ sock_file create unlink };` —— `X:{ }` 是**类**的列表，
   权限不能写进去）。这份策略从来没被编译过，所以一直没人发现。

### 9.2 旧的直接 `/data` 执行方案：**实测被挡死**（§7 未验证项 2 的答案）

早期计划的机制是"壳 exec `/data` 里的载荷"。实测**三条 neverallow 互相咬住**，
每条都单独试过：

| # | 规则 | 挡住了什么 |
|---|---|---|
| ① | `private/domain.te:315` | "除了 appdomain，谁都不许执行 /data 下的东西" → 壳必须在 appdomain 里 |
| ② | `public/domain.te:1186` | 只有 zygote/runas/app_zygote 那几家能转换**进** appdomain → **init 拉不起一个 appdomain 的壳** |
| ③ | `public/domain.te:959` | coredomain 只能对 `system_file_type` 有 `entrypoint` → **coredomain 的服务没法从 /data 的载荷进入** |

中间还撞过另外几条，一并记下（都是试出来的）：

- `sepolicy_tests`：`file_contexts` 里任何 `/data` 路径对应的类型**必须**带
  `data_file_type`，否则 `The following types on /data/ must be associated with ...`
- `private/domain.te:249`：要被执行，类型必须是 `system_file_type` /
  `vendor_file_type` / **`exec_type`** / … 之一 → 载荷必须带 `exec_type`
- `public/domain.te:502`：**只有内核**能 `relabelto` 成 `exec_type`
  → "运行时给它打标签"这条路也堵死
- 带 `app_data_file_type` 会引入 `allow installd ... relabelto` 之类的既有规则，
  又和 502 打架

**只让开一条没用** —— 三条一起才构成"服务能起来"的完整链路。

### 9.3 曾提出的两条绕行方案（后续均被否决）

| 方案 | 代价 |
|---|---|
| **A. 壳跑 `shell` 域 + 服务放弃 `coredomain`** | 当时推演认为可绕过 ②、③。代价：常驻服务跑在 **shell 域**（adb 调试域，权限很宽）；服务不再是 coredomain（SELinux 语义上"假装不是平台核心"，Treble 的分层保证就没了）。该推演后来被 §9.4 的 neverallow 级联实测否决。 |
| **B. 载荷放 `/system`，用 `adb disable-verity` + `remount` 换** | 载荷使用 `system_file_type` 本身符合执行策略；当时预期可用 `disable-verity` + `remount` 更新。§9.4 的设备实测确认当前 ROM 无法这样改写分区。 |

以上是后续实测前的判断。§9.4 确认 A、B 两条路都不适用于未配套策略的当前 ROM。
`daemon/launcher.cpp`（壳）与 `tools/rc-update.sh`（推送/切换/回滚）保留在树中，
HTTP API 使用同一版本槽协议；接入新的 launcher/domain 后即可作为设备内更新通道。

### 9.4 两条路都实测过了：都不通

**方案 B（载荷放 /system）—— 这个 ROM 上做不到**：

| 手段 | 实测结果 |
|---|---|
| `adb disable-verity` | `only works for userdebug builds` —— 我们**就是** userdebug，它仍然拒绝 |
| `adb remount` | `liblp: WritePrimaryMetadata ... failed: Operation not permitted` + `/system_ext: Read-only file system`。动态分区（super）的元数据在运行时改不了 |
| 模拟器 `-writable-system` | 标志确认已传给 qemu（`ps` 里查得到），但 `adb remount` 仍失败：`Consider providing all the dependencies to enable overlayfs`。没有 overlayfs 也没有 scratch 分区 |

结论：这套 ROM 是**动态分区 + AVB**，`/system` 在运行时就是只读的。
`-writable-system` 的"临时副本"也救不了，因为 overlayfs 起不来。

**方案 A（壳跑 shell 域 + 服务放弃 coredomain）—— 会引发 neverallow 级联**：

去掉 `coredomain` 之后，域就落进 `{ domain -appdomain -coredomain ... }` 这个集合，
而 AOSP 对"既不是核心域、也不是应用域"的域限制**多得多**。实测第一条就撞上：

    neverallow on line 829 of public/domain.te violated by
        allow remote_control remote_control_payload_exec:file { entrypoint };

那条规则禁止这类域访问 `core_data_file_type`（它只豁免 appdomain 和 coredomain）。
可 **sepolicy_tests 又要求 /data 下的类型必须带 core_data_file_type** ——
两个要求直接对立，不是"再加一条 allow"能解决的。

要真走 A，得把载荷槽挪出 `/data`（否则躲不开 core_data_file_type），
那又回到方案 B 的死路。**所以 A 不是"多花点工夫"，是此路不通。**

### 9.5 当前结论与 API 更新通道

上面的实测结论仍适用于**直接从 `/data` 执行载荷、但没有配套 launcher/domain
设计**的旧方案：三条 neverallow 互相咬住（§9.2），把服务改成普通 shell 域或
把文件写进动态 `/system` 也不能绕开它们（§9.4）。这不是 HTTP API 本身的限制，
而是 ROM 启动链和 SELinux 类型必须一起落地。

当前更新 API 的安全流程是：

1. `POST /api/v1/update` 上传原始 ELF。服务端重新计算 SHA-256、检查 ELF magic，
   用临时文件 + `fsync` + 原子提交写入 `releases/<sha256>/remote-control`，只暂存，
   不改变当前运行版本。
2. `POST /api/v1/update/apply` 提交一个已暂存的 SHA-256。服务端再次校验文件，
   原子写入 `current` 指针，并通过 restart hook 请求 init/supervisor 重启。
3. `GET /api/v1/update` 轮询 `running`/`buildId`；确认它等于目标 SHA-256 后，
   才把更新视为成功。
4. 新版本无法启动或验证失败时，启动壳按
   `current -> previous -> last-good -> 镜像原版` 依次尝试；
   `POST /api/v1/update/rollback` 也优先切回 `previous`，没有可用上一版时
   才使用 `last-good` 或镜像原版，然后重启。

要使第 2 步真正执行新载荷，ROM 需要同时满足：

- `remote-control.rc` 的 service 命令指向 `/system/bin/remote-control-launch`；
- launcher 与载荷的 SELinux domain、`exec_type`、`file_contexts` 和允许规则通过
  当前 AOSP 的 `neverallow` 检查；
- init 在 `post-fs-data` 创建 `/data/misc/remote-control/releases`，并授予 launcher
  与服务运行 UID 读取、校验和维护版本槽的权限；版本槽本身不作为 SELinux entrypoint。

如果 ROM 还保持旧的 `/system/bin/remote-control` service 定义，更新 API 仍会安全
完成上传和校验，但 apply 后只能继续运行镜像中的旧版本。此时更新走既有的正规通道：

    cd dev/04-android-rom
    ./scripts/build-rom.sh && ./scripts/package-rom.sh && ./scripts/emulator.sh restart <实例>

全流程约 3 分钟，改动被编进镜像、跨重启、可回滚（git）。

`daemon/launcher.cpp` 与 `tools/rc-update.sh` 仍用于 ROM/宿主侧部署和排障；HTTP API
是设备内的同一套版本槽协议。发布前请先以 `GET /api/v1/update` 验证运行版本，
不要只根据上传请求的 HTTP `201` 判定替换已经生效。
