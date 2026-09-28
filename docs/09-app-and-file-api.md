# 09 · 应用与文件管理 API

> 📖 **接口的权威说明在 [`docs/api/`](api/README.md)。**
>
> 本文是**设计记录** —— 记录当时的取舍、踩过的坑和实测数据。
> 接口的参数、响应、错误码以 `docs/api/` 为准；两边不一致时，
> 是本文没跟上，不是接口变了。
>
> 也可以直接问服务：`GET /api/v1/describe` 是接口清单的机器可读版本。

---

> v2 协议。七项能力，全部在真实 Android 12（x86_64 模拟器，userdebug）上验证通过。

---

## 协议约定

| 方向 | 格式 | 为什么 |
|---|---|---|
| 请求 payload | **NUL 分隔的 UTF-8 字符串** | 参数少而固定。字符串本身不含 NUL，零依赖、无歧义，比 JSON 少一个解析器 |
| 应答 payload | **JSON** | 结构化、字段会增长。只需要"写"不需要"读"，所以不必引解析库 |
| 大块数据 | **memfd + `SCM_RIGHTS`** | 复用截图已经在用的通道。没有大小限制（SEQPACKET 单条消息约 208KB 上限，APK 动辄几十 MB） |

应答体统一走 fd 回传，`reply.cmd` 决定客户端怎么解释它：
`Cmd::Capture` 是原始像素，其余是 UTF-8 JSON。

**失败时也带 JSON**（里面有服务端给的可读原因），客户端先读内容再看状态码，
报错就是服务端原话而不是"internal error"。

### flags

| 位 | 名字 | 用于 |
|---|---|---|
| `1<<3` | `kFlagIncludeSystem` | ListApps：含系统应用 |
| `1<<4` | `kFlagWithMetadata` | ListApps：附带路径/版本/installer |
| `1<<5` | `kFlagReplace` | InstallApp：`-r` 覆盖安装 |
| `1<<6` | `kFlagRecursive` | FileOp：mkdir 建多级 / delete 递归 |

---

## 命令

### 10 · ListApps

```
请求 payload: 无
flags:        kFlagIncludeSystem, kFlagWithMetadata
应答:         {"ok":true,"count":136,"includeSystem":true,
              "apps":[{"package":"com.android.settings",
                       "apkPath":"/system_ext/priv-app/Settings",
                       "versionCode":31,"installer":null,"system":true}]}
```

**应用标签和图标不在这里取。** `pm list packages` 给不出标签，逐个
`dumpsys package` 对 100+ 应用太慢；而上位应用一个
`PackageManager.getApplicationLabel()` 就有了，还带图标。
分工：daemon 给包名和廉价元数据，展示层交给上位应用。

### 11 · AppInfo — 应用清单

```
请求 payload: "<package>"
应答:         {"ok":true,"package":..,"versionName":"12","versionCode":31,
              "uid":1000,"minSdk":31,"targetSdk":31,"apkPath":..,"dataDir":..,
              "system":true,"enabled":true,"signatureDigest":"b4addb29",
              "firstInstallTime":"2023-05-04 10:39:46",
              "permissions":[...101 项],"activities":[...201 项],
              "services":[...],"receivers":[...],"providers":[...]}
```

数据来自 `dumpsys package <pkg>`。

⚠️ **组件清单只包含声明了 intent-filter 的组件** —— `dumpsys` 的
resolver table 是按 intent 组织的，没有 filter 的组件不会出现。
对"这个应用能做什么"这个问题够用；要完整清单得解析 APK 的 manifest。

### 12 · LaunchApp

```
请求 payload: "<package>[\0<activity>]"
应答:         {"ok":true,"package":..,"component":"com.android.settings/.Settings"}
```

不给 activity 时先 `cmd package resolve-activity --brief <pkg>` 解析默认入口；
解析不到就退回 `monkey -p <pkg> -c android.intent.category.LAUNCHER 1`。

不加 `-W`（不等启动完成）—— 慢启动的应用会把请求拖住。

### 13 · KillApp

```
请求 payload: "<package>"
应答:         {"ok":true,"package":..,"stillRunning":false}
```

`am force-stop` 是异步的，所以**会再查一次 pidof 确认**（先立即查，
还在就等 500ms 再查），把结论一并返回，而不是只报"命令发出去了"。

### 14 · ForegroundApp

```
应答: {"ok":true,"package":"com.android.launcher3",
       "activity":"com.android.launcher3.uioverrides.QuickstepLauncher",
       "pid":1067,"userId":0}
```

解析 `dumpsys activity activities` 的 `mResumedActivity`。
息屏或开机中没有前台 Activity 时返回 `kErrNotFound` 而不是空数据。

### 15 · InstallApp

```
请求: 通过 SCM_RIGHTS 传一个 fd，内容是 APK
flags: kFlagReplace
应答: {"ok":true,"bytes":12702,"replace":true}
```

服务端把 fd 落到临时文件再 `pm install`（`pm install` 只接受路径，不读 stdin），
不管成败都删临时文件。

**为什么用 fd 而不是把字节塞进 payload**：SEQPACKET 单条消息有大小上限。

### 16 · Download

