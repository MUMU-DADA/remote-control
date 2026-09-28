// socket_server.h — Unix domain socket 服务端
//
// 负责：
//   1. 创建/接管监听 socket（支持 init socket activation 与手动 bind 两种模式）
//   2. 用 SOCK_SEQPACKET 收发定长请求/应答
//   3. 用 SCM_RIGHTS 把截图 memfd 传给客户端
//   4. 用 SO_PEERCRED 校验调用方 UID

#pragma once

#include <functional>
#include <string>

#include "protocol.h"

namespace remote_control {

// ReplyPacket 定义在 protocol.h（dispatch 层也要构造它）

// 请求处理回调。返回处理结果。
// 参数：请求头、NUL 分隔的 payload、客户端传来的 fd（无则 -1）、对端 uid。
// reqFd 的所有权属于调用方（ServeConnection），handler 不该关闭它。
using RequestHandler = std::function<ReplyPacket(
        const Request&, const std::string& payload, int reqFd, int peerUid)>;

class SocketServer {
  public:
    // init socket activation 模式：name 对应 remote-control.rc 里 `socket remote-control ...`
    static SocketServer FromInitSocket(const std::string& name);

    // 手动 bind 模式（开发期用）
    static SocketServer FromPath(const std::string& path);

    // 放宽 socket 文件权限。
    //
    // 默认 0660 只允许属主和同组访问。上位应用以自己的 UID 运行时不在此列，
    // 需要放宽到 0666。**这是一个真实的安全权衡**：放宽后同一台设备上
    // 任何进程都能控制这个服务（截图、注入触控、装应用、删下载目录里的文件），
    // 所以默认保守，由部署方显式决定。
    //
    // 生产环境的更好做法是建一个专用 AID、让客户端进程加入该组，
    // 并在 ServeConnection 里按 SO_PEERCRED 校验 uid —— 那段 TODO 还在。
    void SetSocketMode(mode_t mode) { socketMode_ = mode; }

    ~SocketServer();

    // 可移动、不可拷贝。
    // 工厂函数（FromPath / FromInitSocket）按值返回，没有移动构造就编不过。
    SocketServer(SocketServer&& other) noexcept;
    SocketServer& operator=(SocketServer&& other) noexcept;
    SocketServer(const SocketServer&) = delete;
    SocketServer& operator=(const SocketServer&) = delete;

    // 创建/接管监听 socket。返回 false 时 error 有值。
    bool Start(std::string* error);

    // 阻塞式 accept 循环，直到 Stop() 被调用。
    void Run(const RequestHandler& handler);

    // 让 Run() 从阻塞中退出。可从其它线程调用。
    void Stop();

    const std::string& path() const { return path_; }

  private:
    SocketServer() = default;

    // 处理单条连接，直到对端关闭。
    void ServeConnection(int connFd, const RequestHandler& handler);

    // 读一个 Request。返回 0 成功，>0 为 errno，<0 表示对端正常关闭。
    // 收一条请求。
    //
    // SEQPACKET 保留消息边界，所以「44 字节头 + 变长 payload」是一条消息，
    // 按 kMaxRequestPayload 一次读进来即可，不需要自己拼长度前缀。
    //
    // payload  ：NUL 分隔的 UTF-8 字符串（见 protocol.h 的 v2 命令说明）
    // outFd    ：客户端传来的 fd（InstallApp 用它传 APK），没有则 -1。
    //            调用方负责关闭。
    //
    // 返回 0 成功；-1 对端关闭；>0 是 errno。
    int RecvRequest(int connFd, Request* out, std::string* payload, int* outFd);

    // 发应答，可选带一个 fd。
    bool SendReply(int connFd, const ReplyPacket& packet);

    std::string path_;
    // 手动模式下的 socket 路径；init 模式下为空
    mode_t      socketMode_ = 0660;   // 可用 --socket-mode 放宽，见 main.cpp 的说明
    std::string initSocketName_;    // init 模式下的 socket 名
    int  listenFd_ = -1;
    bool stop_     = false;
    bool ownsPath_ = false;         // true 表示退出时要 unlink path_
};

}  // namespace remote_control
