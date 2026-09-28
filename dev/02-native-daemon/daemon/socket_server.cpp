// socket_server.cpp

#include "socket_server.h"

#include <string.h>

#include "thread_util.h"

#include <vector>

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "remote_control_log.h"
#include "remote_control_platform.h"

#if REMOTE_CONTROL_HAS_INIT_SOCKET
#include <cutils/sockets.h>
#endif

namespace remote_control {
namespace {

constexpr size_t kMaxFdsPerMessage = 1;
constexpr int    kBacklog          = 8;

// 连接空闲超时。
//
// 为什么必须有：ServeConnection 是在 accept 循环里**串行**调用的。
// 没有超时的话，一个连上来就不发数据的客户端会让 recvmsg 永久阻塞，
// 整个服务随之卡死（症状："服务还在，但谁来都没反应"）。
//
// 为什么操作仍要串行：Injector 是有状态的（downTime、触控槽位映射），
// 并发注入会互相破坏手势状态。连接可以并发，操作不行。
constexpr int kDefaultIdleTimeoutSec = 30;

// 允许用环境变量覆盖，方便测试（不然一个用例要等 30 秒）
// 和运维调优。非法值忽略，回退到默认。
int IdleTimeoutSec() {
    const char* env = getenv("REMOTE_CONTROL_IDLE_TIMEOUT_SEC");
    if (env && *env) {
        const int v = atoi(env);
        if (v > 0 && v <= 3600) return v;
    }
    return kDefaultIdleTimeoutSec;
}

std::string ErrnoString(int e) {
    return std::string(strerror(e)) + " (errno=" + std::to_string(e) + ")";
}

}  // namespace

SocketServer SocketServer::FromInitSocket(const std::string& name) {
    SocketServer s;
    s.initSocketName_ = name;
    return s;
}

SocketServer SocketServer::FromPath(const std::string& path) {
    SocketServer s;
    s.path_ = path;
    return s;
}

SocketServer::SocketServer(SocketServer&& other) noexcept
      : path_(std::move(other.path_)),
        initSocketName_(std::move(other.initSocketName_)),
        listenFd_(other.listenFd_),
        stop_(other.stop_),
        ownsPath_(other.ownsPath_) {
    // 转移所有权，避免 other 析构时关掉我们正在用的 fd / unlink 路径
    other.listenFd_ = -1;
    other.ownsPath_ = false;
}

SocketServer& SocketServer::operator=(SocketServer&& other) noexcept {
    if (this != &other) {
        Stop();
        if (listenFd_ >= 0) close(listenFd_);
        if (ownsPath_ && !path_.empty()) unlink(path_.c_str());

        path_           = std::move(other.path_);
        initSocketName_ = std::move(other.initSocketName_);
        listenFd_       = other.listenFd_;
        stop_           = other.stop_;
        ownsPath_       = other.ownsPath_;

        other.listenFd_ = -1;
        other.ownsPath_ = false;
    }
    return *this;
}

SocketServer::~SocketServer() {
    Stop();
    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
    if (ownsPath_ && !path_.empty()) {
        unlink(path_.c_str());
    }
}

bool SocketServer::Start(std::string* error) {
    // --- 模式 1：init 已创建好 socket，通过环境变量传 fd ---
    if (!initSocketName_.empty()) {
#if REMOTE_CONTROL_HAS_INIT_SOCKET
        // android_get_control_socket 读 ANDROID_SOCKET_<name>
        listenFd_ = android_get_control_socket(initSocketName_.c_str());
        if (listenFd_ < 0) {
            if (error) {
                *error = "android_get_control_socket(" + initSocketName_ +
                         ") failed; 检查 remote-control.rc 里是否有 'socket " +
                         initSocketName_ + " ...'";
            }
            return false;
        }
        // init 已经 listen 过了，这里不要再 listen
        path_ = std::string("/dev/socket/") + initSocketName_;
        ALOGI("remote-control: 接管 init socket fd=%d (%s)", listenFd_, path_.c_str());
        return true;
#else
        // init socket activation 是 Android 专有机制（libcutils）。
        // 主机上（单元测试）只能用 FromPath 模式。
        if (error) {
            *error = "init socket activation 仅在 Android 上可用，"
                     "主机测试请用 FromPath()";
        }
        return false;
#endif
    }

    // --- 模式 2：自己 bind 一个路径 ---
    if (path_.empty()) {
        if (error) *error = "既没有 init socket 名，也没有 socket 路径";
        return false;
    }
    if (path_.size() >= sizeof(sockaddr_un::sun_path)) {
        if (error) *error = "socket 路径过长: " + path_;
        return false;
    }

    listenFd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        if (error) *error = "socket() 失败: " + ErrnoString(errno);
        return false;
    }

