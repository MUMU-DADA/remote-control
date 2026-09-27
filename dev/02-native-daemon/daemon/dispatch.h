// dispatch.h —— 请求分发
//
// 把协议请求翻译成 Capture / Injector / AppOps / FileOps 的调用。
//
// 为什么单独成文件：这样集成测试可以直接用**真实的**分发逻辑，
// 而不是在测试里复制一份。
//
// 应答的两种形态：
//   1. 二进制（截图的帧）—— memfd 里是原始像素
//   2. 结构化（v2 命令）—— memfd 里是 UTF-8 JSON
// 两者都走同一个 SCM_RIGHTS 通道，靠 reply.cmd 区分（见 protocol.h）。

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "protocol.h"

namespace autod {

class Capture;
class Injector;
class AppOps;
class FileOps;
class Keyboard;
class ClipOps;

class Dispatcher {
  public:
    Dispatcher(Capture* capture, Injector* injector);
    ~Dispatcher();

    // 处理一个请求。不会抛异常；失败通过 reply.status 表达。
    //
    // payload : NUL 分隔的字符串参数（可为空）
    // reqFd   : 客户端传来的 fd（InstallApp 的 APK），没有则 -1。
    //           所有权仍属调用方，本函数不关闭它。
    ReplyPacket Handle(const Request& req, const std::string& payload, int reqFd,
                       int peerUid);

    // 把 NUL 分隔的 payload 拆成参数列表。
    // 空 payload 返回空 vector（不是含一个空串的 vector）。
    static std::vector<std::string> SplitPayload(const std::string& payload);

  private:
    ReplyPacket HandleInfo(const Request& req);
    ReplyPacket HandleCapture(const Request& req);
    ReplyPacket HandleTouch(const Request& req);

    // ── v2：应用与文件管理 ──
    ReplyPacket HandleListApps(const Request& req);
    ReplyPacket HandleAppInfo(const Request& req, const std::vector<std::string>& args);
    ReplyPacket HandleLaunchApp(const Request& req, const std::vector<std::string>& args);
    ReplyPacket HandleKillApp(const Request& req, const std::vector<std::string>& args);
    ReplyPacket HandleForegroundApp(const Request& req);
    ReplyPacket HandleInstallApp(const Request& req, int reqFd);
    ReplyPacket HandleDownload(const Request& req, const std::vector<std::string>& args);
    ReplyPacket HandleFileOp(const Request& req, const std::vector<std::string>& args);

    // ── v3：服务自身 ──
    ReplyPacket HandleDescribe(const Request& req);
    ReplyPacket HandleGetConfig(const Request& req);
    ReplyPacket HandleSetConfig(const Request& req, const std::vector<std::string>& args);
    ReplyPacket HandleSelfTest(const Request& req);
    ReplyPacket HandleStats(const Request& req);
    ReplyPacket HandleLog(const Request& req, const std::vector<std::string>& args);
    ReplyPacket HandleShutdown(const Request& req, bool restart);

    // ── v4：手势 / 按键 / 剪贴板 ──
    ReplyPacket HandleGesture(const Request& req);
    ReplyPacket HandleKeyEvent(const Request& req, const std::vector<std::string>& args);
    ReplyPacket HandleClipboard(const Request& req, const std::vector<std::string>& args);
    ReplyPacket HandlePower(const Request& req, const std::vector<std::string>& args);

    Capture*  capture_;
    Injector* injector_;
    std::unique_ptr<AppOps>  appOps_;
    std::unique_ptr<FileOps> fileOps_;
    // 操作串行化锁。
    //
    // ⚠️ 锁必须在**这一层**，不能放在调用方。
    //
    //   Injector 是有状态的（按下/抬起、触控槽位映射、手势的 downTime），
    //   两条传输同时注入会互相破坏对方的手势。
    //
    //   原来锁在 main.cpp 的 HTTP/socket 处理器里 —— 那对流式响应是**无效**的：
    //   streamer 回调是在处理器**返回之后**才由 ServeConnection 调用的，
    //   那时锁早就释放了。所以 WebSocket 里流的每个触控点都是无锁注入的。
    //
    //   放进 Dispatcher::Handle 之后，无论谁调、从哪条传输调，都自动串行。
    std::mutex opMutex_;

    // 键盘是延迟创建的：建了就会在系统里多一个输入设备，
    // 没用到按键功能的部署不该平白多出它。
    std::unique_ptr<Keyboard> keyboard_;
};

}  // namespace autod
