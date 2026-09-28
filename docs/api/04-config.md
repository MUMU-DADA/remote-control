# 配置与部署

---

## 一、配置文件 `/sdcard/remote-control.conf`

守护进程启动时读一次；`enabled` 会被**持续监视**（每 2s），改了立刻生效。

```ini
# remote-control 配置 —— 由上位应用或手工编辑，守护进程启动时读取。
# 改完之后需要重启服务才生效（enabled 除外）。

# 服务是否应当运行
enabled=1

# 监听地址。127.0.0.1 = 仅本机；0.0.0.0 = 对外（注意鉴权设置）
bind=0.0.0.0

# HTTP 监听端口
port=8088

# 是否要求访问令牌。0 = 无鉴权（任何人都能访问接口）
auth=0

# 访问令牌。auth=1 时若为空，守护进程启动时会随机生成并写回这里。
token=
```

| 字段 | 默认 | 说明 |
|---|---|---|
| `enabled` | `1` | **软开关**。0 = 不对外提供服务能力，但**进程继续运行** |
| `bind` | `127.0.0.1` | 监听地址。要对外必须显式写 `0.0.0.0` |
| `port` | `8088` | HTTP 端口 |
| `auth` | `0` | 是否要求令牌 |
| `token` | 空 | 令牌。`auth=1` 且为空时会生成 |

### 为什么是纯文本 key=value

Android 应用侧要用 Java 读写它。引 JSON 库只为存五个字段不划算，
而且双方各用一套 JSON 实现时，格式分歧（转义、数字精度）会变成
很难查的兼容性问题。这个文件的读者是人，可读性也重要。

### 为什么放 `/sdcard`

上位应用是普通 Android 应用（**没有 root**），守护进程是 root。
两边都能读写、都不需要特殊权限的位置，就是共享存储。

放 `/data/local/tmp` 的话应用够不着；放应用私有目录的话守护进程
要模拟应用的身份才能写。

### 首启状态

**文件不存在 = 无鉴权模式**，不是错误。

```
bind=127.0.0.1  port=8088  auth=0  enabled=1
```

⚠️ 内置默认是 `127.0.0.1`（仅本机）。要对外必须显式写 `0.0.0.0` ——
这个不对称是有意的：让"对外"成为一个需要主动做的决定。

---

## 二、优先级

```
命令行显式给的  >  配置文件  >  内置默认
```

这个顺序是有讲究的：上位应用写配置文件、不传命令行参数，所以它能生效；
而调试时 `--http-port 9999` 这种一次性覆盖也不会被文件悄悄改掉。

配置文件路径可以用 `--config` 或环境变量 `REMOTE_CONTROL_CONFIG` 覆盖。

---

## 三、鉴权

### 开启

```bash
# 方式一：改配置文件（推荐 —— 上位应用就是这么做的）
sed -i 's/^auth=0/auth=1/' /sdcard/remote-control.conf
# 重启服务后，token 会被随机生成并写回文件

# 方式二：命令行
remote-control --socket /data/local/tmp/remote-control.sock --http-token <令牌>
```

### 令牌从哪来

`auth=1` 且 `token=` 为空时，守护进程启动时会：

1. 从 **`/dev/urandom`** 取 24 字节
2. base64url 编码（`+/` 换成 `-_`，去掉 padding —— 令牌会出现在 URL 里）
3. **写回配置文件**

```bash
grep '^token=' /sdcard/remote-control.conf
# token=nonKbvyus_2bkj9e849gWgleEDjp_UYP
```

**只生成一次**（不是每次启动都生成）：令牌一变，已经配好它的客户端
就全部失效，用户还得再去文件里看一眼。

取不到 `/dev/urandom` 时**不启用鉴权**，绝不降级成弱令牌 ——
用弱令牌比明说"没开"更危险。

### 三种携带方式

| 方式 | 用途 |
|---|---|
| `Authorization: Bearer <t>` | 标准做法 |
| `X-Remote-Control-Token: <t>` | 不方便设 Authorization 的客户端 |
| `?token=<t>` | `<img src>` / WebSocket 这类**没法设请求头**的场景 |

