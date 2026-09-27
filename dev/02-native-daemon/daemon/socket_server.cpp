// socket_server.cpp

#include "socket_server.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "autod_log.h"
#include "autod_platform.h"

#if AUTOD_HAS_INIT_SOCKET
#include <cutils/sockets.h>
#endif

namespace autod {
namespace {

constexpr size_t kMaxFdsPerMessage = 1;
constexpr int    kBacklog          = 8;

// 连接空闲超时。
//
// 为什么必须有：ServeConnection 是在 accept 循环里**串行**调用的。
// 没有超时的话，一个连上来就不发数据的客户端会让 recvmsg 永久阻塞，
// 整个服务随之卡死（症状："服务还在，但谁来都没反应"）。
//
// 为什么不做并发：Injector 是有状态的（downTime、触控槽位映射），
// 多线程并发注入会互相破坏手势状态。串行是这里的正确设计。
constexpr int kDefaultIdleTimeoutSec = 30;

// 允许用环境变量覆盖，方便测试（不然一个用例要等 30 秒）
// 和运维调优。非法值忽略，回退到默认。
int IdleTimeoutSec() {
    const char* env = getenv("AUTOD_IDLE_TIMEOUT_SEC");
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
#if AUTOD_HAS_INIT_SOCKET
        // android_get_control_socket 读 ANDROID_SOCKET_<name>
        listenFd_ = android_get_control_socket(initSocketName_.c_str());
        if (listenFd_ < 0) {
            if (error) {
                *error = "android_get_control_socket(" + initSocketName_ +
                         ") failed; 检查 autod.rc 里是否有 'socket " +
                         initSocketName_ + " ...'";
            }
            return false;
        }
        // init 已经 listen 过了，这里不要再 listen
        path_ = std::string("/dev/socket/") + initSocketName_;
        ALOGI("autod: 接管 init socket fd=%d (%s)", listenFd_, path_.c_str());
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
    if (chmod(path_.c_str(), 0660) < 0) {
        ALOGW("autod: chmod(%s) 失败: %s", path_.c_str(), strerror(errno));
    }

    if (listen(listenFd_, kBacklog) < 0) {
        if (error) *error = "listen() 失败: " + ErrnoString(errno);
        close(listenFd_);
        listenFd_ = -1;
        unlink(path_.c_str());
        return false;
    }

    ownsPath_ = true;
    ALOGI("autod: 监听 %s (fd=%d)", path_.c_str(), listenFd_);
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
        ALOGE("autod: Run() 在 Start() 之前被调用");
        return;
    }

    while (!stop_) {
        int connFd = accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (connFd < 0) {
            if (errno == EINTR) continue;
            if (stop_) break;
            ALOGE("autod: accept 失败: %s", strerror(errno));
            continue;
        }

        // 给连接设个空闲超时。
        //
        // 没有它的话，一个连上来就不说话的客户端会让 RecvRequest 里的
        // recvmsg 永久阻塞 —— 而 ServeConnection 是串行调用的，
        // 整个服务就被这一个连接卡死了。这是很容易踩的坑：
        // 症状是"服务还在，但谁来都没反应"。
        timeval tv{};
        tv.tv_sec  = IdleTimeoutSec();
        tv.tv_usec = 0;
        setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ServeConnection(connFd, handler);
        close(connFd);
    }
    ALOGI("autod: accept 循环退出");
}

void SocketServer::ServeConnection(int connFd, const RequestHandler& handler) {
    // --- 校验调用方 UID ---
    ucred cred{};
    socklen_t len = sizeof(cred);
    if (getsockopt(connFd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        ALOGI("autod: 新连接 pid=%d uid=%d gid=%d", cred.pid, cred.uid, cred.gid);
        // TODO(鉴权): 在这里按 cred.uid / cred.pid 做白名单。
        //   当前仅依赖 socket 文件权限 (0660) 做粗粒度控制。
        //   生产环境建议：
        //     - 建一个专用的 AID（如 AID_AUTOD），客户端进程加入该组
        //     - 或校验 cred.uid 是否在允许列表内，否则直接拒绝
    }

    while (!stop_) {
        Request req{};
        const int rc = RecvRequest(connFd, &req);
        if (rc < 0) break;          // 对端正常关闭
        if (rc > 0) {
            ALOGW("autod: 读请求失败: %s", strerror(rc));
            break;
        }

        ReplyPacket packet = handler(req, cred.uid);

        if (!SendReply(connFd, packet)) {
            ALOGW("autod: 发应答失败: %s", strerror(errno));
            if (packet.fd >= 0) close(packet.fd);
            break;
        }
        if (packet.fd >= 0) {
            close(packet.fd);   // SCM_RIGHTS 已经让内核复制了一份
        }
    }
}

int SocketServer::RecvRequest(int connFd, Request* out) {
    iovec iov{};
    iov.iov_base = out;
    iov.iov_len  = sizeof(Request);

    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;
    // 请求里不该带 fd。留 no-op 的 cmsg 缓冲以便丢弃对端误传的 fd，
    // 否则内核会因为 cmsg 缓冲不足而在 recvmsg 期间关闭连接。
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
        ALOGW("autod: 连接空闲超过 %d 秒，主动断开", IdleTimeoutSec());
        return -1;
    }

    if (n < 0) return errno;
    if (static_cast<size_t>(n) != sizeof(Request)) return EMSGSIZE;

    // 丢弃并关闭对端误传的 fd，防止 fd 泄漏
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            const size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            const int* fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
            for (size_t i = 0; i < count; ++i) {
                ALOGW("autod: 客户端不应发送 fd，已丢弃 fd=%d", fds[i]);
                close(fds[i]);
            }
        }
    }
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

}  // namespace autod