    // 路径可能残留自上次异常退出
    unlink(path_.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path_.c_str(), path_.size());

    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (error) *error = "bind(" + path_ + ") 失败: " + ErrnoString(errno);
        close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    // 只允许属主和同组访问。调用方 UID 还会在 ServeConnection 里二次校验。
    // 默认 0660：属主与同组可访问。要放宽用 --socket-mode（例如让上位应用
    // 以自己的 UID 连进来）。放宽的代价是同一台设备上任何进程都能控制服务，
    // 所以默认保守，由部署方显式决定。
    if (chmod(path_.c_str(), socketMode_) < 0) {
        ALOGW("remote-control: chmod(%s, %04o) 失败: %s", path_.c_str(),
              static_cast<unsigned>(socketMode_), strerror(errno));
    }

    if (listen(listenFd_, kBacklog) < 0) {
        if (error) *error = "listen() 失败: " + ErrnoString(errno);
        close(listenFd_);
        listenFd_ = -1;
        unlink(path_.c_str());
        return false;
    }

    ownsPath_ = true;
    ALOGI("remote-control: 监听 %s (fd=%d)", path_.c_str(), listenFd_);
    return true;
}

void SocketServer::Stop() {
    stop_ = true;
    if (listenFd_ >= 0) {
        // shutdown 让阻塞中的 accept 返回，避免依赖超时轮询
        shutdown(listenFd_, SHUT_RDWR);
    }
}

void SocketServer::Run(const RequestHandler& handler) {
    if (listenFd_ < 0) {
        ALOGE("remote-control: Run() 在 Start() 之前被调用");
        return;
    }

    while (!stop_) {
        int connFd = accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (connFd < 0) {
            if (errno == EINTR) continue;
            if (stop_) break;
            ALOGE("remote-control: accept 失败: %s", strerror(errno));
            continue;
        }

        // 给连接设个空闲超时 —— 防止连上来就不说话的客户端永久占着线程。
        timeval tv{};
        tv.tv_sec  = IdleTimeoutSec();
        tv.tv_usec = 0;
        setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // 每个连接一个线程。
        //
        // ⚠️ 这里曾经是串行调用（ServeConnection 直接在 accept 循环里跑），
        //    后果是**一个长连接的客户端会把整个服务占住**：
        //      - 上位应用连上后处于空闲，其它客户端连不进来
        //      - 空闲超时一到，服务端把应用那条连接关掉，
        //        而应用并不知道，下次写入直接 Broken pipe
        //    实测就是这么暴露的。
        //
        //    真正的互斥放在 handler 里（见 main.cpp）：连接可以并发，
        //    但 Injector 是有状态的（按下/抬起、槽位映射），
        //    并发注入会互相破坏手势，所以**操作**必须串行。
        // 线程起不来就退回串行，至少不丢连接。
        // SpawnDetached 用 pthread_create，失败返回 false 而不是抛异常 ——
        // AOSP 是 -fno-exceptions，std::thread 抛出来就是整个进程 terminate。
        if (!SpawnDetached([this, connFd, &handler]() {
                ServeConnection(connFd, handler);
                close(connFd);
            })) {
            ALOGW("remote-control: 起线程失败，本连接串行处理");
            ServeConnection(connFd, handler);
            close(connFd);
        }
    }
    ALOGI("remote-control: accept 循环退出");
}