```bash
curl -H "Authorization: Bearer $T" http://host:8088/api/v1/config
curl -H "X-Remote-Control-Token: $T"        http://host:8088/api/v1/config
curl "http://host:8088/api/v1/config?token=$T"
```

⚠️ `?token=` 会把令牌留在 URL 里（日志、浏览器历史）。
只在确实没法带头的场合用它 —— WebSocket 就是这种场合。

### 哪些不校验

**网页本身**（`/`、`/index.html`、`/ui`）不校验。它只是静态页面、
不含任何秘密，而用户得先打开它才有地方输入令牌。

网页会把令牌存在 `localStorage`。
收到 401 时它会弹出输入条，而不是让你对着一个点不动的页面猜。

### ⚠️ 无鉴权 + 对外绑定 = 设备控制权交给整个网络

```
⚠️ HTTP API 绑定到 0.0.0.0 且未开启鉴权 —— 同网络的任何人都能
   完全控制本设备（截图、触控、装应用、删文件）
```

**只告警，不拒绝启动。** 产品要求首启就是无鉴权模式，硬拒绝会让
"先绑 0.0.0.0 试试"这种正常操作直接起不来。

---

## 四、命令行

```
remote-control [选项]

  --socket <路径>      手动 bind 一个 Unix socket（开发期用）
  --init-socket <名字> 接管 init 创建的 socket（生产用，见 remote-control.rc）
  --display <id>       指定显示 ID，0 表示自动选主显示
  --touch-range <WxH>  触控坐标范围，默认取显示分辨率
  --uid <uid>          所有初始化完成后降到该 UID（需要 root）
  --gid <gid>          配套的 GID，省略则用与 uid 相同的值
  --selftest           检查运行环境后退出（首次部署时先跑这个）
  --config <路径>      配置文件，默认 /sdcard/remote-control.conf
  --http-bind <地址>   启用 HTTP/JSON API 并绑定该地址（如 0.0.0.0 对外）
                       不指定则由配置文件决定
  --http-port <端口>   HTTP 端口，默认 8088
  --http-token <令牌>  访问令牌。给了就等于开启鉴权
  --socket-mode <8进制> socket 文件权限，默认 0660
  --foreground         前台运行，日志输出到 stderr
  --verbose            详细日志
  -h, --help           显示本帮助

示例:
  # 开发期：前台跑，自己 bind socket
  remote-control --socket /data/local/tmp/remote-control.sock --foreground --verbose

  # 常用：让配置文件决定监听地址/端口/鉴权（上位应用就是这么管的）
  remote-control --socket /data/local/tmp/remote-control.sock

  # 生产：由 init 拉起，socket 由 init 创建并打好 SELinux 标签
  remote-control --init-socket remote-control
```

### `--socket-mode` 的权衡

默认 `0660`（root:root）。要让上位应用以**自己的 UID** 连入，
得放宽到 `0666` —— 但那意味着同设备任何进程都能控制本服务。

生产环境应该走 SELinux（见 `dev/04-x64-android/device/remote_control_x64_arm64/sepolicy/`），
而不是靠放宽文件权限。

---

## 五、运行时改配置

不改文件、不重启，直接通过 API 改：

```bash
curl -X POST http://host:8088/api/v1/config \
     -H 'Content-Type: application/json' \
     -d '{"log-level":"debug"}'
```

```json
{"ok":true,"applied":["log-level"],"requiresRestart":[],"rejected":[],"changed":true}
```

| 键 | 生效方式 | 说明 |
|---|---|---|
| `verbose` | **热改** | 详细日志 |
| `log-level` | **热改** | `debug` / `info` / `warn` / `error` |
| `display` | **热改** | 显示 ID |
| `socket-mode` | **热改** | socket 文件权限（会重新 chmod） |
| `touch-range` | **热改** | `WxH`。会**重建注入设备** |
| `touch-width` / `touch-height` | **热改** | 单独设某一维 |
| `socket` | 需重启 | socket 一旦 bind 就定了 |
| `init-socket` | 需重启 | 同上 |
| `uid` / `gid` | 需重启 | setuid 不可逆 |
| 其他 | — | `rejected` 里带原因：`未知配置项` |