```
请求 payload: "<url>[\0<filename>[\0<subdir>]]"
应答: {"ok":true,"path":"repo.xml",
       "absPath":"/storage/emulated/0/Download/repo.xml",
       "bytes":420815,"url":"https://..."}
```

**实现方式：运行时 `dlopen("libcurl.so")`。** 为什么不是直接链接：

- NDK 里**没有** curl 的头文件和库，直接链接的话 NDK 构建就没有下载能力
- 而设备上 `/system/lib64/libcurl.so` 是有的（VNDK），实测可加载
- 所以运行期探测、有就用，AOSP 和 NDK 两种构建共用一条代码路径

设备上确实没有可用的 libcurl 时返回 `kErrUnsupported` 并给出明确说明。

安全限制（都是刻意的）：
- 只允许 `http`/`https`。**重定向后也只允许这两个** —— 否则一个 302 到
  `file://` 就能把本地文件读出来
- 有大小上限（默认 512MB），超了立刻中止并**删掉半截文件**
- 总超时（默认 300s）

下载完成后会广播 `MEDIA_SCANNER_SCAN_FILE`，这样文件会出现在系统
"下载"应用和文件管理器里。

### 17 · FileOp

```
请求 payload: "<op>[\0<path>[\0<arg>]]"
op: list | stat | exists | mkdir | delete | rename
```

| op | 应答 |
|---|---|
| `list` | `{"ok":true,"dir":..,"count":N,"entries":[{"name","path","dir","size","mtime"}]}` |
| `stat` | `{"ok":true,"name","path","dir","size","mtime"}` |
| `exists` | `{"ok":true,"exists":true/false}` |
| `mkdir` | `{"ok":true,"path":..}` （`kFlagRecursive` = `-p`） |
| `delete` | `{"ok":true,"path":..,"recursive":..}` |
| `rename` | `{"ok":true,"from":..,"to":..}` |

---

## ⚠️ 安全边界：路径约束

**所有路径参数都来自客户端，而 autod 以 root 运行。一个 `../..` 就能删掉 `/data`。**

所以每个入口都强制走 `FileOps::ResolveInside()`，把路径约束在下载目录内：

```cpp
// 逐段规范化，遇到 ".." 且栈已空 = 试图逃逸 → 拒绝
// 绝对路径只接受已经在下载目录里的（客户端常用 list 返回的 path 直接回传）
// 再对**已存在的最深祖先**做 realpath，确认软链接没有指到外面
```

**软链接检查不能省**：下载目录里一个指向 `/data` 的软链接，会让 `../`
检查形同虚设 —— 因为文件系统层面它确实"在"下载目录下。

实测（全部被拒，错误信息可读）：

```
../../data/system/users.xml   → 路径越界（..）
/data/system/packages.xml     → 路径不在下载目录内（只能是 /storage/emulated/0/Download 下的相对路径）
../DCIM                       → 路径越界（..）
```

还有 17 项单元测试覆盖各种逃逸写法（含 `/sdcard/DownloadX/a` 这种
前缀相同但不是子路径的），见 `tests/test_appops.cpp` 与
`tests/fixtures/`。

---

## 上游依赖与缺失时的行为

| 命令 | 依赖 | 缺失时 |
|---|---|---|
| ListApps / AppInfo / Launch / Kill / Foreground / Install | `/system/bin/{pm,am,cmd,dumpsys}` | `kErrUnsupported` + 列出缺哪个 |
| Download | `libcurl.so`（运行时探测） | `kErrUnsupported` + 提示改用客户端推送 |
| FileOp | 下载目录存在 | `kErrUnsupported` + 列出试过的路径 |

**都是明确的错误消息，不是静默失败。**

---

## 实测记录（Android 12 / x86_64 / userdebug）

```
foreground    → com.android.launcher3/.../QuickstepLauncher, pid 1067
list-apps     → 136 个应用
app-info      → com.android.settings：101 权限 / 201 Activity / 8 Service /
                12 Receiver / 10 Provider，签名 b4addb29
launch        → com.android.settings/.Settings，foreground 确认 pid 2328
kill          → stillRunning:false，前台回到 launcher
install       → 12702 字节经 memfd；android.ext.shared 从
                /system/app/ 变成 /data/app/（真的装成用户应用了）
download      → HTTPS 420815 字节 → /storage/emulated/0/Download/repo.xml
file_op       → mkdir/ls/stat/mv/rm 正常；三种路径逃逸全部被拒
```

---

## 相关文件

| 路径 | 内容 |
|---|---|
| `daemon/protocol.h` | 命令、flags、状态码定义 |
| `daemon/json_writer.h` | 极简 JSON 输出器（只写不读） |
| `daemon/subprocess.h/.cpp` | 无 shell 的子进程执行（argv 数组，绝不拼字符串） |
| `daemon/appops.h/.cpp` | 应用管理，解析逻辑抽成纯函数以便主机测试 |
| `daemon/fileops.h/.cpp` | 下载目录与文件操作，含路径约束 |
| `daemon/http_client.h/.cpp` | libcurl 运行时加载与下载 |
| `tests/test_appops.cpp` | 47 项解析测试（用真实设备输出做夹具） |
| `tests/fixtures/` | 从设备抓下来的 dumpsys / pm 输出 |
