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

namespace autod {

// ReplyPacket 定义在 protocol.h（dispatch 层也要构造它）

// 请求处理回调。返回处理结果。
using RequestHandler = std::function<ReplyPacket(const Request&, int peerUid)>;

class SocketServer {
  public:
    // init socket activation 模式：name 对应 autod.rc 里 `socket autod ...`
    static SocketServer FromInitSocket(const std::string& name);

    // 手动 bind 模式（开发期用）
    static SocketServer FromPath(const std::string& path);

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
    int RecvRequest(int connFd, Request* out);

    // 发应答，可选带一个 fd。
    bool SendReply(int connFd, const ReplyPacket& packet);

    std::string path_;              // 手动模式下的 socket 路径；init 模式下为空
    std::string initSocketName_;    // init 模式下的 socket 名
    int  listenFd_ = -1;
    bool stop_     = false;
    bool ownsPath_ = false;         // true 表示退出时要 unlink path_
};

}  // namespace autod