void SocketServer::ServeConnection(int connFd, const RequestHandler& handler) {
    // --- 校验调用方 UID ---
    ucred cred{};
    socklen_t len = sizeof(cred);
    if (getsockopt(connFd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        ALOGI("remote-control: 新连接 pid=%d uid=%d gid=%d", cred.pid, cred.uid, cred.gid);
        // TODO(鉴权): 在这里按 cred.uid / cred.pid 做白名单。
        //   当前仅依赖 socket 文件权限 (0660) 做粗粒度控制。
        //   生产环境建议：
        //     - 建一个专用的 AID（如 AID_REMOTE_CONTROL），客户端进程加入该组
        //     - 或校验 cred.uid 是否在允许列表内，否则直接拒绝
    }

    while (!stop_) {
        Request req{};
        std::string payload;
        int reqFd = -1;
        const int rc = RecvRequest(connFd, &req, &payload, &reqFd);
        if (rc < 0) break;          // 对端正常关闭
        if (rc > 0) {
            ALOGW("remote-control: 读请求失败: %s", strerror(rc));
            break;
        }

        ReplyPacket packet = handler(req, payload, reqFd, cred.uid);
        if (reqFd >= 0) close(reqFd);   // handler 若需要保留会自己 dup

        if (!SendReply(connFd, packet)) {
            ALOGW("remote-control: 发应答失败: %s", strerror(errno));
            if (packet.fd >= 0) close(packet.fd);
            break;
        }
        if (packet.fd >= 0) {
            close(packet.fd);   // SCM_RIGHTS 已经让内核复制了一份
        }
    }
}

int SocketServer::RecvRequest(int connFd, Request* out, std::string* payload,
                              int* outFd) {
    payload->clear();
    *outFd = -1;

    // 一次读「头 + payload」。SEQPACKET 有消息边界，多读的部分就是 payload，
    // 不需要长度前缀。缓冲区按上限一次备好，超长消息会被内核截断（MSG_TRUNC
    // 不设时剩余部分直接丢弃），下面用 n > 上限 来识别并报错。
    std::vector<char> buf(sizeof(Request) + kMaxRequestPayload);
    iovec iov{};
    iov.iov_base = buf.data();
    iov.iov_len  = buf.size();

    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;
    // 客户端可能传 fd（InstallApp 用 memfd 送 APK），所以这里要能收。
    // 其余命令收到 fd 一律关闭，防止泄漏。
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * kMaxFdsPerMessage)]{};
    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n;
    do {
        n = recvmsg(connFd, &msg, 0);
    } while (n < 0 && errno == EINTR);

    if (n == 0) return -1;                       // 对端正常关闭

    // SO_RCVTIMEO 到期会返回 EAGAIN/EWOULDBLOCK —— 对端连上但不发数据。
    // 单独报出来，否则日志里只会看到一句含糊的"读请求失败"。
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        ALOGW("remote-control: 连接空闲超过 %d 秒，主动断开", IdleTimeoutSec());
        return -1;
    }

    if (n < 0) return errno;
    if (static_cast<size_t>(n) < sizeof(Request)) return EMSGSIZE;

    // 收 fd：最多留一个（第一条消息里的第一个），其余关掉
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        const size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int* fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
        for (size_t i = 0; i < count; ++i) {
            if (*outFd < 0) {
                *outFd = fds[i];
            } else {
                ALOGW("remote-control: 一次只接受一个 fd，丢弃 fd=%d", fds[i]);
                close(fds[i]);
            }
        }
    }

    memcpy(out, buf.data(), sizeof(Request));
    const size_t payloadLen = static_cast<size_t>(n) - sizeof(Request);
    if (payloadLen > 0) payload->assign(buf.data() + sizeof(Request), payloadLen);
    return 0;
}

bool SocketServer::SendReply(int connFd, const ReplyPacket& packet) {
    Reply reply = packet.reply;

    iovec iov{};
    iov.iov_base = &reply;
    iov.iov_len  = sizeof(Reply);

    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    if (packet.fd >= 0) {
        msg.msg_control    = control;
        msg.msg_controllen = sizeof(control);

        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &packet.fd, sizeof(int));
    }

    ssize_t n;
    do {
        n = sendmsg(connFd, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);

    return n == static_cast<ssize_t>(sizeof(Reply));
}

}  // namespace remote_control