### 为什么"改不动的如实说"

`requiresRestart` 里的项**不会**假装成功。假装成功的后果是调用方以为
改了而行为没变 —— 那种不一致最难排查。

同理，`display` 会**立刻 `ResolveDisplay` 一次**当场反馈能不能用，
而不是等下次截图才失败。

---

## 六、supervisor：无 root 的应用怎么管 root 服务

上位应用是普通 Android 应用，**没有 root**，起不了也停不了 root 守护进程。
但它能写共享存储。所以：

```
上位应用 → 写 /sdcard/remote-control.conf
remote-control-supervisord（常驻 root 脚本）→ 监视文件 → 启停/重启 remote-control
                                  → 写 /sdcard/remote-control.status 供应用显示
```

应用侧因此只依赖"文件能写"这一件事，**不需要任何特权**。

### supervisor 的职责：保活

```bash
# 启动（必须 setsid —— adb shell 一退出，普通后台进程会被一起带走）
adb shell "setsid nohup /data/local/tmp/remote-control-supervisord.sh \
           > /data/local/tmp/sup.log 2>&1 < /dev/null &"
```

每 2 秒读一次配置：

- 进程没了 → 启动
- `bind` / `port` / `auth` / `token` 变了 → 重启
- `enabled` 变了 → **不重启**（软开关由守护进程自己处理）

`enabled` 不进重启指纹是有意的：拨开关不该重启服务，
那会打断所有正在看的画面流和触控连接。

### 状态文件 `/sdcard/remote-control.status`

```ini
running=1
pid=2958
bind=0.0.0.0
port=8088
since=1790552517
serving=1
```

应用读这个文件显示状态，**不去探端口**：端口可能被转发规则挡住，
"进程在不在"才是确定的；而且服务没起来时探端口只会得到"连不上"，
分不清是挂了还是没启动。

### 生产部署：做成 init 服务

```bash
# 见 dev/02-native-daemon/remote-control.rc
service remote-control /system/bin/remote-control --init-socket remote-control
    class main
    user root
    group root
    socket remote-control seqpacket 0660 root system
    seclabel u:r:remote-control:s0
    disabled        # 由 ctl.start 拉起，或改成 oneshot 常驻
```

这样才是真正的"开机自启 + 崩溃拉起"。实机部署时用这个，
supervisor 脚本是给**开发期**和没有定制 init 的场景用的。

---

## 七、一键部署（开发机）

```bash
bash tools/lan-up.sh            # 起模拟器 + 部署 + supervisor + 局域网转发
bash tools/lan-up.sh --status   # 看状态
bash tools/lan-up.sh --stop     # 停服务
```

它会：

1. 起模拟器（没在跑的话）并等开机
2. 推 remote-control / rcctl / cliptool.jar / supervisord
3. 写默认配置（**只在文件不存在时** —— 不覆盖你已设好的）
4. `setsid` 启动 supervisor

优先用 AOSP 构建（SurfaceFlinger 后端，23ms/帧），
没有就退回 NDK 构建（screencap 后端，120ms/帧，**慢 3 倍**）。

---

## 八、降权（`--uid` / `--gid`）

```
⚠️ 必须在所有子系统初始化完成之后才调用。
```

`/dev/uinput` 默认是 `0600 root:root`，socket 也可能在 `/dev/socket` 下 ——
一旦降权，这两样都打不开了。

顺序也不能反：**先 setgid 再 setuid**。反过来 `setgid` 会因为
已经没有 `CAP_SETGID` 而失败。

---

## 相关

- [04-config.md 的鉴权部分](#三鉴权) 与 [01-http.md](01-http.md) 的通用约定
- `dev/04-x64-android/device/remote_control_x64_arm64/sepolicy/` —— SELinux 策略（生产环境用这个，不要靠放宽权限）
- `tools/remote-control-supervisord.sh` —— supervisor 实现
