# 09 · 部署与更新：init 固化（自启 / 保活）

> **状态：已实施。** §1–§4 已落地并实测（验收全过，见 §9.1）；
> **§5 的「热替换通道」经实测决定不做** —— 原因见 §9.2–§9.5
> （三条 neverallow 互相咬住，两条绕开的路又分别被动态分区和分区可见性堵死）。
> 更新因此走正规通道：`build-rom.sh` + `package-rom.sh` + `emulator.sh restart`。
>
> **读法建议**：§1–§4 是"为什么这么改"的推导，§9 是**实际做了什么、以及哪些没做**。
> 只想知道现状的话直接看 §9.5。
>
> 原文保留了下述三类标注，便于区分结论的来源：
> `[源码]` = 在本仓库的 `aosp/` 树里逐行核对；`[实测]` = 在 `emulator-5580` 实例上真跑；
> `[未验证]` = 只有推断或只有上游文档，实施时必须先测。
>
> 当前部署方式（手工）见 [`../dev/02-native-daemon/README.md`](../dev/02-native-daemon/README.md) 阶段 1；
> 加组件的三种方式见 [`../dev/04-x64-android/docs/05-adding-components.md`](../dev/04-x64-android/docs/05-adding-components.md)。

---

## 0. 一句话

**正规形态** = init 服务（开机自启 + 崩了自动拉起）+ 专属 SELinux 域 + 编进镜像。

**但** init 不重读 rc，模拟器的 `/system` 又没有任何持久写入通道 —— 所以
"**启动后还能换**"必须靠间接层：`/system` 里放一个永不改动的**壳**，
真正的二进制放 `/data` 里的**版本槽**，`ctl.restart` 秒级切换。

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
| 唯一启动途径是手工 | `dev/04-x64-android/scripts/run-linux.sh`、`…/scripts/emulator.sh` 里**一个字都没提** remote-control（`grep` 命中 0）`[源码]` |

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
    socket remote-control seqpacket 0666 system system   # 见 §4.5
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

`dev/04-x64-android/device/remote_control_x64_arm64/product/remote_control_x64_arm64.mk` 里
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

### 4.5 socket 权限与上位应用不兼容

`.rc:41` 现为 `socket remote-control seqpacket 0660 system system` → POSIX 上只有 system UID 能连；
上位应用是 `untrusted_app` 自己的 UID，**连不上**。而 `docs/02-architecture.md:111` 明确写着
"**init 模式下 `--socket-mode` 无效**"，`daemon/main.cpp` 的用法说明也写着"放宽到 0666 才能让上位应用以自己的 UID 连入"。

**修法**：这一行改 `0666`，真正的边界交给 SELinux 域（`sepolicy/remote_control_controller.te` 已为此写好）。

---

## 5. 热替换通道（本次新增的需求）

### 5.1 先看物理限制（决定哪些做法根本不可行）

| 手段 | 生效时机 | 跨重启 | 依据 |
|---|---|---|---|
| `adb remount`（overlayfs） | 立即 | ❌ | `fs_mgr/fs_mgr_overlayfs.cpp:146` overlay upper 在 `/mnt/scratch`；无 scratch 分区时是临时 fs `[源码]` |
| `-writable-system`（模拟器） | 立即 | ❌ | 模拟器 `-help-writable-system`：临时副本 *"will be destroyed at emulator exit"* `[未验证]`（只信上游文案） |
| 重编 + 重打包 ROM | 重启实例 | ✅ | 正规发布通道（慢） |
| 离线注入 `system.img` | 重启实例 | ✅ | 但 `PRODUCT_USE_DYNAMIC_PARTITIONS` + verity：guest 里 `/vendor`=`dm-3`、`/product`=`dm-2`、`ro.boot.veritymode=enforcing` `[实测]` → 要重做超级分区与 AVB，**不是捷径** |
| Magisk 模块 | 重启 | ✅ | 该 guest **未安装**：`/data/adb/` 是空目录、无 `magisk` 二进制 `[实测]`；要装得先 patch ramdisk |
| **`/data` 放载荷 + init 只跑壳** | **立即（`ctl.restart`）** | ✅ | `/data` 持久；只差一条 sepolicy |

**另外**：init 不重读 rc（§3）→ `.rc` 天然"一次定稿"。这正是"壳"必须存在的原因。

### 5.2 方案：壳（launcher）+ 版本槽

```
/system/bin/remote-control-launch          ← 壳：几十行 C，几乎永不改（正规部分）
/system/etc/init/remote-control.rc         ← 一次定稿，指向壳
        │  读指针 → 校验 sha256 → exec
        ▼
/data/misc/remote-control/
    ├── current -> releases/<sha256>/      ← 原子切换的指针
    ├── releases/<sha256>/remote-control   ← 真正在跑的二进制（可随时替换）
    └── remote-control.conf                ← 配置：服务自己持久化（见 4.4）
```

**为什么这样就能"启动后更新替换"**：

- 换载荷 = push 新二进制 + 切指针 + `setprop ctl.restart remote-control` → **秒级生效，不重启、不重编镜像**。
- `exec` 不改变 PID → init 的进程跟踪照旧，**保活逻辑完全不受影响**。
- 跨重启保留（`/data` 持久）；回滚 = 指针切回去 + `ctl.restart`。

