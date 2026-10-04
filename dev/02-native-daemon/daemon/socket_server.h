// socket_server.h — Unix domain socket 服务端
//
// 负责：
//   1. 创建/接管监听 socket（支持 init socket activation 与手动 bind 两种模式）
//   2. 用 SOCK_SEQPACKET 收发定长请求/应答
//   3. 用 SCM_RIGHTS 把截图 memfd 传给客户端
//   4. 用 SO_PEERCRED 校验调用方 UID

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

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
    // 默认 0660 只允许属主和同组访问。文件权限只是第一层边界；连接建立后还会用
    // SO_PEERCRED 拒绝其它 UID。放宽后其它 UID 仍可占用连接/制造日志噪声，
    // 但不能执行控制命令。
    //
    // 生产环境如果确实需要跨 UID 的 socket 客户端，可在启动前显式设置
    // 允许的 UID；默认只允许 daemon 自己的有效 UID。
    void SetSocketMode(mode_t mode);

    // 设置允许连接的对端 UID。必须在 Run() 前调用；默认值 -1 表示使用
    // daemon 当前的有效 UID（geteuid()）。SO_PEERCRED 失败时连接始终拒绝。
    void SetAllowedPeerUid(uid_t uid) { allowedPeerUid_ = static_cast<int64_t>(uid); }

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
    void Run(RequestHandler handler);

    // 让 Run() 从阻塞中退出。可从其它线程调用。
    void Stop();

    // 供 SIGTERM/SIGINT 处理器调用。只写入自管道并设置原子标志，
    // 不获取锁、不分配内存，符合异步信号处理约束。
    void SignalStop() noexcept;

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

    bool CreateWakePipe(std::string* error);

    std::string path_;
    // 手动模式下的 socket 路径；init 模式下为空
    mode_t      socketMode_ = 0660;   // 可用 --socket-mode 放宽，见 main.cpp 的说明
    std::string initSocketName_;    // init 模式下的 socket 名
    int  listenFd_ = -1;
    int  wakeReadFd_ = -1;
    std::atomic<int> wakeWriteFd_{-1};
    std::atomic<bool> stop_{false};
    bool ownsPath_ = false;         // true 表示退出时要 unlink path_
    int64_t     allowedPeerUid_ = -1; // -1 = 当前进程有效 UID；见 SetAllowedPeerUid()
    struct stat pathIdentity_{};
    bool pathIdentityValid_ = false;

    // 每连接一个 detached worker。停止时必须先 shutdown 它们并等待退出，
    // 否则主线程析构 Dispatcher/SocketServer 后，worker 仍可能访问悬空的
    // handler 或 stop_。connFds_ 中的 fd 由 worker 持有，SocketServer 只负责
    // shutdown 唤醒；worker 退出时从集合移除并关闭 fd。
    mutable std::mutex connMutex_;
    std::condition_variable connCv_;
    std::set<int> connFds_;
};

}  // namespace remote_control
