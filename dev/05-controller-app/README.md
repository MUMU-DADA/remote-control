# 05 · 上位应用（remote-control 控制台）

> 一个只做**服务管理**的 Android 应用：连 `remote-control`、看状态、点屏幕、管应用和文件。
> 它自己**不申请任何权限**，所有能力都由 daemon 执行。

---

## 界面结构

```
┌─ socket 路径 ────────────── [连接] ─┐
│ 状态提示（绿=成功 / 红=失败）        │
│ [状态][应用][文件][控制]             │
├─────────────────────────────────────┤
│ 状态：显示参数 + 当前前台应用 + pid  │
│ 应用：列表（标签/图标由本应用解析）、│
│       点击启动、长按看清单/停止      │
│ 文件：浏览下载目录、下载 URL、        │
│       新建/删除/重命名               │
│ 控制：点击、滑动、截图并显示         │
└─────────────────────────────────────┘
```

## 截图

实机截图在 [`docs/evidence/`](../../docs/evidence/)。
**这些截图是 remote-control 自己拍的** —— 服务在拍那个控制它的应用，完整闭环。

| 文件 | 内容 |
|---|---|
| `controller-app-1.png` | 首次启动，未连接 |
| `controller-app-3.png` | 已连接并查询到状态（含 remote-control 报出的前台应用 = 它自己） |
| `controller-app-4.png` | 应用标签页 |
| `controller-app-7.png` | 修复前的应用列表（显示成 APK 路径 —— 那个 bug 的现场） |
| `controller-app-8.png` | 修复后：显示应用标签「remote-control 控制台」 |

## 分工：为什么标签不在 daemon 里解析

`pm list packages` 给不出应用标签；逐个 `dumpsys package` 对 100+ 应用太慢。
而上位应用一个 `PackageManager.getApplicationLabel()` 就有了，还带图标、
还是本地化的。

所以 **daemon 只返回包名和廉价元数据**（路径/版本/installer），
**展示层交给上位应用**。这是有意为之的分工，不是偷懒。

## 应用申请了什么权限

**一个都没有。** 所有能力都由 daemon 执行，应用只是协议的客户端。
这样即使应用被替换，能做的事也不会超过 daemon 暴露的协议范围。

## 构建

```bash
bash dev/05-controller-app/build-apk.sh          # 出 APK
bash dev/05-controller-app/build-apk.sh --install
```

不依赖 AOSP 的 out/（那个目录经常被别的构建占着），用独立 SDK build-tools。
JDK 直接用 AOSP 树自带的 `prebuilts/jdk/jdk11`。

## ⚠️ 部署时的 SELinux 问题

应用以自己的 UID（`untrusted_app` 域）连不上 `/data/local/tmp/remote-control.sock`
（标签 `shell_data_file`）：

```
avc: denied { write } for name="remote-control.sock"
  scontext=u:r:untrusted_app:s0:c105,c256,c512,c768
  tcontext=u:object_r:shell_data_file:s0  tclass=sock_file
```

**不能**用 `allow untrusted_app shell_data_file:sock_file write` 敷衍 ——
那是把口子开给所有第三方应用。正确做法是给上位应用一个专属域，
见 `dev/04-x64-android/device/remote_control_x64_arm64/dev/04-x64-android/device/remote_control_x64_arm64/sepolicy/remote_control_controller.te`。

本次功能验证临时用了 `setenforce 0`，**这不是可交付的方案**。