### 5.3 设计要点（每条都是会踩的坑）

1. **替换运行中的二进制会 `ETXTBSY`** → 必须"写新文件 + `mv` 覆盖"（同分区 rename 原子），不能原地 `cp`。
2. **sepolicy 要加执行权**：新类型 `remote_control_payload_exec`（`file_type, exec_type`）
   + `file_contexts` 标 `/data/misc/remote-control/releases/[^/]+/remote-control`
   + `allow remote_control <type>:file { execute execute_no_trans read open getattr }`。
   用 `tools/integrate-sepolicy.sh --check` 编一次 sepolicy 就能暴露 neverallow 违反（脚本第 3 步就是干这个的）。
   **万一被 neverallow 挡死，退路是 Magisk**（挂载覆盖到 `/system`，路径与标签都是原生正确的）。
3. **`/data` 是 `nosuid,nodev`**（`[实测]` mount 输出），不影响 exec；但目录权限要够：
   服务用 `user shell` 时，`releases/` 各层需对 shell 可读可进（`0755`，或属主给 shell）。
4. **版本必须可观测**：`--version` + `/api/v1/info` 带 `buildId` + sha256。
   不做必踩"推上去了，但跑的其实还是旧进程"——项目里 `integrate-aosp.sh` / `build-remote-control.sh`
   的源码新鲜度检查就是为同一个坑加的。
5. **坏版本要能自救**：新载荷起不来时，init 会按 `restart_period` 一直拉它（§3 的熔断不适用），服务会彻底失联。
   所以**壳要做探活回退**：起新版后 N 秒内没拿到就绪信号（载荷写 `ready-<sha>` 或壳探 socket）→ 指针切回上一版 → 自己 exit 让 init 拉起回退版本。宿主始终有 `adb root` 作最终兜底。
6. **更新源绝不能放 `/sdcard`**：上位应用能写共享存储，把"可执行载荷"和它放一起 = 把 root 执行权交给任何能写 `/sdcard` 的东西。必须放 `/data/misc/remote-control/`（`0700`）。
7. **配置跟着搬**：见 §4.4；上位应用改用 `POST /config`。

### 5.4 另外三条通道（按场景保留）

| 场景 | 做法 | 代价 |
|---|---|---|
| 当次会话试一把 rc 改动 | `adb root; adb remount; push → mv → setprop ctl.restart remote-control` | 重启失效，仅供调试 |
| 真机/长期形态 | Magisk 模块：`/data/adb/modules/remote-control/system/{bin,etc/init}/…` | 要先装 Magisk（patch ramdisk）；换来"不改 sepolicy 就能覆盖 `/system`" |
| 发布/交付 | 重编 + `package-rom.sh` + `emulator.sh restart` | 慢，但是唯一真相源；实例 sysdir 是**符号链接**（`.run/sysdir-5580/system.img → artifacts/rom-*/system.img`）→ **不用重建实例** `[实测]` |

---

## 6. 实施步骤与验收

### 6.1 步骤（命令级）

