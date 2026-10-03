# macOS 宿主移植调研：远程屏幕控制守护进程的 API 等价性与权限模型

> 研究对象：一个 Linux/Android 上的 C++ 守护进程（屏幕截图 + 编码为 png/jpeg/webp + h264 流；触摸/鼠标/键盘注入；Unix socket + SCM_RIGHTS 传 memfd 零拷贝；HTTP/WebSocket；33 条命令含应用管理、文件读写、剪贴板、屏幕旋转、电源）。
>
> **版本基线（已核实）**：当前（2026 年）macOS 26 "Tahoe" 是出货主力（已见 26.1 / 26.2 / 26.4 / 26.5.2 / 26.6.1 / 26.6.2 等版本号），macOS 27 "Golden Gate" 已发布/在测，配套 Xcode 27。上一代 macOS 15 Sequoia / 14 Sonoma 仍在安全维护（2026-08-06 发布 15.7.9 / 14.8.9）。
> - macOS 27 存在：[support.apple.com/zh-cn/127257](https://support.apple.com/zh-cn/127257)、[MacRumors macOS Golden Gate roundup](https://www.macrumors.com/roundup/macos-27/)、[MacRumors macOS Tahoe roundup](https://www.macrumors.com/roundup/macos-26/)
> - 版本号证据：[Karabiner-DriverKit-VirtualHIDDevice README](https://github.com/pqrs-org/Karabiner-DriverKit-VirtualHIDDevice)（列出 "macOS 27 Golden Gate / macOS 26 Tahoe / 15 / 14 / 13"）
> - [Huntress CVE 分析](https://cybernoz.com/from-screen-share-to-root-access-breaking-down-cve-2026-43760-and-cve-2026-65400-on-macos/)（提到 26.6.1 / 15.7.9 / 14.8.9）
>
> **方法论声明**：以下每条都标注了【已核实】（我实际抓取了该页面/源码/文档元数据）或【未核实】。凡我拿不到正文的（Apple Developer Forums 有机器人墙，只能拿到标题与搜索摘要；Stack Overflow 返回 403，仅有第三方镜像正文），都已明确标注。

---

## 1. 屏幕捕获

### 1.1 四条路径的现状

| API | 最低版本 | 废弃状态 | 需要 Screen Recording 权限 | 备注 |
|---|---|---|---|---|
| `CGWindowListCreateImage` | — | **macOS 15.0 起 obsoleted**（硬错误，不是普通 deprecated） | 是 | 编译期直接失败 |
| `CGDisplayCreateImage` / `CGDisplayCreateImageForRect` | — | 已废弃（具体版本【未核实】） | 是 | 社区一致称 deprecated |
| `CGDisplayStream` | 未标注 | Apple 文档**未**标 deprecated | 是 | 被 Apple 归入"deprecated content capture technologies" |
| ScreenCaptureKit `SCStream` / `SCStreamConfiguration` / `SCContentFilter` | **macOS 12.3** | 当前官方推荐路径 | 是 | — |
| `SCScreenshotManager` | **macOS 14.0**（Mac Catalyst 18.2） | 当前官方推荐路径 | 是 | 单帧截图 |
| `SCScreenshotConfiguration` / `SCScreenshotOutput` | **macOS 26.0**（新增） | 当前官方推荐路径 | 是 | 新增：直接输出 png/jpeg/heic |

**【已核实】** `CGWindowListCreateImage` 在 macOS 15 SDK 里是 **obsoleted**（`unavailable`），编译器报错的原文是：

```
'CGWindowListCreateImage' is unavailable: obsoleted in macOS 15.0 - Please use ScreenCaptureKit instead.
```

- WebKit Bugzilla 277564（2024-08-02 报告，Sequoia 15.0 Beta 24A5298h + Xcode 16 beta 3）明确记录了这条诊断，并且该 issue 被标为 RESOLVED FIXED。来源：[bugs.webkit.org/show_bug.cgi?id=277564](https://bugs.webkit.org/show_bug.cgi?id=277564)
- 同一轮适配还打了另外两个项目：[FreeRDP #10558 "Failure to build for macOS 15 due to use of obsoleted APIs"](https://github.com/FreeRDP/FreeRDP/issues/10558)（FreeRDP 本身就是远程桌面客户端，与本项目场景一致）、[KeePassXC #11288 "Failed build on macOS with Xcode 16.0"](https://github.com/keepassxreboot/keepassxc/issues/11288)。
- **重要区分**：obsoleted ≠ 运行时被移除。用 macOS 15+ SDK **无法编译**这些调用，但已编译好的旧二进制通常仍能链接到符号并运行（Tahoe 上的实测见 1.3，行为已退化）。

**【已核实】ScreenCaptureKit 各符号的最低版本**来自 Apple 文档的 availability 元数据（`developer.apple.com/documentation/.../*.md` 端点里的 JSON 头）：

- `SCStreamConfiguration.queueDepth` → `macOS: 12.3.0 -`，并有一句硬约束：**默认且最小值是 3 帧；不要超过 8 帧**（"Don't exceed a queue depth of eight frames"）。来源：[queueDepth](https://developer.apple.com/documentation/screencapturekit/scstreamconfiguration/queuedepth)
- `SCContentFilter` → `macOS: 12.3.0 -`。来源：[SCContentFilter](https://developer.apple.com/documentation/screencapturekit/sccontentfilter)
- `SCScreenshotManager` → `macOS: 14.0.0 -`（Mac Catalyst 18.2）。来源：[SCScreenshotManager](https://developer.apple.com/documentation/screencapturekit/scscreenshotmanager)
- `SCScreenshotConfiguration` → **`macOS: 26.0.0 -`**。来源：[SCScreenshotConfiguration](https://developer.apple.com/documentation/screencapturekit/scscreenshotconfiguration)
- `CGPreflightScreenCaptureAccess()` → `macOS: 10.15.0 -`，文档未标 deprecated。来源：[CGPreflightScreenCaptureAccess](https://developer.apple.com/documentation/coregraphics/cgpreflightscreencaptureaccess())
- `CGRequestScreenCaptureAccess()` → `macOS: 10.15.0 -`，文档未标 deprecated。来源：[CGRequestScreenCaptureAccess](https://developer.apple.com/documentation/coregraphics/cgrequestscreencaptureaccess())
- `CGDisplayStream` 的文档元数据里 `"deprecated": false`，且未给 introducedAt。来源：[CGDisplayStream](https://developer.apple.com/documentation/coregraphics/cgdisplaystream)

**【已核实】`SCScreenshotConfiguration`（macOS 26 新增）的能力**——这对本项目非常关键，因为它把「截图 + 编码成 png/jpeg」整个外包给了系统：

- `contentType: UTType` —— 输出格式 **HEIC / JPEG / PNG**
- `fileURL: URL?` —— **直接落盘**，不用自己写编码器
- `dynamicRange` —— SDR / HDR / 两者
- `width` / `height` / `sourceRect` / `destinationRect`
- `showsCursor`、`includeChildWindows`、`ignoreShadows`、`ignoreClipping`、`displayIntent`（local vs canonical display）
- `class var supportedContentTypes: [UTType]`
- 方法：`captureScreenshot(contentFilter:configuration:completionHandler:)` 与 `captureScreenshot(rect:configuration:completionHandler:)`，返回 `SCScreenshotOutput`

来源：[SCScreenshotConfiguration](https://developer.apple.com/documentation/screencapturekit/scscreenshotconfiguration)、[SCScreenshotManager](https://developer.apple.com/documentation/screencapturekit/scscreenshotmanager)

### 1.2 HDR / 宽色域 / 帧率

**【已核实】** WWDC24 Session 10088 "Capture HDR content with ScreenCaptureKit"（讲者 Ben Harry, ScreenCaptureKit 团队）逐条给出了 HDR 配置：

- `captureDynamicRange` → `hdrLocalDisplay` 或 `hdrCanonicalDisplay`
- `pixelFormat` → 至少 **10-bit per component**，"For most situations 10-bit YCbCr will be the best choice"
- `colorSpaceName` → HDR 需要 HLG 或 PQ transfer function（例如 Display P3 PQ）
- `colorMatrix` → BGRA ↔ YCbCr 转换
- 两个预设：`captureHDRStreamLocalDisplay` / `captureHDRStreamCanonicalDisplay`（截图侧同样有预设）
- `SCScreenshotManager` 也支持 HDR 截图；要 `CMSampleBuffer` 用 `captureSampleBuffer`，要 `CGImage` 用 `captureImage`
- 同一场 session 还新增了麦克风采集（`captureMicrophone` / `microphoneCaptureDeviceID`）和 `SCRecordingOutput` 直录文件

来源：WWDC24 逐字稿镜像 [nonstrict.eu/wwdcindex/wwdc2024/10088](https://nonstrict.eu/wwdcindex/wwdc2024/10088/)，官方页 [developer.apple.com/videos/play/wwdc2024/10088](https://developer.apple.com/videos/play/wwdc2024/10088/)

**【已核实】** 帧率相关的一条现实约束（平台不同但同源）：iPadOS 27 上即使把 `minimumFrameInterval` 设成 1/120，在 120Hz ProMotion 设备上仍被限制在 60fps（Apple 论坛帖标题 "[ScreenCaptureKit on iPadOS 27 is capped at 60 fps on 120Hz ProMotion devices, even with minimumFrameInterval set to 1/120](https://developer.apple.com/forums/thread/848296)"）。这说明 `minimumFrameInterval` 只是"上限请求"，不保证达到。

**【未核实】** 以下我没有在本轮拿到一手证据，不要当结论用：

- macOS 上 `SCStream` 能否稳定跑 120fps（我只核实了 60fps 这一档有现实依据）
- `SCStream` 交付的 `CMSampleBuffer` 里 `CVPixelBuffer` 是否是 IOSurface-backed、是否真正做到 GPU 零拷贝到 VideoToolbox
- 30/60fps 1080p 下的具体 CPU 占用数字
- `CGDisplayCreateImage` 被标 deprecated 的确切 SDK 版本（我只有 Apple 论坛帖标题 "[Question about deprecated CGDisplayCreateImage()](https://developer.apple.com/forums/thread/748798)" 作为旁证，正文被机器人墙拦住）

### 1.3 一个必须在真机上复测的坏消息

**【已核实（第三方 issue，含逐项测试矩阵）】** trycua/cua issue #870，2026-01-21 提交、2026-03-17 更新，环境 macOS 26.2 Tahoe Build 25C56。作者在 Tahoe 上实测的截图 API 矩阵：

| API | Tahoe 26.x 上的结果 |
|---|---|
| `CGWindowListCreateImage`（OnScreenOnly） | 只有桌面背景，没有应用窗口 |
| `CGWindowListCreateImage`（IncludingWindow + windowID） | 返回 None |
| `CGDisplayCreateImage` | 只有桌面背景 |
| `CGDisplayCreateImageForRect` | 只有桌面背景 |
| ScreenCaptureKit `SCScreenshotManager` | TCC 错误 **-3801** |
| `screencapture` 命令行 | "could not create image from display" |
| VNC framebuffer | 黑屏 |

TCC 数据库里显示 `kTCCServiceScreenCapture | Python | auth_value=2 (allowed)`，但 SCK 仍返回：

```
Error Domain=com.apple.ScreenCaptureKit.SCStreamErrorDomain Code=-3801
"The user declined TCCs for application, window, display capture"
```

作者自陈根因可能是「macOS Tahoe 要求 app bundle」**或**「Lume VM 的显示合成问题」——他自己并列了这两种可能，没有定论。来源：[trycua/cua#870](https://github.com/trycua/cua/issues/870)

> 这条的价值：它给出了 **-3801 这个错误码的语义**（"The user declined TCCs…"），且证明「旧 CG 截图 API 在新系统上可能只拿到桌面」。但它是在 VM 里做的，**不能直接推广到物理机**。

### 1.4 结论

> **结论**：做全屏捕获只有一条路——**ScreenCaptureKit `SCStream`**。旧 CG 路径（`CGWindowListCreateImage` / `CGDisplayCreateImage` / `CGDisplayStream`）中，`CGWindowListCreateImage` 在 macOS 15 SDK 里已被 **obsoleted**，你用它就编译不过；`CGDisplayCreateImage` 已 deprecated；`CGDisplayStream` 虽未被文档标 deprecated，但被 Apple 在 Sequoia 里归入"deprecated content capture technologies"。`SCScreenshotManager` 是单帧 API，不要拿它轮询当流用（每次都是独立请求）。
>
> **风险**：① 帧率上限不可靠，`minimumFrameInterval` 只是请求值；② HDR 需要 10-bit + HLG/PQ 全链路重配，不是开个开关；③ `queueDepth` 硬上限 8 帧（默认 3），低延迟设计必须围着这个小队列做背压/丢帧；④ Tahoe 上旧 API 已经出现「只截到桌面」和 -3801 的现实案例，且 VM 环境不可信。
>
> **推荐做法**：用 **`SCStream` + `SCStreamOutput`** 走流式（不是 `SCScreenshotManager` 轮询），`SCStreamConfiguration` 设 720p/1080p + 30/60fps + `queueDepth` 3–4；要「截图命令」时在 macOS 26+ 上用 `SCScreenshotConfiguration` 直接产出 png/jpeg（省掉自写编码器），在 26 以下退回 `captureImage` 拿 `CGImage` 再自己编码；h264 流把 `SCStream` 的 `CMSampleBuffer` 直接喂 VideoToolbox，避免中间拷贝；旧 CG API 全部从代码里删掉，不要留编译开关。

---

## 2. 内容保护（Android `FLAG_SECURE` 的等价物）

### 2.1 直接结论：macOS 没有面向「窗口 owner 的一行开关」

**【已核实】** Apple 对 `NSWindow.SharingType.none` 的官方定性是该枚举的摘要原文：

> **A legacy constant that macOS no longer uses.**

其 Discussion 逐字为：

> `NSWindowSharingNone` can cause content to not be available in certain sharing situations. **Don't use this value to hide or omit content from being captured. Instead, use FairPlay Streaming (FPS).**

来源：Apple 文档元数据 `NSWindow.SharingType.none`，摘要与 Discussion 我在 JSON 端点里读到了原文：[developer.apple.com/documentation/appkit/nswindow/sharingtype-swift.enum/none](https://developer.apple.com/documentation/appkit/nswindow/sharingtype-swift.enum/none)

同一份文档里 `SharingType.readWrite` 被标 `deprecated: true`；`readOnly` 未标废弃。也就是说这个枚举整体处在"遗留"状态。

**【已核实（标题级证据，正文被机器人墙拦截）】** 而且**即使设了也不再可靠**——Apple 论坛有一条明确的反例帖：

> **"On macOS 15.4+, NSWindow with kCGWindowSharingStateSharingNone still captured by ScreenCaptureKit"**
> [developer.apple.com/forums/thread/792152](https://developer.apple.com/forums/thread/792152)

我抓到了标题但正文被 `verify-human` 拦截（该站对自动化访问做了人机校验），所以**具体复现条件与 Apple 的回复我未核实**。但标题本身已经说明：`.none` 不是可以依赖的保护机制。

### 2.2 `SCContentFilter` 的作用方向是反的

**【已核实】** `SCContentFilter` 的构造器（macOS 12.3+）全部是**捕获方**在做选择：

- `init(desktopIndependentWindow:)` —— 只抓指定窗口
- `init(display:including:)` / `init(display:includingWindows:)` —— 只抓指定窗口
- `init(display:excludingWindows:)` —— 抓整个 display，**排除**指定窗口
- `init(display:excludingApplications:exceptingWindows:)` —— 抓整个 display，**排除**指定 app
- `init(display:includingApplications:exceptingWindows:)` —— 只抓指定 app

来源：[SCContentFilter](https://developer.apple.com/documentation/screencapturekit/sccontentfilter)

**这对本项目意味着**：被捕获的进程完全无法通过 `SCContentFilter` 保护自己——filter 由捕获方构造。反过来，如果被控 Mac 想让你「别拍某个 app」，只能由你（守护进程）主动把它加进 `excludingApplications`，也就是**靠自觉**。

### 2.3 DRM / 受保护视频：真实存在，而且是整会话级别的

**【已核实（生产代码，含逐条注释）】** screenpipe 的 `drm_detector.rs`（活跃维护的项目，文件头部注释就是一份设计文档）记录了 macOS 上的实际行为：

- **DRM 流媒体服务在 ScreenCaptureKit 激活时会显示黑屏**。代码里枚举的 app 名单包括：Netflix、Disney+、Hulu、Prime Video、Apple TV（原生 app 名为 `TV`）、Peacock、Paramount+、HBO Max / Max、Crunchyroll、DAZN；域名名单包括 netflix.com / disneyplus.com / hulu.com / primevideo.com / tv.apple.com / peacocktv.com / paramountplus.com / play.max.com / crunchyroll.com / dazn.com，以及 amazon.com/gp/video/ 路径。
- **远程桌面客户端也会主动遮黑自己的窗口**：Omnissa Horizon Client（含 "Next"）和旧版 VMware Horizon Client —— "blank their windows while any app holds an SCK session"。
- **黑屏范围是整个 SCK 会话，不是单个窗口**：代码注释明确写 "macOS DRM blacks out protected content **whenever ScreenCaptureKit is active on ANY display**"。所以即使 DRM 窗口在另一块屏上，你的捕获也会被影响。
- **仅仅"跳过这一帧的捕获"不够**：注释写 "we must fully release all SCK handles AND stop calling any SCK APIs (including monitor enumeration)"。他们的实现是：检测到 DRM 时停掉所有 monitor（释放 SCK 句柄）、连 `list_monitors_detailed()` 都不调（避免触碰 SCK）、只保留基于 Accessibility 的焦点 app 轮询；切回非 DRM app 后整体重启。
- 检测只能用非 SCK 的 API：`AXUIElement` 取 focused app、浏览器读 `AXDocument` 拿 URL（Safari / Arc 走 AppleScript 兜底）、`CGWindowListCopyWindowInfo` 扫描屏幕上是否还有 DRM 窗口。

来源：[screenpipe/crates/screenpipe-engine/src/drm_detector.rs](https://github.com/screenpipe/screenpipe/blob/main/crates/screenpipe-engine/src/drm_detector.rs)

### 2.4 被抓进程能不能绕过？——不能（但有前提）

**【已核实部分】** 结论成立的理由链：

1. Apple 官方**明确拒绝**把 `sharingType` 当作保护手段，并把内容保护导向 **FairPlay Streaming**（见 2.1 的官方原文）。也就是说保护是在**内容管线**层面做的，不在窗口属性层面。
2. DRM 黑屏是**系统在合成/编码路径**上做的（screenpipe 观察到的是"SCK 一活跃就黑"，且跨 display、跨进程生效），捕获进程没有任何 API 可以把它关掉——它只能选择"不抓"。
3. 论坛标题级证据显示 `sharingType = .none` 在 macOS 15.4+ 已被 SCK 穿透，所以窗口 owner 的控制力也在下降。

**【未核实】** 我没有拿到 Apple 关于"捕获方无法绕过 DRM"的正式书面声明（没有这样的文档）。另外以下我都没核实：

- Safari vs Chrome 在 Netflix/Disney+ 上的行为差异
- QuickTime / HDCP 内容在截图里的具体表现
- `NSSecureTextField` 在**别人的**进程截图里是否被特殊处理（我没有找到任何 Apple 文档说它会被保护）
- ScreenCaptureKit 是否有私有/受限 entitlement 能绕过 TCC 弹窗
- `EnableSecureEventInput`（安全输入）对合成键盘事件的影响 —— 这个对"注入"比"捕获"更相关，见第 3 节

### 2.5 结论

> **结论**：macOS 上**没有** `FLAG_SECURE` 的等价物。`NSWindow.sharingType = .none` 被 Apple 官方定性为「macOS 已不再使用的遗留常量」并明确劝阻用它隐藏内容；`SCContentFilter` 的排除能力属于捕获方而非被捕获方；真正起作用的是 **FairPlay/DRM 在系统合成路径上的黑屏**，它会在任何 SCK 会话活跃时把受保护内容（甚至整屏）变黑。
>
> **风险**：① 黑屏是**会话级**而非窗口级——你只要持有 SCK 会话，DRM 内容一播放就可能整屏变黑，用户体验会很差；② 仅仅"这一帧不抓"没用，必须彻底释放 SCK 句柄并停止调用一切 SCK API（包括枚举显示器）；③ 检测 DRM 只能靠 Accessibility + `CGWindowListCopyWindowInfo` 启发式，本质是 app/域名名单，会漏（新服务、改名、WebView 内嵌）；④ `.none` 已不可靠，任何依赖它做"隐私模式"的假设都可能在 15.4+ 上失效。
>
> **推荐做法**：照抄 screenpipe 的模式——**检测到 DRM/受保护内容就整体停采并拆掉 SCK 会话**，用 AX 焦点轮询 + 窗口标题/URL 名单做判定，恢复时重建流；把 DRM app/域名名单做成可配置；在文档里明确告知用户「受保护视频播放期间远程画面会变黑，这是系统行为，不是 bug」；绝不要向客户承诺"能看到 Netflix 画面"。

---

## 3. 输入注入

### 3.1 权限 API（这一节是本节最硬的部分）

**【已核实】** 现代 macOS 有两套官方权限查询 API，分别对应两个不同的 TCC 服务：

**(a) Accessibility（`kTCCServiceAccessibility`）—— 用于「发送」事件**

- `AXIsProcessTrusted()` / `AXIsProcessTrustedWithOptions(_:)`，配 `kAXTrustedCheckOptionPrompt` 触发弹窗。该符号位于 ApplicationServices / HIServices。来源：[AXIsProcessTrustedWithOptions](https://developer.apple.com/documentation/applicationservices/1459186-axisprocesstrustedwithoptions)（正文页在 robots 后面，我只核实了 URL 与符号归属）

**(b) Input Monitoring（`kTCCServiceListenEvent`）—— 用于「监听」事件，以及 Post（`PostEvent`）**

IOKit 侧有两个函数、两个 request type：

```rust
// 来自 openlogi-hid 的实现（已核实源码）
use objc2_io_kit::{IOHIDAccessType, IOHIDCheckAccess, IOHIDRequestAccess, IOHIDRequestType};

IOHIDCheckAccess(IOHIDRequestType::ListenEvent)   // -> Granted / Denied / Unknown，从不弹窗
IOHIDRequestAccess(IOHIDRequestType::ListenEvent) // 会弹窗
```

该源码的注释给了三条关键语义（我逐字引用其要点）：

1. 「`IOHIDRequestAccess` **blocks the calling thread until the user answers the consent dialog**（或状态已确定时立即返回）——调用方必须在 async runtime 之外执行它。」
2. 「TCC 授权是**绑定到发起请求的代码签名身份**的，而且**必须是真正需要它的那个进程**去问 —— 这里必须是 agent 本身，而不是 GUI。」原文："a TCC grant is scoped to the code-signing identity that asks for it, and the agent (not the GUI) is the one that actually opens HID devices."
3. `IOHIDCheckAccess` 不弹窗，`IOHIDRequestAccess` 才弹窗；两者成对使用。

来源：[docs.rs openlogi-hid permissions.rs](https://docs.rs/openlogi-hid/latest/src/openlogi_hid/permissions.rs.html)

`IOHIDRequestType` 同时有 `ListenEvent` 与 `PostEvent` 两个取值（源码里 import 的就是这个 enum）。

**【已核实（仅标题）】** 这套 API 有已知坑：Apple 论坛有一帖标题为 "[IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) does not work](https://developer.apple.com/forums/thread/809431)"（正文被机器人墙拦）。所以**不要只信 `IOHIDCheckAccess` 的返回值**，应该"查 + 试"结合。

### 3.2 沙盒与 App Store

**【已核实（仅标题级）】** 三条件相关的 Apple 论坛帖，正文均被机器人墙拦，但标题已经足够定性：

- "[Clarification on Accessibility and Input Monitoring APIs for App Store Apps](https://developer.apple.com/forums/thread/780626)"
- "[Accessibility Permission In Sandbox For Keyboard](https://developer.apple.com/forums/thread/789896)"
- "[Clipboard manager rejected under Guideline 2.4.5 for using CGEvent.post — what is the correct approach?](https://developer.apple.com/forums/thread/820594)"

**【未核实】** App Sandbox 到底在哪一层拦 `CGEventPost`（是硬拦还是"能调用但事件被丢弃"），我没有拿到一手证据。

**但可以确定的策略结论**：一个远程屏幕控制守护进程**不要走 Mac App Store**。第 3 条标题直接说明 `CGEvent.post` 会导致 2.4.5 拒审。目标是 Developer ID 直发 + 公证（见第 7 节）。

### 3.3 root LaunchDaemon 能注入输入吗？——不能

**【已核实】** 这是本节最重要的结论，有明确的社区权威答案 + Apple 文档引文。Stack Overflow 73596370 的题面与本项目场景几乎完全一致（用 `CGEventCreateKeyboardEvent` + `CGEventPost(kCGHIDEventTap, …)`，在普通 app 里正常，装成 SMJobBless 风格的 `/Library/PrivilegedHelperTools` + launchd daemon 后就失效）。被采纳的回答：

> Daemons don't have access to the window session. UI events should be pushed back through an XPC channel to a process in the user's session (usually running as the user, not root). If you want boot config, consider **LaunchAgents instead of LaunchDaemons**.
>
> 并引用 Apple 文档：**"Core frameworks depend on the window server and are therefore only available to applications running in a login session."** 你的 daemon 不在 login session 里。

原文：[Stack Overflow 73596370](https://stackoverflow.com/questions/73596370/cgeventpost-is-not-working-in-macos-launchd-daemon)（我抓 SO 被 403，正文来自其完整镜像 [腾讯云开发者社区 ask/sof/107263523](https://cloud.tencent.com/developer/ask/sof/107263523)，该镜像保留了答案全文与原始链接）

### 3.4 官方「虚拟输入设备」路径：存在，但代价极高

**【已核实】** 如果想做 Android `/dev/uinput` 那种"真·虚拟硬件"，macOS 上的官方路径是 **DriverKit + HIDDriverKit 系统扩展**。以 Karabiner-DriverKit-VirtualHIDDevice 为权威样本（它就是干这个的）：

| 要求 | 内容 |
|---|---|
| 技术形态 | DriverKit 系统扩展（不是 kext，不是用户态 `IOHIDUserDevice`） |
| macOS 支持 | macOS 13 / 14 / 15 / 26 Tahoe / **27 Golden Gate**（27 仅 Apple Silicon） |
| 客户端权限 | **必须以 root 运行**。README 原文："the virtual devices will only accept commands from processes running with **root privileges**"，理由是"能发键鼠等于完全控制 macOS，为防止恶意软件操纵虚拟设备" |
| 用户交互 | 必须由用户手动批准系统扩展（"Follow any macOS prompts to approve the system extension"），可能需要重启 |
| 签名 | **一般开发者账号拿不到 DriverKit 签名权限，必须向 Apple 单独申请**（"a general developer account lacks the necessary permissions for DriverKit signing, so you need to apply to Apple for higher privileges"） |
| 必需 entitlement | `com.apple.developer.driverkit`、`.family.hid.device`、`.family.hid.eventservice`、`.transport.hid`、**`com.apple.developer.hid.virtual.device`** |
| 额外 entitlement | `com.apple.developer.driverkit.userclient-access` —— README 原文："must be applied for from Apple, and **unless individually authorized, it cannot be granted to your application**" |
| 构建环境 | 需要 macOS 26+ / Xcode 27+ |
| 客户通信 | 客户端通过 **Unix domain STREAM socket** 与 daemon 通信：`/Library/Application Support/org.pqrs/tmp/rootonly/karabiner_virtual_hid_device_service.sock` |
| 常驻 | README 推荐用 launchd 跑，并设 `ProcessType: Interactive` |

来源：[Karabiner-DriverKit-VirtualHIDDevice README](https://github.com/pqrs-org/Karabiner-DriverKit-VirtualHIDDevice)

**【未核实】** 以下我都没有在本轮拿到一手证据，**不要当结论**：

- `IOHIDUserDevice`（用户态虚拟 HID，`IOHIDUserDeviceCreate`）在当前 macOS 上是否仍可用、是否需要 root、是否需要 entitlement。我只找到了 Rust 绑定文档页 [docs.rs objc2-io-kit IOHIDUserDevice](https://docs.rs/objc2-io-kit/latest/x86_64-apple-darwin/objc2_io_kit/struct.IOHIDUserDevice.html)，没有核实其运行时约束。
- `IOHIDPostEvent` 是否为私有 API；`NX_SYSDEFINED` / `NX_KEYTYPE_*` 常量的可用性。
- 私有 mach service `com.apple.iohideventsystem` / `IOHIDEventSystemClient` 的现状。
- `CGEventCreateTouchEvent` 是否为私有 API。**我倾向它是私有的，但这是记忆不是核实**，请自行验证。
- `MultitouchSupport.framework`（多点触控 / 触控板手势注入）的可用性。**未核实**。
- `CGEventPostToPid` 的行为与权限要求。**未核实**（我只在题面里见过这个名字，本轮没有找到任何一手来源）。
- `CGEventTap` 到底需要 Input Monitoring 还是 Accessibility（或两者）。**未核实**。
- `EnableSecureEventInput` 是否会让安全输入框拒绝合成键盘事件。**未核实**——但对本项目很重要，建议列入真机验证清单。

### 3.5 结论

> **结论**：远程控制的**唯一务实路径**是 `CGEventPost` 系列（鼠标/键盘/滚轮）跑在**用户 GUI 会话内**、持有 **Accessibility（`kTCCServiceAccessibility`）**授权的进程里；官方"虚拟 HID 设备"路径（DriverKit）虽然存在且是唯一能骗过"真硬件"假设的手段，但需要 Apple 单独授予的 DriverKit 签名权限 + `com.apple.developer.hid.virtual.device` + `driverkit.userclient-access`，还要用户批准系统扩展、客户端必须 root —— 对本项目属于"另一个项目"量级。**root LaunchDaemon 无法注入输入**，因为 Core 框架依赖 WindowServer，daemon 不在 login session 里。
>
> **风险**：① 权威答案（3.3）来自 Stack Overflow + Apple 归档文档，我**没能在 Apple 当前在线文档里找到同一句话**，建议真机复测；② TCC 授权绑定"发起请求的代码签名身份 + 必须由真正使用权限的进程去申请"，把权限请求放在错误进程里会得到"看起来授权了但不生效"；③ App Store 走不通（`CGEvent.post` 触发 2.4.5 拒审）；④ 一堆关键 API 的私有/公开边界我未核实（见 3.4），如果后续要走虚拟 HID 路线必须先补这一课。
>
> **推荐做法**：架构上分两层 —— **root LaunchDaemon** 只做特权事（文件、电源、安装、升级），**每用户一个 Aqua 会话内的 LaunchAgent** 做捕获 + 输入注入，两者用 XPC/Mach service 通信（见第 6 节）。权限检查用 `AXIsProcessTrustedWithOptions` + `IOHIDCheckAccess(kIOHIDRequestTypePostEvent)` 双保险，并且**由那个真正发事件的 agent 进程自己去 `IOHIDRequestAccess`**。先做 CGEventPost 版本上线，把虚拟 HID 作为后续可选项。

---

## 4. 权限与授权（TCC）——重点

### 4.1 两个服务的申请与检查

| 服务 | 检查 API | 申请 API | 最低版本 |
|---|---|---|---|
| Screen Recording `kTCCServiceScreenCapture` | `CGPreflightScreenCaptureAccess()` | `CGRequestScreenCaptureAccess()` | macOS 10.15 |
| Accessibility `kTCCServiceAccessibility` | `AXIsProcessTrusted()` / `AXIsProcessTrustedWithOptions(kAXTrustedCheckOptionPrompt)` | 同上（带 prompt option） | — |
| Input Monitoring `kTCCServiceListenEvent` | `IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)` | `IOHIDRequestAccess(kIOHIDRequestTypeListenEvent)` | — |
| Input Monitoring（Post） | `IOHIDCheckAccess(kIOHIDRequestTypePostEvent)` | `IOHIDRequestAccess(kIOHIDRequestTypePostEvent)` | — |

来源：前两行见 1.1 与 3.1 的 URL；后两行见 [openlogi-hid permissions.rs](https://docs.rs/openlogi-hid/latest/src/openlogi_hid/permissions.rs.html)

**【未核实 / 已知坑（仅标题级）】** `CGRequestScreenCaptureAccess()` 的行为有一个著名陷阱：**每个进程生命周期只弹一次**，且在用户授权前返回 false，即使授权了也要**重启进程**才能生效。我找到的对应 Apple 论坛帖是 "[Understanding CGRequestScreenCaptureAccess()](https://developer.apple.com/forums/thread/732726)"，但正文被机器人墙拦住，**具体语义我未核实**，请真机验证。

### 4.2 授权绑定在什么上？（重编译/换路径会不会失效）

**【已核实（安全研究 wiki，2026-07-14 更新）】** TCC 的存储模型：

- `tccd` 守护进程**按 Bundle ID + code-signing requirement（csreq）作为键**存储授权。原文："The `tccd` daemon stores grants keyed by Bundle ID plus code-signing requirement (csreq), so a permission is tied to a specific signed binary and is inherited from the responsible parent process."
- **两个数据库**：
  - 每用户库：`$HOME/Library/Application Support/com.apple.TCC/TCC.db` —— **可用 Full Disk Access 写入，不受 SIP 保护**
  - 系统库：`/Library/Application Support/com.apple.TCC/TCC.db` —— **受 SIP 保护**
  - **两个库本身都受 TCC 保护**，只有 FDA 或持有 `kTCCServiceEndpointSecurityClient` 的进程才能干净地读取
- `access` 表关键列：`service`（如 `kTCCServiceSystemPolicyAllFiles`）、`client`（bundle id 或路径）、`client_type`（**0 = bundle，1 = path**）、`auth_value`（**0 = denied，1 = unknown，2 = allowed，3 = limited**）、`auth_reason`、`csreq`（签名校验 blob）
- Apple 自家二进制携带**预授权 entitlement**，永不弹窗、也永不出现在数据库里

来源：[Encod3d-Sec/TORCH wiki: macOS TCC](https://raw.githubusercontent.com/Encod3d-Sec/TORCH/refs/heads/main/wiki/techniques/macos/macos-tcc.md)

**由此推出的实际含义（推断，非逐字核实）**：

- TCC 匹配的是 **csreq（designated requirement）**，不是裸 CDHash。因此用 **Developer ID 签名 + 稳定 bundle ID + 稳定 Team ID** 时，**重编译通常不会失效**——因为 DR 只看 Team ID + bundle ID，不看具体二进制哈希。
- **ad-hoc 签名（`codesign -s -`）没有稳定身份**，每次重编译 DR 都变，**授权会失效、必须重新授权**。
- **换路径**：`client_type = 1` 那一类（以裸路径为键）会因移动/改名而失效；以 bundle ID 为键的（`client_type = 0`）只要 bundle ID 不变就相对稳定。
- **换 bundle ID = 换身份**，必然要重新授权。

**【未核实】** 上述"DR 而非 CDHash"的推断我没有拿到一手 Apple 文档。请把「用 Developer ID 签名 + 固定 bundle ID → 重编译不掉授权」当作**待验证假设**。

### 4.3 无 GUI、launchd 拉起的命令行守护进程（不是 .app）能不能拿到这两项权限？

**【已核实（多来源交叉）】** 这是**当前 macOS 26 上正在恶化的一个真实变化**：

**(a) macOS 26.1 (Tahoe) 起，裸可执行文件在屏幕录制隐私 UI 里不再出现。** Apple 论坛存在两个直指这一点的帖子，标题即结论：

- "[Issue: Plain Executables Do Not Appear Under 'Screen & System Audio Recording' on macOS 26.1 (Tahoe)](https://developer.apple.com/forums/thread/807898)"
- "[Background Unix executable not appearing in Screen Recording permissions UI (macOS Tahoe 26.1)](https://developer.apple.com/forums/thread/807323)"

我通过搜索摘要拿到了 807898 的开头文字："I am investigating a change in macOS 26.1 (Tahoe) where plain (non-bundled) executables that request screen recording ac…"。第三方（trycua/cua #870）引用同源帖子给出了完整表述：

> "macOS 26.1 (Tahoe) appears to **require app bundles** for an item to be shown in the Screen Recording privacy UI. Plain (non-bundled) executables that request screen recording access **no longer appear** under System Settings → Privacy & Security → Screen & System Audio Recording."
>
> "Tahoe still prompts for permission and still allows the executable to capture the screen once permission is granted, but **the executable never shows up in the UI list**."

来源：[trycua/cua#870](https://github.com/trycua/cua/issues/870)（转述 Apple 论坛 [thread/806187](https://developer.apple.com/forums/thread/806187) 与 [thread/807323](https://developer.apple.com/forums/thread/807323)）

**(b) 明文结论**：

- 裸二进制**仍然能拿到授权并工作**（Tahoe 仍会弹窗、授权后仍能截屏）。
- 但它**不出现在系统设置里** → 用户无法核验、无法手动吊销，企业无法审计。这是运维灾难，而且从趋势看 Apple 随时可能进一步收紧（只弹窗不给用）。
- `SCScreenshotManager` 在同类场景下报 **-3801 "The user declined TCCs…"**，说明"有授权但用不了"的状态确实会发生。

**(c) launchd 侧的正式警告（已核实原文）**——`launchd.plist(5)` 的 CAVEATS 段落逐字为：

> Daemons and agents managed by launchd are subject to macOS user privacy protections. **Specifying privacy sensitive files and folders in a launchd plist may not have the desired effect, and may prevent the job from running.**

来源：[launchd.plist(5) man page](https://keith.github.io/xcode-man-pages/launchd.plist.5.html)

**【未核实】** 以下我都没有核实，请勿当结论：

- 「TCC 要求必须从 Finder 启动过一次」——**未核实**。我没有找到任何一手来源。
- 「TCC 归因给 responsible process，所以从 Terminal 跑会继承 Terminal 的授权」——这是社区广泛认知，我找到的最接近的一手材料是 Objective-See 的 SyScan 2018 演讲 PDF "[UI, backed by TCC](https://objective-see.org/talks/Wardle_SyScan2018.pdf)"，但**我没有读正文**。**未核实**。
- 非 bundle 场景下弹窗到底归因给谁（launchd？还是根本没有 UI 可弹）。**未核实**——这是真机验证的第一优先级。

### 4.4 `tccutil` / TCC.db / MDM PPPC

**`tccutil`（已核实其能力边界）**：

```bash
tccutil reset All app.some.bundleid   # 重置单个 app（会重新弹窗）
tccutil reset All                     # 重置全部
```

**它只能重置/吊销，永远不能授予**。来源：[TORCH wiki](https://raw.githubusercontent.com/Encod3d-Sec/TORCH/refs/heads/main/wiki/techniques/macos/macos-tcc.md)

**直接改 TCC.db（已核实其可行性与前提）**：

```bash
# 每用户库（需 FDA 才可写）
sqlite3 ~/Library/Application\ Support/com.apple.TCC/TCC.db \
  "select service, client, auth_value, auth_reason from access;"

# 系统库（SIP 保护）
sqlite3 /Library/Application\ Support/com.apple.TCC/TCC.db \
  "select client from access where service='kTCCServiceSystemPolicyAllFiles' and auth_value=2;"
```

插入授权还需要构造 csreq blob：

```bash
REQ_STR=$(codesign -d -r- /Applications/Utilities/Terminal.app/ 2>&1 | awk -F ' => ' '/designated/{print $2}')
echo "$REQ_STR" | csreq -r- -b /tmp/csreq.bin
```

来源：[TORCH wiki](https://raw.githubusercontent.com/Encod3d-Sec/TORCH/refs/heads/main/wiki/techniques/macos/macos-tcc.md)

> **但请务必注意**：同一份 wiki 把"写 TCC.db 提权"归入**后渗透技术**，并指出最有效的变体（`$HOME` 劫持，CVE-2020-9934 类）**已被 Apple 修补**，剩下的手段依赖未打补丁的系统。所以对企业部署而言，**直接改 TCC.db 不是可依赖的授权手段**，而且它会让你的 daemon 在签名/公证/EDR 审查里非常难看。

**MDM PPPC（Privacy Preferences Policy Control）**：

- Apple 官方页面（**我未能渲染其正文**，页面是 JS 渲染的，但 URL 存在且是当前文档）：[Privacy Preferences Policy Control device management payload settings for Apple devices](https://support.apple.com/guide/deployment/dep38df53c2a/web)
- 配置描述文件的 payload 类型是 `com.apple.TCC.configuration-profile-policy`，核心是 `Services` 字典，社区广泛使用的键包括 `Authorization`、`CodeRequirement`、`Identifier`、`IdentifierType`、`StaticCode`、`Allowed`。
- **【已核实的一句关键抱怨】** 2024-10 一篇关于 Sequoia 弹窗的文章下，企业用户评论（获 5 票）："Still no way to manage this properly for enterprise management tools… **PPPC needs to be updated to allow central authorization of screen recording and prompting.**" 来源：[MacRumors 2024-10-07](https://www.macrumors.com/2024/10/07/apple-screen-recording-popup-update/)
- **【未核实】** 到底哪些服务能通过 PPPC **完全静默授予**（不弹窗）。Accessibility 通常被认为可以静默授予；**Screen Recording 的静默授予长期存在限制与企业抱怨**，但我没有拿到 Apple 官方 SupportedServices 表的正文。**这条必须查 Apple 官方 PPPC 文档的 Services 列表确认**。

### 4.5 macOS 15 / 26 的新限制（弹窗）

**【已核实】** macOS 15 Sequoia 引入了屏幕录制权限的**定期重新确认弹窗**。事实链：

1. Sequoia beta 期间频率是**每周**，用户强烈反弹；正式发布前改为**每月**。
2. macOS 15.1 beta 6 的发行说明原文（MacRumors 引用）：

   > Applications using our **deprecated content capture technologies** now have enhanced user awareness policies. Users will see fewer dialogs if they regularly use apps in which they have already acknowledged and accepted the risks.

3. 弹窗文案（MacRumors 引用）：

   > [App Name] is requesting to bypass the system private window picker and directly access your screen and audio. This will allow [App Name] to record your screen and system audio, including personal or sensitive information that may be visible or audible.

4. **没有任何办法永久移除该弹窗**（"There is no option to remove the popup permanently"）。

来源：[MacRumors: Apple Tweaks Screen Recording App Permissions to Decrease Popup Frequency in macOS Sequoia 15.1](https://www.macrumors.com/2024/10/07/apple-screen-recording-popup-update/)（发布日期 2024-10-07）

注意 15.1 说明里那句 "**deprecated** content capture technologies" —— Apple 自己把旧 CG 捕获路径定性为 deprecated，这与第 1 节的 SDK obsoleted 互相印证。

**macOS 26 / 27 的新变化（已核实部分）**：

- macOS 26.0 新增 `SCScreenshotConfiguration` / `SCScreenshotOutput`（见 1.1），是捕获侧最大的 API 增量。
- macOS 26.1 起裸可执行文件不再出现在屏幕录制 UI（见 4.3）。
- **2026 年 TCC 整体在收紧**：2026-10 有报道称 Apple 正在修改 macOS 隐私设置，以阻止第三方 app 滥用 Full Disk Access 读取 Messages 历史（起因是 Meta 的 AI agent Muse 争议；Patrick Wardle 的评论："with FDA, any non-root file is readable"）。来源：[ArsTechnica via Cybernoz, 2026-10-03](https://cybernoz.com/apple-changes-full-disk-access-permissions-to-curb-abuse-from-ai-agents/)（原文 [arstechnica.com/security/2026/10/…](https://arstechnica.com/security/2026/10/apple-changes-full-disk-access-permissions-to-curb-abuse-from-ai-agents/)）
  - **含义**：TCC 的规则**在 2026 年仍在变**，任何"现在能用"的授权技巧都要按季度复验。

**【未核实】** macOS 26/27 是否对**屏幕录制**的弹窗频率或 CPPC 静默授权做了进一步改动。我只核实了 26.1 的"裸二进制不出现在 UI"这一条。

### 4.6 结论

> **结论**：① 检查/申请 API 明确：Screen Recording 用 `CGPreflightScreenCaptureAccess` / `CGRequestScreenCaptureAccess`，Accessibility 用 `AXIsProcessTrusted*`，Input Monitoring 用 `IOHIDCheckAccess` / `IOHIDRequestAccess`。② 授权按 **Bundle ID + code-signing requirement（csreq）** 存储，**ad-hoc 签名重编译必然掉授权，Developer ID + 固定 bundle ID 相对稳定**。③ **裸命令行守护进程在 macOS 26.1+ 拿不到 UI 条目**——能弹窗、授权后能用，但用户在系统设置里看不到它，等于不可审计、不可吊销。④ `tccutil` 只能重置不能授予；直接写 TCC.db 是后渗透技术、不可依赖；MDM PPPC 是唯一的企业静默路径，但 Screen Recording 的静默授予长期受限。⑤ Sequoia 起有**每月弹窗且无法永久关闭**。
>
> **风险**：① 26.1 的 bundle 要求是**正在演进的限制**，趋势只会更严；② 每月弹窗会持续打断无人值守的远程控制场景（这是产品级体验问题，不是技术问题）；③ PPPC 对 Screen Recording 的静默授权能力我**未能核实**，如果这是企业批量部署的核心需求，必须先确认，否则整个交付模型要改；④ 2026-10 的 FDA 收紧说明 TCC 规则仍在变动，任何设计都要留"权限模型改版"的余量。
>
> **推荐做法**：**把守护进程做成 .app bundle 里的 helper**（哪怕它是纯命令行、无 UI），用一个极小的 `.app` 承载 bundle ID 与签名身份，通过 `SMAppService` 注册（见第 6 节）；用 **Developer ID 签名 + 固定 bundle ID + 固定 Team ID**，绝不用 ad-hoc 发布；在 UI 里主动引导用户去系统设置授权并检测 `CGPreflightScreenCaptureAccess` 状态变化；企业部署优先走 MDM PPPC，但**先验证 Screen Recording 能否静默授予**，不行就退回"引导 + 文档 + 支持工单"模式；把"每月需重新确认"写进产品文档与 SLA。

---

## 5. 进程内机制等价物

### 5.1 `memfd_create` 的替代

**【已核实】** Apple 的 `shm_open(2)` man page 给出了几条**与 Linux 显著不同**的语义：

- 「There is **no visible entry in the file system** for the created object in this implementation.」——不会像 Linux 那样在 `/dev/shm` 下看到文件。
- 「When a shared memory object is created, it **persists until it is unlinked and all other references are gone**. Objects **do not persist across a system reboot**.」——即 `shm_unlink` 后已 mmap 的引用仍存活，符合"引用计数回收"模型，比 Linux 更接近 `memfd` 的生命周期语义。
- 「The new file descriptor will have the **`FD_CLOEXEC` flag set**.」——**与 Linux 的 `memfd_create` 默认不同**，如果你依赖 fd 跨越 exec 传递，需要显式 `fcntl` 清掉。
- 名字长度受 `PSHMNAMLEN` 限制（定义在 `<sys/posix_shm.h>`）。
- 支持 `O_CREAT` / `O_EXCL` / `O_TRUNC`，且「There is no visible entry in the file system」意味着**没有天然的名字冲突检测之外的清理机制**。

来源：[shm_open(2) man page](https://keith.github.io/xcode-man-pages/shm_open.2.html)

**【未核实】** `SHM_ANON`（FreeBSD 的匿名共享内存标志）**在 macOS 的 `shm_open` man page 中没有出现**。我倾向认为 macOS 不支持 `SHM_ANON`，但**这是推断，不是核实**。相关讨论线索：[Stack Overflow "Does Mac OS have a way to create an anonymous file mapping?"](https://stackoverflow.com/questions/39779517/does-mac-os-have-a-way-to-create-an-anonymous-file-mapping)（未读正文）。

**【未核实】** macOS 上真正的"零拷贝 fd-less"路径通常被认为是 **Mach memory objects**（`mach_make_memory_entry_64` + `mach_msg` 的 OOL descriptor）以及 **IOSurface**（跨进程 GPU 缓冲共享）。我**没有在本轮核实**任何一条：`mach_make_memory_entry_64` / `MACH_MSG_OOL_DESCRIPTOR` / `IOSurfaceCreate` + `IOSurfaceLookup` 的可用性、沙盒限制、以及 IOSurface 能否跨进程传递。**建议把"SCStream 的 CVPixelBuffer → 编码器"这一段留作后续单独调研**，不要在本轮结论里下判断。

### 5.2 `SCM_RIGHTS` 传 fd：**可用**

**【已核实，来自 Apple 官方 man page 原文】** `unix(4)`：

> The UNIX-domain family supports the `SOCK_STREAM` and `SOCK_DGRAM` socket types and uses filesystem pathnames for addressing.
>
> **`SOCK_STREAM` sockets also support the communication of UNIX file descriptors through the use of the `msg_control` field in the msg argument to `sendmsg(2)` and `recvmsg(2)`.**
>
> Any valid descriptor may be sent in a message. The file descriptor(s) to be passed are described using a struct cmsghdr that is defined in the include file ⟨sys/socket.h⟩. The type of the message is **`SCM_RIGHTS`**, and the data portion of the messages is an array of integers representing the file descriptors to be passed.
>
> The received descriptor is a **duplicate** of the sender's descriptor, as if it were created with a call to `dup(2)`. **Per-process descriptor flags, set with `fcntl(2)`, are not passed to a receiver.** Descriptors that are awaiting delivery, or that are purposely not received, are **automatically closed by the system when the destination socket is closed**.

来源：[unix(4) man page](https://keith.github.io/xcode-man-pages/unix.4.html)

**移植要点**：`dup` 语义、`fcntl` 标志不传递（所以 O_NONBLOCK 之类需要接收方自己设）、未接收的 fd 随 socket 关闭自动回收——这三条与 Linux 一致。**限制：man page 没有给出单条消息最多能带多少个 fd 的上限**，**【未核实】**。

### 5.3 `SOCK_SEQPACKET`：**macOS 的 AF_UNIX 不支持**（本节最重要结论）

**【已核实，两条独立证据】**

**证据 A —— Apple 官方 man page 明确只列了两种类型。** `unix(4)` 原文（同一段落里，如果是支持的类型必然会像 `SOCK_STREAM` 那样被点名）：

> The UNIX-domain protocol family **supports the `SOCK_STREAM` and `SOCK_DGRAM` socket types** and uses filesystem pathnames for addressing.

来源：[unix(4)](https://keith.github.io/xcode-man-pages/unix.4.html)

**证据 B —— 一个专门做跨平台 socketpair 的库，在源码里显式绕开。** `socketpair` crate v0.19.8（作者 sunfishcode，即 rustix 的作者；发布日期 2026-09-02）的 `rustix.rs` 源码：

```rust
/// Create a socketpair and return seqpacket handles connected to each end.
#[cfg(not(any(target_os = "ios", target_os = "macos")))]
#[inline]
pub fn socketpair_seqpacket() -> io::Result<(SocketpairStream, SocketpairStream)> {
    let (a, b) = rustix::net::socketpair(
        AddressFamily::UNIX, SocketType::SEQPACKET, SocketFlags::CLOEXEC, None)?;
    ...
}

/// Create a socketpair and return seqpacket handles connected to each end.
#[cfg(any(target_os = "ios", target_os = "macos"))]
#[inline]
pub fn socketpair_seqpacket() -> io::Result<(SocketpairStream, SocketpairStream)> {
    // Darwin doesn't support `SEQPACKET` on `UNIX`-domain sockets, so we use
    // `DGRAM` instead, which also provides packet-boundary semantics. `DGRAM`
    // isn't reliable in general, but is commonly understood to be reliable
    // in the `UNIX` domain.
    //
    // And, Darwin doesn't have `SOCK_CLOEXEC`. So we call `ioctl_fioclex`
    // to emulate it.
    let (a, b) = rustix::net::socketpair(
        AddressFamily::UNIX, SocketType::DGRAM, SocketFlags::empty(), None)?;
    rustix::io::ioctl_fioclex(&a)?;
    rustix::io::ioctl_fioclex(&b)?;
    ...
}
```

来源：[socketpair crate 源码 rustix.rs](https://docs.rs/socketpair/latest/src/socketpair/rustix.rs.html)、[socketpair_seqpacket 文档](https://docs.rs/socketpair/latest/socketpair/fn.socketpair_seqpacket.html)

**这段源码同时给出了两个可操作的事实**：

1. **macOS 的 AF_UNIX 没有 `SOCK_SEQPACKET`**，正确的替代是 **`SOCK_DGRAM`**，作者明确说明它"也提供包边界语义"且"在 UNIX domain 内被普遍认为是可靠的"。
2. **macOS 没有 `SOCK_CLOEXEC`**，需要用 `ioctl(FIOCLEX)` 模拟。

**【未核实（诚实披露）】** 我**没有**直接 grep XNU 源码（`bsd/kern/uipc_usrreq.c` 体积过大，本轮未能拉取全文）。我找到的 XNU 源码入口是 [apple-oss-distributions/xnu rel/xnu-11215 bsd/kern/uipc_usrreq.c](https://github.com/apple-oss-distributions/xnu/blob/rel/xnu-11215/bsd/kern/uipc_usrreq.c)，**如果这条结论对你的架构是决定性的，请在那里确认 `uipc_attach` / `unp_attach` 对 `so_type` 的校验分支**。不过 Apple 官方 man page + rustix 作者的平台适配代码两条独立证据已经足以支撑工程决策。

**旁证（仅标题）**：WebKit 有一个 commit 标题为 "[GTK][Mac] socketpair assertion failure"，与上述平台差异一致（我未能读取 commit 正文）。来源：[WebKit commit 48092a2](https://github.com/WebKit/WebKit/commit/48092a210df085a482aea3f1b5ec2569700aa97c)

**顺带一个容易混淆的点**：`launchd.plist(5)` 的 `Sockets` 字典里 `SockType` 接受 `"seqpacket"`（"The default is 'stream' and other valid values for this key are 'dgram' and 'seqpacket' respectively"）。**这不代表 AF_UNIX 支持 seqpacket** —— 该键配合 `SockFamily`/`SockProtocol` 用于 inet 套接字；配合 `SockPathName` 时是 Unix 套接字。来源：[launchd.plist(5)](https://keith.github.io/xcode-man-pages/launchd.plist.5.html)

### 5.4 `SO_PEERCRED` 的等价物

**【已核实，来自 Apple man page 原文】** `unix(4)` 的凭据段落：

> The effective credentials (i.e., the user ID and group list) of a peer on a `SOCK_STREAM` socket may be obtained using the **`LOCAL_PEERCRED`** socket option. This may be used by a server to obtain and verify the credentials of its client, and vice versa by the client to verify the credentials of the server. These will arrive in the form of a filled in **struct xucred** (defined in sys/ucred.h).
>
> The credentials presented to the server (the `listen(2)` caller) are those of the client **when it called `connect(2)`**; the credentials presented to the client (the `connect(2)` caller) are those of the server **when it called `listen(2)`**. **This mechanism is reliable; there is no way for either party to influence the credentials presented to its peer** except by calling the appropriate system call (e.g., `connect(2)` or `listen(2)`) under different effective credentials.

来源：[unix(4)](https://keith.github.io/xcode-man-pages/unix.4.html)

**【已核实】** `getpeereid(3)`：

```c
int getpeereid(int s, uid_t *euid, gid_t *egid);
```

> The `getpeereid()` function returns the **effective user and group IDs** of the peer connected to a UNIX-domain socket. The argument s **must be a UNIX-domain socket (unix(4)) of type `SOCK_STREAM`** on which either `connect(2)` or `listen(2)` have been called.
>
> \[Implementation note\] On FreeBSD, `getpeereid()` is implemented in terms of the `LOCAL_PEERCRED` unix(4) socket option.
>
> 错误码包括 `EINVAL`："The argument s does not refer to a socket of type `SOCK_STREAM`"。

来源：[getpeereid(3) man page](https://keith.github.io/xcode-man-pages/getpeereid.3.html)

**【由此得到的关键工程事实】**

| 维度 | Linux `SO_PEERCRED` | macOS `LOCAL_PEERCRED` / `getpeereid` |
|---|---|---|
| 返回内容 | pid + uid + gid | **xucred：uid + gid + group list**（无 pid） |
| 取值时机 | connect 时快照 | **connect/listen 时快照**（不是 getsockopt 时） |
| 可否伪造 | 否 | **否**（man page 原文保证） |
| 适用 socket 类型 | 多种 | **仅 `SOCK_STREAM`**（`getpeereid` 显式 `EINVAL`） |

**【未核实】** `LOCAL_PEERPID` / `LOCAL_PEEREUUID` 是否存在及其语义。这两个名字**没有出现在我读到的 `unix(4)` 与 `getpeereid(3)` man page 里**，所以我无法确认 macOS 是否提供对端 PID。**如果你的鉴权逻辑依赖 PID，这条必须先验证**（我倾向认为 `LOCAL_PEERPID` 存在，但这是记忆不是核实）。

### 5.5 UID 鉴权逻辑的差异（重点）

**【分析（基于 5.4 的已核实事实）】** 把 `SO_PEERCRED` 换成 `getpeereid` / `LOCAL_PEERCRED` 后，如果你的鉴权是"UID 必须等于某个值"，会有以下差异：

1. **拿不到 PID** —— Linux 上你可能用 pid 做额外校验（如检查进程路径、防重放），macOS 侧这个维度可能没有。**未核实 `LOCAL_PEERPID` 是否存在**，保守假设是没有。
2. **只有 effective uid/gid** —— `getpeereid` 给的是 **euid**，不是 ruid。一个 setuid 程序或换了 euid 的进程会以 euid 出现。Linux 的 `SO_PEERCRED` 给的也是 effective credentials，所以这一条两边一致。
3. **拿到的是 connect 时刻的身份** —— 对端 connect 之后再 `setuid` 不会改变你看到的凭据（man page 明确说"no way for either party to influence"）。这实际上**比 Linux 更安全**，但也意味着你不能用它做"连接后重新验证身份"。
4. **`getpeereid` 只支持 `SOCK_STREAM`** —— 这对本项目**恰好是好事**：因为 5.3 已经确定 macOS 上 AF_UNIX 没有 `SOCK_SEQPACKET`，你本来就得改成 `SOCK_STREAM`（加长度前缀）或 `SOCK_DGRAM`。**但如果你选 `SOCK_DGRAM`，`getpeereid` 会直接 `EINVAL`** —— 这是两个决策耦合在一起的地方，必须一起定：**要 `getpeereid` 就必须用 `SOCK_STREAM`**（或用 `LOCAL_PEERCRED` socket option，man page 未明说它是否支持 DGRAM）。
5. **UID 鉴权的粒度问题** —— 在 root LaunchDaemon + 用户 agent 的架构（第 6 节）下，socket 是 root 拥有的，任何**以该用户身份运行**的进程都能通过 UID 校验。这在 macOS 上尤其危险，因为用户会话里的任何 app 都能以该 uid 连接。**建议在 UID 校验之外叠加代码签名校验**（`SecCode*` / `SecRequirement`），或干脆用 **Mach service + XPC 的 `xpc_connection` 代码签名要求**机制——后者是 Apple 的原生方案，能让内核在连接建立时校验对端签名。**【未核实】** 我没有在本轮核实 `xpc_connection` 的代码签名校验 API 细节。

### 5.6 `epoll` / `eventfd` / `timerfd` / `signalfd` 的对应

**【已核实，全部来自 Apple 的 `kqueue(2)` man page】**

Apple 文档化的 filters 共 9 个：`EVFILT_READ`、`EVFILT_EXCEPT`、`EVFILT_WRITE`、`EVFILT_AIO`、`EVFILT_VNODE`、`EVFILT_PROC`、`EVFILT_SIGNAL`、`EVFILT_MACHPORT`、`EVFILT_TIMER`。

**`epoll` → `kqueue` / `kevent`**：

- `kevent(kq, changelist, nchanges, eventlist, nevents, &timeout)`，timeout 是 `struct timespec`；NULL 表示无限等待；`kevent64`/`kevent_qos` 可用 `KEVENT_FLAG_IMMEDIATE` 做 poll 语义。
- `EV_CLEAR`：**"After the event is retrieved by the user, its state is reset. This is useful for filters which report state transitions instead of the current state."** —— 这就是 `EPOLLET` 的对应物。
- `EV_ONESHOT` = `EPOLLONESHOT` 的对应物。
- **注意**：「The queue is not inherited by a child created with `fork(2)`.」—— 与 epoll 一致。
- **重要差异**：`EVFILT_READ` 对 **regular file (vnode)** 的语义是「returns when the file pointer is not at the end of file」，**不是** Linux 上"普通文件永远 ready"的行为。所以移植 epoll 代码时，对普通文件的就绪判断逻辑要重写。
- `EVFILT_READ` 对 socket 受 `SO_RCVLOWAT` 影响，也可用 `NOTE_LOWAT` 设 per-filter 低水位。
- `EVFILT_WRITE`：「this filter is **not supported for vnodes**」。

**`eventfd` → 没有直接等价物**：

- **`EVFILT_USER` 没有出现在 Apple 当前的 `kqueue(2)` man page 的 filter 列表里。** 这是本轮一个重要发现。**【未核实】** `EVFILT_USER` 在 Darwin 的 `<sys/event.h>` 里是否存在——**我倾向存在但未文档化，请不要当结论**。
- 可用的替代：pipe（自管道）、`EVFILT_MACHPORT`（Mach port 收到消息时触发，man page 明确支持）、或 libdispatch 的 `dispatch_source_t`（`DISPATCH_SOURCE_TYPE_DATA_ADD` / Mach port source）。

**`timerfd` → `EVFILT_TIMER`（已核实其全部参数）**：

- `data` 指定超时周期，`fflags` 可选 `NOTE_SECONDS` / `NOTE_USECONDS` / `NOTE_NSECONDS` / `NOTE_MACHTIME` 指定单位。
- `NOTE_ABSOLUTE`：建立一次性绝对到期定时器（语义变为 `EV_ONESHOT`），绝对时间基于 `gettimeofday`；配 `NOTE_MACHTIME` 时基于 `mach_absolute_time()`。
- **默认周期性**，除非指定 `EV_ONESHOT`。返回时 `data` 是自上次 arming 或上次投递以来超期的次数。
- **自动设置 `EV_CLEAR`**。
- **定时器合并（timer coalescing）——对本项目影响很大**：可用 `NOTE_CRITICAL`（"override default power-saving techniques to more strictly respect the leeway value"）、`NOTE_BACKGROUND`（"apply more power-saving techniques"）、`NOTE_LEEWAY`（`ext[1]` 存放用户提供的 deadline slop）。
- 与 launchd 联动：`launchd.plist(5)` 的 `LegacyTimers` 键说明「By default on OS X Mavericks version 10.9 and later, **timers created by launchd jobs are coalesced**」，要精确计时得设 `LegacyTimers = true`，且「This key may have no effect if the job's `ProcessType` is not set to `Interactive`」。
- 替代方案：`dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, ...)`（**【未核实】** 其 leeway 参数与 kqueue 的精度差异）。

**`signalfd` → 没有直接等价物，但有三个**：

- **`EVFILT_SIGNAL`**（已核实原文）：「Takes the signal number to monitor as the identifier and returns when the given signal is generated for the process. **This coexists with the `signal()` and `sigaction()` facilities, and has a lower precedence.** **Only signals sent to the process, not to a particular thread, will trigger the filter.** The filter will record all attempts to deliver a signal to a process, even if the signal has been marked as `SIG_IGN`. Event notification happens **before** normal signal delivery processing. `data` returns the number of times the signal has been generated since the last call to `kevent()`. **This filter automatically sets the `EV_CLEAR` flag internally.**」
- `dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, ...)`
- 经典 self-pipe trick
- **注意**：`EVFILT_SIGNAL` 与 `sigaction` 共存且**优先级更低**，这一点与 `signalfd`（接管信号投递）语义不同，移植时要重新设计信号处理路径。

来源（本节全部）：[kqueue(2) man page](https://keith.github.io/xcode-man-pages/kqueue.2.html)、[launchd.plist(5)](https://keith.github.io/xcode-man-pages/launchd.plist.5.html)

**【未核实】** `os_unfair_lock` 不能用于跨进程共享内存（它是进程内的），应改用 `pthread_mutex` + `PTHREAD_PROCESS_SHARED` —— 这是我的既有认知，**本轮没有核实**，且我记得 macOS 上 robust process-shared mutex 历史上有问题，**请自行验证**。

### 5.7 结论

> **结论**：① `memfd_create` **没有等价物**——`shm_open` 是最接近的（引用计数回收、不跨重启、fd 带 `FD_CLOEXEC`、文件系统里不可见），但**没有 `memfd` 的"无名 + 无残留"保证**；真正的零拷贝应研究 Mach memory object / IOSurface（本轮未核实）。② **`SCM_RIGHTS` 在 macOS 的 AF_UNIX `SOCK_STREAM` 上完全可用**，`dup` 语义、`fcntl` 标志不传递、未接收 fd 自动回收三条与 Linux 一致。③ **macOS 的 AF_UNIX 不支持 `SOCK_SEQPACKET`** —— 必须改用 `SOCK_DGRAM`（保留包边界）或长度前缀 `SOCK_STREAM`；同时 **macOS 没有 `SOCK_CLOEXEC`**，要用 `ioctl(FIOCLEX)` 补。④ `SO_PEERCRED` → `LOCAL_PEERCRED`（`struct xucred`，uid+gid+组列表）或 `getpeereid(3)`（只有 euid/egid，且**仅 `SOCK_STREAM`**）；凭据在 connect/listen 时快照、**不可伪造**。⑤ `epoll` → `kqueue`（`EV_CLEAR` 对应 `EPOLLET`）；`timerfd` → `EVFILT_TIMER`；`signalfd` → `EVFILT_SIGNAL`；**`eventfd` 没有干净的等价物**（`EVFILT_USER` 未出现在 Apple 的 man page 里）。
>
> **风险**：① **`SOCK_SEQPACKET` 与"要用 `getpeereid`"这两个决策互相耦合**——选 `SOCK_DGRAM` 就丢掉 `getpeereid`（它只支持 `SOCK_STREAM`），必须一起定；② 定时器**默认被合并**（kqueue 的 NOTE_LEEWAY 家族 + launchd 的 `LegacyTimers`），对一个遥控守护进程的定时精度是实打实的影响，必须显式处理；③ `EVFILT_READ` 对普通文件的语义与 Linux 不同，任何"拿 kqueue 当 epoll 用"的机械替换会在文件描述符上出错；④ UID 鉴权在 macOS 上粒度偏粗（同用户任意进程都能连），**且我未能确认是否有对端 PID 可用**。
>
> **推荐做法**：socket 层统一走 **`AF_UNIX` + `SOCK_STREAM` + 长度前缀 framing**（同时满足 `getpeereid` 与 `SCM_RIGHTS` 两个需求，代价是失去天然的包边界），零拷贝仍用 `SCM_RIGHTS` 传 `shm_open` 得到的 fd；鉴权从"纯 UID"升级为 **UID + 代码签名校验**（`SecCode` 或直接上 Mach service + XPC 的签名要求），并在文档里记录 UID 校验的粒度局限；事件循环选 **kqueue**，定时用 `EVFILT_TIMER` 并**显式评估是否要 `NOTE_CRITICAL` / 关闭 launchd 的 `LegacyTimers` 合并**；`eventfd` 的用途用 pipe 或 Mach port 替代，不要赌 `EVFILT_USER`。

---

## 6. launchd 常驻

### 6.1 plist 位置与关键键

**【已核实，来自 `launchd.plist(5)` 的 FILES 段原文】**

```
~/Library/LaunchAgents      Per-user agents provided by the user.
/Library/LaunchAgents       Per-user agents provided by the administrator.
/Library/LaunchDaemons      System-wide daemons provided by the administrator.
/System/Library/LaunchAgents    Per-user agents provided by OS X.
/System/Library/LaunchDaemons   System-wide daemons provided by OS X.
```

**[已核实] 关键键（逐条来自 man page）**：

- `Label`（必填，唯一标识；约定 plist 文件名 = `<Label>.plist`）
- `Program`（`execv` 的第一个参数，**必须是绝对路径**）或 `ProgramArguments`
- `BundleProgram` —— 原文：「This key maps to the first argument of `execv(3)` and is an **app-bundle relative path** to the executable for the job. **This key is only supported for plists that are installed using `SMAppService`.**」→ 这条把"bundle 化"和"现代安装方式"绑在了一起，见 6.3。
- `KeepAlive` —— 可以是 `true`，也可以是条件字典。**多个键之间是 OR 关系**；可用条件：`SuccessfulExit`、`Crashed`、`PathState`、`OtherJobEnabled`。`NetworkState` **已不再实现**（"no longer implemented as it never acted how most users expected"）。**`KeepAlive` 隐式蕴含 `RunAtLoad`**。退出频繁会被限流。
- `RunAtLoad` —— 原文劝退：「**This key should be avoided**, as speculative job launches have an adverse effect on system-boot and user-login scenarios.」
- `ThrottleInterval` —— **默认 10 秒**："by default, jobs will not be spawned more than once every 10 seconds"。对一个要快速重启的守护进程，这是必须知道的。
- `StandardOutPath` / `StandardErrorPath` —— 语义已核实；注意 `StandardErrorPath` 原文：「this file is opened as **readable and writable** as mandated by the POSIX specification for unclear reasons」→ **不要假设它是 append-only 日志**。
- `ProcessType` —— `Background` / `Standard`（等于不设）/ `Adaptive` / `Interactive`。**`Interactive` 是唯一"和 app 同等待遇、无资源限制"的档位**，README 级的实践（Karabiner）也是推荐 `Interactive`。
- `LimitLoadToSessionType` —— 原文：「This key **only applies to jobs which are agents**. There are no distinct sessions in the privileged system context.」支持的类型字符串包括 `Aqua` / `Background` / `LoginWindow` / `StandardIO`（**具体合法取值列表我未核实**）。→ **这是让 agent 只在有 GUI 登录会话时启动的正规手段**。
- `UserName` / `GroupName` —— 原文：「This key is **only applicable for services that are loaded into the privileged system domain**.」→ 只有 LaunchDaemon 能用；对 agents，`UserName` 被忽略。
- `MachServices` —— 注册 Mach bootstrap 服务；`ResetAtClose` 子键控制端口回收语义。**注册了 `MachServices` 的 job 必须 check in**（`xpc_connection_create_mach_service` 或 `bootstrap_check_in`），这是 man page 里少数用 **MUST** 的条款。
- `Sockets` —— 含 `SockPathName`（隐含 `SockFamily = "Unix"`）、`SockPathOwner`、`SockPathGroup`、`SockPathMode`（**注意 man page 的已知 bug 提示：plist 不支持八进制，要写十进制**）、`SecureSocketWithKey`。→ **launchd 可以替你创建并持有 Unix socket 的 fd，按需拉起进程**，这对本项目是很合适的 on-demand 模型。
- `SessionCreate` —— 原文：「This key specifies that the job should be spawned into a **new security audit session** rather than the default session for the context it belongs to. See `auditon(2)` for details.」→ **这不是"创建 GUI 会话"！** 它创建的是安全审计会话（`auditon`），与 Aqua/WindowServer 无关。**这是本节最容易被误读的键**，如果指望它拿到 GUI 会彻底落空。
- `ExitTimeOut`、`AbandonProcessGroup`、`LowPriorityIO`、`Umask`、`WorkingDirectory`、`EnvironmentVariables`、`LaunchOnlyOnce`、`WatchPaths`（man page 强烈劝退：filesystem 监控天生 race-prone）、`QueueDirectories`、`StartInterval`（注意：「If the system is asleep during the time of the next scheduled interval firing, **that interval will be missed due to shortcomings in `kqueue(3)`**」）、`StartCalendarInterval`、`EnablePressuredExit`、`LegacyTimers`。

**[已核实] 行为约束（EXPECTATIONS 段）**：launchd 直接拉起的进程**禁止**调用 `daemon(3)`、**禁止** fork 后让父进程退出；**不应**把 stdio 重定向到 `/dev/null`（launchd 会隐式处理）；**应当**用 dispatch source 处理 `SIGTERM` 并快速退出。

**[已核实] 最重要的 CAVEATS 段（原文）**：

> Daemons and agents managed by launchd are **subject to macOS user privacy protections**. Specifying privacy sensitive files and folders in a launchd plist **may not have the desired effect, and may prevent the job from running**.

来源：[launchd.plist(5)](https://keith.github.io/xcode-man-pages/launchd.plist.5.html)

### 6.2 **核心问题：root LaunchDaemon 能注入输入吗？**

**【已核实】不能。必须在用户的 GUI 会话里跑。**

这是第 3.3 节同一个结论，在这里从架构角度重申。证据链有三条互相独立：

**(1) 权威答案（社区 + Apple 文档引文）**：Stack Overflow 73596370 被采纳的回答（完整原文见 3.3）：

> Daemons don't have access to the window session. UI events should be pushed back through an XPC channel to a process in the user's session (usually running as the user, not root). If you want boot config, consider **LaunchAgents instead of LaunchDaemons**.
> Apple 文档引文：**"Core frameworks depend on the window server and are therefore only available to applications running in a login session."**

来源：[SO 73596370](https://stackoverflow.com/questions/73596370/cgeventpost-is-not-working-in-macos-launchd-daemon)，正文镜像：[腾讯云 ask/sof/107263523](https://cloud.tencent.com/developer/ask/sof/107263523)

**(2) `SessionCreate` 不是 GUI 会话**（见 6.1 的 man page 原文）—— 想靠它把 daemon 塞进 GUI 会话是行不通的。

**(3) TCC 是 per-user 的**（见 4.2、4.4 的已核实事实）—— 授权存在 **`~/Library/Application Support/com.apple.TCC/TCC.db`** 这个**每用户**数据库里。因此：

> **root 进程给自己授予 Accessibility，不会让登录用户的会话获得 Accessibility；反之亦然。** 一个以 root 身份跑的 LaunchDaemon **在用户的 TCC 上下文里没有任何 Accessibility 授权**，即使它有 root。

这条是本项目最容易踩的坑：root ≠ 绕过 TCC。

### 6.3 现代安装方式：`SMAppService`

**【已核实】** `SMAppService` 是 **macOS 13.0+** 的 API（Mac Catalyst 16.0），用于替代旧的 plist 安装方式。Apple 文档原文：

> In macOS 13 and later, use `SMAppService` to register and control `LoginItems`, `LaunchAgents`, and `LaunchDaemons` as helper executables for your app.
>
> - For `SMAppServices` initialized as `LaunchAgents`, the `register()` and `unregister()` methods provide a replacement for **installing property lists in `~/Library/LaunchAgents` or `/Library/LaunchAgents`**.
> - For `SMAppServices` initialized as `LaunchDaemons`, the `register()` and `unregister()` methods provide a replacement for **installing property lists in `/Library/LaunchDaemons`**.

关键 API：

- `class func daemon(plistName: String) -> Self` / `class func agent(plistName: String) -> Self` / `class func loginItem(identifier: String) -> Self` / `class var mainApp: SMAppService`
- `func register() throws` —— **原文注释：「Registers the service so it can begin launching subject to user approval.」** → **需要用户批准**
- `func unregister()`
- `var status: SMAppService.Status` —— "A property that describes registration or authorization state of the service"
- `class func openSystemSettingsLoginItems()` —— 直接打开系统设置的登录项面板（引导用户授权的正规手段）
- `class func statusForLegacyPlist(at: URL) -> SMAppService.Status`

来源：[SMAppService](https://developer.apple.com/documentation/servicemanagement/smappservice)

**与 6.1 的联动**：`BundleProgram` 键「only supported for plists that are installed using `SMAppService`」→ **用 `SMAppService` 才能让 plist 引用 app bundle 内的可执行文件**。这与 4.3 的结论（必须 bundle 化）完美衔接：**bundle 化 + SMAppService 注册是同一条技术路线**。

**【未核实】** `SMJobBless` 的确切废弃状态与迁移细节。我只找到 Apple 论坛帖标题 "[Migrating away from SMJobBless](https://developer.apple.com/forums/thread/725811)"（正文被机器人墙拦）。**未核实**。

### 6.4 日志：`os_log` vs 文件

**【已核实】** `StandardOutPath` / `StandardErrorPath` 的语义（见 6.1）。**关键注意**：`StandardErrorPath` 以读写方式打开，且 launchd 会隐式重定向 daemon 的 stdio。→ 文件日志可行，但要自己处理轮转（launchd 不做轮转）。

**【未核实】** 以下全部未核实，**不要当结论**：

- 统一日志（Unified Logging）是否自动捕获 launchd job 的 stdout
- `os_log` 的 `%{public}` / `%{private}` 隐私语义
- info/debug 级别默认不持久化、需要 `log config --mode "level:debug"` 或配置描述文件才能看到
- `log stream` / `log show --last 1h --predicate 'process == "x"'` 的具体用法与限制
- 日志丢弃/限流的行为

我建议真机验证：跑一个 `os_log` 输出 + 一个 `StandardOutPath` 输出的最小 daemon，分别用 `log show` 和读文件对照。

### 6.5 必须先读的安全教训（2026 年真实事件）

**【已核实】** 2026 年 7–8 月，macOS 自带的 Screen Sharing（RFB/VNC）服务爆出两个漏洞，构成了完整的「远程屏幕共享 → root」链条：

| CVE | 性质 | 说明 |
|---|---|---|
| **CVE-2026-43760** | 认证后 confused-deputy | bynar.io 于 2026-07-29 披露；经 legacy VNC 认证的用户可通过 `SSFileCopySender` 以 **root** 读写任意文件系统对象 |
| **CVE-2026-65400** | **认证前** | `screensharingd` 的 SRP 实现里 frame-length validator 错误地返回陈旧的成功状态，导致连接被视为已认证，且会话**无加密保护**（明文）→ 任意文件读写，进而远程代码执行 |

关键细节（对本项目直接有借鉴价值）：

- Screen Sharing 用 **RFB（VNC）协议**，支持两条认证路径：原生 Apple（SRP，对真实 macOS 用户账号认证）与 legacy VNC（单一密码，不与具体用户绑定，可操作当前登录用户）。
- 由两个特权 helper 支撑：`SSFileCopySender` 与 `SSFileCopyReceiver`。在原生认证路径下绑定用户上下文；**在 legacy VNC 下以 root 运行**。
- `SSFileCopySender` 持有 Apple 私有的 `kTCCServiceSystemPolicyAllFiles` entitlement，**直接绕过 TCC** —— 这就是它能读任意文件的原因。
- RCE 路径之一就是**创建 LaunchDaemon**（重启后执行反向 shell）；另一条是改 `.zshenv`。
- 影响版本：**≤ macOS 26.5.2 / ≤ 15.7.7 / ≤ 14.8.7**；修复版本 **26.6.1 / 15.7.9 / 14.8.9**（2026-08-06 发布）。
- **常规加固无效**："removing allowed user accounts, disabling legacy VNC password authentication, or rotating the VNC password have no effect"（因为它是认证前漏洞）。
- 检测手段：**Endpoint Security 框架**的 `ES_EVENT_TYPE_NOTIFY_SCREENSHARING_ATTACH` / `_DETACH` 事件（Apple 在 **macOS 13.0** 引入），可用 `eslogger` 订阅；异常指标包括 `session_username: root` 和 `authentication_type: SRP`（正常应为 `RSA-SRP`）。
- 托管 Mac mini 提供商大量把 SSH / Screen Sharing 默认开启并暴露公网，Censys 上"数万台潜在脆弱主机"。

来源：[Huntress 分析（Cybernoz 转载，2026-08-21）](https://cybernoz.com/from-screen-share-to-root-access-breaking-down-cve-2026-43760-and-cve-2026-65400-on-macos/)，原文 [huntress.com/blog/macos-screen-sharing-rce-patched](https://www.huntress.com/blog/macos-screen-sharing-rce-patched)

### 6.6 结论

> **结论**：① plist 放 `~/Library/LaunchAgents`（用户级）、`/Library/LaunchAgents`（管理员为用户装）、`/Library/LaunchDaemons`（系统级）。② **root LaunchDaemon 不能注入输入**——Core 框架依赖 WindowServer，daemon 不在 login session 里；而且 **Accessibility TCC 是 per-user 的**，root 身份无法替代用户会话里的授权。`SessionCreate` 只创建安全审计会话，**拿不到 GUI**。③ 必须在**用户的 Aqua 会话内**跑一个 LaunchAgent 来做捕获与注入，特权操作留给 root LaunchDaemon，两者用 XPC/Mach service 通信。④ 现代安装走 **`SMAppService`（macOS 13+）**，它需要用户批准，并且是唯一支持 `BundleProgram` 键的安装方式。⑤ `KeepAlive` 隐式开启 `RunAtLoad`，`ThrottleInterval` 默认 10 秒，`RunAtLoad` 被官方劝退，`ProcessType: Interactive` 是给交互式守护进程的正确档位。
>
> **风险**：① `SessionCreate` 极易被误读成"创建 GUI 会话"，照着这个理解做架构会全盘失败；② 定时器默认被合并（`LegacyTimers`）+ `ThrottleInterval` 默认 10 秒，遥控场景的响应性与重启速度都会受影响；③ launchd 的 CAVEATS 明确说隐私敏感路径写在 plist 里可能**导致 job 直接跑不起来**——这是静默失败，排查成本高；④ **2026 年的 Screen Sharing CVE 链条说明"远程屏幕控制"在 macOS 上是高危攻击面**：认证前漏洞 + 明文会话 + root 文件读写 + LaunchDaemon 落地。任何自研守护进程都会继承同等的审视强度（来自 EDR、企业安全团队、以及 Apple 自己）。
>
> **推荐做法**：**双层架构**：root `LaunchDaemon`（`/Library/LaunchDaemons`，只做特权事：文件、电源、安装/升级，通过 `MachServices` 暴露 XPC）+ **每用户一个 Aqua 会话内的 `LaunchAgent`**（`LimitLoadToSessionType = Aqua`，做 ScreenCaptureKit 捕获 + CGEventPost 注入）。**权限必须由真正使用它的那个进程去申请**（agent 申请 Accessibility/Screen Recording，daemon 申请 root 级资源）。用 `SMAppService` 安装以获得 `BundleProgram` 支持与正确的授权 UI。plist 里设 `ProcessType: Interactive`、`KeepAlive` 用条件字典而非 `true`、`ThrottleInterval` 按需下调。**绝不把服务暴露到公网**——绑 loopback 或走 SSH 隧道/VPN；集成 `ES_EVENT_TYPE_NOTIFY_SCREENSHARING_*` 类事件做审计；建立明确的补丁响应流程（参考本次修复窗口：披露到修复约 1 周）。

---

## 7. 交叉编译与签名公证

### 7.1 在 Linux 上交叉编译到 macOS：技术上可行，但有个硬缺口

**【已核实】** **osxcross 在 2026 年仍是活跃维护的项目**。README 给出了三套构建风味：

| 风味 | 工具链 | 说明 |
|---|---|---|
| `stable`（默认） | cctools 986 + ld64 711 | 稳定可用 |
| `latest` | cctools 1030.6.3 + ld64 956.6 | 构建最慢 |
| `llvm` | LLVM 工具 + `ld64.lld` | **最容易构建，官方推荐给新项目**；支持 arm64 / arm64e / x86_64，不支持 i386 / x86_64h |

已核实的能力清单：

- **Host OS**：Linux、\*BSD；**Host 架构**：x86、x86_64、ARM、AArch64/arm64
- **Target 架构**：arm64、arm64e、x86_64、i386
- 提供 cctools-port（`ar`、`lipo`、`otool`）+ `ld64`（或 `ld64.lld`），以及 CMake toolchain file
- 额外脚本可构建 LLVM/Clang/LLD、**Apple Clang**、GCC、compiler-rt
- 自带一个最小 MacPorts 包管理器
- 用法示例：`arm64-apple-darwinXX-clang++ test.cpp -O3 -o test`、`xcrun clang++ -arch x86_64 -arch arm64 -o test`（universal binary）
- Deployment target 默认值：SDK ≥ 14.0 → macOS 10.13

来源：[osxcross README](https://raw.githubusercontent.com/tpoechtrager/osxcross/master/README.md)

**但缺口在这里（已核实 README 原文）**：

> #### Prerequisites
> 1. **Generate the SDK** and place the resulting archive in the `tarballs/` directory.
>
> ### Packaging the SDK
> SDKs can be extracted either from the full Xcode or from the Xcode Command Line Tools.

也就是说：**osxcross 不含 SDK，你必须自己从 Xcode 或 Command Line Tools 里提取**。工具链不是障碍，**SDK 才是**。

**【未核实】** 以下我都没有核实：

- osxcross 是否提供 `codesign` 的替代品。README 列出的工具是 `ar` / `lipo` / `otool` / `ld64` / `ld64.lld` / `llvm-*`，**没有提到 `codesign` 或 `notarytool`**。我倾向认为**不能**在 Linux 上产出可公证的签名，但这是推断。
- Zig `zig cc` 作为 macOS 交叉编译器的现状（是否携带 libSystem stub、能否链接 AppKit/ScreenCaptureKit 这类 framework）。**完全未核实**。
- 能否从 Linux 用 osxcross 链接 **ScreenCaptureKit / AppKit** 这些 framework。SDK 里应该含 `.tbd` stub 与头文件，理论上可以链接，但**我没有核实**，而且 Objective-C/Swift runtime 与 `Info.plist`/bundle 组装、`codesign`、`notarytool` 这些环节仍卡在 Apple 工具链上。

### 7.2 Apple SDK 授权：法律问题（我必须明说未核实）

**【未核实，且属于需要法务确认的事项】**

- Apple 的《Xcode and Apple SDKs License Agreement》PDF 地址我从搜索中拿到：[images.apple.com/legal/sla/docs/xcode.pdf](https://images.apple.com/legal/sla/docs/xcode.pdf)，但**抓取失败**（内容类型为 `application/pdf`，我的工具不支持）。因此**我没有读到条款原文**。
- 我从搜索中确认存在一个 Apple 开发者论坛帖子，标题正是：[macOS SDK and XCode Software License Agreement for cross-compile on Linux](https://developer.apple.com/forums/thread/114179)（正文被机器人墙拦）。**标题本身说明这个问题被反复提出**，但我**没有拿到 Apple 的答复内容**。
- 业界共识（**这是我的转述，不是核实的条款**）：Apple SDK 的许可限制其使用于 Apple 品牌硬件；在 Linux 上使用从 Xcode 提取的 SDK 处于灰色/违规地带。

**结论：这一条我给不出可靠答案，请让法务或 Apple 开发者支持确认，不要基于我的推测做决策。**

### 7.3 签名与公证：对本 daemon 是不是硬性要求？

**【已核实，来自 Apple 官方 notarization 文档】**

**(a) 什么时候需要公证**：

> - Beginning in **macOS 10.14.5**, software signed with a **new Developer ID certificate** and all new or updated kernel extensions **must be notarized to run**.
> - Beginning in **macOS 10.15**, **all software built after June 1, 2019, and distributed with Developer ID must be notarized**.
> - 但通过 **Mac App Store** 分发的软件不需要（App Store 提交流程已包含等效检查）。

**(b) 公证的硬性技术要求（Apple 逐条列出）**：

- 所有分发的可执行文件都要**启用代码签名**且签名有效
- 使用 **Developer ID** application / kext / system extension / installer 证书（**不能用** Mac Distribution、**ad hoc**、Apple Developer、或本地开发证书）
- 为 app **和 command line targets** 启用 **Hardened Runtime**
- 签名包含 **secure timestamp**
- **不要**带 `com.apple.security.get-task-allow`（任何形式为 `true` 的变体）
- 链接 **macOS 10.9 或更高**的 SDK
- entitlement 必须是格式正确的 XML、ASCII 编码

**(c) 可公证的交付物类型**：macOS apps、非 app bundle（如 kext）、磁盘映像（UDIF）、**flat installer packages**。→ 你的 `.pkg` 在列。

**(d) 工具链迁移（硬性且已生效）**：

> **Starting November 1, 2023**, the Apple notary service **no longer accepts uploads from `altool` or Xcode 13 or earlier**. If you notarize your Mac software with the `altool` command-line utility or Xcode 13 or earlier, you need to transition to the **`notarytool`** command-line utility or upgrade to Xcode 14 or later. Existing notarized software will continue to function properly.

自动化流程用 **`notarytool`** + **`stapler`**（随 Xcode 提供），或直接用 Notary API。签名证书由 Account Holder 签发。

来源：[Notarizing macOS software before distribution](https://developer.apple.com/documentation/security/notarizing_macos_software_before_distribution)、[Apple notary service update（Upcoming Requirements, id=11012023a）](https://developer.apple.com/news/upcoming-requirements/?id=11012023a)

**【已核实（来自 Karabiner README，一个真实交付样本）】** 一个实际分发的 macOS 虚拟输入项目在 2026 年的构建环境要求是：**macOS 26+ / Xcode 27+ / Command Line Tools / XcodeGen**；签名需要 `Developer ID Application` 与 `Developer ID Installer` 两个身份（用 `security find-identity -p codesigning -v` / `-p basic -v` 查询）；公证用 `xcrun notarytool store-credentials "<profile>" --apple-id … --team-id …` 存凭据，然后 `make notarize`。来源：[Karabiner README](https://github.com/pqrs-org/Karabiner-DriverKit-VirtualHIDDevice)

**【未核实】** 以下都未核实：

- **Apple Silicon 上所有可执行文件必须至少 ad-hoc 签名才能运行** —— 这是广泛认知，但我**没有在本轮找到一手 Apple 文档**。
- **Hardened Runtime 是否会阻止 `CGEventPost`**，以及是否有对应的 `com.apple.security.cs.*` entitlement 需要加。**未核实**。我的判断是 TCC 才是真正的门槛，Hardened Runtime 不额外加要求，但**这是判断不是事实**。
- 使用 ScreenCaptureKit 是否需要任何 entitlement（除了 TCC 弹窗）。**未核实**（DriverKit 虚拟 HID 那条路是明确需要 Apple 单独授予 entitlement 的，见 3.4，但那不是 SCK）。

### 7.4 结论

> **结论**：① **在 Linux 上交叉编译到 macOS 技术上可行且 osxcross 在 2026 年仍活跃维护**（stable / latest / llvm 三套风味，支持 arm64/arm64e/x86_64），但**它不含 SDK，你必须从 Xcode 或 Command Line Tools 里自行提取**。② **SDK 的合法性我未能核实**——Apple 的 Xcode/SDK 许可协议 PDF 我抓不到正文，只确认了 Apple 论坛上存在同名讨论帖；这是一个需要法务确认的开放问题，**不要在未确认前把它当作可选项**。③ **签名与公证是硬性要求**：macOS 10.15 起所有用 Developer ID 分发、且构建于 2019-06-01 之后的软件**必须公证才能运行**；公证要求 Developer ID 证书 + Hardened Runtime + secure timestamp + 无 get-task-allow + macOS 10.9+ SDK。④ `altool` 自 **2023-11-01** 起不再被公证服务接受，必须用 **`notarytool`**（`stapler` 装订 ticket）。⑤ 一个真实交付样本（Karabiner）的构建环境是**macOS 26+ / Xcode 27+**。
>
> **风险**：① **没有 Mac 就没有 `codesign` / `notarytool` / 真机验证**——交叉编译能产出二进制，但产不出可分发、可公证、TCC 行为可验证的交付物；② **ad-hoc 签名每次重编译都会掉 TCC 授权**（第 4 节），所以"本地开发用 ad-hoc"这条路在调试权限流程时反而更痛苦，建议开发期就用固定 Developer ID；③ SDK 授权是**法律风险**，不是技术风险，且我无法替你确认；④ `Hardened Runtime` 与 `CGEventPost` 的交互我未核实，可能在公证后才发现问题。
>
> **推荐做法**：**接受"必须有一台 Mac"这个前提**——用它做 CI 的最终签名/公证节点（Linux + osxcross 可以做日常编译与快速迭代，但最终产物必须在 Mac 上签名、公证、并在真机上验证 TCC 流程）。开发早期就配置 **Developer ID 证书 + 固定 bundle ID + 固定 Team ID**，避免 ad-hoc 反复掉授权。用 **`notarytool`**（不要碰 `altool`）。交付形态用 **flat `.pkg`** + `SMAppService` 注册（第 6 节），确保 bundle 化（第 4 节）。**SDK 授权问题先问法务再动手。**

---

## 8. 总表：原机制 → macOS 等价物

| 原 Linux/Android 机制 | macOS 等价物 | 需要什么权限 | 改动量 | 置信度 |
|---|---|---|---|---|
| SurfaceFlinger 截图（png/jpeg/webp） | `SCStream` 流式捕获 +（macOS 26+）`SCScreenshotConfiguration` 直出 png/jpeg/heic；26 以下 `captureImage` 拿 `CGImage` 自行编码 | Screen Recording（`kTCCServiceScreenCapture`） | 大 | 高 |
| `CGWindowListCreateImage` / `CGDisplayCreateImage` 旧路径 | **不可用**：前者 macOS 15 SDK 起 obsoleted（编译失败），后者已 deprecated | — | —（必须删除） | 高 / 中（后者的确切废弃版本未核实） |
| H.264 编码流 | `SCStream` 的 `CMSampleBuffer` → VideoToolbox | 同 Screen Recording | 中 | 中（VideoToolbox 细节本轮未核实） |
| 内容保护 / `FLAG_SECURE` | **无等价物**。`NSWindow.sharingType = .none` 被 Apple 官方定性为"macOS 已不再使用的遗留常量"且在 15.4+ 已被 SCK 穿透；真正生效的是 FairPlay/DRM 在系统合成层的黑屏（**会话级**，SCK 一活跃就黑） | 无（无法申请，也无法绕过） | 大（需重构为"检测到 DRM 就整体停采并拆掉 SCK 会话"） | 高（Apple 文档原文）；DRM 行为为**高**（生产代码证据），但"无法绕过"无正式书面声明 |
| `/dev/uinput` 虚拟输入设备（轻量版） | `CGEventPost` / `CGEventCreateMouseEvent` / `CGEventCreateKeyboardEvent` / `CGEventCreateScrollWheelEvent`，**跑在用户 Aqua 会话内** | Accessibility（`kTCCServiceAccessibility`）；检查用 `AXIsProcessTrusted*` 或 `IOHIDCheckAccess(kIOHIDRequestTypePostEvent)` | 小 | 高（权限 API）；`CGEventTap`/`CGEventPostToPid` 的具体权限要求**未核实** |
| `/dev/uinput` 虚拟输入设备（真·虚拟硬件版） | DriverKit + HIDDriverKit 系统扩展（参考 Karabiner-DriverKit-VirtualHIDDevice） | **root** + Apple 单独授予的 DriverKit 签名权限 + `com.apple.developer.hid.virtual.device` + `com.apple.developer.driverkit.userclient-access` + 用户手动批准系统扩展 | 大（等同另起项目） | 高 |
| 多点触控 / 触控板手势注入 | **未核实**（`CGEventCreateTouchEvent` 是否为私有 API 未核实；`MultitouchSupport.framework` 未核实） | 未核实 | 未核实 | 低 |
| `CGEventPost` 从 root LaunchDaemon 发送 | **不可行**。daemon 不在 login session，Core 框架依赖 WindowServer；且 Accessibility TCC 是 per-user 的 | — | —（架构必须分层） | 高 |
| `memfd_create` 零拷贝共享内存 | `shm_open` + `mmap`（引用计数回收、不跨重启、文件系统不可见、**fd 带 `FD_CLOEXEC`**）；真正零拷贝考虑 Mach memory object / IOSurface | 无（沙盒下受限，细节未核实） | 中 | 高（`shm_open` 语义）；Mach/IOSurface **未核实**；`SHM_ANON` **未核实**（man page 未提及） |
| `SCM_RIGHTS` 传 fd | **同名可用**（`AF_UNIX` + `SOCK_STREAM`）。`dup` 语义、`fcntl` 标志不传递、未接收 fd 随 socket 关闭自动回收 | 无 | 小 | 高（Apple `unix(4)` man page 原文）；单消息 fd 数量上限**未核实** |
| `AF_UNIX` + `SOCK_SEQPACKET` | **不支持**。改用 `SOCK_DGRAM`（保留包边界、UNIX domain 内可靠）或长度前缀 `SOCK_STREAM`。附带：**macOS 无 `SOCK_CLOEXEC`**，用 `ioctl(FIOCLEX)` 模拟 | 无 | 中 | 高（Apple man page + rustix 作者源码双证据）；**未直接 grep XNU 源码** |
| `SO_PEERCRED` 鉴权 | `LOCAL_PEERCRED`（`struct xucred`：uid + gid + 组列表）或 `getpeereid(3)`（仅 euid/egid，**仅 `SOCK_STREAM`**）。凭据在 connect/listen 时快照，**不可伪造** | 无 | 小（但鉴权逻辑要重审） | 高（man page 原文）；`LOCAL_PEERPID` 是否存在**未核实** |
| 基于 UID 的鉴权 | 可用，但**粒度偏粗**：同用户会话内任意进程都能连；且拿不到对端 PID（未核实）。建议叠加代码签名校验或改用 XPC 的签名要求 | 无（若走 XPC 代码签名校验需相应 entitlement，细节未核实） | 中 | 中 |
| `epoll` | `kqueue` / `kevent`（`EV_CLEAR` ≈ `EPOLLET`，`EV_ONESHOT` ≈ `EPOLLONESHOT`）。**注意**：`EVFILT_READ` 对普通文件语义与 Linux 不同；kqueue 不被 fork 继承 | 无 | 中 | 高 |
| `eventfd` | **无干净等价物**。用 pipe 或 Mach port（`EVFILT_MACHPORT`）或 `dispatch_source`。`EVFILT_USER` **未出现在 Apple 的 `kqueue(2)` man page 里** | 无 | 中 | 高（"man page 未列"）；`EVFILT_USER` 是否存在于 `<sys/event.h>` **未核实** |
| `timerfd` | `EVFILT_TIMER`（`NOTE_SECONDS`/`USECONDS`/`NSECONDS`/`MACHTIME`、`NOTE_ABSOLUTE`、`NOTE_LEEWAY`/`NOTE_CRITICAL`/`NOTE_BACKGROUND`）。**默认被合并**；launchd job 的定时器默认也合并（`LegacyTimers` 可关，但需 `ProcessType: Interactive`） | 无 | 小（但需显式处理精度） | 高 |
| `signalfd` | `EVFILT_SIGNAL`（与 `sigaction` 共存、**优先级更低**、仅进程定向信号、自动 `EV_CLEAR`、返回自上次 `kevent` 以来的次数）或 `dispatch_source(DISPATCH_SOURCE_TYPE_SIGNAL)` 或 self-pipe | 无 | 小 | 高 |
| `/proc` 进程信息 | **未核实**（`libproc` / `sysctl` 路径本轮未调研） | 未核实（部分可能需 entitlement） | 未核实 | 低 |
| systemd / init 开机自启 | `launchd`：`/Library/LaunchDaemons`（root 系统级）或 `~/Library/LaunchAgents`、`/Library/LaunchAgents`（用户级）。现代安装用 `SMAppService`（macOS 13+，需用户批准） | 无特殊权限，但**受 TCC 隐私保护约束**（man page CAVEATS 明确警告可能直接导致 job 跑不起来） | 中 | 高 |
| root LaunchDaemon 注入输入 | **不可行**。必须 `LimitLoadToSessionType = Aqua` 的每用户 LaunchAgent 在 GUI 会话内做。`SessionCreate` 只创建**安全审计会话**，拿不到 GUI | Accessibility 必须由**用户会话内的进程**申请 | 大（需双层架构） | 高 |
| 剪贴板 | **未核实**（`NSPasteboard`，本轮未调研） | 无（沙盒下受限） | 未核实 | 低 |
| 屏幕旋转 | **未核实**（预期为 CoreGraphics 显示配置 API，本轮未调研） | 未核实 | 未核实 | 低 |
| 电源管理 | **未核实**（预期为 IOKit power API / `pmset`，需 root，本轮未调研） | 预期 root | 未核实 | 低 |
| 应用管理 | **未核实**（预期为 `NSWorkspace` + Accessibility `AXUIElement`，本轮未调研） | 部分需 Accessibility | 未核实 | 低 |
| 文件读写 | **未核实**（POSIX 可用，但受 TCC 分目录保护：Desktop / Documents / Downloads / Full Disk Access） | 各目录对应 TCC 服务 | 未核实 | 低（TCC 分目录机制本身为高置信） |
| HTTP / WebSocket | **未核实**（BSD socket 可用；Apple 侧推荐 Network.framework，本轮未调研） | 无 | 小 | 中 |

### 表外补充：三个跨领域的整体判断

1. **架构必须分层**：root `LaunchDaemon`（特权操作）+ 每用户 Aqua 会话内的 `LaunchAgent`（捕获 + 注入），XPC/Mach service 通信。这是本报告中最有决策价值的一条结论，由三个独立事实共同推出：daemon 无 WindowServer 连接（6.2）、Accessibility TCC 是 per-user（4.2/6.2）、`SessionCreate` 不是 GUI 会话（6.1）。
2. **必须 bundle 化**：macOS 26.1 起裸可执行文件不出现在屏幕录制 UI（4.3），且 `SMAppService` 的 `BundleProgram` 键只对 `SMAppService` 安装的 plist 生效（6.1/6.3）。**"无 GUI 的裸命令行守护进程"这个形态在 macOS 26 上已经是死路。**
3. **"必须有一台 Mac"**：交叉编译可行但产不出可公证、可验证的交付物（7.1/7.3）。签名/公证是 macOS 10.15 起的硬性要求。

### 本报告明确未能核实的清单（建议列入真机验证 / 法务确认）

**技术（真机验证）**：
1. TCC 是否要求 app「从 Finder 启动过一次」——本轮无任何一手来源
2. TCC 归因给 responsible process（从 Terminal 运行会继承 Terminal 授权）——只有 2018 年 Objective-See 演讲标题，未读正文
3. 非 bundle 场景下屏幕录制弹窗到底归因给谁、是否弹得出来
4. `CGRequestScreenCaptureAccess()` 是否每进程只弹一次、是否需重启才生效
5. `SCStream` 在 macOS 上能否稳定 120fps；30/60fps 1080p 的 CPU 占用
6. `SCStream` 的 `CMSampleBuffer` 是否 IOSurface-backed、是否 GPU 零拷贝
7. Mach memory object（`mach_make_memory_entry_64` / OOL descriptor）与 IOSurface 跨进程共享的可行性与沙盒限制
8. `SHM_ANON` 在 macOS 上是否存在
9. `LOCAL_PEERPID` / `LOCAL_PEEREUUID` 是否存在（决定能否做 PID 级鉴权）
10. `SCM_RIGHTS` 单条消息可携带的 fd 数量上限
11. `EVFILT_USER` 在 Darwin `<sys/event.h>` 中是否存在
12. `os_unfair_lock` 在共享内存中的不可用性 + `pthread_mutex` `PTHREAD_PROCESS_SHARED` / robust mutex 在 macOS 上的实际可用性
13. `CGEventTap` 需要 Input Monitoring 还是 Accessibility
14. `CGEventPostToPid` 的行为与权限要求
15. `CGEventCreateTouchEvent` 是否私有 API；`MultitouchSupport.framework` 可用性
16. `IOHIDUserDevice` 在当前 macOS 上是否可用、是否需要 root/entitlement
17. `IOHIDPostEvent` 是否私有；`NX_SYSDEFINED` / `NX_KEYTYPE_*` 常量可用性
18. `EnableSecureEventInput` 是否会让安全输入框拒绝合成键盘事件
19. Hardened Runtime 是否影响 `CGEventPost`；是否需 `com.apple.security.cs.*` entitlement
20. ScreenCaptureKit 是否需要除 TCC 之外的 entitlement
21. Apple Silicon 上"可执行文件必须至少 ad-hoc 签名"的一手文档
22. `os_log` 的隐私/持久化/限流行为；统一日志是否捕获 launchd job 的 stdout
23. `SMJobBless` 的确切废弃状态
24. `LimitLoadToSessionType` 的合法取值全集
25. macOS 26/27 是否进一步改动屏幕录制的弹窗频率或 CPPC 静默授权

**法务**：
26. **Apple SDK 许可协议是否允许在非 Apple 硬件上使用从 Xcode 提取的 SDK**（我只能确认该协议 PDF 的存在与 Apple 论坛上同名讨论帖的存在，未能读到条款原文）
27. **MDM PPPC 到底能否静默授予 Screen Recording**（Apple 官方 PPPC Services 列表页面是 JS 渲染的，我未能读到正文；企业用户 2024 年的公开抱怨暗示不能）

**架构（决定性的）**：
28. **XNU 源码中 `uipc_usrreq.c` 对 `SOCK_SEQPACKET` 的实际校验分支**（本轮只有 Apple man page + rustix 作者源码两条独立证据，未直接 grep 内核源码）。若这条结论对你的架构是决定性的，请到 [apple-oss-distributions/xnu rel/xnu-11215 bsd/kern/uipc_usrreq.c](https://github.com/apple-oss-distributions/xnu/blob/rel/xnu-11215/bsd/kern/uipc_usrreq.c) 确认。