```bash
# 1 修 §4 的阻塞项（4.1 域名、4.2 脚本路径、4.3 产品清单、4.4 UID/路径、4.5 socket 权限）
# 2 .rc 定稿：指向壳；--config 指到 /data/misc/remote-control/remote-control.conf；以后不再动 rc
# 3 加壳：daemon/launcher.cpp（进同一个 Android.bp）+ sepolicy 新类型 + file_contexts
# 4 新增 tools/rc-update.sh：
#     push     校验 sha256 → 写 releases/<sha>/ → 不动指针（安全）
#     switch   原子切指针 + setprop ctl.restart remote-control
#     rollback 指针切上一版 + ctl.restart
#     list     列出所有版本 + 当前指针
#     verify   比对 /api/v1/info 的 buildId/sha256 与本地二进制是否一致
bash tools/integrate-sepolicy.sh
cd dev/04-x64-android
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
| 5 | `tools/rc-update.sh switch` | 5 秒内 `/api/v1/info` 的 buildId 变化（热替换） |
| 6 | `tools/rc-update.sh rollback` | 回到上一版并能正常工作 |

---

## 7. 风险、退路、未验证项

**风险与退路**

- 坏载荷导致崩溃循环 → 壳的探活回退（§5.3-5）；最终兜底是宿主 `adb root`。
- `exec` 从 `/data` 可能触到 AOSP neverallow → 编译 sepolicy 即可暴露；退路是 Magisk 挂载覆盖。
- **壳与 `.rc` 本身仍只能靠重编 ROM 更新**（init 不重读 rc）。所以设计上要把"会变的东西"全部挤进载荷，
  让壳薄到几乎不需要动 —— 这是整套方案能长期成立的前提。
- 手工部署的二进制与镜像里的可能**不同源**（改名重构期间就出现过：guest 里跑的是名字不同的旧构建）。
  所以 `verify` 子命令比对 sha256 是必需的，不是锦上添花。

**未验证项（实施时必须先测）**

1. `user system` 能否抓帧 —— `validateScreenshotPermissions` 对 AID_SYSTEM 没有 UID 白名单，只能靠平台签名权限；`[未验证]`
   （注意 `CAPTURE_DISPLAY_BY_ID` 那条路径在 `SurfaceFlinger.cpp:5336` 是显式白名单了 AID_SYSTEM 的，但走 `captureDisplay` 不是这条路。）
2. `exec` 从 `/data` 是否被 neverallow 挡（§5.3-2）。
3. `-writable-system` 的临时副本是否真在模拟器退出时销毁（§5.1，只信上游 help 文案）。
4. 本次结论全部基于**改名重构后**的树（`autod → remote-control`）。若后续还有批量改名，
   §4.1/4.2 这类"连字符 vs 下划线"的错配会再次出现 —— 建议把这两条加进改名脚本的自检。

---

## 8. 明确不做

- **不把更新源放 `/sdcard`**（见 §5.3-6）。
- **不用"进程保活"替换软开关**：`enabled=0` 必须继续是 HTTP 503 的软开关、不真停进程，
  否则没人能把它开回来（现状是对的，保持）。
- **不为了热替换去改 AOSP 上游文件**（设备树/sepolicy 走 `apply-overlay.sh` 与
  `integrate-sepolicy.sh` 的既有落点）。

---

## 9. 实施记录（2026-09-29）

> 本次按本计划实施后的实际结果。**§5 的核心机制实测被 AOSP 策略挡死**，
> 下面把它逐条记下来，避免下次再花一遍时间。

### 9.1 §3 / §4 已落地

| 项 | 结果 |
|---|---|
| §3 保活 | `oneshot` 已删（它和"崩了自动拉起"语义相反）、加 `restart_period 5`、保留 `disabled` + `on property:sys.boot_completed=1` |
| §4.1 域名 | `.rc` 的 `seclabel` 改下划线；`remote_control.te` 注释里的文件名一并改 |
| §4.2 脚本 | `integrate-sepolicy.sh` 不再按连字符找文件；顺带发现它**从来没接进过树** |
| §4.3 镜像 | 产品清单补 `PRODUCT_PACKAGES`，**还要补 `PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST`** —— 只写前者会撞 artifact path requirement，构建直接失败（计划里没提这条） |
| §4.4 UID | 改 `user shell`（`/sdcard` 是 FUSE 挡的，加 sepolicy 也没用） |
| §4.5 socket | 改 `0666` |

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

### 9.2 §5 热替换通道：**实测被挡死**（§7 未验证项 2 的答案）

计划 §5 的机制是"壳 exec `/data` 里的载荷"。实测**三条 neverallow 互相咬住**，
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

### 9.3 要绕开的话，只有这两条路（都需要人来定）

| 方案 | 代价 |
|---|---|
| **A. 壳跑 `shell` 域 + 服务放弃 `coredomain`** | ②对 `-shell` 有豁免、③只管 coredomain，两条都绕开了。代价：一个常驻服务跑在 **shell 域**（adb 调试域，权限很宽）；服务不再是 coredomain（SELinux 语义上"假装不是平台核心"，Treble 的分层保证就没了）。已确认没有 neverallow 挡非 coredomain 调 SurfaceFlinger，技术上可行。 |
| **B. 载荷放 `/system`，用 `adb disable-verity` + `remount` 换** | 完全合法（载荷是 `system_file_type`，本来就能执行）。代价：要先关 verity（测试机可接受，正式机不行）；换版本 = push 到 `/system/bin` + `ctl.restart`，仍是秒级、也跨重启。计划的 §5.1 表格里列过这条。 |

**本次先不选**：这两个都是安全/架构层面的取舍，不是"哪个能编过"的问题。
在定下来之前，`.rc` 直接指向 `/system/bin/remote-control`（§3/§4 的自启与保活
不受影响 —— 那才是本次需求的根因部分）。
`daemon/launcher.cpp`（壳）与 `tools/rc-update.sh`（推送/切换/回滚）都已写好、
能编译，选 A 或 B 之后接上即可。

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

### 9.5 最终结论

热替换通道的**前提**（能执行 /data 里的载荷）在这个 AOSP 版本上对
"非 appdomain 的常驻服务"不成立。三条 neverallow 互相咬住（§9.2），
两条绕开的路又各自被动态分区和 neverallow 级联堵死（§9.4）。

**因此本次不做热替换**，更新走既有的正规通道：

    cd dev/04-x64-android
    ./scripts/build-rom.sh && ./scripts/package-rom.sh && ./scripts/emulator.sh restart <实例>

全流程约 3 分钟，改动被编进镜像、跨重启、可回滚（git）。它比"秒级热替换"慢，
但**没有拿安全换** —— 服务保持专属 SELinux 域（计划 §2 的第 ③ 条腿），
这才是这套东西的正规形态。

`daemon/launcher.cpp` 与 `tools/rc-update.sh` 保留在树里：若将来上
Magisk（`/data/adb/modules` 是 appdomain 之外的另一条路）或换 AOSP 版本，
它们可以直接接上；在那之前不要把它们当可用功能。
